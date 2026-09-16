# display_sim

Runs `lib/DisplayTask/DisplayTask.cpp`'s actual rendering code host-side, with
no ESP32 and no physical panel, so a display change can be watched as an
image/animation instead of reasoned about blindly and OTA-deployed on faith.
See that file's top-of-file comment for how the swap works (`#ifdef ARDUINO`
picks `Adafruit_ST7735` + real SPI vs. `FakeCanvas`, an in-memory RGB565
framebuffer with the same method surface, backed by the real Adafruit_GFX
font table so text layout matches the physical panel exactly).

## Build and run

```sh
c++ -std=gnu++17 -Wall -Wextra \
  -Ilib/DisplayTask -Ilib/Messaging -Ilib/DosingModel \
  lib/DisplayTask/DisplayTask.cpp -o tools/display_sim/display_sim

mkdir -p /tmp/dsim_out
./tools/display_sim/display_sim /tmp/dsim_out
```

Writes `boot.dsim`, `dose.dsim`, `ota.dsim` -- one scripted scenario each
(see `DisplaySimMain.h`), captured at a fixed 24fps regardless of how fast
the underlying simulated clock moves.

## Viewing a capture

`.dsim` is a tiny custom format: 4-byte magic `DSIM`, then four `uint32`
(width, height, frame count, fps), then that many frames of raw RGB565
pixels concatenated, row-major. Convert one to an animated GIF:

```sh
python3 tools/display_sim/dsim_to_gif.py /tmp/dsim_out/dose.dsim /tmp/dsim_out/dose.gif --scale 4
```

Not wired into `pio test` -- this is a visualization tool, not a pass/fail
check. Whether it's worth adding golden-frame regression assertions later
(pin known-good pixel data, fail on an unintended diff) is an open question,
not attempted here.

## Adding a new scenario

Add a segment to `DisplaySimMain.h`'s `main()` -- `runSegment(dump, last,
someDisplayCommand, durationMs)` steps simulated time forward, interpolating
`current_grams`/`elapsed_s` from wherever `last` currently is to the given
command, capturing frames as it goes. `applyModeChange` mirrors
`displayTaskFn`'s own mode-change screen clear by hand (kept in sync
manually, not shared code -- see its own comment).
