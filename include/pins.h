#pragma once

// GPIO assignments for the Waveshare ESP32-S3-Touch-LCD-2.8, from the Waveshare
// wiki and its LVGL_Arduino demo (Display_ST7789.h, Touch_CST328.h). Also used
// on the board: SD card 14/16/17/21, IMU + RTC I2C 10/11 (int 9, 12, 13),
// audio 38/47/48, battery ADC 8, power key 6/7, UART0 43/44, USB 19/20.

// Relay / SSR input driving the grinder motor (GPIO15 on the 12-pin header: not
// used on the board and not a strapping pin). -1 = motor commands are only
// logged. Fit a pull-down (10k to GND) so the relay stays off while the ESP32
// is in reset or booting.
constexpr int kRelayPin = 15;
constexpr bool kRelayActiveHigh = true;

// ST7789 display, SPI (write only, no MISO). Native orientation is portrait
// 240x320; the firmware rotates it to landscape.
constexpr int kLcdMosi = 45;
constexpr int kLcdSclk = 40;
constexpr int kLcdCs = 42;
constexpr int kLcdDc = 41;
constexpr int kLcdRst = 39;
constexpr int kLcdBacklight = 5;  // PWM, active high

// CST328 capacitive touch, I2C (own bus, separate from the IMU/RTC bus).
constexpr int kTouchSda = 1;
constexpr int kTouchScl = 3;
constexpr int kTouchInt = 4;
constexpr int kTouchRst = 2;
