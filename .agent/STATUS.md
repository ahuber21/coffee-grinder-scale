# Status

*Last updated: 2026-09-12 — first real first-use session on the
physical device after the handoff/redesign work: found and fixed a
structural bug that made the topup mechanism permanently unable to
fire (AR-052), added the NVS persistence it needed (AR-028), froze the
FINALIZE timer and lengthened the post-dose auto-tare grace period
(AR-053/054), wired up a real SCREENSAVER idle timer that blanks the
panel (AR-055), and added a Model tab to the SPA showing the dosing
algorithm's live fitted parameters and formulas. See also
`two_paragraph_breakdown.md` for a short, always-current summary.*

## Where things stand

Planning and design are done; implementation is underway. Branch
`rewrite/rtos-fork`. Standing rules in `AGENTS.md`, full decision log in
`DECISIONS.md` (D1-D22), all findings in `ARS.md` (AR-001-058, all
resolved or non-blocking), design docs in `.agent/design/`.

**First real first-use session (2026-09-12):** the owner used the
device for actual dosing for the first time since the redesign work and
reported three issues, all fixed: (1) a session stopped GRINDING
correctly short of target but then never actually topped up, going
straight to FINALIZE after a delay -- traced to `computeTopupDecision`
structurally never being able to fire a pulse at the cold-start prior
(`2 x residual_sd` exactly consumed the entire 0.3g overshoot budget,
leaving zero margin for any pulse, permanently, since `TopupModelV1`
never persisted across reboots either) -- see AR-052/AR-028 for the
full root-cause and fix (a genuine reconstruction-accuracy bug plus a
deliberate, owner-approved margin change, `overshoot_k_sigma` 2.0 ->
1.5). (2) FINALIZE's displayed timer kept counting instead of freezing
-- AR-053. (3) Auto-tare fired almost immediately on returning to
IDLE -- AR-054, now a 10s grace period. Also, reviewing the codebase
turned up that `SCREENSAVER` was fully implemented in `DisplayTask` and
its timeout setting fully validated/exposed, but nothing ever actually
triggered the transition -- fixed with a real idle timer, and per the
owner's earlier "blank now" decision, it now cuts the backlight rather
than showing a clock (AR-055). Finally, since the topup-model
investigation required actually inspecting the algorithm to explain it
to the owner, that became a real feature: a new `MODEL_STATE` telemetry
event and a new SPA "Model" tab (`webapp/src/pages/Model.tsx`) show the
grind-rate/coast/topup models' current fitted values next to the exact
stop-time and topup-decision formulas, with today's numbers substituted
in. Reviewing that explanation, the owner caught two more real issues:
the rate/coast models decayed with a 45-day recency half-life for no
good reason (this specific grinder has run unchanged for 7+ years --
AR-056, now disabled, matching `TopupModel`'s existing no-decay
default), and the topup-pulse duration had no hard floor tied to the
physical relay's real minimum actuation time (~300ms, below which it
just stalls and produces zero output rather than a smaller dose --
AR-057, now a real 350ms floor independent of whatever the model
happens to have learned). Also added, per a follow-up request: SCREENSAVER
now wakes on a large weight change (configurable, default 2g), not just
a button press (AR-058).

**Live debugging session (2026-09-11, after the first OTA deploy):**
diagnosed and fixed, in order, using WS `"log"`-channel diagnostics
(no serial cable access) rather than guessing: buttons not registering
at all (AR-041, asymmetric active-high/active-low wiring the rewrite
had assumed was uniform), the scale reading a fixed value regardless of
force (AR-042, load-cell bridge excitation never powered), gain/speed
silently reverting to hardware power-on defaults (AR-043, settings
applied before `ADS1232::begin()` instead of after), and a display
flicker during OTA (AR-044, `DosingTask`/`NetworkTask` racing on the
same display mailbox). Then a batch of 5 owner-requested UX fixes: the
OTA flicker (same as AR-044), a full-integer display instead of a
"MAX" placeholder past the one-decimal layout's width (AR-045),
unconditional auto-tare while idle (AR-046), a display frame-rate floor
raised from 30fps to 60fps after confirming via live measurement that
30fps was hitting its floor with zero rendering overrun (so the cap
itself, not render time, was the limiting factor), and confirming
FINALIZE already showed live weight (no fix needed). Two more real bugs
surfaced from continued live use: GRINDING/TOPUP's stop-condition math
compared absolute scale weight against the target instead of the
weight change since the grind's software-tare baseline, causing a
session to jump straight to FINALIZE with no grinding when the
baseline wasn't near zero (AR-047); and the settings write
path/broadcast only ever covered 6 of ~23 `SettingsSnapshot` fields,
so gain/speed/read_samples and most timeout/topup-model tunables were
silently unreachable from the SPA despite already having validators in
`SettingsTask.cpp` (AR-048) — the Settings page now has a Basic section
plus a "Show advanced settings" section covering every field.

**Continued live use (2026-09-11, same session) surfaced two more real
bugs, both fixed:** a sensor glitch mid-grind (e.g. lifting the cup)
could corrupt the online rate model and force an immediate false
FINALIZE, since the per-sample regression had no plausibility guard and
a non-positive rate estimate was (wrongly) read as "stop now" rather
than "untrustworthy" (AR-049, two new native tests). Separately, the
owner asked for the ADC to dynamically run at 80 SPS while the reading
is actively changing and fall back to 10 SPS once settled, for snappier
response without sacrificing at-rest precision — three implementation
attempts all ended up oscillating live for the same underlying reason
(the ADS1232 driver's own "changing" flag isn't a reliable trigger for
a real-time decision at any single sample rate), and the owner asked to
drop the feature rather than keep iterating (AR-050, logged as a
standing finding for any future attempt). `ScaleTask` is back to
static, settings-controlled ADC speed.

**Handoff session (2026-09-11, owner stepped away with open-ended
direction):** "the transitions should not happen [dynamic speed] ...
work on reviewing the codebase and making everything better ... improve
the UI design ... it should look like an Apple product ... you have
full freedom." Completed: (1) the dynamic-speed revert above; (2) fixed
a structural `DisplayTask` bug where the boot splash was drawn via a
direct call before the main render loop's mode-change tracking existed,
bypassing it entirely rather than flowing through the same path as
every other screen (AR-051); (3) a real visual redesign of both the TFT
and the SPA around iOS/macOS system colors and (on the SPA) the
`-apple-system` font stack (D22) — the TFT gained a top-of-screen
progress bar and small accent underlines without any custom font (flash
headroom is too tight, ~85KB, to safely add one), and the SPA was fully
reskinned (segmented-control nav, grouped-list settings rows, translucent
status pills, tabular-number readouts) while keeping every existing CSS
class name so no component logic changed. Verified: firmware builds
clean and 24/24 native tests pass after every change in this session;
the SPA build was visually checked in a real Chrome tab (Live/Settings/
History, including the "Show advanced settings" section) against the
dev server — desktop width only; the sandboxed browser tooling couldn't
actually shrink its viewport to confirm phone width this session, so
that's leaning on the CSS having kept the same flex/wrap structure
already phone-verified pre-redesign (AR-035), not a fresh visual check.
Both firmware and the SPA are deployed to the device and confirmed
serving (HTTP 200, clean boot, settings snapshot round-trips
correctly).

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

- **Web SPA** (`webapp/`, D4/D19): React + TypeScript + Vite, plain CSS
  (no framework) matching the old `dev/graph`/`dev/settings` mocks' dark
  aesthetic. Three tabs (hash-routed, no router library): **Live** (real-time
  weight/target/session status off the `/ws` telemetry stream, a Chart.js
  weight-vs-time chart per session, a manual "request a dose" form via
  `dose_request`, recent log lines), **Settings** (read/write the 8
  `SettingsSnapshot` fields the firmware currently accepts writes for,
  armed-confirm WiFi reset/reboot buttons), **History** (queries PostgREST
  *directly from the browser*, per D9 — bypasses the device entirely;
  recent-sessions table, per-session event/raw-sample detail, and a live
  D13-accuracy scoreboard computed from whatever real completed sessions
  exist so far). `lib/NetworkTask/NetworkTask.cpp` now mounts LittleFS
  (`formatOnFail=true`, so a never-flashed device self-formats on first
  boot) and serves the SPA from `/` via `serveStatic`, replacing the old
  placeholder page; `buildSettingsJson` was extended to include
  `calibration_factor` (closing a gap found while building the Settings
  page — it was writable but not broadcast). `platformio.ini` gained
  `board_build.filesystem = littlefs` and a `[platformio] data_dir =
  webapp/dist` (a real gotcha: `data_dir` is a project-wide
  `[platformio]`-section option, not per-`[env]` — it's silently ignored
  if placed under `[env:esp_wroom_02]`, which is where it went on the
  first attempt here before being caught and fixed).

  Verified: `npm run build` (`tsc --noEmit && vite build`) succeeds, 40
  modules, ~312KB output (103KB gzipped) against the 1.375MB `spiffs`-
  labeled partition. `pio run -t buildfs -e esp_wroom_02` packages it into
  a real LittleFS image (**never** `uploadfs` — that's the owner's call).
  `pio run -e esp_wroom_02` and `pio test -e native` (22/22) both still
  pass with LittleFS enabled. **Also visually verified** in a real Chrome
  tab against `npm run dev` (desktop + 400px-phone widths): all three
  tabs render correctly, no console errors, hash routing works without a
  full reload, WS-disconnected states render correctly, and the History
  tab's direct browser→PostgREST fetch (D9) genuinely round-tripped
  against the live `192.168.0.111:3000` deployment with no CORS issues.
  Found and fixed two real phone-width layout bugs in the process (AR-035)
  — `.setting-row` not wrapping, and both History tables missing a
  horizontal-scroll container. **Still not verified**: the actual `/ws`
  contract against a live ESP32 running this firmware — no hardware has
  run this build yet (AR-035, still open on that half).

- **First real deployment to the physical device** (OTA, owner's explicit
  go-ahead each time). Found and fixed two stacked bugs live, in order:
  `calibration_factor`'s own compiled-in default was `0.0f`, silently
  zeroing the scale on first boot (AR-037) — fixed by changing the
  default to `1.0f`. A one-time migration was then added to fold the
  pre-rewrite firmware's still-present EEPROM-emulated settings forward
  (that data survives OTA/serial reflashing, since neither touches NVS)
  — its first version migrated `calibration_factor` but not
  `gain`/`speed`/`read_samples`, which silently broke the migrated
  factor since it's only meaningful relative to the gain it was measured
  under (AR-038, ~127x reading error, matching the real gain=128 vs.
  the compiled default gain=1). Fixed in the same migration pass; once
  the correct values (`gain=128`, `speed=10`, `read_samples=12`,
  `calibration_factor≈0.000922`) were confirmed live on the device, the
  migration code was deleted as a completed one-off and those values
  became the compiled-in defaults directly (D20) — this is a single
  fixed device, not a fleet, so the migration could only ever run once.
  Also logged: a "read-only" API smoke test
  (`/api/getDosage?grams=18`) actually triggered a real grind, since
  that endpoint enqueues a live `DoseRequest` (AR-036) — no harm done,
  logged as a process lesson about testing against live hardware.
- **Comment/documentation cleanup** (D21, owner-directed): every
  class/struct/function/enum across the rewritten `lib/` and `webapp/`
  code now has a doxygen-style doc comment; inline comments were
  tightened to plain, self-contained explanations (no bare
  `D12`/`§4.5`/`AR-016`-style citations requiring another file to
  understand); the fully-superseded old modules (`lib/API`, `lib/Display`,
  `lib/RawDataWebSocket`, `lib/WebSocketGraph`, `lib/WebSocketLogger`,
  `lib/WebSocketMetrics`, `lib/WebSocketSettings`, and the local-only
  `dev/graph`/`dev/settings` mock tooling) were deleted outright rather
  than left as dead weight (closing AR-026); `platformio.ini`'s
  `lib_ignore` updated to match. `README.md` rewritten to briefly
  describe the current rewrite (architecture, how to connect — SPA URL,
  `/ws`, OTA — repo layout) instead of the original pre-rewrite feature
  list. Verified after: `pio run -e esp_wroom_02` succeeds at the exact
  same flash size as before (93.2%, confirming no behavior changed),
  `pio test -e native` still 22/22.

- **SPA filesystem image uploaded to the physical device** (`pio run -t
  uploadfs -e esp_wroom_02_ota`, owner's go-ahead). Verified live:
  `GET /` on the device returns the real built `index.html`, and its
  referenced JS/CSS assets both serve with correct size/content-type.
  `http://eureka.local/` is now a fully working SPA end-to-end on real
  hardware, not just in `npm run dev`.

**Not yet started:** decommissioning `coffee_grinder_api` (waits until
the new pipeline is verified in real use). Everything else the original
7-task skeleton and the SPA left stubbed now has a real implementation,
deployed and verified on the physical device.

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

- **TFT burn-in / lifetime, part 2**: part 1 (a working idle timer that
  blanks the panel via the backlight after `screensaver_timeout_s`, see
  AR-055) is done. Part 2 -- tying the panel's power to the actual
  coffee machine's on/off state, either pulled from the owner's Home
  Assistant instance or by having the ESP32 poll the machine directly
  -- is a deliberate follow-up, not started ("blank now, HA later").

## Parked ideas (out of scope for this rewrite)

See `.agent/ideas.md` — currently: a possible future grinder/chute
modification to reduce topup-pulse clumping.
