#pragma once

#include <lvgl.h>

// ST7789 display (SPI with DMA, LVGL's generic MIPI driver) and touch (I2C:
// CST328 on board V1, CST3530 on V2) of the Waveshare ESP32-S3-Touch-LCD-2.8,
// rotated to the 320x240 landscape the UI is built for. Call lv_init() first. The backlight stays off
// until displayBacklight(true), so the boot garbage in the panel RAM is never
// visible. All LVGL calls must stay on the loop task.
lv_display_t* displayBegin();
void displayBacklight(bool on);
