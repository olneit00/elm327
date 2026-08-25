#pragma once
//
// DisplayManager owns the LVGL lifecycle for the GC9A01 head unit.
//
// It initializes LVGL, registers the display through the built-in LovyanGFX
// driver, and advances LVGL's timer handler so queued draw commands reach the
// panel. Touch-input registration stays off because the head unit display is
// output only.
//
// Thread safety: LVGL itself assumes every lv_* call happens on a single
// task. This project's lv_* calls do NOT all originate from one task,
// though - besides the main loop task (DisplayManager::tick(), ClockScreen,
// RadioScreen), RadioService's status/station-selected callbacks run
// synchronously inside AsyncWebServer request handlers (the async_tcp
// task) whenever a web API call triggers them, and those callbacks can
// reach ClockScreen/RadioScreen. Calling lv_* functions from two tasks
// without synchronization corrupts LVGL's internal state and has crashed/
// rebooted the ESP32 in practice. Anyone about to call an lv_* function -
// from ANY task, including the main loop task - must hold a
// DisplayManager::Lock for the duration of that call (or batch of calls).
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class DisplayManager {
 public:
  // Initializes LVGL and registers the GC9A01 panel. Returns false if the
  // built-in LovyanGFX driver could not create a display.
  static bool begin(unsigned int width, unsigned int height);

  // Runs LVGL's internal handler; call periodically from loop(). Acquires
  // the shared LVGL lock internally.
  static void tick();

  // RAII guard around the shared LVGL mutex - see class-level comment.
  // It is a *recursive* mutex, so nested acquisition (e.g. a helper that
  // takes its own Lock while a caller already holds one) is safe.
  // Always check acquired(): a failed acquisition (after a generous
  // timeout) means some other task is stuck holding the lock, and
  // touching LVGL anyway would be exactly the bug this class prevents.
  class Lock {
   public:
    Lock();
    ~Lock();
    bool acquired() const { return _acquired; }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    bool _acquired;
  };

 private:
  static SemaphoreHandle_t _lvglMutex;
};
