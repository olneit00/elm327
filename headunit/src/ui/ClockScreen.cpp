#include <math.h>

#include "assets/clock_face.c"
#include "assets/needle_clock_hour.c"
#include "assets/needle_clock_min.c"
#include "ui/ClockScreen.h"

namespace {
constexpr int16_t kCenterX = 120;
constexpr int16_t kCenterY = 120;
constexpr int16_t kTickInner = 96;
constexpr int16_t kTickOuter = 106;

// The compiled-in clock_face asset always provides dial, numbers, and ticks.
constexpr bool kFaceBitmapAvailable = true;

// Second hand: reaches the minute track with a short counterweight tail.
constexpr int16_t kSecondLen = 110;
constexpr int16_t kTailLen = 12;

float DegToRad(float deg) { return deg * static_cast<float>(M_PI) / 180.0F; }

void Polar(int16_t cx, int16_t cy, int16_t len, float angleDeg, lv_point_precise_t& out) {
  float rad = DegToRad(angleDeg);
  out.x = cx + static_cast<int16_t>(lroundf(len * sin(rad)));
  out.y = cy - static_cast<int16_t>(lroundf(len * cos(rad)));
}

// Second hand: 6 deg per second.
float SecondAngle(TimeOfDay t) { return 6.0F * t.second; }

// Minute hand advances continuously across the second field.
float MinuteAngle(TimeOfDay t) { return 6.0F * (t.minute + t.second / 60.0F); }

// Hour hand sweeps smoothly across the minute field.
float HourAngle(TimeOfDay t) {
  return 30.0F * (t.hour + t.minute / 60.0F + t.second / 3600.0F);
}
}  // namespace

// Mounts a historical needle sprite: the generated bitmaps point to 12
// o'clock and carry their hub pivot, so positioning is just placing that
// pivot onto the dial center; update() only rotates around it.
static void mountNeedle(lv_obj_t* hand, const lv_image_dsc_t* sprite, int32_t pivotX,
                        int32_t pivotY) {
  lv_image_set_src(hand, sprite);
  lv_image_set_pivot(hand, pivotX, pivotY);
  lv_image_set_antialias(hand, true);
  lv_obj_set_pos(hand, kCenterX - pivotX, kCenterY - pivotY);
}

void ClockScreen::create() {
   // Only build objects once; repeated calls use setVisible instead (prevents LVGL leaks).
   if (secondHand_ != nullptr) {
      setVisible(true);
      return;
   }

   lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x101418), 0);
   lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

   // Historical face as the bottom-most layer: first child under all hands.
   faceImage_ = lv_image_create(lv_screen_active());
   lv_image_set_src(faceImage_, &clock_face);
   lv_obj_set_pos(faceImage_, 0, 0);

   // Tick marks and numbers are part of the face bitmap; the procedural
   // ticks are only a fallback for a build without the bitmap asset.
   if (!kFaceBitmapAvailable) {
      renderFace();
   }

   // Historical Opel hands: hour below, minute above (smaller hub), both
   // rotated bitmap sprites. The sprites carry their own hubs, so no
   // procedural center cap is needed.
   hourHand_ = lv_image_create(lv_screen_active());
   mountNeedle(hourHand_, &needle_clock_hour, NEEDLE_CLOCK_HOUR_PIVOT_X,
               NEEDLE_CLOCK_HOUR_PIVOT_Y);
   minuteHand_ = lv_image_create(lv_screen_active());
   mountNeedle(minuteHand_, &needle_clock_min, NEEDLE_CLOCK_MIN_PIVOT_X,
               NEEDLE_CLOCK_MIN_PIVOT_Y);

   // Second hand on top: thin red line, procedural.
   secondHand_ = createSecondHand();

   // Station-info overlay: created last so it renders above the hands.
   // Hidden by default; showStationInfo()/update() control its lifetime.
   _stationLabel = lv_label_create(lv_screen_active());
   lv_obj_remove_style_all(_stationLabel);
   lv_obj_set_style_bg_color(_stationLabel, lv_color_hex(0x000000), 0);
   lv_obj_set_style_bg_opa(_stationLabel, LV_OPA_70, 0);
   lv_obj_set_style_radius(_stationLabel, 6, 0);
   lv_obj_set_style_pad_all(_stationLabel, 6, 0);
   lv_obj_set_style_text_color(_stationLabel, lv_color_hex(0xFFFFFF), 0);
   lv_obj_set_style_text_align(_stationLabel, LV_TEXT_ALIGN_CENTER, 0);
   lv_label_set_long_mode(_stationLabel, LV_LABEL_LONG_DOT);
   lv_obj_set_width(_stationLabel, 140);
   lv_obj_align(_stationLabel, LV_ALIGN_CENTER, 0, 0);
   lv_obj_add_flag(_stationLabel, LV_OBJ_FLAG_HIDDEN);

   _visible = true;
}

void ClockScreen::setVisible(bool visible) {
   if (_visible == visible || secondHand_ == nullptr) return;
   _visible = visible;

   // Toggle visibility of all clock objects without destroying them.
   lv_obj_t* objs[] = {faceImage_, minuteHand_, hourHand_, secondHand_};
   for (lv_obj_t* obj : objs) {
      if (obj) {
         if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(obj);
         } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
         }
      }
   }

   // The station-info overlay follows _stationLabelVisible (still within
   // its 5s window) rather than always mirroring the clock's own
   // visibility - re-showing the clock mid-countdown should still show it.
   if (_stationLabel) {
      if (visible && _stationLabelVisible) {
         lv_obj_clear_flag(_stationLabel, LV_OBJ_FLAG_HIDDEN);
         lv_obj_invalidate(_stationLabel);
      } else {
         lv_obj_add_flag(_stationLabel, LV_OBJ_FLAG_HIDDEN);
      }
   }
}

void ClockScreen::attachTimeSource(ITimeSource* source) { source_ = source; }

void ClockScreen::setManualTime(TimeOfDay value, bool enabled) {
  manualEnabled_ = enabled;
  manualTime_ = value;
}

void ClockScreen::update() {
  _applyPendingStationInfo();

  if (_stationLabelVisible && millis() >= _stationLabelHideAtMs) {
    _stationLabelVisible = false;
    if (_stationLabel) {
      lv_obj_add_flag(_stationLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (source_ == nullptr && !manualEnabled_) {
    return;
  }
  TimeOfDay t = manualEnabled_ ? manualTime_ : source_->now();

  // Bitmap hands: rotate around the hub pivot. LVGL angles are 0.1 deg,
  // positive clockwise, matching the sprites' 12 o'clock rest pose.
  int32_t hourAngle = static_cast<int32_t>(lroundf(HourAngle(t) * 10.0F)) % 3600;
  if (hourAngle != hourAngle10_) {
    hourAngle10_ = hourAngle;
    lv_image_set_rotation(hourHand_, hourAngle);
  }

  int32_t minuteAngle = static_cast<int32_t>(lroundf(MinuteAngle(t) * 10.0F)) % 3600;
  if (minuteAngle != minuteAngle10_) {
    minuteAngle10_ = minuteAngle;
    lv_image_set_rotation(minuteHand_, minuteAngle);
  }

  reorientSecondHand(SecondAngle(t));
}

void ClockScreen::showStationInfo(const String& text, uint32_t durationMs) {
  // Callable from any FreeRTOS task (see header comment) - never touches
  // LVGL here. Only stash the request; _applyPendingStationInfo() (called
  // from update() on the main loop task) does the actual rendering.
  portENTER_CRITICAL(&_pendingMux);
  strlcpy(_pendingStationText, text.c_str(), sizeof(_pendingStationText));
  _pendingDurationMs = durationMs;
  _pendingStationInfo = true;
  portEXIT_CRITICAL(&_pendingMux);
}

void ClockScreen::_applyPendingStationInfo() {
  bool pending;
  char text[sizeof(_pendingStationText)];
  uint32_t durationMs;

  portENTER_CRITICAL(&_pendingMux);
  pending = _pendingStationInfo;
  if (pending) {
    memcpy(text, _pendingStationText, sizeof(text));
    durationMs = _pendingDurationMs;
    _pendingStationInfo = false;
  }
  portEXIT_CRITICAL(&_pendingMux);

  if (!pending || _stationLabel == nullptr) return;  // create() has not run yet

  lv_label_set_text(_stationLabel, text);
  _stationLabelVisible = true;
  _stationLabelHideAtMs = millis() + durationMs;

  if (_visible) {
    lv_obj_clear_flag(_stationLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(_stationLabel);
  }
}

lv_obj_t* ClockScreen::createSecondHand() {
  lv_obj_t* hand = lv_line_create(lv_screen_active());
  lv_obj_remove_style_all(hand);
  lv_obj_set_style_line_width(hand, 2, 0);
  lv_obj_set_style_line_color(hand, lv_color_hex(0xFF3B30), 0);
  lv_obj_set_style_line_opa(hand, LV_OPA_COVER, 0);
  lv_obj_set_style_line_rounded(hand, true, 0);
  return hand;
}

void ClockScreen::reorientSecondHand(float angleDeg) {
  // Tail sticks out opposite the tip through the dial center.
  Polar(kCenterX, kCenterY, kTailLen, angleDeg + 180.0F, secondPts_[0]);
  Polar(kCenterX, kCenterY, kSecondLen, angleDeg, secondPts_[1]);
  lv_line_set_points(secondHand_, secondPts_, 2);
  lv_obj_invalidate(secondHand_);
}

void ClockScreen::renderFace() {
  for (int i = 0; i < 12; ++i) {
    float angle = 30.0F * i;
    Polar(kCenterX, kCenterY, kTickInner, angle, tickPts_[i][0]);
    Polar(kCenterX, kCenterY, kTickOuter, angle, tickPts_[i][1]);

    lv_obj_t* line = lv_line_create(lv_screen_active());
    lv_obj_remove_style_all(line);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_line_set_points(line, tickPts_[i], 2);
    lv_obj_invalidate(line);
  }
}
