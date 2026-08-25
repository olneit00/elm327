#pragma once
//
// ClockScreen renders an analog clock onto the active screen. It owns the
// dial face bitmap, the historical Opel hour/minute hands as rotated image
// sprites, and a procedural second hand.
//
// Hour and minute hands are pre-rendered RGB565A8 sprites (assets generated
// from the instrument photo sheet) pointing to 12 o'clock; update() only sets
// the rotation angle around the hub pivot baked into each sprite. The second
// hand stays an lv_line whose endpoint array lives inside this instance:
// LVGL stores a pointer to the array passed to lv_line_set_points() rather
// than copying it, so per-frame updates happen in place.
//
#include <lvgl.h>
#include <Arduino.h>

// (ClockScreen owns an analog clock; see header comment.)

#include "time/TimeProvider.h"

class ClockScreen {
 public:
  // Builds the static dial (ring + 12 hour ticks) and the three hands.
  void create();

  // Show or hide the clock screen without recreating objects (prevents LVGL leaks).
  void setVisible(bool visible);
  bool isVisible() const { return _visible; }

  // Sets the backing time source; update() polls it every frame.
  void attachTimeSource(ITimeSource* source);

  // When enabled, overrides the attached source with a fixed stamp. Used to
  // validate exact hand positions independently of any clock (test mode).
  void setManualTime(TimeOfDay value, bool enabled);

  // Recomputes hand angles from the current time and invalidates them so
  // the next DisplayManager::tick() flushes the changes. Also drives the
  // station-info overlay's auto-hide timeout - must be called every frame.
  void update();

  // Shows `text` (station name or "xx.xx MHz") in a small label centered
  // over the dial for durationMs, then hides it again automatically. Used
  // to briefly confirm which station was just selected (tune/seek) without
  // requiring the separate RadioScreen to be visible.
  //
  // Safe to call from ANY FreeRTOS task: acquires the shared
  // DisplayManager::Lock before touching LVGL (see that class for why -
  // this is reached from RadioService's tune/seek notification chain,
  // which runs synchronously inside AsyncWebServer request handlers, i.e.
  // the async_tcp task, not the main loop task that normally owns LVGL).
  void showStationInfo(const String& text, uint32_t durationMs = 5000);

 private:
  void renderFace();
  lv_obj_t* createSecondHand();
  void reorientSecondHand(float angleDeg);

  ITimeSource* source_ = nullptr;
  bool manualEnabled_ = false;
  TimeOfDay manualTime_{0, 0, 0};
  bool _visible = false;

  lv_obj_t* secondHand_ = nullptr;
  lv_obj_t* minuteHand_ = nullptr;
  lv_obj_t* hourHand_ = nullptr;
  lv_obj_t* faceImage_ = nullptr;

  // Station-info overlay (see showStationInfo()). _stationLabelVisible
  // tracks whether info is "currently pending" independent of whether the
  // clock screen itself is visible (setVisible() re-applies it if the user
  // switches back to the clock while the 5s window hasn't elapsed yet).
  lv_obj_t* _stationLabel = nullptr;
  bool _stationLabelVisible = false;
  uint32_t _stationLabelHideAtMs = 0;

  // Last handed-out rotation values in LVGL 0.1 deg units; skips redundant
  // invalidations when a hand did not visibly move.
  int32_t hourAngle10_ = -1;
  int32_t minuteAngle10_ = -1;

  // Persistent point storage consumed by the second-hand line (see header comment).
  lv_point_precise_t secondPts_[2];
  lv_point_precise_t tickPts_[12][2];
};