# Status

*Last updated: 2026-09-11 — all 7 FreeRTOS tasks have real bodies (only the
web SPA itself remains unbuilt).*

## Where things stand

Planning and design are done; implementation is underway. Branch
`rewrite/rtos-fork`. Standing rules in `AGENTS.md`, full decision log in
`DECISIONS.md` (D1-D17), all findings in `ARS.md` (AR-001-034, all
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
  The ADS1232 driver is vendored in directly (D6, executed via D16 — the
  submodule was never checked out in this repo, so its two source files
  were copied from the sister checkout instead of running
  `git submodule update --init`). Old `lib/API`/`Display`/
  `WebSocketSettings`/etc. modules are left in place for reference
  (AR-026) but no longer compiled into `esp_wroom_02` (nothing includes
  their headers).

- **SettingsTask, DisplayTask, NetworkTask, TelemetryTask real bodies**
  (all four remaining stubs from the skeleton pass, implemented in
  parallel by four subagents in isolated git worktrees, then merged):
  - **SettingsTask**: real `SettingsSnapshot` NVS persistence via
    `Preferences` (`"settings"` namespace, one key per field, a
    `schema_ver` guard), sharing the same field-validation functions as
    the live write path (AR-016) rather than a second copy. `TopupModelV1`
    NVS persistence is a separate structure and is still stubbed
    (AR-028).
  - **DisplayTask**: real ST7735 rendering for all 11 `DisplayMode`
    values, porting the old code's already-correct dirty-rect redraw
    discipline (only repaint changed pixels, not full-screen) rather than
    a naive per-frame redraw — and fixing two latent bugs surfaced in the
    process (AR-018 full-redraw-on-CONFIRM, AR-012 stale connection
    indicator never erased). `current_color`/`target_color`/`time_color`,
    idle countdown, debug IP, and OTA percent aren't populated by their
    producer tasks yet (AR-032) — cosmetic gap, not a DisplayTask bug.
  - **NetworkTask**: real WiFiManager provisioning (AP name `"Eureka
    setup"`, ported from the old code), `eureka.local` mDNS, and real
    ArduinoOTA gated by the existing `otaSafeToStart()` (D12) — the gate
    works by never pumping `ArduinoOTA.handle()` while a grind is active
    (so the espota handshake never even starts), with an `onStart()`
    check as a narrow race backstop. AsyncWebServer is live with the D4
    consolidated single-websocket channel scaffolded (4-client cap per
    AR-014, `cleanupClients()` per AR-013, typed JSON envelope, real
    inbound `settings_write`/`dose_request` handling forwarded to the
    owning task) — the SPA content itself is explicitly out of scope here
    (placeholder root response) and AP-portal status during provisioning
    is currently log-only, not shown on-screen, since DisplayTask now
    exclusively owns the SPI bus (AR-033).
  - **TelemetryTask**: real PostgREST POST/PATCH against the live `v2`
    schema (`/sessions`, `/events`, `/raw_samples`), plain HTTP with no
    auth header per the deployment's actual `postgrest_anon`-by-port
    config, live-verified against `192.168.0.111:3000` and cleaned up
    afterward. TOPUP pulse timestamps are currently approximated from one
    value rather than DosingTask's two real ones (AR-029), session
    `outcome` (aborted vs. timed_out) is inferred heuristically rather
    than reported explicitly (AR-030), and `firmware_version` posts a
    hardcoded placeholder pending a real version-stamping scheme
    (AR-031).

  Verified after merging all four: `pio run -e esp_wroom_02` succeeds —
  **RAM 10.0%, flash 89.7%** (up from 4.4%/21.7% at the skeleton stage;
  flagged as AR-034, worth checking before the SPA/LittleFS work or any
  further library additions land, since the app partition now has
  limited headroom left). `pio test -e native` still 22/22
  (`lib/DosingModel` itself untouched by any of this).

**Not yet started:** the web SPA itself (and serving it from LittleFS —
D4), decommissioning `coffee_grinder_api` (waits until the new pipeline is
verified in real use). Everything else the original 7-task skeleton left
stubbed now has a real implementation.

## Infrastructure on hand

- Read-only Postgres role `claude_agent` on `192.168.0.111`
  (`.agent/secrets/pg_agent.env`, gitignored) — used for all the analysis
  above, scoped to old tables only, no access to the new `v2` schema.
- PostgREST live at `http://192.168.0.111:3000` (see `AGENTS.md`), with
  its own scoped `postgrest_anon`/`postgrest_authenticator` roles.
  `TelemetryTask` now posts to it for real (see above) — the `v2` tables
  are no longer purely theoretical, though nothing has posted from *actual
  hardware* yet (only build-time/native verification and a live curl-style
  request-shape check so far).
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
