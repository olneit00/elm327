#pragma once
//
// GpsScreen - LVGL UI showing GPS data received over the CAN bus
// (CanBusReceiver) on the GC9A01 round display. Mirrors RadioScreen's
// container/label layout and visibility handling (issue #15).
//

#include <lvgl.h>
#include "CanBusReceiver.h"

class GpsScreen {
public:
    GpsScreen();
    ~GpsScreen();

    // Create the GPS UI on the active LVGL screen
    void create();

    // Update UI with the latest GPS snapshot from CanBusReceiver
    void update(const CanGpsData& gps);

    // Set visibility (for switching between ClockScreen/RadioScreen/GpsScreen)
    void setVisible(bool visible);

    // Check if currently visible
    bool isVisible() const { return _visible; }

private:
    bool _visible = false;

    lv_obj_t* _container = nullptr;
    lv_obj_t* _titleLabel = nullptr;
    lv_obj_t* _positionLabel = nullptr;
    lv_obj_t* _fixLabel = nullptr;
    lv_obj_t* _satellitesLabel = nullptr;
    lv_obj_t* _speedLabel = nullptr;
    lv_obj_t* _altitudeLabel = nullptr;
    lv_obj_t* _busLabel = nullptr;

    void _createContainer();
    void _createLabels();
};
