//
// RadioScreen implementation
// LVGL UI for FM/RDS Radio on GC9A01 round display
//

#include <cstdio>
#include "ui/RadioScreen.h"
#include "radio/RadioTypes.h"
#include "display/DisplayManager.h"
#include <lvgl.h>

// Colors for classic radio look (dark mode with green/orange accents)
static const lv_color_t COLOR_BG = { 0x0a, 0x0a, 0x0a };
static const lv_color_t COLOR_BG_ELEVATED = { 0x12, 0x12, 0x12 };
static const lv_color_t COLOR_FG = { 0xe0, 0xe0, 0xe0 };
static const lv_color_t COLOR_FG_MUTED = { 0x88, 0x88, 0x88 };
static const lv_color_t COLOR_ACCENT = { 0x00, 0xcc, 0x44 };
static const lv_color_t COLOR_ACCENT_DIM = { 0x00, 0x88, 0x33 };
static const lv_color_t COLOR_ACCENT_WARM = { 0xff, 0x88, 0x00 };
static const lv_color_t COLOR_DANGER = { 0xcc, 0x33, 0x33 };
static const lv_color_t COLOR_BORDER = { 0x2a, 0x2a, 0x2a };

static const lv_font_t* FONT_SMALL = &lv_font_montserrat_14;
static const lv_font_t* FONT_MEDIUM = &lv_font_montserrat_14;
static const lv_font_t* FONT_LARGE = &lv_font_montserrat_14;

RadioScreen::RadioScreen() {
}

RadioScreen::~RadioScreen() {
    if (_container) {
        lv_obj_del(_container);
        _container = nullptr;
    }
}

void RadioScreen::create() {
    // Runs once from setup(), before the web server (and its async_tcp
    // task) is up - see DisplayManager::Lock for why every LVGL touchpoint
    // takes it regardless.
    DisplayManager::Lock guard;
    if (!guard.acquired()) return;

    _createContainer();
    _createFrequencyDisplay();
    _createProgramService();
    _createRadioText();
    _createIndicators();
    _createVolumeControl();
    
    // Initially hidden
    lv_obj_add_flag(_container, LV_OBJ_FLAG_HIDDEN);
    _visible = false;
}

void RadioScreen::_createContainer() {
    _container = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(_container);
    lv_obj_set_size(_container, 240, 240);
    lv_obj_set_style_bg_color(_container, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 16, 0);
    lv_obj_set_flex_flow(_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void RadioScreen::_createFrequencyDisplay() {
    // Large frequency display
    _freqLabel = lv_label_create(_container);
    lv_label_set_text(_freqLabel, "87.50 MHz");
    lv_obj_set_style_text_font(_freqLabel, FONT_LARGE, 0);
    lv_obj_set_style_text_color(_freqLabel, COLOR_ACCENT, 0);
    lv_obj_set_style_text_align(_freqLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_freqLabel, 208);
}

void RadioScreen::_createProgramService() {
    // Station name (PS)
    _psLabel = lv_label_create(_container);
    lv_label_set_text(_psLabel, "--");
    lv_obj_set_style_text_font(_psLabel, FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(_psLabel, COLOR_FG, 0);
    lv_obj_set_style_text_align(_psLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_psLabel, 208);
    lv_label_set_long_mode(_psLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

void RadioScreen::_createRadioText() {
    // RadioText (song info)
    _rtLabel = lv_label_create(_container);
    lv_label_set_text(_rtLabel, "Warte auf RDS...");
    lv_obj_set_style_text_font(_rtLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_rtLabel, COLOR_ACCENT_WARM, 0);
    lv_obj_set_style_text_align(_rtLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_rtLabel, 208);
    lv_label_set_long_mode(_rtLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

void RadioScreen::_createIndicators() {
    // Container for stereo/RSSI indicators
    lv_obj_t* indicatorCont = lv_obj_create(_container);
    lv_obj_remove_style_all(indicatorCont);
    lv_obj_set_size(indicatorCont, 208, 40);
    lv_obj_set_flex_flow(indicatorCont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicatorCont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(indicatorCont, 20, 0);

    // Stereo indicator
    _stereoIndicator = lv_label_create(indicatorCont);
    lv_label_set_text(_stereoIndicator, "MONO");
    lv_obj_set_style_text_font(_stereoIndicator, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_stereoIndicator, COLOR_FG_MUTED, 0);

    // RSSI bar
    _rssiBar = lv_bar_create(indicatorCont);
    lv_obj_set_size(_rssiBar, 80, 10);
    lv_bar_set_range(_rssiBar, 0, 120);
    lv_bar_set_value(_rssiBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_rssiBar, COLOR_BORDER, 0);
    lv_obj_set_style_bg_color(_rssiBar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(_rssiBar, 5, 0);
    lv_obj_set_style_radius(_rssiBar, 5, LV_PART_INDICATOR);

    // State label (Scanning, Tuning, etc.)
    _stateLabel = lv_label_create(_container);
    lv_label_set_text(_stateLabel, "Idle");
    lv_obj_set_style_text_font(_stateLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_stateLabel, COLOR_FG_MUTED, 0);
    lv_obj_set_style_text_align(_stateLabel, LV_TEXT_ALIGN_CENTER, 0);
}

void RadioScreen::_createVolumeControl() {
    // Volume section at bottom
    lv_obj_t* volCont = lv_obj_create(_container);
    lv_obj_remove_style_all(volCont);
    lv_obj_set_size(volCont, 208, 50);
    lv_obj_set_flex_flow(volCont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(volCont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(volCont, 8, 0);

    // Volume label + value
    lv_obj_t* volLabelRow = lv_obj_create(volCont);
    lv_obj_remove_style_all(volLabelRow);
    lv_obj_set_size(volLabelRow, 208, 20);
    lv_obj_set_flex_flow(volLabelRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volLabelRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* volTitle = lv_label_create(volLabelRow);
    lv_label_set_text(volTitle, "Lautstärke");
    lv_obj_set_style_text_font(volTitle, FONT_SMALL, 0);
    lv_obj_set_style_text_color(volTitle, COLOR_FG_MUTED, 0);

    _volumeLabel = lv_label_create(volLabelRow);
    lv_label_set_text(_volumeLabel, "50%");
    lv_obj_set_style_text_font(_volumeLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_volumeLabel, COLOR_ACCENT, 0);

    // Volume slider
    _volumeBar = lv_slider_create(volCont);
    lv_obj_set_width(_volumeBar, 208);
    lv_slider_set_range(_volumeBar, 0, 100);
    lv_slider_set_value(_volumeBar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_volumeBar, COLOR_BORDER, 0);
    lv_obj_set_style_bg_color(_volumeBar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_volumeBar, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_radius(_volumeBar, 5, 0);
    lv_obj_set_style_radius(_volumeBar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(_volumeBar, 0, LV_PART_KNOB);

    // Mute indicator
    _muteLabel = lv_label_create(volCont);
    lv_label_set_text(_muteLabel, "");
    lv_obj_set_style_text_font(_muteLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_muteLabel, COLOR_DANGER, 0);
}

void RadioScreen::update(const RadioStatus& status) {
    if (!_visible) return;

    // update() is reached from RadioService's status notification chain,
    // which - via any /api/radio/* web handler - can run synchronously
    // inside the async_tcp task rather than the main loop task. See
    // DisplayManager::Lock for why every LVGL touchpoint must take it.
    DisplayManager::Lock guard;
    if (!guard.acquired()) return;

    _updateFrequency(status.frequency);
    _updateProgramService(status.programService);
    _updateRadioText(status.radioText);
    _updateRSSI(status.rssi);
    _updateStereo(status.stereo);
    _updateVolume(volumeHardwareToPercent(status.volume), status.muted);
    _updateState(status.state);
}

void RadioScreen::setVisible(bool visible) {
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

void RadioScreen::_updateFrequency(uint16_t freq) {
    if (!_freqLabel) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f MHz", freq / 100.0f);
    lv_label_set_text(_freqLabel, buf);
}

void RadioScreen::_updateProgramService(const char* ps) {
    if (!_psLabel) return;
    if (ps && ps[0] != '\0') {
        lv_label_set_text(_psLabel, ps);
    } else {
        lv_label_set_text(_psLabel, "--");
    }
}

void RadioScreen::_updateRadioText(const char* rt) {
    if (!_rtLabel) return;
    if (rt && rt[0] != '\0') {
        lv_label_set_text(_rtLabel, rt);
    } else {
        lv_label_set_text(_rtLabel, "Kein RadioText");
    }
}

void RadioScreen::_updateRSSI(uint8_t rssi) {
    if (!_rssiBar) return;
    lv_bar_set_value(_rssiBar, rssi, LV_ANIM_ON);
}

void RadioScreen::_updateStereo(bool stereo) {
    if (!_stereoIndicator) return;
    lv_label_set_text(_stereoIndicator, stereo ? "STEREO" : "MONO");
    lv_obj_set_style_text_color(_stereoIndicator, stereo ? COLOR_ACCENT : COLOR_FG_MUTED, 0);
}

void RadioScreen::_updateVolume(uint8_t volumePercent, bool muted) {
    if (!_volumeBar || !_volumeLabel || !_muteLabel) return;
    
    lv_slider_set_value(_volumeBar, volumePercent, LV_ANIM_ON);
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", volumePercent);
    lv_label_set_text(_volumeLabel, buf);
    
    if (muted) {
        lv_label_set_text(_muteLabel, "MUTED");
    } else {
        lv_label_set_text(_muteLabel, "");
    }
}

void RadioScreen::_updateState(RadioState state) {
    if (!_stateLabel) return;
    const char* stateStr = "Idle";
    switch (state) {
        case RadioState::Off: stateStr = "Aus"; break;
        case RadioState::Idle: stateStr = "Bereit"; break;
        case RadioState::Tuning: stateStr = "Stimmt ab..."; break;
        case RadioState::Seeking: stateStr = "Sucht..."; break;
        case RadioState::Scanning: stateStr = "Sendersuchlauf"; break;
    }
    lv_label_set_text(_stateLabel, stateStr);
}