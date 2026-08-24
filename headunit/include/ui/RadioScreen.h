#pragma once
//
// RadioScreen - LVGL UI for FM/RDS Radio on GC9A01 round display
//

#include <lvgl.h>
#include "radio/RadioTypes.h"

class RadioScreen {
public:
    RadioScreen();
    ~RadioScreen();

    // Create the radio UI on the active LVGL screen
    void create();

    // Update UI with current radio status
    void update(const RadioStatus& status);

    // Set visibility (for switching between ClockScreen and RadioScreen)
    void setVisible(bool visible);

    // Check if currently visible
    bool isVisible() const { return _visible; }

private:
    bool _visible = false;

    // UI objects
    lv_obj_t* _container = nullptr;
    lv_obj_t* _freqLabel = nullptr;
    lv_obj_t* _psLabel = nullptr;
    lv_obj_t* _rtLabel = nullptr;
    lv_obj_t* _rssiBar = nullptr;
    lv_obj_t* _stereoIndicator = nullptr;
    lv_obj_t* _volumeBar = nullptr;
    lv_obj_t* _volumeLabel = nullptr;
    lv_obj_t* _muteLabel = nullptr;
    lv_obj_t* _stateLabel = nullptr;

    // Helper methods
    void _createContainer();
    void _createFrequencyDisplay();
    void _createProgramService();
    void _createRadioText();
    void _createIndicators();
    void _createVolumeControl();

    void _updateFrequency(uint16_t freq);
    void _updateProgramService(const char* ps);
    void _updateRadioText(const char* rt);
    void _updateRSSI(uint8_t rssi);
    void _updateStereo(bool stereo);
    void _updateVolume(uint8_t volumePercent, bool muted);
    void _updateState(RadioState state);
};