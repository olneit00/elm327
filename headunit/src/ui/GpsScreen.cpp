//
// GpsScreen implementation
// LVGL UI showing GPS data received over the CAN bus on the GC9A01 round
// display. See headunit/include/ui/GpsScreen.h.
//

#include <cstdio>
#include "ui/GpsScreen.h"
#include "display/DisplayManager.h"
#include <lvgl.h>

// Same palette as RadioScreen for visual consistency across screens.
static const lv_color_t COLOR_BG = { 0x0a, 0x0a, 0x0a };
static const lv_color_t COLOR_FG = { 0xe0, 0xe0, 0xe0 };
static const lv_color_t COLOR_FG_MUTED = { 0x88, 0x88, 0x88 };
static const lv_color_t COLOR_ACCENT = { 0x00, 0xcc, 0x44 };
static const lv_color_t COLOR_ACCENT_WARM = { 0xff, 0x88, 0x00 };
static const lv_color_t COLOR_DANGER = { 0xcc, 0x33, 0x33 };

static const lv_font_t* FONT_SMALL = &lv_font_montserrat_14;
static const lv_font_t* FONT_MEDIUM = &lv_font_montserrat_14;

GpsScreen::GpsScreen() {
}

GpsScreen::~GpsScreen() {
    if (_container) {
        lv_obj_del(_container);
        _container = nullptr;
    }
}

void GpsScreen::create() {
    // Runs once from setup(); see RadioScreen::create() / DisplayManager::Lock
    // for why every LVGL touchpoint takes the shared lock.
    DisplayManager::Lock guard;
    if (!guard.acquired()) return;

    _createContainer();
    _createLabels();

    lv_obj_add_flag(_container, LV_OBJ_FLAG_HIDDEN);
    _visible = false;
}

void GpsScreen::_createContainer() {
    _container = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(_container);
    lv_obj_set_size(_container, 240, 240);
    lv_obj_set_style_bg_color(_container, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 16, 0);
    lv_obj_set_flex_flow(_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(_container, 6, 0);
}

void GpsScreen::_createLabels() {
    _titleLabel = lv_label_create(_container);
    lv_label_set_text(_titleLabel, "GPS");
    lv_obj_set_style_text_font(_titleLabel, FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(_titleLabel, COLOR_ACCENT, 0);

    _positionLabel = lv_label_create(_container);
    lv_label_set_text(_positionLabel, "--.------, --.------");
    lv_obj_set_style_text_font(_positionLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_positionLabel, COLOR_FG, 0);
    lv_obj_set_style_text_align(_positionLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_positionLabel, 208);

    _fixLabel = lv_label_create(_container);
    lv_label_set_text(_fixLabel, "Kein Fix");
    lv_obj_set_style_text_font(_fixLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_fixLabel, COLOR_FG_MUTED, 0);

    _satellitesLabel = lv_label_create(_container);
    lv_label_set_text(_satellitesLabel, "Satelliten: -");
    lv_obj_set_style_text_font(_satellitesLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_satellitesLabel, COLOR_FG, 0);

    _speedLabel = lv_label_create(_container);
    lv_label_set_text(_speedLabel, "Speed: -- km/h  Kurs: --°");
    lv_obj_set_style_text_font(_speedLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_speedLabel, COLOR_FG, 0);

    _altitudeLabel = lv_label_create(_container);
    lv_label_set_text(_altitudeLabel, "Höhe: -- m");
    lv_obj_set_style_text_font(_altitudeLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_altitudeLabel, COLOR_FG, 0);

    _busLabel = lv_label_create(_container);
    lv_label_set_text(_busLabel, "CAN: --");
    lv_obj_set_style_text_font(_busLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_busLabel, COLOR_ACCENT_WARM, 0);
}

void GpsScreen::update(const CanGpsData& gps) {
    if (!_visible) return;

    // update() can be called from the main loop task; take the shared LVGL
    // lock regardless, matching RadioScreen::update().
    DisplayManager::Lock guard;
    if (!guard.acquired()) return;

    char buf[48];

    if (gps.valid) {
        snprintf(buf, sizeof(buf), "%.6f, %.6f", gps.latitude, gps.longitude);
    } else {
        snprintf(buf, sizeof(buf), "--.------, --.------");
    }
    lv_label_set_text(_positionLabel, buf);

    if (gps.activeFix) {
        snprintf(buf, sizeof(buf), "Fix: 3D  Q=%u", gps.fixQuality);
        lv_label_set_text(_fixLabel, buf);
        lv_obj_set_style_text_color(_fixLabel, COLOR_ACCENT, 0);
    } else {
        lv_label_set_text(_fixLabel, "Kein Fix");
        lv_obj_set_style_text_color(_fixLabel, COLOR_FG_MUTED, 0);
    }

    snprintf(buf, sizeof(buf), "Satelliten: %u / %u", gps.satellitesInUse, gps.satellitesInView);
    lv_label_set_text(_satellitesLabel, buf);

    snprintf(buf, sizeof(buf), "Speed: %.1f km/h  Kurs: %.0f°", gps.speedKmh, gps.courseDeg);
    lv_label_set_text(_speedLabel, buf);

    snprintf(buf, sizeof(buf), "Höhe: %.0f m", gps.altitudeMsl);
    lv_label_set_text(_altitudeLabel, buf);

    // Bus freshness: no CAN frame seen recently means the transceiver/wiring
    // or the motor-node itself is down, distinct from "GPS has no fix".
    bool stale = (gps.lastUpdateMs == 0) || (millis() - gps.lastUpdateMs > 10000);
    lv_label_set_text(_busLabel, stale ? "CAN: keine Daten" : "CAN: verbunden");
    lv_obj_set_style_text_color(_busLabel, stale ? COLOR_DANGER : COLOR_ACCENT_WARM, 0);
}

void GpsScreen::setVisible(bool visible) {
    if (visible == _visible) return;
    _visible = visible;

    DisplayManager::Lock guard;
    if (!guard.acquired()) return;

    if (visible) {
        lv_obj_clear_flag(_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_container, LV_OBJ_FLAG_HIDDEN);
    }
}
