# Headunit (ESP32 + GC9A01)

Standalone head unit project: an ESP32 driving a 1.28" round GC9A01 display,
rendering an analog clock with LVGL on top of the LovyanGFX driver.

## Library selection

- **LovyanGFX** - ships a dedicated `Panel_GC9A01` class, so the display
  driver reduces to a configuration header with no manual init sequence.
  It uses the ESP32's DMA-assisted SPI path for cheap full-frame flushes.
- **LVGL 9** - provides the widget tree, invalidation, and the built-in
  LovyanGFX driver (`LV_USE_LOVYAN_GFX`). All configuration lives in
  `platformio.ini` via `LV_CONF_SKIP`, so there is no per-version
  `lv_conf.h` to drift.
- **TFT_eSPI / Arduino_GFX** were evaluated. TFT_eSPI needs a hand-written
  GC9A01 init sequence; Arduino_GFX works but is a thinner test surface on
  this panel. LovyanGFX is the lower-risk choice and is the driver the LVGL
  Arduino integration docs recommend.

## Layout

Display driver header, pulled in by LVGL via `LV_LGFX_USER_INCLUDE`:

- `include/display/DisplayDevice.hpp` - declares the `LGFX` class (GC9A01
  panel + SPI bus + pin wiring) that the built-in LovyanGFX driver instantiates.
- `include/hardware/Pins.h` - all GPIO and panel constants, the single place
  where wiring changes land.

App modules:

- `src/display/DisplayManager.cpp` - LVGL init + periodic `lv_timer_handler`.
- `src/time/TimeProvider.cpp` - `ITimeSource` plus an RTC-agnostic
  `MillisClock` and a `FixedTimeSource` used by the test mode.
- `src/ui/ClockScreen.cpp` - builds the dial and hands, recomputes hand
  endpoints from time each frame, and invalidates only the dirty objects.
  Point arrays persist inside the instance because `lv_line_set_points`
  stores a reference, not a copy.

`src/main.cpp` wires the modules and, under `HEADUNIT_TEST_MODE`, steps
through fixed timestamps to validate hand placement.

## Build

```
pio build -e esp32dev      # normal build (virtual 12:00 clock)
pio build -e test          # validation build with fixed hand stamps
pio upload -e esp32dev     # flash
```

GPIO map (matches `Pins.h`): SCK 18, MOSI 23, CS 5, DC 27, RST 33.

## Hand test

The `test` environment compiles with `HEADUNIT_TEST_MODE`, drives the hands
through a fixed timestamp sequence (one every 2 s), and prints each stamp
over serial so placement mistakes are obvious against the visible tick marks.