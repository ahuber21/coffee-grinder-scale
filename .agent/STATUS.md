# Status

*Last updated: 2026-09-11 — FreeRTOS task skeleton implemented and building.*

## Where things stand

Planning and design are done; implementation is underway. Branch
`rewrite/rtos-fork`. Standing rules in `AGENTS.md`, full decision log in
`DECISIONS.md` (D1-D17), all findings in `ARS.md` (AR-001-027, all
resolved or non-blocking), design docs in `.agent/design/`.

**Design work completed:**
- **Audit** of the existing firmware (20 findings, `ARS.md`). Nothing
  contradicts "mostly bug-free day-to-day" — latent/edge-case issues, not
  live misbehavior. Most consequential: a second ISR/task shared-state
  race (AR-009), a fallback stop path that silently bypasses the
  topup-margin strategy (AR-011), a WebSocket client-cleanup gap
  (AR-013/014).
- **Topup/dosing model** (`design/topup-model.md`), built from live
  queries against real historical data. Replaces the static lookup table
  and single-point rate estimate with small recursive-least-squares
  models. Found and fixed in design: a firmware bug corrupting ~28% of
  historical topup logs (AR-021), a quantified real overshoot problem in
  the *current* system (~21% of shortest pulses breach the 0.3g cap,
  AR-023), and — the largest single finding — a ~0.49g "coast" effect
  (coffee still landing for ~1.4s after relay-off, 2.5-3x bigger than the
  topup-pulse noise floor) that the firmware currently doesn't anticipate
  at all (AR-025/D14, `design/coast-effect.md`). D13: the 80%/Δ0.05g
  accuracy target stands unchanged — owner confirmed the physical
  mechanism (clumping within the relay's minimum on-time) and the path to
  it is a better main-grind stop estimate (Models A + C), not a more
  precise topup pulse (capped by clumping).
- **FreeRTOS task architecture** (`design/rtos-architecture.md`): 7 tasks,
  strict single-writer state ownership, queues vs. overwrite-mailboxes
  chosen per link. Closes 15 audit findings by construction. D12: OTA is
  refused outright during a grind, not aborted.

**Implemented:**
- `lib/DosingModel/` — all three models (main-grind rate, topup pulse
  response, coast anticipation) from `topup-model.md` §4/§8, natively
  unit-tested (22/22 passing, independently re-verified). Not yet wired
  into any actual control loop — that happens once the FreeRTOS task
  implementation exists.
- PostgREST deployment on `192.168.0.111` (`design/postgrest-deployment.md`,
  `design/db-schema/001_sessions_schema.sql`). New `v2` schema
  (sessions/events/raw_samples, session-linked, coast fields included)
  applied additively; old tables/the still-running `coffee_grinder_api`
  untouched. Running as systemd service `postgrest`, verified end-to-end
  (POST/PATCH tested, test rows cleaned up, independently re-verified:
  service active+enabled, LAN-reachable, `v2` tables empty, legacy row
  counts unchanged). Nothing posts to it yet — that's the firmware's job,
  not built yet.

- **FreeRTOS task skeleton** (`design/rtos-architecture.md` implemented):
  all 7 tasks (`lib/ScaleTask`, `lib/DosingTask`, `lib/InputTask`,
  `lib/DisplayTask`, `lib/SettingsTask`, `lib/NetworkTask`,
  `lib/TelemetryTask`) created via `xTaskCreatePinnedToCore` with the
  design doc's priorities/cores/stack sizes (`lib/Messaging/TaskConfig.h`).
  `src/main.cpp` is now just `initQueuesAndEvents()` + 7 task-creation
  calls — no FSM/rendering/networking logic of its own (D15). Every §3
  queue/mailbox is real and wired: button ISRs push `ButtonEdge` with no
  shared-state writes (AR-001/AR-007's fix), Input task's one debounce path
  applies uniformly to every button/state (AR-008's fix), Scale task drains
  to a real queue Dosing task processes every tick, Settings task
  distributes a `SettingsSnapshot` via one overwrite mailbox per subscriber
  (AR-009's fix) with validated field writes (AR-016's fix) and NVS I/O
  stubbed to compiled-in defaults (real NVS read/write is follow-up work,
  per the task brief), the §8 startup event group gates Dosing/Network/
  Telemetry behind `SETTINGS_LOADED|SCALE_READY|DISPLAY_READY`, and D12's
  OTA-refuse-during-grind gate (`otaSafeToStart()`) is real against the
  shared status bits. `lib/DosingModel/` is genuinely wired into Dosing
  task's per-sample path (not a stub): `MainGrindModel::addSample`/
  `predictStopTimeMsWithCoast` drive the main-grind stop decision every
  `ScaleSample`, `TopupModel::computeTopupDecision`/`recordPulse` drive the
  TOPUP pulse loop, `CoastModel::recordCoast` closes the loop at STOPPING,
  and all three get folded into one `PersistRequest` at FINALIZE (§5 — a
  session-boundary event, never per-sample). AR-011's and AR-004's fixes
  are concrete in this code (`target_grams_corrected` as the one canonical
  stop-comparison value; `computeCorrectedTarget()` as the one function
  both the button and API/`DoseRequest` paths call).
  Display/Network/Telemetry task *bodies* are stubbed to logging (real
  ST7735 rendering, WiFiManager/AsyncWebServer/ArduinoOTA, and the
  PostgREST POST are all follow-up work) — see D15-D17 and AR-026/AR-027
  for the implementation-pass decisions/findings this produced. The
  ADS1232 driver is vendored in directly (D6, executed via D16 — the
  submodule was never checked out in this repo, so its two source files
  were copied from the sister checkout instead of running
  `git submodule update --init`). Verified: `pio run -e esp_wroom_02`
  succeeds (RAM 4.4%, flash 21.7%); `pio test -e native` still 22/22
  (`lib/DosingModel` itself untouched). Old `lib/API`/`Display`/
  `WebSocketSettings`/etc. modules are left in place for reference
  (AR-026) but no longer compiled into `esp_wroom_02` (nothing includes
  their headers).

**Not yet started:** web SPA, real ST7735 rendering in `DisplayTask`, real
WiFiManager/AsyncWebServer/ArduinoOTA in `NetworkTask`, real NVS read/write
in `SettingsTask`, wiring the PostgREST POST into `TelemetryTask`,
decommissioning `coffee_grinder_api` (waits until the new pipeline is
verified in real use).

## Infrastructure on hand

- Read-only Postgres role `claude_agent` on `192.168.0.111`
  (`.agent/secrets/pg_agent.env`, gitignored) — used for all the analysis
  above, scoped to old tables only, no access to the new `v2` schema.
- PostgREST live at `http://192.168.0.111:3000` (see `AGENTS.md`), with
  its own scoped `postgrest_anon`/`postgrest_authenticator` roles.
- `coffee_grinder_api` (Python trampoline on `192.168.0.112`) still
  running unchanged — retirement (D8) waits until firmware actually posts
  to PostgREST and the new pipeline is verified in real use.

## Standing constraints (full detail in `AGENTS.md`)

- **No OTA deploy to the physical device without the owner's explicit
  go-ahead, every time.** Building/testing freely is fine and expected.
- Hardware is fixed — firmware/software rewrite only.
- Cheapest model suitable for each task.

## Open questions for the owner

None right now.

## Parked ideas (out of scope for this rewrite)

See `.agent/ideas.md` — currently: a possible future grinder/chute
modification to reduce topup-pulse clumping.
