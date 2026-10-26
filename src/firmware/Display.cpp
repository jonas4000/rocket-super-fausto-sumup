#include "Display.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>

#include "pins.h"
// Not pulled in by lvgl.h in LVGL 9.2.
#include "src/drivers/display/st7789/lv_st7789.h"

namespace {

// Native panel size (portrait). The UI sees 320x240 after the rotation.
constexpr uint32_t kPanelW = 240, kPanelH = 320;
// Turns the portrait panel into landscape. If the picture is upside down for
// how the board is mounted, use LV_DISPLAY_ROTATION_270; touch follows.
constexpr lv_display_rotation_t kRotation = LV_DISPLAY_ROTATION_90;
constexpr uint32_t kSpiHz = 80 * 1000 * 1000;
// Two partial buffers of 40 lines (25 KB each) in DMA-capable internal RAM:
// LVGL renders into one while the other is sent.
constexpr size_t kBufBytes = 320 * 40 * sizeof(uint16_t);

// Panel tuning from Waveshare's demo (porch, gate, VCOM, power, gamma).
// Sleep out, pixel format and address mode (rotation) come from LVGL's
// driver. RAMCTRL E8 makes the panel take RGB565 little-endian, the order
// LVGL renders in, so no byte swap is needed.
// Format: command, number of bytes, bytes; LV_LCD_CMD_DELAY_MS/EOF ends it.
const uint8_t kPanelInit[] = {
    0xB0, 2, 0x00, 0xE8,                          // RAMCTRL
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,        // PORCTRL
    0xB7, 1, 0x75,                                // GCTRL
    0xBB, 1, 0x1A,                                // VCOMS
    0xC0, 1, 0x2C,                                // LCMCTRL
    0xC2, 2, 0x01, 0xFF,                          // VDVVRHEN
    0xC3, 1, 0x13,                                // VRHS
    0xC4, 1, 0x20,                                // VDVS
    0xC6, 1, 0x0F,                                // FRCTRL2 (60 Hz)
    0xD0, 2, 0xA4, 0xA1,                          // PWCTRL1
    0xD6, 1, 0xA1,
    0xE0, 14, 0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30,
    0xE1, 14, 0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E,
    LV_LCD_CMD_DELAY_MS, LV_LCD_CMD_EOF,
};

esp_lcd_panel_io_handle_t gIo = nullptr;
lv_display_t* gDisplay = nullptr;

// The SPI driver waits for queued pixel transfers before sending a command,
// so the next flush's window commands never cut into the previous transfer.
void sendCmd(lv_display_t*, const uint8_t* cmd, size_t, const uint8_t* param, size_t paramSize) {
  esp_lcd_panel_io_tx_param(gIo, *cmd, param, paramSize);
}

void sendColor(lv_display_t*, const uint8_t* cmd, size_t, uint8_t* param, size_t paramSize) {
  esp_lcd_panel_io_tx_color(gIo, *cmd, param, paramSize);
}

// From the SPI interrupt: the buffer has been sent and LVGL may reuse it.
bool colorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  lv_display_flush_ready(gDisplay);
  return false;
}

// --- Touch -------------------------------------------------------------------
// Board V1 has a CST328 (0x1A, 16-bit registers), V2 (shipped since June 2026)
// a CST3530 (0x58, 32-bit registers). Detected at boot like Waveshare's V2
// demo (Touch_CST328.cpp, Touch_CST3530.cpp), whose protocol this follows.
// Both give coordinates of the portrait panel; LVGL rotates them with the
// display.
enum class TouchChip { None, Cst328, Cst3530 };

constexpr uint8_t kCst328Addr = 0x1A;
constexpr uint32_t kCst328Points = 0xD000;  // id/status, x[11:4], y[11:4], x[3:0] y[3:0], pressure
constexpr uint32_t kCst328Count = 0xD005;   // low nibble: points; write 0 to acknowledge
constexpr uint32_t kCst328InfoMode = 0xD101;
constexpr uint32_t kCst328NormalMode = 0xD109;
constexpr uint32_t kCst328ResX = 0xD1F8;     // 2 x uint16 LE: resolution x, y
constexpr uint32_t kCst328BootTime = 0xD1FC;  // bytes 2-3: 0xCACA when the firmware is ok

constexpr uint8_t kCst3530Addr = 0x58;
constexpr uint32_t kCst3530Data = 0xD0070000;     // 9 bytes, first point at 4..8
constexpr uint32_t kCst3530EndRead = 0xD00002AB;  // acknowledge, no data

// The controllers report about every 10 ms while touched, and a read without
// a new report doesn't mean the finger is gone. Released only after this long
// without a report, so a held button (Manual grind) doesn't flicker.
constexpr uint32_t kReleaseMs = 60;

TouchChip gChip = TouchChip::None;
uint16_t gResX = kPanelW, gResY = kPanelH;
bool gPressed = false;
int32_t gX = 0, gY = 0;
uint32_t gLastReportMs = 0;

// Register addresses are big-endian, regBytes long (2 or 4).
bool touchWrite(uint8_t addr, uint32_t reg, size_t regBytes, const uint8_t* data, size_t len) {
  Wire1.beginTransmission(addr);
  for (size_t i = regBytes; i-- > 0;) Wire1.write(static_cast<uint8_t>(reg >> (8 * i)));
  if (len) Wire1.write(data, len);
  return Wire1.endTransmission(true) == 0;
}

bool touchRead(uint8_t addr, uint32_t reg, size_t regBytes, uint8_t* data, size_t len) {
  Wire1.beginTransmission(addr);
  for (size_t i = regBytes; i-- > 0;) Wire1.write(static_cast<uint8_t>(reg >> (8 * i)));
  if (Wire1.endTransmission(false) != 0) return false;  // repeated start
  if (Wire1.requestFrom(addr, len) != len) return false;
  return Wire1.readBytes(data, len) == len;
}

bool probeCst328() {
  uint8_t res[4] = {}, boot[4] = {};
  touchWrite(kCst328Addr, kCst328InfoMode, 2, nullptr, 0);
  const bool ok = touchRead(kCst328Addr, kCst328ResX, 2, res, sizeof res) &&
                  touchRead(kCst328Addr, kCst328BootTime, 2, boot, sizeof boot);
  touchWrite(kCst328Addr, kCst328NormalMode, 2, nullptr, 0);
  if (!ok || boot[2] != 0xCA || boot[3] != 0xCA) return false;
  // Scale if the controller reports another resolution than the panel's.
  const uint16_t resX = res[0] | res[1] << 8, resY = res[2] | res[3] << 8;
  if (resX && resY) {
    gResX = resX;
    gResY = resY;
  }
  return true;
}

bool probeCst3530() {
  Wire1.beginTransmission(kCst3530Addr);
  return Wire1.endTransmission(true) == 0;
}

void touchBegin() {
  Wire1.begin(kTouchSda, kTouchScl, 400000);
  pinMode(kTouchInt, INPUT);
  pinMode(kTouchRst, OUTPUT);
  digitalWrite(kTouchRst, LOW);
  delay(20);
  digitalWrite(kTouchRst, HIGH);
  delay(300);  // controller firmware boot

  // CST3530 first: a plain address check, while the CST328 probe would log
  // I2C errors on every V2 boot.
  if (probeCst3530()) gChip = TouchChip::Cst3530;
  else if (probeCst328()) gChip = TouchChip::Cst328;
  static const char* const kNames[] = {"NOT FOUND", "CST328", "CST3530"};
  Serial.printf("touch: %s, %ux%u\n", kNames[static_cast<int>(gChip)], gResX, gResY);
}

// One new report for the first point, in controller coordinates.
bool readPoint(uint32_t& x, uint32_t& y) {
  if (gChip == TouchChip::Cst328) {
    uint8_t count = 0;
    if (!touchRead(kCst328Addr, kCst328Count, 2, &count, 1)) return false;
    const uint8_t n = count & 0x0F;
    if (n == 0) return false;
    uint8_t p[5];
    const bool ok = n <= 5 && touchRead(kCst328Addr, kCst328Points, 2, p, sizeof p);
    const uint8_t zero = 0;
    touchWrite(kCst328Addr, kCst328Count, 2, &zero, 1);
    if (!ok) return false;
    x = (p[1] << 4) | (p[3] >> 4);
    y = (p[2] << 4) | (p[3] & 0x0F);
    return true;
  }
  if (gChip == TouchChip::Cst3530) {
    uint8_t b[9];
    if (!touchRead(kCst3530Addr, kCst3530Data, 4, b, sizeof b)) return false;
    const uint8_t n = b[3] & 0x0F;
    const bool ok = n >= 1 && n <= 5 && (b[8] & 0xF0) != 0;
    touchWrite(kCst3530Addr, kCst3530EndRead, 4, nullptr, 0);
    if (!ok) return false;
    x = ((b[7] & 0x0F) << 8) | b[4];
    y = ((b[7] & 0xF0) << 4) | b[5];
    return true;
  }
  return false;
}

void readTouch(lv_indev_t*, lv_indev_data_t* data) {
  const uint32_t now = millis();
  uint32_t x, y;
  if (readPoint(x, y)) {
    gX = min<uint32_t>(x * kPanelW / gResX, kPanelW - 1);
    gY = min<uint32_t>(y * kPanelH / gResY, kPanelH - 1);
    gLastReportMs = now;
    if (!gPressed) Serial.printf("touch down %d,%d (panel)\n", (int)gX, (int)gY);
    gPressed = true;
  } else if (gPressed && now - gLastReportMs > kReleaseMs) {
    gPressed = false;
    Serial.println("touch up");
  }
  data->point.x = gX;
  data->point.y = gY;
  data->state = gPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

void displayBacklight(bool on) {
  // 20 kHz PWM (inaudible), 10 bit; full brightness for now.
  static bool attached = false;
  if (!attached) attached = ledcAttach(kLcdBacklight, 20000, 10);
  ledcWrite(kLcdBacklight, on ? 1023 : 0);
}

lv_display_t* displayBegin() {
  pinMode(kLcdBacklight, OUTPUT);
  digitalWrite(kLcdBacklight, LOW);

  // Hardware reset; LVGL's driver then sends a software reset as well.
  pinMode(kLcdRst, OUTPUT);
  digitalWrite(kLcdRst, LOW);
  delay(20);
  digitalWrite(kLcdRst, HIGH);
  delay(120);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = kLcdMosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = kLcdSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kBufBytes;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = kLcdCs;
  io.dc_gpio_num = kLcdDc;
  io.spi_mode = 0;
  io.pclk_hz = kSpiHz;
  io.trans_queue_depth = 10;
  io.on_color_trans_done = colorDone;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                                           &io, &gIo));

  gDisplay = lv_st7789_create(kPanelW, kPanelH, LV_LCD_FLAG_NONE, sendCmd, sendColor);
  lv_st7789_send_cmd_list(gDisplay, kPanelInit);
  lv_st7789_set_invert(gDisplay, true);  // this IPS panel needs inversion on

  void* buf1 = heap_caps_malloc(kBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void* buf2 = heap_caps_malloc(kBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_display_set_buffers(gDisplay, buf1, buf2, kBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_rotation(gDisplay, kRotation);

  touchBegin();
  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, readTouch);
  lv_indev_set_display(touch, gDisplay);
  // Read every 10 ms instead of every frame: quicker flaps in Flappy Beans
  // and a reliable release timeout.
  lv_timer_set_period(lv_indev_get_read_timer(touch), 10);

  return gDisplay;
}
