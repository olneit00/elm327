#pragma once
//
// AppConfig - central place for cross-module constants that would otherwise
// be duplicated (and risk silently drifting out of sync) across RadioTypes.h,
// RadioService, and RadioStore. Add new shared limits/timings here instead of
// re-declaring a literal in whichever file happens to need it first.
//
#include <stdint.h>
#include <stddef.h>

namespace app_config {

// FM band (Europe): 87.50-108.00 MHz in 10 kHz steps.
constexpr uint16_t kMinFrequency10kHz = 8750;
constexpr uint16_t kMaxFrequency10kHz = 10800;

// Maximum number of stations kept in a single scan pass and persisted to
// RadioStore. Must match the fixed-size RadioStation array in RadioService.
constexpr size_t kMaxStations = 50;

// Stations weaker than this RSSI (dBµV) are skipped during a scan. Field
// tests show RSSI < 36 is mostly noise/weak adjacent carriers - only show
// stations with RSSI >= 36.
constexpr uint8_t kMinStationRssi = 36;

}  // namespace app_config
