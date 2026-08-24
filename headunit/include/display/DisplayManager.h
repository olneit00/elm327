#pragma once
//
// DisplayManager owns the LVGL lifecycle for the GC9A01 head unit.
//
// It initializes LVGL, registers the display through the built-in LovyanGFX
// driver, and advances LVGL's timer handler so queued draw commands reach the
// panel. Touch-input registration stays off because the head unit display is
// output only.
//
class DisplayManager {
 public:
  // Initializes LVGL and registers the GC9A01 panel. Returns false if the
  // built-in LovyanGFX driver could not create a display.
  static bool begin(unsigned int width, unsigned int height);

  // Runs LVGL's internal handler; call periodically from loop().
  static void tick();
};