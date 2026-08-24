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
  // the next DisplayManager::tick() flushes the changes.
  void update();

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

  // Last handed-out rotation values in LVGL 0.1 deg units; skips redundant
  // invalidations when a hand did not visibly move.
  int32_t hourAngle10_ = -1;
  int32_t minuteAngle10_ = -1;

  // Persistent point storage consumed by the second-hand line (see header comment).
  lv_point_precise_t secondPts_[2];
  lv_point_precise_t tickPts_[12][2];
};