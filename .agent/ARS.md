# ARs — findings & open questions log

"AR" here = a finding worth tracking: a bug, a design smell, a piece of
code whose rationale is unclear, or a question that needs the owner's
input before proceeding. Log one as soon as you notice it — don't wait
until a dedicated audit pass. Update its status as it's resolved. Keep
entries short; link to `DECISIONS.md` if resolving one produces a real
architectural decision.

Format per entry:

```
### AR-NNN — short title
- **Area**: e.g. firmware/dosing, firmware/display, web, data-infra
- **Status**: open | confirmed | fixed | wontfix | needs-owner-input
- **Found**: date, by what (file/commit/observation)
- **What**: the finding, concretely
- **Why it matters**: concrete consequence if left as-is
- **Resolution**: (fill in once acted on)
```

---

### AR-001 — Button ISR mutates shared `state` without synchronization
- **Area**: firmware/architecture
- **Status**: open
- **Found**: 2026-09-11, reading `src/main.cpp` (`button_interrupt<pin>`,
  the `back` button ISR, and the main `switch(state)` loop)
- **What**: `state`, `button`, `last_button` are `volatile` globals written
  directly from interrupt context (`IRAM_ATTR` handlers) and read/written
  from the main loop with no critical section, mutex, or atomic. `volatile`
  prevents compiler reordering but does not make this safe against a
  genuine race between ISR and loop context on a dual-core part.
- **Why it matters**: plausible source of the "sporadic glitches and
  self-triggering" the README already admits to needing manual filtering
  for. The FreeRTOS task rewrite should replace this with a proper queue
  (ISR → `xQueueSendFromISR` → dosing/input task) instead of shared-memory
  signaling.
- **Resolution**: —

### AR-002 — `coffee_grinder_api/main.py` hardcodes a live DB password
- **Area**: data-infra
- **Status**: open — flagged only, not rotated (live credential on a
  running service; coordinate before changing)
- **Found**: 2026-09-11, `DB_PASSWORD = "..."` (plaintext literal) in
  `main.py` on `192.168.0.112` — value intentionally omitted here, don't
  re-add it to this repo
- **What**: plaintext Postgres password committed to that service's own
  git repo.
- **Why it matters**: moot once `coffee_grinder_api` is retired (D8), but
  worth confirming the credential gets rotated/removed as part of
  decommissioning `.112`, not just abandoned in a still-readable repo.
- **Resolution**: —

### AR-003 — `dev/graph` and `dev/settings` are disconnected mock tooling, not real dev tools
- **Area**: web
- **Status**: open
- **Found**: 2026-09-11, `dev/graph/websockets_server.py` and
  `dev/settings/websockets_server.py` serve canned/fake data on
  `localhost:8765`, unrelated to the real device.
- **What**: these look like live development tools from the README/repo
  layout but don't talk to the device at all.
- **Why it matters**: dead weight that could mislead a future contributor
  (or agent) into thinking they're a real integration point. Drop them
  once the new SPA replaces the three separate served pages, rather than
  porting them forward.
- **Resolution**: —

### AR-004 — Hardcoded dose correction constant in the API path
- **Area**: firmware/dosing
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp` `loopIdle()`: `float correction =
  requested_grams > 1.5f ? 1.5f : 0.0f;` with the comment "correction
  hardcoded for now"
- **What**: the HTTP API dosing path (`/api/getDosage`) applies a
  different, hardcoded correction margin instead of using the configurable
  `top_up_margin_single`/`top_up_margin_double` settings the button paths
  use.
- **Why it matters**: inconsistent behavior between button-triggered and
  API-triggered grinds; author's own comment marks it as unfinished. The
  new topup model should have one consistent way of deriving the
  correction margin regardless of trigger source.
- **Resolution**: —

### AR-005 — EEPROM settings versioning has no real migration path
- **Area**: firmware/settings
- **Status**: open
- **Found**: 2026-09-11, `WebSocketSettings.h`: single `magic = 0xCAFEBABE`
  field on the `Scale` struct, no version field, no migration logic
  observed alongside it.
- **What**: any change to the `Scale` struct's layout risks silently
  reading garbage (or the magic mismatching and falling back to defaults,
  losing calibration) rather than migrating old data forward.
- **Why it matters**: worth deciding deliberately whether the rewrite's
  settings storage (NVS is the more idiomatic ESP32 choice over raw
  EEPROM) gets real schema versioning, given calibration data is
  operationally important (re-calibrating is annoying).
- **Resolution**: —

### AR-006 — Pin sharing between `BUTTON_RIGHT` and `DISPLAY_MISO_PIN`
- **Area**: firmware/hardware
- **Status**: open — plausibly intentional, needs verification during
  the port, not a blocking concern
- **Found**: 2026-09-11, `include/defines.h`: `BUTTON_RIGHT` and
  `DISPLAY_MISO_PIN` are both pin 12.
- **What**: likely intentional and harmless — the ST7735 display is
  write-only from the MCU's perspective (no MISO read-back needed), so the
  pin is probably free to reuse for a button. Should be confirmed rather
  than assumed once the display driver is re-examined, since getting this
  wrong would produce very confusing intermittent bugs.
- **Resolution**: —

### AR-007 — Back-button ISR has zero debounce and bypasses all state gating, causing spurious display clears mid-grind
- **Area**: firmware/architecture (extends AR-001)
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp:219-226` (`back_button_isr` lambda,
  attached with `attachInterrupt(..., RISING)`), compared against
  `button_interrupt<pin>` (lines 138-163) and the `BUTTON_PRESSED` dispatch
  in `loop()` (lines 268-277).
- **What**: `button_interrupt<pin>` (left/right) only reacts when
  `state == CONFIRM` or `state == IDLE || SCREENSAVER`, and is gated by a
  time-based debounce (`now - button_pressed_millis <
  settings.scale.button_debounce_ms`, line 142) before it does anything.
  `back_button_isr` has neither guard: it unconditionally sets
  `old_state_button_press = state; state = BUTTON_PRESSED; button = back;`
  regardless of current state, and with no debounce check at all. If `back`
  is pressed (or bounces) while the machine is e.g. `RUNNING`, the very next
  `loop()` iteration sees `state != old_state_loop` → `display.clear()`
  fires, then the `BUTTON_PRESSED` case restores `state =
  old_state_button_press` (since `old_state_button_press` is neither `DEBUG`
  nor `IDLE`) — which triggers a *second* `need_to_clear` → a second
  `display.clear()` on the following iteration, plus one skipped
  `loopRunning()` iteration (missed scale/graph/rawData sample).
- **Why it matters**: this is a second, structurally different race/flicker
  hazard beyond the one AR-001 already flags (which is about `state`/
  `button` being written from ISR context at all). Even once that's fixed
  with a proper queue, the *asymmetry* between the two ISRs (no debounce,
  no state check on `back`) is a design inconsistency that should be
  resolved deliberately in the rewrite, not just ported — it's a plausible
  contributor to the "sporadic glitches" the README mentions, specifically
  visible as a flicker during active grinding when `back` is bumped.
- **Resolution**: —

### AR-008 — CONFIRM's second button press skips the noise-debounce state that the first press gets
- **Area**: firmware/architecture
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp:146-154` (`button_interrupt<pin>`,
  `state == CONFIRM` branch) vs. `loopButtonFilter()` (lines 404-422).
- **What**: the *first* button press (IDLE/SCREENSAVER → BUTTON_FILTER)
  goes through `loopButtonFilter()`, which re-samples `digitalRead(button)`
  after `button_debounce_min_hold` (20ms) and reverts to IDLE if the pin
  isn't still held low — an explicit contact-bounce/noise filter. The
  *second* press, while in `CONFIRM` (confirming the dose and arming
  `grinderOn()` via `TARE`→`CONFIGURED`), transitions straight from ISR
  context to `TARE` with only the time-based debounce
  (`button_pressed_millis`) applied — it never passes through
  `BUTTON_FILTER`.
- **Why it matters**: a single noise glitch on the button GPIO while sitting
  in `CONFIRM` (e.g. the machine is bumped, EMI from the grinder motor
  relay) can look like a valid confirm press and immediately start the
  grinder with no hold-time verification, whereas the identical glitch
  arriving in `IDLE` would be caught and discarded. This asymmetry should
  be a deliberate, stated choice in the rewrite, not an accident of how the
  state machine grew.
- **Resolution**: —

### AR-009 — `settings.scale.*` is read from ISR/loop context while written from the AsyncTCP web-server task with no synchronization
- **Area**: firmware/architecture (a second race distinct from AR-001)
- **Status**: open
- **Found**: 2026-09-11, `lib/WebSocketSettings/WebSocketSettings.cpp`
  `handleWebSocketText()` (lines 561-756) writes `scale.*` fields directly
  from the ESPAsyncWebServer/AsyncTCP callback context (a different
  FreeRTOS task, commonly pinned to the other core from `loop()`).
  Meanwhile `src/main.cpp` reads `settings.scale.*` pervasively from
  `loop()` and — notably — from **inside the button ISR itself**:
  `button_interrupt<pin>` at line 142 reads
  `settings.scale.button_debounce_ms` directly.
- **What**: none of `Scale`'s fields (`unsigned long` timers, `float`
  calibration/rate values, the `topup_lookup_table[6]` array) are atomic,
  `volatile`, or protected by a mutex/critical section. A settings-page
  "Save" can race with an in-progress read of the same multi-byte field
  from the main loop or an ISR, risking a torn read (e.g. a `float` or
  `unsigned long` observed as a mix of old/new bytes) on a dual-core part.
- **Why it matters**: this is a second, independent instance of the
  ISR/cross-task shared-state hazard AR-001 already flags for `state`/
  `button` — but for the settings struct, which is larger, written far more
  rarely (good) yet touched from *many* more read sites including directly
  inside an `IRAM_ATTR` handler (bad: reading a non-trivial struct field
  from ISR context while another task can be mid-write is a genuine bug
  class, not just poor style). The rewrite's FreeRTOS task design should
  route settings updates through the same queue/notification discipline as
  input events, or at minimum guard the struct with a spinlock/mutex and
  keep ISR reads to `volatile` scalars only.
- **Resolution**: —

### AR-010 — EEPROM struct-evolution "migration" blindly byte-copies a prefix with no validation
- **Area**: firmware/settings (extends AR-005)
- **Status**: open
- **Found**: 2026-09-11, `lib/WebSocketSettings/WebSocketSettings.cpp:794-820`
  (`loadScaleFromEEPROM`), introduced in commit `dcbebfd` ("run time
  modifications").
- **What**: AR-005 already flags that there's no real migration path; this
  is the concrete mechanism that exists instead. On magic mismatch, the
  code assumes the *previous* struct layout's first 8 fields
  (`read_samples` … `top_up_margin_double`) are byte-identical in position
  and type to the *current* struct's first 8 fields ("This works because we
  moved magic to the end, so the memory layout of the start matches" —
  comment at line 805), and copies them out of the raw EEPROM bytes with
  **no range/sanity validation** before calling `saveScaleToEEPROM()`. On a
  genuinely blank/erased EEPROM (all `0xFF`), `calibration_factor` would be
  read as the float bit-pattern `0xFFFFFFFF` (NaN) and fed straight into
  `scale.setCalFactor()` in `setupScale()` with no check.
- **Why it matters**: this "migration" is correct only by a fragile,
  undocumented invariant — the *next* time a field is inserted, reordered,
  or resized anywhere in that 8-field prefix (not just anywhere in the
  struct), this silently reads garbage into calibration/target-dose values
  instead of falling back to defaults, and there's nothing that would catch
  it (no bounds check, no NaN check). Worth deciding deliberately in the
  rewrite: real versioned migration (a version field + explicit per-version
  upgrade functions) vs. accepting "wrong version = reset to defaults" as
  the simpler, safer contract.
- **Resolution**: —

### AR-011 — Weight-based fallback stop threshold uses the *uncorrected* target, undermining the topup-margin strategy if the rate/time estimate never fires
- **Area**: firmware/dosing
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp:605-614` (`loopRunning`), specifically
  line 608: `if ((grams > target_grams) || (stop_time_calculated && now >=
  calculated_stop_millis))`. Confirmed intentional via commit `ad0c227`
  ("use weight comparison only as fallback, prioritize calculated
  runtime"), which changed this from `target_grams_corrected` to
  `target_grams`.
- **What**: the *primary* stop mechanism is a rate/time estimate
  (`calculated_stop_millis`), computed once `grams` crosses
  `rate_calculation_percentage` of `target_grams` (line 568-570). The
  *fallback* — meant only to catch cases where the estimate never engages
  (e.g. `stop_time_calculated` never becomes true because the flow-rate
  threshold is never reached, a slow start, or a jam) — compares raw
  `grams` against the **full** `target_grams`, not
  `target_grams_corrected` (`target_grams` minus the configured topup
  margin). If the primary mechanism never fires, the grinder runs
  continuously all the way to the full target weight in one pour, entirely
  skipping the topup-margin design (stop short, then fine-adjust in small
  pulses during `TOPUP`) that the rest of the system is built around.
- **Why it matters**: `DECISIONS.md` D7 sets a hard cap on overshoot
  (≤0.3g, "weighted as more critical to avoid than undershoot") precisely
  because a single continuous pour to full target has much worse worst-case
  accuracy than a margin-then-topup approach. This fallback path is the one
  case in the current code where that safety margin is silently dropped.
  Worth explicitly deciding in the new topup model whether the fallback
  should ever target anything other than `target_grams_corrected`.
- **Resolution**: —

### AR-012 — Connection-status indicator dot is never erased, and isn't part of either layout's change-detection
- **Area**: firmware/display
- **Status**: open
- **Found**: 2026-09-11, `lib/Display/Display.cpp` — `drawConnectionIndicator`
  (lines 674-681) only draws when `color != 0`, with no corresponding
  "erase" branch for `color == 0`. In `displayGrindingLayout` (lines
  138-308), the "skip if nothing visible changed" check (lines 190-199,
  `colorChangedForFPS`/`sameCurrent`/`sameTarget`/`sameTime`) never
  compares `connectionIndicatorColor` at all — only the big-digit text
  color is tracked for change detection. `displayIdleLayout` *does* track
  `lastIdleConnectionColor` (line 371-372), but still has no erase path
  when the new color is `0`.
- **What**: (1) once a client connects and the small status circle
  (`drawConnectionIndicator`, bottom-left, 3px radius) is drawn in a color,
  it is never cleared when all clients disconnect (`color` reverts to `0`)
  — it stays on screen as a stale artifact until the next full
  `display.clear()` (i.e. next state transition). (2) In the grinding
  layout specifically, an indicator-color-only change (e.g. a metrics
  client connects mid-grind while the weight/time text happens to be
  unchanged that tick) is dropped entirely and never drawn, because it's
  outside the change-detection comparison.
- **Why it matters**: directly relevant to the "display rendering, FPS/
  flicker" area the audit asked about — this is a real, reproducible visual
  bug (stale/incorrect connection indicator), not just a flicker
  inefficiency. The rewrite's redraw-diffing should explicitly include
  every visible element (including status indicators) in its dirty-check,
  and erase-on-change needs to be symmetric with draw-on-change.
- **Resolution**: —

### AR-013 — No WebSocket endpoint ever calls `cleanupClients()`
- **Area**: firmware/web (resource leak)
- **Status**: open
- **Found**: 2026-09-11, grep across `lib/` for `cleanupClients` returns no
  matches. All five endpoints (`WebSocketLogger`, `WebSocketSettings`,
  `WebSocketGraph`, `WebSocketMetrics`, `RawDataWebSocket`) construct an
  `AsyncWebSocket` and register `onEvent`, but none periodically call
  `AsyncWebSocket::cleanupClients()`.
- **What**: ESPAsyncWebServer's own guidance is that `cleanupClients()`
  should be called periodically (typically once per `loop()`) to reap
  client objects/buffers for connections that have already closed;
  otherwise they can linger in the socket's internal client list. This
  codebase never does so, on any of the five sockets.
- **Why it matters**: over a long uptime with repeated browser
  connect/disconnect cycles (settings page reloads, graph tab left open
  across WiFi hiccups, phone browser backgrounding/foregrounding), this is
  a slow heap leak — plausibly part of why long-uptime devices in this
  class eventually need a reboot. Cheap and mechanical to fix; worth
  carrying the fix (a single `cleanupClients()` call per loop, or per
  socket) into the rewrite's consolidated single-channel design (D4) from
  day one.
- **Resolution**: —

### AR-014 — Per-socket connection caps are inconsistent, and two sockets have none at all
- **Area**: firmware/web
- **Status**: open
- **Found**: 2026-09-11: `WebSocketSettings::onWebSocketEvent` caps at 1
  client (`WebSocketSettings.cpp:538`), `RawDataWebSocket`'s handler caps at
  1 (`RawDataWebSocket.cpp:22`), `WebSocketLogger::onWebSocketEvent` caps at
  3 (`WebSocketLogger.cpp:161`) — but `WebSocketGraph::handleWebSocketEvent`
  is a literal empty stub ("Handle WebSocket events if needed",
  `WebSocketGraph.cpp:198-203`) with no cap at all, and
  `WebSocketMetrics::handleEvent` (`WebSocketMetrics.cpp:22-32`) also never
  checks `server->count()`.
- **What**: three different connection-limit policies (1, 3, unlimited)
  across five conceptually-similar endpoints, with no stated rationale for
  the difference — reads as organic growth (each socket bolted on
  separately) rather than a deliberate policy.
- **Why it matters**: `WebSocketGraph` and `WebSocketMetrics` broadcast via
  `_ws.textAll()` on every update (graph: on every `updateGraphData` call;
  metrics: throttled to ~6-7Hz but still continuous during a grind) — an
  unbounded number of forgotten-open browser tabs each cost a `textAll()`
  send and, per AR-013, are never cleaned up either. Combined, these two
  findings compound: the sockets most likely to accumulate stale clients
  are exactly the ones with no cap. The rewrite's single consolidated
  channel (D4) sidesteps this by construction, but it's worth confirming
  the new design picks one deliberate connection-limit policy rather than
  inheriting the inconsistency.
- **Resolution**: —

### AR-015 — `/api/getDosage` has no input validation and builds its JSON response by unescaped string concatenation
- **Area**: firmware/dosing, web
- **Status**: open
- **Found**: 2026-09-11, `lib/API/API.cpp:15-33`
  (`handleGetDosageRequest`).
- **What**: the `grams` query parameter is parsed with
  `gramsParam.toFloat()`, which silently returns `0.0` for any non-numeric
  input — there is no distinction between "caller asked for 0g" and
  "caller sent garbage". There is also no range check (negative values,
  absurdly large values) before `newDosageValue`/`newValueReceived` are
  latched and later consumed by `loopIdle()` (which applies the AR-004
  hardcoded correction and moves the state machine to `CONFIRM`). The
  success response is built as `"{\"dosage\": \"" + gramsParam + "
  grams\"}"` — the raw, unescaped request parameter is spliced directly
  into a JSON string literal, so a value containing `"` or control
  characters produces a malformed JSON response.
- **Why it matters**: this endpoint is unauthenticated and reachable to
  anything on the LAN (consistent with the project's accepted-risk posture
  per D5, so not a "fix the security model" ask) — but as a matter of basic
  robustness, a malformed/negative/garbage `grams` value should be rejected
  with a 400 rather than silently coerced to 0 and threaded through the
  dosing state machine. The physical confirm-button gate (CONFIRM state)
  limits the blast radius, but the API's own response is still incorrect
  for adversarial input independent of what the firmware does with the
  value.
- **Resolution**: —

### AR-016 — Unvalidated `rate_default` setting can drive a divide-by-zero and undefined-behavior float→integer cast
- **Area**: firmware/dosing, firmware/settings
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp:580-595` (`loopRunning`) combined
  with `lib/WebSocketSettings/WebSocketSettings.cpp:622-625` /
  `716-722` (settings ingestion for `rate_default`, `rate_min_valid`,
  `rate_max_valid` — accepted with no bounds checking, see also AR-009).
- **What**: when the measured `calculated_rate` falls outside
  `[rate_min_valid, rate_max_valid]`, it's replaced with
  `settings.scale.rate_default` (line 584) with no validation that
  `rate_default` itself is positive/non-zero. `run_duration =
  target_grams_corrected / calculated_rate` (line 588) then divides by
  whatever was set. If a settings-page edit (or a bad `batch:` payload)
  sets `rate_default` to `0` — nothing in `handleWebSocketText` rejects
  that — this produces `run_duration = inf` (or NaN if
  `target_grams_corrected` is also 0), which is then cast to
  `unsigned long` at line 595 (`(unsigned long)(run_duration * 1000)`):
  converting a non-finite float to an integer type is undefined behavior
  in C++, not just "a very large number."
- **Why it matters**: a single bad value in the settings UI (typo,
  copy-paste error, or a future automated tuning pipeline writing a
  degenerate value) can produce UB in the stop-time calculation for every
  subsequent grind until corrected. The rewrite's settings validation
  should reject/clamp non-positive values for anything used as a divisor,
  not just trust values coming off the wire.
- **Resolution**: —

### AR-017 — `loopButtonPressedDebug()`'s `left`/`right` cases are unreachable dead code
- **Area**: firmware/architecture
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp:450-465`.
- **What**: `loopButtonPressedDebug()` is only invoked when `state ==
  BUTTON_PRESSED` and `old_state_button_press == DEBUG` (see the dispatch
  at lines 268-277). The *only* code path that can produce
  `old_state_button_press == DEBUG` is `back_button_isr` (lines 220-225),
  which unconditionally sets `button = back` in the same breath. The
  left/right `button_interrupt<pin>` ISR never fires a `state` transition
  while `state == DEBUG` (its guard only allows `state == CONFIRM` or
  `state == IDLE || SCREENSAVER`, `main.cpp:146-159`), so `button` can
  never be `left` or `right` at the point `loopButtonPressedDebug()` runs.
  Its `case left: state = DEBUG; break;` and `case right: state = DEBUG;
  break;` (lines 452-457) are therefore provably unreachable.
- **Why it matters**: small, but a concrete example of dead code from
  organic growth — the debug-mode exit path presumably intended left/right
  to also do *something* in debug mode (stay in debug? cycle a
  sub-view?), but wiring never allows it. Worth deciding in the rewrite
  whether debug mode should actually respond to left/right, rather than
  silently porting unreachable branches forward.
- **Resolution**: —

### AR-018 — `displayConfirmLayout` full-screen-clears on every redraw, with a comment admitting the author wasn't sure this was right
- **Area**: firmware/display
- **Status**: open
- **Found**: 2026-09-11, `lib/Display/Display.cpp:460-554`
  (`displayConfirmLayout`), particularly the comment block at lines 480-491:
  *"...since the user complained about flicker, we should avoid full clear
  if possible, but since the value doesn't change often in confirm screen
  ... we can afford a full clear on first draw, and then nothing. However,
  to support the 'large digits' style ... we need the smart clear logic."*
- **What**: every other layout function in this file
  (`displayGrindingLayout`, `displayIdleLayout`, `displayScreensaver`) was
  specifically rewritten (per commit `40c3dc3`, "fix display clears") to do
  targeted `fillRect` clears of only the region that changed, precisely to
  avoid the flicker the comment references. `displayConfirmLayout` is the
  one holdout still doing `m_display.fillScreen(ST7735_BLACK)`
  unconditionally on every value change (line 532), and the comment
  explicitly flags this as a known gap rather than a deliberate choice
  ("we might want to..." / "we will just clear... or the whole screen if we
  want to be sure").
- **Why it matters**: matches the audit's "comment reveals the author knew
  it was incomplete" pattern directly. In practice low-impact today (this
  screen only redraws once per value, gated by `lastTargetGrams ==
  targetGrams`), but it's an inconsistency a future maintainer would
  reasonably assume was intentional if not flagged, and the rewrite should
  apply the same partial-redraw discipline uniformly rather than leaving
  one screen as the unexplained exception.
- **Resolution**: —

### AR-019 — Topup lookup-table bucket boundaries are duplicated (and must stay in sync) between firmware and the settings UI
- **Area**: firmware/dosing, web
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp:704-719` (`loopTopUp`, the
  `gap <= 0.1f / 0.2f / 0.3f / 0.4f / 0.5f` → `index` ladder feeding
  `settings.scale.topup_lookup_table[index]`) vs. the six hardcoded bucket
  labels in `lib/WebSocketSettings/WebSocketSettings.cpp:445-484`
  ("Topup 0.0-0.1g [s]" … "Topup >0.5g [s]").
- **What**: the mapping from weight-gap to lookup-table index is a
  hand-written if/else ladder in firmware; the *meaning* of each of the 6
  table slots (the gap ranges) is separately hardcoded as prose labels in
  the settings page HTML. Nothing ties these together structurally — the
  array size (6) and the bucket edges (0.1/0.2/0.3/0.4/0.5) are asserted in
  two independent places.
- **Why it matters**: low risk today since both were added together in the
  same commit (`3be7842`, "use LUT for topup times"), but it's exactly the
  kind of pairing that silently desyncs the next time either side is
  tuned — someone changes the bucket count or edges in firmware without
  remembering the UI labels are now lying about what each field controls
  (or vice versa: relabels the UI without realizing firmware logic must
  match). Worth generating the table/label pairing from one source of
  truth in the rewrite (matches D7's move away from this lookup table
  entirely, but the general "don't hand-duplicate a mapping across two
  files" lesson applies wherever settings UI and firmware logic must agree
  on structure).
- **Resolution**: —

### AR-020 — OTA `upload_port` in `platformio.ini` hardcodes an IP the project's own docs say is stale
- **Area**: firmware/build
- **Status**: open — trivial to fix, noted for completeness
- **Found**: 2026-09-11, `platformio.ini:38`
  (`[env:esp_wroom_02_ota]`, `upload_port = 192.168.0.118`).
- **What**: `.agent/AGENTS.md` records that the device "was
  `192.168.0.118` / mDNS `esp32-98cdac595620` before rename" and is now
  reachable at `eureka.local`. The OTA build environment still hardcodes
  the old numeric IP.
- **Why it matters**: harmless as long as nobody runs `pio run -e
  esp_wroom_02_ota -t upload` against a stale address (and per the hard
  constraint in `AGENTS.md`, nobody but the owner should be running OTA
  uploads at all right now) — but it's a small piece of config rot that
  would silently fail or target the wrong host if DHCP ever reassigns that
  IP. Worth pointing `upload_port` at `eureka.local` in the rewrite's
  `platformio.ini` instead of a numeric address, consistent with D5.
- **Resolution**: —

### AR-021 — `loopTopUp()`'s first metrics event misreports the main-grind tail as a topup pulse, contaminating the historical `topup` table
- **Area**: firmware/dosing, data-infra
- **Status**: confirmed (found via data analysis, then traced to the exact
  code path)
- **Found**: 2026-09-11, during topup-model data analysis
  (`.agent/design/topup-model.md` §0). `src/main.cpp`: `grinderOff()` at
  the end of `loopRunning()` sets `grinder_runtime_millis` to the *main
  grind's* duration; state moves to `TOPUP`; the very first pass through
  `loopTopUp()` (with `grinder_is_running == false`) calls
  `metrics.sendTopUp(grinder_runtime_millis, delta_grams)` before any real
  topup pulse has fired — so the first "topup" event reported for every
  single session is actually the main grind's own runtime/weight,
  mislabeled.
- **What**: confirmed statistically — reconstructing per-session clusters
  in the historical `topup` table, row 1 of every cluster averages ~11s
  runtime / ~11.7g weight (obviously a full dose, not a pulse), while rows
  2+ average 0.5-1.7s / <1.5g (genuine pulses). 2,122 of 7,647 historical
  `topup` rows (~28%) are this misattributed main-grind tail, not real
  topup data.
- **Why it matters**: harmless to current on-device behavior (it's just a
  metrics/logging event, doesn't affect grinder control), but it
  corrupted a chunk of the very historical dataset D7's model is being
  fit from — the topup-model analysis had to filter it out by heuristic
  (first row per ~20s-gap cluster). The new `sessions` schema (D8/D9,
  and `.agent/design/topup-model.md` §6) should tag events by explicit
  `event_type` (`MAIN_GRIND`/`TOPUP`) at write time so this class of bug
  is structurally impossible going forward, and the firmware fix itself
  (don't emit a topup event before a real pulse has happened) should ship
  with the rewrite.
- **Resolution**: —

### AR-022 — D7's 80%/Δ0.05g accuracy target may not be physically achievable on the current hardware — needs owner input
- **Area**: firmware/dosing
- **Status**: fixed
- **Found**: 2026-09-11, `.agent/design/topup-model.md` §5, from live
  analysis of 5,525 genuine historical topup pulses.
- **What**: sensor noise floor is ~0.02g (2.5x tighter than needed — not
  the bottleneck). But the smallest *reliably controllable* topup
  increment — the output of the shortest pulse that reliably clears the
  ~300ms electromechanical dead zone — has a measured spread of roughly
  0.15-0.2g effective size with ~0.14g sd around it. That is physically
  coarser than the 0.05g "spot on" tolerance by roughly 3-4x: a single
  relay-switched topup pulse cannot reliably add exactly 0.05g on this
  grinder, full stop, no amount of modeling fixes that. Landing inside
  0.05g in 80% of sessions therefore depends almost entirely on the
  *main grind* itself stopping close to target on its own, not on topup
  fine-tuning it in — sessions that need more than ~0.15g of topup
  correction will mostly land in the 0.05-0.2g band, not under 0.05g.
- **Why it matters**: this is the owner's own stated success criterion
  (D7), not an implementation detail — it should not be silently
  reinterpreted. The 95%/Δ0.2g and overshoot-≤0.3g targets both look
  achievable (see AR-023 for a currently-unmet baseline on the overshoot
  side). The 80%/Δ0.05g target is achievable but tight, and its
  achievability now depends on how good the *main-grind* stop estimate
  turns out to be in practice, not on the topup logic. Given hardware is
  fixed (D1), there's no mechanical lever to pull here.
- **Resolution**: Resolved, no change to D7's numbers — see `DECISIONS.md`
  D13. Owner confirmed the physical read (relay minimum on-time roughly
  0.3-0.4s; within that window ground coffee clumps unpredictably, and
  whether a clump falls is what produces the ~0.15-0.2g granularity — not
  something a smarter topup-pulse model can fix). Strategy: chase 0.05g
  primarily through a much better *main-grind* stop estimate — a
  continuous flow during the main grind isn't subject to the same
  clumping (it's a steady stream, not discrete bursts) — so topup is
  invoked as rarely as possible, rather than trying to make topup pulses
  themselves more precise than the mechanism allows. Owner independently
  tried a naive `total_grams_out / time_running` rate estimate and found
  it insufficient; this is exactly why `topup-model.md`'s Model A uses a
  continuous regression over the post-dead-time plateau instead of a
  single division from t=0 — see the addendum added there.

### AR-023 — Current topup lookup table already breaches the 0.3g overshoot cap ~21% of the time at its shortest pulse setting
- **Area**: firmware/dosing
- **Status**: confirmed (data-driven finding about the *current live*
  system's real-world behavior, distinct from AR-011's separate fallback-
  path bug)
- **Found**: 2026-09-11, `.agent/design/topup-model.md` §1.4/§5, from the
  400-500ms bucket of genuine historical topup pulses (n=3,911): 20.9%
  exceeded 0.3g added by that single pulse alone; 44.9% exceeded 0.2g.
- **What**: the shortest bucket of `topup_lookup_table` fires whenever the
  remaining gap is ≤0.1g — but the pulse duration that bucket is tuned to
  has enough inherent scatter that roughly 1 in 5 firings alone overshoots
  the 0.3g cap, independent of any other bug. This is a property of the
  mechanism (dead time + flow-rate noise at short durations), not a coding
  mistake, but the current lookup table has no logic to account for it —
  it always fires the configured duration regardless of the risk.
- **Why it matters**: directly relevant to the owner's stated top
  complaint (overshoot forces discarding ground coffee). Strong evidence
  the *current* device already overshoots more often in practice than the
  new D7 target allows, not just a theoretical risk. The proposed model
  (`.agent/design/topup-model.md` §4.5) addresses this directly: refuse to
  fire below a minimum controllable gap, and aim each pulse at ~90% of the
  remaining gap rather than 100%, rather than unconditionally firing a
  fixed duration.
- **Resolution**: —

### AR-024 — Should an OTA update mid-grind abort the grind, or should OTA be refused while a grind is in progress?
- **Area**: firmware/architecture, product decision
- **Status**: needs-owner-input (low urgency — has a stated safe default,
  not blocking further design work)
- **Found**: 2026-09-11, `.agent/design/rtos-architecture.md` §7/§10, while
  designing the Network↔Dosing task interaction around OTA.
- **What**: today's firmware only pumps `ArduinoOTA.handle()` from
  `loopIdle()`/`loopScreensaver()`, so an OTA flash can only ever start
  while idle — a grind in progress structurally can't be interrupted by
  one. The new task architecture decouples OTA handling (Network task) from
  grind state (Dosing task) as a matter of responsiveness, which reopens
  the question explicitly: should Dosing task (a) force an immediate
  `STOPPING` (relay off) if an OTA flash begins mid-grind, or (b) should
  Network task refuse to start an OTA flash at all while Dosing is outside
  `IDLE`/`SCREENSAVER`? The design doc recommends (a) as the safer default
  given a live relay is involved, but (b) is a defensible alternative.
- **Why it matters**: low real-world likelihood (OTA deploys are already
  gated to the owner only, per `AGENTS.md`'s hard constraint — nobody is
  triggering one by accident mid-grind), so this doesn't block further
  work; the recommended default ((a), abort-and-stop) can be implemented
  and revisited if the owner prefers (b).
- **Resolution**: Fixed — owner chose (b). Network task refuses to start
  an OTA flash (rejects `ArduinoOTA`'s begin) while Dosing task is outside
  `IDLE`/`SCREENSAVER`, rather than aborting a grind already in progress.
  See `DECISIONS.md` D12.

### AR-025 — Main-grind stop calculation doesn't anticipate "coast" weight (coffee still landing after relay-off), and it's a bigger effect than the topup-pulse noise floor
- **Area**: firmware/dosing
- **Status**: confirmed, design addition made (not yet implemented in code)
- **Found**: 2026-09-11, `.agent/design/coast-effect.md` (owner-prompted
  investigation), analyzing 282 reconstructed main-grind sessions spanning
  the full ~1-year history via `raw_data`.
- **What**: after `grinderOff()`, the scale reading continues climbing for
  a median of ~1.4s before settling — median **0.49g** added during that
  window (IQR 0.38-0.60g), correlated with flow-rate-at-cutoff (r=0.50),
  not with dose size (r=0.07). `loopRunning()`'s stop check
  (`src/main.cpp:608-609`) has no anticipation of this at all — it fires
  purely on raw `grams > target_grams` or a calculated stop time.
- **Why it matters**: this is **2.5-3x bigger** than the ~0.14-0.2g
  topup-pulse noise floor that `.agent/design/topup-model.md` §5 already
  identified as the actual accuracy bottleneck. Every main grind today
  overshoots its own intended stop point by about this much before topup
  ever gets involved — directly relevant to both the overshoot cap (D7)
  and the 80%/Δ0.05g target (D13), and one of the more promising concrete
  levers found for the latter, since it's a main-grind-side fix rather
  than a topup-pulse-side one (which is capped by clumping per D13,
  unlike this).
- **Resolution**: Fixed. Design addition — a third small persisted model
  ("Model C", `coast_weight_hat`) alongside the two from
  `topup-model.md` §4, subtracting a predicted coast offset from the
  effective stop threshold. See `topup-model.md` §8 for the full design.
  Implemented in `lib/DosingModel/` (`CoastModel` class,
  `MainGrindModel::predictStopTimeMsWithCoast`), commit `f9fad2f`, 6 new
  unit tests passing (22/22 total in the module).

### AR-026 — Old `lib/` web/display modules are now dead weight, unbuilt but unremoved
- **Area**: firmware/architecture, cleanup
- **Status**: open — not blocking, flagged for a later pass
- **Found**: 2026-09-11, implementing `design/rtos-architecture.md`'s task
  skeleton (`src/main.cpp` rewrite).
- **What**: `lib/API`, `lib/Display`, `lib/RawDataWebSocket`,
  `lib/WebSocketGraph`, `lib/WebSocketLogger`, `lib/WebSocketMetrics`,
  `lib/WebSocketSettings` are no longer referenced by anything (the new
  `src/main.cpp` doesn't include their headers) and PlatformIO's chain LDF
  correctly excludes them from the `esp_wroom_02` build as a result — so
  they cost nothing at build time, but they're now stale reference material
  sitting in `lib/` with no indication they're superseded. `Display` in
  particular still has real, salvageable ST7735 rendering logic (the
  targeted-`fillRect` partial-redraw discipline AR-018 discusses) that
  `DisplayTask` will eventually want to port in.
- **Why it matters**: a future contributor (or agent) browsing `lib/` has no
  signal that these are pre-rewrite artifacts rather than active code —
  same class of confusion AR-003 already flagged for `dev/graph`/
  `dev/settings`. Left in place deliberately for this pass (real ST7735/
  WiFiManager/AsyncWebServer porting is explicitly out of scope per the
  task brief that produced this skeleton), but worth either deleting the
  ones with nothing left to salvage (`API`, `RawDataWebSocket`,
  `WebSocketGraph`, `WebSocketLogger`, `WebSocketMetrics`) or clearly
  marking all of them superseded once the tasks that would replace them
  (`DisplayTask`, `NetworkTask`) have real bodies.
- **Resolution**: —

### AR-027 — Dosing task's TOPUP loop has no cap on iteration count, only a wall-clock safety cutoff
- **Area**: firmware/dosing
- **Status**: open — theoretical, not exercised (no real hardware/relay
  timing yet to validate against)
- **Found**: 2026-09-11, `lib/DosingTask/DosingTask.cpp`
  (`handleTopupSample`'s `TopupPhase::DECIDING` branch and the
  `topup_timeout_ms * 10` safety cutoff below the switch).
- **What**: each `TopupModel::computeTopupDecision()` call can in principle
  keep firing sub-controllable-gap pulses indefinitely if the model's
  predicted weight consistently undershoots the real gap by a small margin
  (each pulse closes some of the gap but never enough to drop below
  `min_topup_grams`, and never enough to trip the `topup_timeout_ms * 10`
  wall-clock cutoff within a small number of iterations). The `TopupModel`
  itself (`lib/DosingModel`) has no protection against this either — it's a
  per-Dosing-task-loop concern, not a model concern.
- **Why it matters**: low real-world likelihood (§4.5's aim-at-90%-of-gap
  logic converges fast in practice per `topup-model.md`'s historical data),
  and the wall-clock cutoff is a real backstop, just a coarse one (10x
  `topup_timeout_ms` could be several seconds of extra relay activity in a
  pathological case). Worth adding an explicit max-pulse-count guard
  alongside the wall-clock one once this runs against real hardware and the
  actual convergence behavior can be observed, rather than guessing at a
  number now.
- **Resolution**: —

### AR-028 — `TopupModelV1` NVS persistence still stubbed after the SettingsTask NVS pass
- **Area**: firmware/settings, firmware/dosing
- **Status**: open
- **Found**: 2026-09-11, implementing real `SettingsSnapshot` NVS
  read/write in `lib/SettingsTask/SettingsTask.cpp` (`design/topup-model.md`
  §5 calls for persisting the recency-weighted model state across reboots).
- **What**: the task brief scoped this pass to `SettingsSnapshot` only;
  `TopupModelV1`'s own NVS load/save (a separate, larger persisted
  structure — the model's running regression state) is untouched and still
  effectively volatile across reboots.
- **Why it matters**: every device reboot currently loses the
  recency-weighted model's learned state and restarts from the prior/
  cold-start values documented in `topup-model.md` §4 — functionally
  correct (the model degrades gracefully to its prior) but throws away
  real learning between power cycles, which matters for D13's accuracy
  target given how few sessions a home device runs per day.
- **Resolution**: —

### AR-029 — TelemetryTask approximates TOPUP pulse relay-on/off timestamps with one shared value
- **Area**: firmware/telemetry, data-infra
- **Status**: open
- **Found**: 2026-09-11, wiring `TelemetryTask` to POST `/events` rows for
  each `TOPUP_PULSE` telemetry event.
- **What**: `TelemetryEvent` as currently defined only carries the
  pulse's settle-complete timestamp, not DosingTask's real
  `g_topup_pulse_start_ms`/relay-off timestamps, so both the events-table
  columns for pulse start and pulse end are populated from the one value
  TelemetryTask actually has.
- **Why it matters**: degrades the precision of exactly the topup-pulse
  duration data `topup-model.md`'s regression is fit from (D7) — new
  sessions posted through the rewritten pipeline would carry coarser
  timing than the historical `topup` table did, undermining the model's
  own future retraining data. Fix is mechanical: forward the two real
  timestamps DosingTask already has locally into `TelemetryEvent`.
- **Resolution**: —

### AR-030 — Session `outcome` (completed/aborted/timed_out) is inferred heuristically by TelemetryTask, not reported explicitly by DosingTask
- **Area**: firmware/telemetry, data-infra
- **Status**: open
- **Found**: 2026-09-11, wiring the `PATCH /sessions` finalize call.
- **What**: DosingTask's telemetry stream doesn't currently distinguish
  *why* a session ended (BACK-button abort vs. the TOPUP-loop wall-clock
  safety cutoff in AR-027 vs. normal completion) — both non-normal paths
  collapse to a generic `aborted` outcome in the schema rather than
  `timed_out` getting its own value.
- **Why it matters**: loses a real signal for later analysis (AR-027's
  safety cutoff firing in practice would be exactly the kind of thing
  worth knowing about from field data, and currently looks identical to a
  manual abort in the database).
- **Resolution**: —

### AR-031 — No firmware-version scheme yet; PostgREST rows post a hardcoded placeholder
- **Area**: firmware/telemetry, data-infra
- **Status**: open
- **Found**: 2026-09-11, TelemetryTask needed *some* value for the
  schema's `NOT NULL firmware_version` column and used the literal string
  `"rtos-rewrite-dev"`.
- **What**: the project has no build-time version-stamping mechanism
  (git SHA, semver tag, build timestamp) wired into the firmware yet.
- **Why it matters**: minor now (single developer, single branch), but
  once this firmware is actually flashed and iterated on, session rows in
  Postgres become unable to distinguish which build produced them — makes
  debugging field behavior against a specific commit much harder later.
  Cheap to fix whenever OTA/build tooling is next touched (e.g. embed
  `git describe` via a PlatformIO build flag).
- **Resolution**: —

### AR-032 — Several `DisplayCommand` fields (colors, idle countdown, debug IP, OTA percent) aren't populated by the tasks that should fill them yet
- **Area**: firmware/display, firmware/dosing, firmware/network
- **Status**: open
- **Found**: 2026-09-11, implementing real DisplayTask rendering — it
  renders every `DisplayMode` correctly against whatever `DisplayCommand`
  it's handed, but DosingTask's `sendDisplayCommand()` doesn't yet set
  `current_color`/`target_color`/`time_color` or `idle_h/m/s/ms`, and
  NetworkTask doesn't yet set `debug_ip`/`ota_percent`.
- **Why it matters**: cosmetic only — DisplayTask defaults colors
  sensibly per-mode and renders gracefully on zero/empty fields — but the
  SCREENSAVER, DEBUG, and OTA_UPDATE screens will look sparse/incomplete
  until Dosing/Network tasks are extended to populate their half of the
  contract. Not a bug in DisplayTask; a remaining wiring gap in its
  producers.
- **Resolution**: —

### AR-033 — WiFiManager AP-provisioning status no longer drawn directly to the display (deliberate, from the single-writer SPI rule)
- **Area**: firmware/display, firmware/network
- **Status**: open — behavior change worth the owner's awareness, not a
  bug
- **Found**: 2026-09-11, implementing real NetworkTask WiFi provisioning.
  The old pre-rewrite code drew AP-portal status directly to the ST7735
  from WiFiManager callbacks running in Network's context.
- **What**: under the new architecture DisplayTask exclusively owns the
  SPI bus (single-writer rule, closes the original audit's shared-resource
  findings) — so NetworkTask can no longer draw to the screen itself
  during WiFi setup and currently only logs AP-portal status instead.
- **Why it matters**: during initial WiFi provisioning (captive portal
  active), the display won't show "connect to Eureka setup" or similar —
  arguably the single moment this information is most useful, since
  there's no other UI to convey it. Proper fix is a new `DisplayMode` /
  `DisplayCommand` that NetworkTask sends to DisplayTask to render instead
  of drawing directly — small, contained addition, just not done yet.
- **Resolution**: —

### AR-034 — Flash usage jumped from 21.7% to 89.7% across the four parallel task implementations, before the SPA/LittleFS work has even landed
- **Area**: firmware/build, firmware/network
- **Status**: open — needs-owner-awareness, not yet a hard blocker
- **Found**: 2026-09-11, `pio run -e esp_wroom_02` after merging
  SettingsTask/DisplayTask/NetworkTask/TelemetryTask's real
  implementations (RAM 10.0%, flash 89.7% of the ESP32-WROOM's 1.25MB
  app partition). NetworkTask's own build alone measured 74.7% before the
  other three were merged in — WiFiManager + ESPAsyncWebServer + AsyncTCP
  + ArduinoOTA + HTTPClient/WiFiClientSecure account for most of the jump.
- **Why it matters**: the web SPA (D4) still needs to be built and served
  from a LittleFS partition — that's a separate filesystem partition, not
  app flash, so it doesn't directly compete with this number, but the
  *app* partition itself now has only ~10% headroom left for the SPA's own
  serving code, any future task growth, or partition-table adjustments
  (e.g. OTA needs two app partitions to swap between — worth double-
  checking `partitions.csv` still has room for a second 1.25MB-class OTA
  slot at this size). Not urgent, but the next piece of work that touches
  partitioning or adds a library dependency should check `pio run`'s
  size report before assuming there's slack.
- **Resolution**: —
