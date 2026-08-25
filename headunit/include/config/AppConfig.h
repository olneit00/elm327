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

}  // namespace app_config
