#include <Arduino.h>
#include <lvgl.h>

#include "display/DisplayDevice.hpp"
#include "display/DisplayManager.h"
#include "hardware/Pins.h"

namespace {

uint32_t MillisTick() { return millis(); }

LGFX lcd;

// Partial render buffer (RGB565). 24 lines of a 240-px panel = 11,520 bytes.
// Sized to fit DRAM alongside the growing WiFi/Radio web-radio stack.
lv_color_t renderBuffer[pins::SCREEN_WIDTH * 24];

// Non-DMA flush: pushImage (synchronous SPI, same path as fillScreen).
void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  static uint32_t s_flushes = 0;
  if (s_flushes < 8 || (s_flushes % 500) == 0) {
    Serial.printf("[FLUSH] #%lu (%d,%d)-(%d,%d) %ux%u\n",
                  s_flushes, area->x1, area->y1, area->x2, area->y2, w, h);
  }
  ++s_flushes;

  lcd.startWrite();
  lcd.pushImage(area->x1, area->y1, w, h, (uint16_t*)px_map);
  lcd.endWrite();

  lv_display_flush_ready(disp);
}

}  // namespace

bool DisplayManager::begin(unsigned int width, unsigned int height) {
  Serial.println(F("[DISPLAY] init LCD..."));
  if (!lcd.init()) {
    Serial.println(F("[DISPLAY] lcd.init() FAILED"));
    return false;
  }
  lcd.setColorDepth(16);
  lcd.setRotation(0);
  // LVGL 9 renders RGB565 little-endian (unswapped); LovyanGFX pushImage
  // expects native-endian uint16 buffers only when swapBytes is enabled.
  // Without this every pixel is byte-swapped on the panel (beige -> slate blue).
  lcd.setSwapBytes(true);
  Serial.println(F("[DISPLAY] LCD ready"));

  // Direct fillScreen test (known to work)
  lcd.fillScreen(0xF800); // red
  Serial.println(F("[DISPLAY] fillScreen red"));
  delay(300);

  // Direct pushImage test (non-DMA) - does THIS work?
  uint16_t greenBuf[60];
  for (int i = 0; i < 60; ++i) greenBuf[i] = 0x07E0; // green
  lcd.startWrite();
  lcd.pushImage(0, 0, 60, 1, greenBuf);
  lcd.endWrite();
  Serial.println(F("[DISPLAY] pushImage green strip (top)"));
  delay(500);

  // Full red via pushImage
  uint16_t redBuf[240];
  for (int i = 0; i < 240; ++i) redBuf[i] = 0xF800;
  lcd.startWrite();
  for (int y = 0; y < 240; ++y) {
    lcd.pushImage(0, y, 240, 1, redBuf);
  }
  lcd.endWrite();
  Serial.println(F("[DISPLAY] pushImage full red"));
  delay(500);

  // Now test LVGL with non-DMA pushImage flush
  lv_init();
  lv_tick_set_cb(MillisTick);

  lv_display_t* display = lv_display_create(width, height);
  lv_display_set_flush_cb(display, FlushCb);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display, renderBuffer, nullptr, sizeof(renderBuffer),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  Serial.printf("[DISPLAY] LVGL display=%p\n", (void*)display);

#ifdef HEADUNIT_TEST_MODE
  // LVGL color test - test mode only
  Serial.println(F("[DISPLAY] LVGL color test"));
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0xFF0000), 0);
  lv_timer_handler();
  delay(500);
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x00FF00), 0);
  lv_timer_handler();
  delay(500);
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x0000FF), 0);
  lv_timer_handler();
  delay(500);

  Serial.println(F("[DISPLAY] LVGL color test done"));
#endif
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
  lv_timer_handler();
  Serial.println(F("[DISPLAY] ready"));
  return true;
}

void DisplayManager::tick() { lv_timer_handler(); }