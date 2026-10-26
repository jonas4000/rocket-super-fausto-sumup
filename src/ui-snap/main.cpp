// Headless UI snapshots: runs the real Ui + Controller on an in-memory
// 320x240 display with a fake clock and scripted taps, writing BMP files.
// Used to check layouts without the hardware or a window.
//
//   .pio/build/ui-snap/program <step>...
//   steps: tap X Y | wait MS | approve | decline | shot NAME (writes NAME.bmp)
//          | press X Y / release (hold a touch, e.g. the Manual grind button)
//          | state (prints the controller state)
//          | offline / online (Wi-Fi lost / back)
//          | dump (prints the object tree with coordinates)
//          | shots N (sets the Single counter, e.g. 24 to make the next shot #25)
//          | score NAME N (adds a Flappy Beans score to the top 5)
//          | easy (Flappy Beans with wide openings: steady taps score points)
//          | hour H (local hour; -1 = clock unknown,
//            the default) | day D (local date as yyyymmdd, default 20261016)

#include <lvgl.h>
#include <src/widgets/span/lv_span_private.h>  // span text for dump (debug tool only)

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Controller.h"
#include "Doses.h"
#include "Ui.h"

using namespace grinder;

namespace {

constexpr int32_t kW = 320;
constexpr int32_t kH = 240;

uint32_t gTick = 0;
uint16_t gFrame[kW * kH];
uint8_t gDrawBuf[kW * kH * 2];
lv_point_t gPoint = {0, 0};
bool gPressed = false;

int gHour = -1;
uint32_t gDay = 20261016;

uint32_t nowMs() { return gTick; }

struct FakeClock : WallClock {
  bool now(LocalTime& out) override {
    out = {gDay, gHour, 0};
    return gHour >= 0;
  }
};

struct FakeNet : Connectivity {
  bool up = true;
  bool online() override { return up; }
};

struct FakeReader : PaymentProvider {
  PaymentStatus next = PaymentStatus::Pending;
  bool startCheckout(uint32_t, const char*) override {
    next = PaymentStatus::Pending;
    return true;
  }
  PaymentStatus pollStatus() override { return next; }
  void cancelCheckout() override {}
  void refundLastPayment() override {}
};

struct NullMotor : Motor {
  void setOn(bool) override {}
};

struct NullStore : SettingsStore {
  void load(Dose*, size_t) override {}
  void save(const Dose*, size_t) override {}
};

void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  auto* src = reinterpret_cast<uint16_t*>(px);
  int32_t w = lv_area_get_width(area);
  for (int32_t y = area->y1; y <= area->y2; ++y)
    std::memcpy(&gFrame[y * kW + area->x1], &src[(y - area->y1) * w], w * 2);
  lv_display_flush_ready(disp);
}

void readPointer(lv_indev_t*, lv_indev_data_t* data) {
  data->point = gPoint;
  data->state = gPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void writeBmp(const char* name) {
  char path[256];
  std::snprintf(path, sizeof path, "%s.bmp", name);
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  const uint32_t rowBytes = kW * 3;  // 960, already 4-byte aligned
  const uint32_t dataSize = rowBytes * kH;
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int off, uint32_t v) {
    for (int i = 0; i < 4; ++i) header[off + i] = (v >> (8 * i)) & 0xFF;
  };
  put32(2, 54 + dataSize);
  put32(10, 54);
  put32(14, 40);
  put32(18, kW);
  put32(22, static_cast<uint32_t>(-kH));  // top-down
  header[26] = 1;
  header[28] = 24;
  put32(34, dataSize);
  std::fwrite(header, 1, sizeof header, f);
  for (int32_t i = 0; i < kW * kH; ++i) {
    uint16_t v = gFrame[i];
    uint8_t bgr[3] = {static_cast<uint8_t>((v & 0x1F) << 3),
                      static_cast<uint8_t>(((v >> 5) & 0x3F) << 2),
                      static_cast<uint8_t>(((v >> 11) & 0x1F) << 3)};
    std::fwrite(bgr, 1, 3, f);
  }
  std::fclose(f);
  std::printf("wrote %s\n", path);
}

void dump(lv_obj_t* obj, int depth) {
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const char* text = "";
  if (lv_obj_check_type(obj, &lv_label_class)) {
    text = lv_label_get_text(obj);
  } else if (lv_obj_check_type(obj, &lv_spangroup_class) &&
             lv_spangroup_get_span_count(obj) > 0) {
    text = lv_spangroup_get_child(obj, 0)->txt;  // first span
  }
  std::printf("%*s(%d,%d)-(%d,%d) %s\n", depth * 2, "", static_cast<int>(a.x1),
              static_cast<int>(a.y1), static_cast<int>(a.x2), static_cast<int>(a.y2), text);
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
    dump(lv_obj_get_child(obj, i), depth + 1);
}

}  // namespace

int main(int argc, char** argv) {
  lv_init();
  lv_tick_set_cb(nowMs);

  lv_display_t* disp = lv_display_create(kW, kH);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, gDrawBuf, nullptr, sizeof gDrawBuf,
                         LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(disp, flush);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, readPointer);

  FakeReader reader;
  NullMotor motor;
  NullStore store;
  FakeClock wallClock;
  FakeNet net;
  Controller ctl(reader, motor, store, gDoses, kDoseCount);
  ctl.setWallClock(&wallClock);
  ctl.setConnectivity(&net);
  ctl.begin(nowMs());
  Ui ui(ctl, nowMs, "1234");
  ui.begin(lv_screen_active());

  auto step = [&](uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
      gTick += 5;
      ctl.tick(gTick);
      ui.update();
      lv_timer_handler();
    }
  };
  step(50);

  for (int i = 1; i < argc; ++i) {
    const char* cmd = argv[i];
    if (!std::strcmp(cmd, "tap") && i + 2 < argc) {
      gPoint = {static_cast<int32_t>(std::atoi(argv[i + 1])),
                static_cast<int32_t>(std::atoi(argv[i + 2]))};
      i += 2;
      gPressed = true;
      step(100);
      gPressed = false;
      step(100);
    } else if (!std::strcmp(cmd, "press") && i + 2 < argc) {
      gPoint = {static_cast<int32_t>(std::atoi(argv[i + 1])),
                static_cast<int32_t>(std::atoi(argv[i + 2]))};
      i += 2;
      gPressed = true;
      step(100);
    } else if (!std::strcmp(cmd, "release")) {
      gPressed = false;
      step(100);
    } else if (!std::strcmp(cmd, "offline") || !std::strcmp(cmd, "online")) {
      net.up = !std::strcmp(cmd, "online");
      step(50);
    } else if (!std::strcmp(cmd, "state")) {
      std::printf("state=%s\n", stateName(ctl.state()));
    } else if (!std::strcmp(cmd, "wait") && i + 1 < argc) {
      step(static_cast<uint32_t>(std::atoi(argv[++i])));
    } else if (!std::strcmp(cmd, "approve")) {
      reader.next = PaymentStatus::Succeeded;
    } else if (!std::strcmp(cmd, "decline")) {
      reader.next = PaymentStatus::Failed;
    } else if (!std::strcmp(cmd, "hour") && i + 1 < argc) {
      gHour = std::atoi(argv[++i]);
    } else if (!std::strcmp(cmd, "day") && i + 1 < argc) {
      gDay = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (!std::strcmp(cmd, "shots") && i + 1 < argc) {
      gDoses[0].count = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (!std::strcmp(cmd, "score") && i + 2 < argc) {
      ctl.submitScore(argv[i + 1], static_cast<uint32_t>(std::atoi(argv[i + 2])), gTick);
      i += 2;
    } else if (!std::strcmp(cmd, "easy")) {
      FlappyConfig c;
      c.gap = 150;
      ui.setGameConfig(c);
    } else if (!std::strcmp(cmd, "dump")) {
      dump(lv_screen_active(), 0);
    } else if (!std::strcmp(cmd, "shot") && i + 1 < argc) {
      lv_obj_invalidate(lv_screen_active());
      lv_refr_now(disp);
      writeBmp(argv[++i]);
    } else {
      std::fprintf(stderr, "bad step: %s\n", cmd);
      return 1;
    }
  }
  return 0;
}
