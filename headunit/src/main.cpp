#include <Arduino.h>
#include <LittleFS.h>
#include <ESPmDNS.h>

#include "display/DisplayManager.h"
#include "hardware/Pins.h"
#include "time/TimeProvider.h"
#include "ui/ClockScreen.h"
#include "ui/RadioScreen.h"
#include "radio/RadioService.h"
#include "radio/RadioStore.h"
#include "net/WifiManager.h"
#include "web/WebServer.h"
#include "config/AppConfig.h"

namespace {

// Default builds use a virtual clock that starts at 12:00:00 and advances
// with uptime. Swap for a real RTC / SNTP source behind ITimeSource later.
#if defined(HEADUNIT_TEST_MODE)
// Test mode drives the hands from a fixed stamp sequence in loop(), so the
// placement can be validated without any live clock backing.
FixedTimeSource timeSource(TimeOfDay{12, 0, 0});
#else
MillisClock timeSource(TimeOfDay{12, 0, 0});
#endif

ClockScreen clockScreen;
RadioScreen radioScreen;
RadioService radioService;
RadioStore radioStore;
WifiManager wifiManager;
WebServer webServer(radioService, wifiManager);

#if defined(HEADUNIT_TEST_MODE)
// Known stamps that place all three hands at unambiguous orientations.
const TimeOfDay kTest[] = {
    {12, 0, 0},   // all hands point up
    {3, 15, 45},  // hour and minute in the first quadrant
    {6, 30, 30},  // hands spread across the dial
    {9, 45, 15},  // hour/min back, second in the third quadrant
    {1, 2, 3},    // sub-degree minute/sec placement
};
uint32_t lastStepMs_ = 0;
int stepIndex_ = 0;
#endif

// Radio test state
uint32_t lastRadioTestMs = 0;
int radioTestStep = 0;
bool radioInitialized = false;
bool webInitialized = false;
uint32_t lastConfigSaveMs = 0;
static constexpr uint32_t CONFIG_SAVE_INTERVAL_MS = 5000;

// Dirty-check state for persistence: avoids writing to flash on every
// CONFIG_SAVE_INTERVAL_MS tick when nothing actually changed, which would
// otherwise wear down the small LittleFS partition over months of
// always-on, in-vehicle operation.
RadioConfig lastSavedConfig;
bool configEverSaved = false;
RadioStation lastSavedStations[app_config::kMaxStations];
size_t lastSavedStationCount = 0;
bool stationsEverSaved = false;

// Screen switching state
bool showRadioScreen = false;
uint32_t lastScreenSwitchMs = 0;

// RadioService callbacks for WebServer
void onRadioStatus(const RadioStatus& status) {
    webServer.broadcastStatus(status);
    // Update RadioScreen if visible
    if (showRadioScreen) {
        radioScreen.update(status);
    }
}

void onStationList(const RadioStation* stations, size_t count) {
    webServer.broadcastStationList(stations, count);
}

void onScanProgress(uint8_t progress, uint8_t count, bool scanning) {
    webServer.broadcastScanProgress(progress, count, scanning);
}

// Briefly shows the newly selected station's name (if RDS has already
// decoded it) or its frequency in the center of the clock face, regardless
// of which screen is currently active (see ClockScreen::showStationInfo()).
void onStationSelected(uint16_t frequency, const char* programService) {
    String text = (programService != nullptr && programService[0] != '\0')
                      ? String(programService)
                      : String(frequency / 100.0f, 2) + " MHz";
    clockScreen.showStationInfo(text);
}

// WifiManager callback
void onStaConnect(bool success, const IPAddress& ip) {
    Serial.printf("[WIFI] STA callback: success=%d, ip=%s\n", success, ip.toString().c_str());
    if (success) {
        // mDNS only makes sense once we're actually on the home/existing
        // network (the isolated AP has no other clients to resolve names
        // for); lets you reach the device at http://opel-radio.local
        // instead of having to look up its DHCP-assigned IP, which is the
        // main point of joining an existing network in the first place -
        // easier debugging/access than being confined to the AP.
        if (MDNS.begin("opel-radio")) {
            MDNS.addService("http", "tcp", 80);
            Serial.println(F("[WIFI] mDNS responder started: http://opel-radio.local"));
        } else {
            Serial.println(F("[WIFI] mDNS responder failed to start"));
        }
    }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println(F("Headunit GC9A01 analog (LVGL + LovyanGFX) + Si4703 Radio"));

  // Initialize LittleFS and RadioStore
  if (!radioStore.begin()) {
    Serial.println(F("[STORE] Failed to initialize"));
  }

  // Load persisted config
  RadioConfig config;
  if (radioStore.loadConfig(config)) {
    radioService.applyConfig(config);
    Serial.printf("[STORE] Restored: freq=%.2f MHz, vol=%d%%, muted=%s\n",
                  config.lastFrequency / 100.0f,
                  volumeHardwareToPercent(config.lastVolume),
                  config.lastMuted ? "yes" : "no");
  }

  // Load persisted station list so it survives a reboot
  {
    RadioStation stations[50];
    size_t count = 0;
    if (radioStore.loadStations(stations, 50, count) && count > 0) {
      radioService.setStations(stations, count);
      Serial.printf("[STORE] Restored %u stations from disk\n", count);
    } else {
      Serial.println(F("[STORE] No persisted stations found"));
    }
  }

  if (!DisplayManager::begin(pins::SCREEN_WIDTH, pins::SCREEN_HEIGHT)) {
    Serial.println(F("[DISPLAY] init failed"));
    while (true) {}
  }
  Serial.println(F("[DISPLAY] ready"));

  // Build the face and hands in every mode so update() always has targets.
  clockScreen.attachTimeSource(&timeSource);
  clockScreen.create();

  // Create RadioScreen
  radioScreen.create();
  Serial.println(F("[RADIO] Screen created"));

  // Initialize Radio
  Serial.println(F("[RADIO] Initializing Si4703..."));
  if (radioService.begin()) {
    radioInitialized = true;
    Serial.println(F("[RADIO] Si4703 initialized successfully"));
    
    // Power ON the radio
    radioService.powerOn();
    Serial.println(F("[RADIO] Radio powered ON"));
    
    // Tune to last frequency (already set via applyConfig)
    radioService.setVolume(config.lastVolume);
    if (config.lastMuted) {
      radioService.setMuted(true);
    }
  } else {
    Serial.println(F("[RADIO] Si4703 initialization FAILED - check wiring"));
  }

  // Register RadioService callbacks
  radioService.setStatusCallback(onRadioStatus);
  radioService.setStationListCallback(onStationList);
  radioService.setScanProgressCallback(onScanProgress);
  radioService.setStationSelectedCallback(onStationSelected);

  // Sync the on-screen clock from RDS time (group 4A) when a station broadcasts it.
  // MillisClock advances from its seed, so (re)seeding it to the RDS time keeps the
  // hands on real time between RDS updates.
  radioService.setTimeCallback([](uint8_t hour, uint8_t minute, uint8_t second) {
    timeSource.setSeed(TimeOfDay{hour, minute, second});
    Serial.printf("[CLOCK] RDS time synced: %02u:%02u:%02u\n", hour, minute, second);
  });

  // Initialize WiFi + WebServer
  Serial.println(F("[WEB] Starting WiFi AP + WebServer..."));
  // WPA2 password: an empty one creates an open AP, letting anyone in range
  // control the radio/WiFi config or sniff the STA credentials transmitted
  // in POST /api/wifi/connect and /api/radio/config over it (WPA2 requires
  // >= 8 characters, enforced in WifiManager::_startAP()).
  if (wifiManager.begin("Opel-Radio", "OpelRadio1935")) {
    if (webServer.begin(80)) {
      webInitialized = true;
      Serial.println(F("[WEB] Server ready"));
    }
  }
  wifiManager.setStaConnectCallback(onStaConnect);

  // Additionally join a previously configured home/existing WiFi network
  // (STA mode, alongside our own AP) if credentials were saved via
  // POST /api/radio/config - much easier to reach/debug the device on a
  // known network than being confined to the isolated AP. This used to be
  // wired up but the actual auto-connect-at-boot call had gone missing;
  // the rest of the STA plumbing (WIFI_AP_STA mode, RadioConfig.staSsid/
  // staPassword persistence, the non-blocking connectSta() state machine)
  // was already in place. connectSta() does not block - the connection
  // attempt and its timeout are polled from wifiManager.loop() below.
  if (strlen(config.staSsid) > 0) {
    Serial.printf("[WIFI] Auto-connecting to saved network: %s\n", config.staSsid);
    wifiManager.connectSta(config.staSsid, config.staPassword);
  }

#if defined(HEADUNIT_TEST_MODE)
  clockScreen.setManualTime(kTest[0], true);
  lastStepMs_ = millis();
  stepIndex_ = 0;
  Serial.println(F("[TEST] cycling through fixed hand stamps every 2 s"));
#endif
}

void loop() {
#if defined(HEADUNIT_DEMO_MODE) || defined(HEADUNIT_TEST_MODE)
// Demo mode: screen switching, radio test sequences (enabled by default unless both are disabled)
#define ENABLE_DEMO_SEQUENCES 1
#else
#define ENABLE_DEMO_SEQUENCES 0
#endif

#if defined(HEADUNIT_TEST_MODE)
  if (millis() - lastStepMs_ > 2000) {
    lastStepMs_ = millis();
    stepIndex_ = (stepIndex_ + 1) % (sizeof(kTest) / sizeof(kTest[0]));
    TimeOfDay t = kTest[stepIndex_];
    clockScreen.setManualTime(t, true);
    Serial.printf("[TEST] hand stamp %d:%02d:%02d\n", t.hour, t.minute,
                  t.second);
  }
  clockScreen.update();
  DisplayManager::tick();
  delay(16);
#else
  clockScreen.update();  // move hands from the running virtual clock
  DisplayManager::tick();

  // Radio service loop
  if (radioInitialized) {
    radioService.loop();

#if ENABLE_DEMO_SEQUENCES
    // Simple radio test sequence (every 10 seconds) - demo mode only
    if (millis() - lastRadioTestMs > 10000) {
      lastRadioTestMs = millis();
      const RadioStatus& status = radioService.getStatus();
      Serial.printf("[RADIO] Freq: %.2f MHz, RSSI: %d, Stereo: %s, PS: %s\n",
                    status.frequency / 100.0f,
                    status.rssi,
                    status.stereo ? "Yes" : "No",
                    status.programService);

      // Test sequence: seek up every 10 seconds
      if (radioTestStep % 2 == 0) {
        Serial.println(F("[RADIO] Seek Up..."));
        radioService.seekUp();
      } else {
        Serial.println(F("[RADIO] Seek Down..."));
        radioService.seekDown();
      }
      radioTestStep++;
    }
#endif
  }

  // WebServer + WifiManager loop (async, but keep for consistency)
  if (webInitialized) {
    webServer.loop();
  }
  if (wifiManager.isApActive()) {
    wifiManager.loop();
  }

  // Periodic config save (debounced by interval, and only actually written
  // to flash if something changed since the last save).
  if (radioInitialized && millis() - lastConfigSaveMs > CONFIG_SAVE_INTERVAL_MS) {
    lastConfigSaveMs = millis();
    RadioConfig config = radioService.getConfig();
    if (!configEverSaved || config != lastSavedConfig) {
      radioStore.saveConfig(config);
      lastSavedConfig = config;
      configEverSaved = true;
    }

    // Also save station list if we have one and it actually changed
    // (e.g. a new scan completed or a favorite was toggled).
    size_t count = 0;
    const RadioStation* stations = radioService.getStations(count);
    bool stationsChanged = !stationsEverSaved || count != lastSavedStationCount;
    if (!stationsChanged) {
      for (size_t i = 0; i < count; i++) {
        if (!stationPersistEquals(stations[i], lastSavedStations[i])) {
          stationsChanged = true;
          break;
        }
      }
    }
    if (count > 0 && stationsChanged) {
      radioStore.saveStations(stations, count);
      for (size_t i = 0; i < count; i++) {
        lastSavedStations[i] = stations[i];
      }
      lastSavedStationCount = count;
      stationsEverSaved = true;
    }
  }

#if ENABLE_DEMO_SEQUENCES
  // Screen switching test: toggle between clock and radio every 15 seconds (demo mode only)
  if (millis() - lastScreenSwitchMs > 15000) {
    lastScreenSwitchMs = millis();
    showRadioScreen = !showRadioScreen;
    if (showRadioScreen) {
      clockScreen.setVisible(false);
      radioScreen.setVisible(true);
      Serial.println(F("[SCREEN] Switched to RadioScreen"));
    } else {
      radioScreen.setVisible(false);
      clockScreen.setVisible(true);
      Serial.println(F("[SCREEN] Switched to ClockScreen"));
    }
  }
#endif

  delay(33);
#endif
}