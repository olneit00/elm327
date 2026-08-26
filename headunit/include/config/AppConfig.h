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

// Stations weaker than this RSSI (dBµV) are skipped during a scan.
// Originally 36, calibrated with AGC disabled. Enabling AGC (see
// RadioService::_initHardware()) shifted the RSSI distribution down for
// weaker stations - the adaptive front-end reports lower RSSI for the same
// physical signal than the old fixed-gain setup did - so 36 started
// rejecting stations the chip's own seek logic considered valid (issue #2).
// Per the Si4702/03-C19 datasheet, RSSI is dBµV in 1 dB steps (max ~75);
// the default stereo/mono blend balance band is 31-49 dBµV (BLNDADJ=0), so a
// clean stereo-capable station is well above ~30 dBµV. Stations below 25 are
// mostly noise / weak adjacent carriers that just clutter the list; raise
// toward 36 if the current value still lets too much junk through, or lower
// toward 18 if real stations are being dropped again.
constexpr uint8_t kMinStationRssi = 25;

}  // namespace app_config
