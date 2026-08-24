# Clock assets

Drop the historical clock face here so the head unit can reuse the same
background as the existing `motor-node` instrumentation.

## Current pipeline

`generate_clock_face.py` builds `include/assets/clock_face.c` (LVGL
RGB565A8, 240x240) from the historical dial photo `uhr_optimiert.png`
(next to the workspace root): it crops the dark bezel away so the dial
fills the whole round display, flattens the dial to two tones (uniform
cream, pure black - no photographic gradients), keeps the small brass
hub, and applies a circular alpha mask. Re-run after changing the source
photo: `python assets/clock/generate_clock_face.py [path\to\photo.png]`.
`uhr_ziffernblatt_240.png` is the generated 240x240 source of the asset.
Display-side color/geometry notes: see `PLAN_DISPLAY_FIX.md` in the
headunit root.

The current `ClockScreen` draws a procedural dial (a ring and 12 hour ticks)
and three rotation hands with `lv_line` geometry, so it runs without any
image assets. A photographic/julia-style clock face can be added later as an
LVGL image behind those hands.

## Expected files

Tracked by the head unit code path:

- `background.bin` / `background` conversion payload - 240x240 RGB565
  LVGL `LV_IMAGE` descriptor for the static dial.
- Optional: PNG sources for the three hands (`hour.png`, `minute.png`,
  `second.png`) rotated around pivots at the dial hub.

## Generating from GIMP / LVGL

1. Export the dial as a 240x240 PNG.
2. Use LVGL's `lv_img_tools` (or Image Converter in SquareLine) to produce a
   `#define LV_<asset>_CDATA_UPDATE...` array, then reference it as the
   `src` of an `lv_image` behind `ClockScreen::create()`.
3. Hands are already rendered procedurally, so only the background is truly
   required for the historical look. Rotating image hands can replace the
   `lv_line` hands later without changing the `TimeProvider` contract.