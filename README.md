# coffee-grinder-scale

ESP32 firmware that weighs coffee as it grinds, stops at a target dose,
then tops up with short pulses to close the gap. Same physical behavior
as the original project; this branch (`rewrite/rtos-fork`) is a ground-up
FreeRTOS rewrite, kept as a permanent fork rather than merged back to
`main`. Full rationale, current status, and design docs live in
`.agent/` -- start with `.agent/AGENTS.md`.

<img src="https://github.com/ahuber21/coffee-grinder-scale/blob/main/.doc/shot.png" width=300>

## Architecture

Seven FreeRTOS tasks -- Scale, Dosing, Input, Display, Settings, Network,
Telemetry -- communicating over queues/mailboxes instead of one
`loop()`/state machine. Full design: `.agent/design/rtos-architecture.md`.

## Connecting to the device

- **Web app**: `http://eureka.local/` (mDNS, no OTA password -- trusted
  home LAN only). A small React SPA (`webapp/`) with three tabs: **Live**
  (current weight/target, a session chart, manual dose request),
  **Settings** (calibration factor, target doses, top-up margins, button
  debounce, WiFi reset/reboot), **History** (past sessions, queried
  straight from PostgREST in the browser -- bypasses the device). Replaces
  the old `/console` page and the local-only `dev/graph`/`dev/settings`
  mock tooling.
- **Realtime channel**: one WebSocket at `/ws` (typed JSON envelope) --
  what the SPA's Live/Settings tabs actually talk to, and connectable
  directly from any other client.
- **OTA**: `pio run -t upload -e esp_wroom_02_ota` (espota, no password).
  Serial: `pio run -t upload -e esp_wroom_02`.
- **History data** is also queryable directly against PostgREST without
  going through the device at all -- see
  `.agent/design/postgrest-deployment.md`.

## Repo layout

- `lib/*Task/` -- the seven FreeRTOS tasks, one per directory.
- `lib/Messaging/` -- shared message structs, queues/mailboxes, task config.
- `lib/DosingModel/` -- the topup/coast/main-grind models, unit-tested
  (`pio test -e native`).
- `lib/ADS1232/` -- the vendored load-cell ADC driver, unchanged from the
  original project (D6).
- `webapp/` -- the SPA; see `webapp/README.md` to build/develop it.
- `.agent/` -- the rewrite's working docs: `AGENTS.md` (rules),
  `STATUS.md` (current state), `DECISIONS.md` (why), `ARS.md` (findings
  log), `design/` (the design docs this was built from).

## Hardware

Built on the hardware from jousis' espresso-scale
(https://gitlab.com/jousis/espresso-scale). Portable to other ESP32-based
systems as long as they also use the ADS1232. Open an issue with
questions.

Grinder: Eureka Mignon, modded per this
[Tech Dregs video](https://www.youtube.com/watch?v=ksemL5_kvDw). The
220V version's power supply can't run the ESP32, hence an external wall
plug. The case is basic but works; the buttons
(https://www.amazon.de/gp/product/B0BF51N8CK) needed some debounce
filtering in software -- worth trying different buttons/pull-ups if you
build this yourself.
