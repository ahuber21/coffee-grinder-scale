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
- **Status**: fixed
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
  target given how few sessions a home device runs per day. It also
  meant AR-052's cold-start deadlock recurred on literally every boot,
  with no way for real pulse data to ever accumulate past it.
- **Resolution**: `loadTopupModelFromNvs`/`saveTopupModelToNvs` now do a
  real round-trip -- the whole `TopupModelV1` POD struct as one raw-bytes
  NVS blob (`Preferences::putBytes`/`getBytes`), guarded by the struct's
  own `version` field (mismatch or wrong byte count falls back to
  cold-start priors, same discipline as `SettingsSnapshot`'s
  `schema_ver`) and by the existing `isPlausibleTopupModel` bounds
  check. Saved once per completed session, right where `DosingTask`
  already builds the merged blob for the persist-request queue.

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
- **Status**: open — deferred. Owner has seen this (2026-09-11) and
  confirmed it's a "sad side-effect," not urgent; leave for a later pass,
  don't pick this up proactively.
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

### AR-034 — Flash usage jumped from 21.7% to 93.2% across the four parallel task implementations plus LittleFS; ~86KB headroom left in the app partition
- **Area**: firmware/build, firmware/network
- **Status**: open — needs-owner-awareness, not a blocker (confirmed OTA
  still functions). Update: enabling `board_build.filesystem = littlefs`
  for the SPA (D19) added another ~47KB on top of the 89.7% figure below,
  bringing it to **93.2% (1,222,181 / 1,310,720 bytes)** — **88,539 bytes
  (6.8%) headroom** left in the currently-inactive OTA slot. Still
  functional, but the margin is now thin enough that the next feature
  added to the app binary (not the SPA's own assets, which live in the
  separate LittleFS partition and don't count against this number) should
  budget carefully, and `min_spiffs.csv` (see below) is worth actually
  planning to switch to rather than treating as a hypothetical lever.
- **Found**: 2026-09-11, `pio run -e esp_wroom_02` after merging
  SettingsTask/DisplayTask/NetworkTask/TelemetryTask's real
  implementations (RAM 10.0%, flash 1,175,133 / 1,310,720 bytes = 89.7%).
  Confirmed no partition-table change happened (`platformio.ini`'s
  `board_build.partitions` override is still commented out, so the
  framework default `default.csv` is in effect: two 0x140000-byte
  (1,310,720B) OTA app slots `app0`/`app1`, plus an untouched separate
  0x160000-byte `spiffs` partition reserved for the future SPA/LittleFS
  image). **OTA is structurally unaffected** — the two-slot scheme is
  intact — but the currently-inactive slot only has **135,587 bytes
  (10.3%) headroom** left for a new image to be written into via OTA.
  Ranked by static library size (`xtensa-esp32-elf-size` on each
  `lib*/*.a`): ESPAsyncWebServer 80.8KB, WiFiManager 80.4KB, WiFi (core)
  44.3KB, legacy WebServer (pulled in by WiFiManager's captive portal)
  35.2KB, HTTPClient 19.7KB, WiFiClientSecure/mbedTLS 12.2KB, AsyncTCP
  11.6KB, ArduinoOTA 8.7KB, ESPmDNS 5.7KB — roughly 300KB total for the
  networking stack alone, plus Adafruit GFX (21.7KB) linking in for the
  first time now that DisplayTask actually calls it (it was already a
  declared dependency, just previously unused/garbage-collected by the
  linker).
- **Why it matters**: none of this is waste — it's the real cost of the
  features NetworkTask/TelemetryTask exist to provide — but ~130KB is a
  thin margin. The web SPA's own *assets* go to the separate spiffs
  partition and don't compete with this number, but any serving glue code
  added to the app binary itself, or any future `lib_dep` addition (or
  task growth), should check `pio run`'s size report against this budget
  before assuming there's slack. If it's ever exceeded, `min_spiffs.csv`
  (already present as a commented-out option in `platformio.ini`) trades
  spiffs/LittleFS space for a larger app partition — worth knowing that
  lever exists, not necessarily worth pulling yet.
- **Resolution**: —

### AR-035 — SPA (`webapp/`) visually verified in a browser (dev server); still never checked against a live ESP32
- **Area**: web, firmware/network
- **Status**: partially resolved. Update 2026-09-11 (same day, later):
  visually inspected all three tabs (Live/Settings/History) against
  `npm run dev` in a real Chrome tab (Claude-in-Chrome extension
  reconnected after a Chrome restart) at both desktop and 400px-phone
  widths. Confirmed: no console errors on load or navigation, hash-based
  tab switching works without a full reload, the WS status dot correctly
  shows disconnected/red with no device present, the "Grind" button is
  correctly disabled while disconnected, the armed-confirm WiFi
  reset/reboot pattern works (arms independently per-button, cancels
  correctly), and — genuinely useful — the History tab's direct
  browser-to-PostgREST fetch (D9) actually round-tripped live against
  `192.168.0.111:3000` from the browser with zero CORS errors, correctly
  rendering "No sessions recorded yet." for the still-empty `v2` tables.
  Found and fixed two real phone-width layout bugs in the process:
  `.setting-row` didn't wrap (label/value/input/button all fought for one
  line at 400px) and both `<table>`s in History.tsx had no horizontal
  scroll container despite `white-space: nowrap` cells (would have
  overflowed the page once real session rows exist, since none did at
  verification time to visually trigger it) — both fixed in
  `webapp/src/index.css`/`History.tsx`.

  Update 2026-09-11 (later still): the SPA's LittleFS image was
  uploaded to the physical device. `GET /` on the real device now
  returns the actual built `index.html`, and its referenced JS/CSS
  assets both serve with correct size/content-type -- the SPA is
  genuinely reachable end-to-end on real hardware, not just in
  `npm run dev`. Still open: nobody has opened that page in a browser
  pointed at the live device and exercised the actual `/ws` contract
  during a real session (message shapes are only checked by hand
  against NetworkTask.cpp's source, never watched live) -- doing that
  safely needs a real grind to generate telemetry, which is exactly the
  kind of live-hardware action this project treats carefully.
- **Why it matters**: a clean type-check doesn't catch a WebSocket
  message the frontend mis-parses at runtime despite matching types on
  paper. The layout/CORS/console-error/static-serving class of risk
  this AR originally flagged is now covered; the live `/ws` telemetry
  path specifically is not, and won't be until a real dosing session
  is watched through the SPA.
- **Resolution**: partially — see Status. Fully closes once this firmware
  runs on the real device and the SPA is exercised against it.

### AR-036 — A "read-only" API smoke test against the live device actually triggered a real grind
- **Area**: process, firmware/network
- **Status**: confirmed, lesson learned, not a code bug
- **Found**: 2026-09-11, verifying the freshly OTA-deployed firmware.
  `curl "http://192.168.0.118/api/getDosage?grams=18"` was run assuming
  it was a harmless read-only check of the new HTTP route -- it isn't:
  the endpoint enqueues a real `DoseRequest` to Dosing task, identical
  to pressing a physical button. The device accepted it and the grinder
  started running.
- **Why it matters**: this is exactly the kind of action the project's
  own safety rules exist to prevent, and it happened by not thinking
  through a "test" request's real side effect before sending it to
  live hardware. No harm resulted this time (motor power was
  independently switched off shortly after by the owner as a backup
  safety measure), but it's worth recording as a concrete example: any
  request sent to the physical device that can reach Dosing task's
  queues must be treated as a real action, not a diagnostic, regardless
  of what it's named.
- **Resolution**: no code change; verification going forward uses
  genuinely passive checks (a WS connection's automatic settings
  broadcast, GET requests with no side-effecting handler) instead of
  assuming an endpoint is safe from its name/shape.

### AR-037 — `calibration_factor`'s own compiled-in default silently zeroed the scale on first boot
- **Area**: firmware/settings, firmware/scale
- **Status**: fixed
- **Found**: 2026-09-11, immediately after the first OTA deploy to the
  physical device: the scale read exactly 0.0g regardless of load.
  Root cause: `SettingsSnapshot::calibration_factor` defaulted to
  `0.0f`, and `ScaleTask` forwards this straight into
  `ADS1232::setCalFactor` (`units = raw * calFactor`) -- multiplying
  every real reading by zero. The validator meant to police this field
  already treated `0.0f` as invalid, but the schema's own default was
  that same invalid value.
- **Why it matters**: any device's first boot on this firmware (a fresh
  NVS partition, or later a factory reset) would silently zero the
  scale rather than failing visibly or falling back to something
  merely uncalibrated.
- **Resolution**: default changed to `1.0f`, matching the ADS1232
  driver's own internal default -- a fresh/uncalibrated scale now reads
  raw counts (visibly wrong, but a real number) instead of exactly zero.

### AR-038 — First legacy-settings migration omitted gain/speed/read_samples, silently breaking the migrated calibration factor
- **Area**: firmware/settings, firmware/scale
- **Status**: fixed
- **Found**: 2026-09-11, right after AR-037's fix was deployed: the
  scale now updated with weight changes, but a real 126.6g weight
  change read back as only about -1.0g. A one-time migration path was
  added to fold the pre-rewrite firmware's still-present EEPROM-emulated
  settings blob into the new NVS schema on first boot (that data
  survives OTA/serial reflashing, since neither touches NVS). The first
  version of that migration copied `calibration_factor` across but not
  `gain`/`speed`/`read_samples` -- and `calibration_factor` is only
  meaningful relative to the ADC gain it was measured under. The
  device's real hardware config (gain 128) didn't match the new
  firmware's compiled default (gain 1), a ~128x mismatch that lines up
  almost exactly with the observed ~127x reading error.
- **Why it matters**: a partial migration can be worse than none --
  it looked successful (real, non-default, non-zero values came back)
  while actually producing a badly wrong scale reading.
- **Resolution**: fixed in the same migration pass (added gain/speed/
  read_samples), and the schema version was bumped once more to force
  a clean re-migration on the next boot after the fix shipped. The
  migration code itself has since been removed as a completed one-off
  (see DECISIONS.md) -- 128/10/12 are now the compiled-in defaults
  directly, reflecting this device's actual, confirmed hardware
  configuration.

### AR-039 — CONFIRM had no timeout, could get permanently stuck, and that also permanently blocked OTA with no remote recovery
- **Area**: firmware/dosing, firmware/network
- **Status**: fixed
- **Found**: 2026-09-11, live on the device: the display showed "0.0
  OK?" (the CONFIRM screen) and stayed there through repeated button
  presses. `SettingsSnapshot.confirm_timeout_ms` already existed as a
  field for exactly this purpose but was never wired to anything --
  CONFIRM only ever left via a matching/non-matching button press, with
  no fallback. Independently confirmed by a second symptom: the next
  OTA upload failed ("No response from the ESP") because
  `otaSafeToStart()` treats every non-IDLE/SCREENSAVER/BOOT state,
  including CONFIRM, as an active session and refuses to even pump
  ArduinoOTA's protocol handling -- so a stuck CONFIRM also permanently
  blocked OTA, and there was no WS/HTTP command that could cancel it
  remotely. Recovered this time via a physical power-cycle.
- **Why it matters**: a device that can get permanently stuck with no
  remote recovery path is a real operational hazard, independent of
  whatever originally causes a given state to stop advancing -- the
  guard rail matters even without knowing that root cause. (Root cause
  of *this specific instance* -- why button presses stopped registering
  in the first place -- is still open; diagnostic logging was added
  in the same pass to help pin it down, see below.)
- **Resolution**: two fixes. (1) `confirm_timeout_ms` now actually wired
  up -- CONFIRM lapses back to IDLE on its own if nothing confirms or
  cancels it in time, same as FINALIZE/GRINDING/TOPUP already did with
  their own timeouts. (2) A general 60s backstop added after the state
  machine's per-state handling: any non-idle state that persists longer
  than that is forced back to IDLE (relay off) regardless of cause --
  a last-resort net for whatever isn't covered by a purpose-built
  timeout (e.g. TARE waiting on a scale sample that never arrives), so
  no future gap in this reasoning can strand the device or block OTA
  for more than a bounded time again.

### AR-040 — Button/state diagnostic logging added over the existing WS "log" channel
- **Area**: firmware/dosing, firmware/input
- **Status**: fixed — logging kept as permanent instrumentation
- **Found**: 2026-09-11, needed to diagnose AR-039's underlying "why
  did button presses stop registering" question without a serial
  cable. `TelemetryType::LOG_LINE` -> the WS `"log"` envelope -> the
  SPA's Live page already existed as plumbing but nothing emitted
  LOG_LINE events for button/state activity. Added: InputTask logs
  every debounce decision (button id, pin, whether it read HIGH at
  verification time), independent of whether a press is actually sent
  on; DosingTask logs every ButtonPress it receives (with the state it
  arrived in) and every state transition.
- **Why it matters**: this turns "buttons do nothing" from a guess
  into an observable fact -- whether the ISR/debounce layer ever sees a
  press, whether Dosing task receives it, and what it decides to do
  with it, all become visible live over `/ws` without touching the
  device physically.
- **Resolution**: the logging directly surfaced AR-041's root cause
  (asymmetric button wiring). Since it only fires on actual button/state
  activity, not on a timer, it stays in as permanent low-volume
  instrumentation rather than being pulled back out.

### AR-041 — Button polarity assumed uniform across all three buttons
- **Area**: firmware/input
- **Status**: fixed
- **Found**: 2026-09-11, live: pressing LEFT/RIGHT/BACK produced no
  state change. AR-040's debounce logging showed presses never
  reaching the "accepted" branch. Reading `git show main:src/main.cpp`
  showed the real hardware wiring: LEFT/RIGHT are `INPUT_PULLUP` +
  `FALLING` (active-low), BACK is `INPUT_PULLDOWN` + `RISING`
  (active-high) -- asymmetric by hardware design, not a uniform
  active-HIGH scheme.
- **Why it matters**: the rewrite's `InputTask` used one `INPUT` +
  confirm-on-HIGH path for all three buttons, so LEFT/RIGHT (which
  idle HIGH and pull LOW when pressed) never registered a press at
  all.
- **Resolution**: `pinMode` set per-button to match the real wiring
  (`INPUT_PULLUP` for LEFT/RIGHT, `INPUT_PULLDOWN` for BACK), and a
  `kActiveLevel[3]` lookup table replaces the single hardcoded
  "confirm HIGH" check in both the ISR and the debounce verification.

### AR-042 — Load-cell bridge excitation (ADC_LDO_EN_PIN) never powered
- **Area**: firmware/scale
- **Status**: fixed
- **Found**: 2026-09-11, live: the scale reading never changed under
  physical force, even after AR-041's button fix let a grind start.
  `ADC_LDO_EN_PIN` was defined in `defines.h` but never referenced
  anywhere in the rewrite's `ScaleTask`.
- **Why it matters**: without powering the load cell's bridge
  excitation, the ADS1232 reads a fixed value regardless of applied
  force -- the scale is completely unresponsive, not just noisy or
  miscalibrated.
- **Resolution**: `pinMode(ADC_LDO_EN_PIN, OUTPUT); digitalWrite(ADC_LDO_EN_PIN, HIGH);`
  added before `g_ads.begin()` in `ScaleTask::scaleTaskFn`.

### AR-043 — Gain/speed/read_samples settings clobbered by ADS1232::begin()
- **Area**: firmware/scale
- **Status**: fixed
- **Found**: 2026-09-11, live: even after AR-042, the scale's gain
  was suspected still at its power-on default of 1 rather than the
  configured 128.
- **Why it matters**: `ADS1232::begin()` hardcodes the gain pins to
  their power-on default (gain=1) as part of powering on and
  calibrating the chip. `ScaleTask` was calling
  `applySettingsIfChanged()` (which applies gain/speed/read_samples)
  *before* `begin()`, so every real setting was immediately
  overwritten back to the hardware default the moment `begin()` ran.
- **Resolution**: reordered `ScaleTask::scaleTaskFn`'s init sequence to
  `begin() -> applySettingsIfChanged() -> initRingBuffer() -> tare()`
  -- settings are applied strictly after `begin()`'s reset, and before
  `initRingBuffer()`, which reads the now-correct `read_samples` value.

### AR-044 — DosingTask and NetworkTask raced on the display mailbox during OTA
- **Area**: firmware/dosing, firmware/network, firmware/display
- **Status**: fixed
- **Found**: 2026-09-11, live: the display flickered rapidly through
  multiple screens during an OTA flash.
- **Why it matters**: `DosingTask` asserts its own display mode on
  essentially every loop iteration (every ~5ms), while `NetworkTask`
  asserts `OTA_UPDATE` sporadically as flash progress updates arrive.
  Both write the same overwrite-mailbox, so whichever task wrote most
  recently wins -- the two modes flip back and forth, each transition
  forcing a full-screen clear in `DisplayTask`.
- **Resolution**: `DosingTask` now skips its own `sendDisplayCommand()`
  call while `kOtaInProgressBit` is set, ceding the display mailbox to
  `NetworkTask` for the duration of the flash.

### AR-045 — Weight overflow past the one-decimal layout showed a "MAX" placeholder
- **Area**: firmware/display
- **Status**: fixed
- **Found**: 2026-09-11, owner feedback: didn't want the actual value
  hidden behind a placeholder when the reading gets too wide to fit
  the normal one-decimal layout.
- **Resolution**: past the same width threshold that used to trigger
  "MAX", `DisplayTask` now renders the full integer value
  (`snprintf("%.0f", ...)`) instead.

### AR-046 — Scale never auto-tared while idle
- **Area**: firmware/scale
- **Status**: fixed
- **Found**: 2026-09-11, owner feedback: a scale left sitting idle
  should self-correct small drift/residue instead of requiring a
  manual tare every time, and with no magnitude threshold gating it --
  any idle+stable reading should be re-tared.
- **Resolution**: `ScaleTask` now calls `g_ads.tare()` whenever
  `kDosingActiveBit` is clear and the current sample is `stable`,
  rate-limited to once per second purely to avoid calling `tare()` on
  every single sample.

### AR-047 — GRINDING/TOPUP stop conditions compared absolute weight against target, not weight-since-tare
- **Area**: firmware/dosing
- **Status**: fixed
- **Found**: 2026-09-11, live: starting a dose from a scale reading
  far from zero (e.g. right after removing something from the scale,
  before AR-046's auto-tare had a chance to catch up) caused the
  session to jump straight to FINALIZE with no grinding or topup
  activity.
- **Why it matters**: `handleGrindingSample`'s raw-weight fallback and
  `handleTopupSample`'s gap calculation both compared the scale's
  current *absolute* reading against `target_grams`/
  `target_grams_corrected`, which are only meaningful relative to the
  weight recorded at grind start (`g_grams_on_grind_start`). Whenever
  that baseline isn't near zero, the computed gap is wildly wrong (in
  this case implausibly large), the topup model correctly refuses to
  fire on it, and the existing "accept the undershoot" fallback path
  sends the session straight to FINALIZE.
- **Resolution**: both comparisons now use the weight delta since
  `g_grams_on_grind_start`, matching the delta-based math
  `handleGrindingSample`'s time-estimate check already used.

### AR-048 — Settings write path and broadcast covered only 6 of ~23 SettingsSnapshot fields
- **Area**: firmware/settings, firmware/network, web
- **Status**: fixed
- **Found**: 2026-09-11, owner feedback: gain/speed/read_samples and
  most timeout/topup-model settings, all previously adjustable, were
  missing from the SPA's Settings page entirely.
- **Why it matters**: `SettingsTask.cpp` already had a validator for
  every `SettingsSnapshot` field, but `SettingsFieldId`,
  `settingsFieldFromName`, and `applyWrite` only covered
  calibration/dose/margin/debounce/wifi -- the rest were unreachable
  from any client no matter what the SPA sent, and `buildSettingsJson`
  didn't broadcast them either, so they weren't even visible.
- **Resolution**: extended `SettingsFieldId`, `settingsFieldFromName`,
  `applyWrite`, and `buildSettingsJson` to cover every field except the
  two write-only WiFi action flags. The SPA's Settings page now has a
  Basic section plus a "Show advanced settings" section (gain/speed as
  hardware-constrained dropdowns, the ring-buffer window, and every
  timeout/topup-model tunable).

### AR-049 — A mid-grind sensor glitch could corrupt the online rate model and trigger an immediate false "done"
- **Area**: firmware/dosing, lib/DosingModel
- **Status**: fixed
- **Found**: 2026-09-11, live: lifting the cup off the scale mid-grind
  (weight briefly reads far below the grind-start baseline) caused the
  session to jump straight to FINALIZE with no further grinding --
  distinct from AR-047, which was about the *pre-dose* baseline, not a
  glitch arriving after GRINDING had already started correctly.
- **Why it matters**: `MainGrindModel::addSample` folded every raw
  sample into its online regression with no plausibility check at all
  (unlike `TopupModel::recordPulse`, which already has hard-bound and
  statistical-consistency rejection for exactly this class of glitch).
  A single wild outlier could corrupt the fitted slope, and
  `predictStopTimeMs` compounded it: a non-positive rate estimate was
  treated as "stop right now" instead of "this estimate is
  untrustworthy" -- the least safe possible interpretation of bad data.
- **Resolution**: `addSample` now rejects (holds the last known-good
  sample instead of folding in) any sample implying more than a 1g drop
  since the last accepted one -- real grind weight only increases, mod
  noise. `predictStopTimeMs` now returns "no prediction" (never fires)
  rather than "stop now" when the rate is non-positive. Two native
  tests added (`test_main_grind_model_ignores_sudden_weight_drop`,
  `test_main_grind_model_nonpositive_rate_never_predicts_immediate_stop`).

### AR-050 — Dynamic ADC speed switching (attempted, reverted)
- **Area**: firmware/scale
- **Status**: wontfix -- reverted per owner direction after repeated live failures
- **Found**: 2026-09-11, owner request: escalate the ADC to 80 SPS while
  the reading is actively changing (manual weight changes or an active
  grind), fall back to 10 SPS once settled, for a snappier display
  without sacrificing at-rest precision.
- **What happened**: three attempts, each defeated by the same
  underlying issue -- `ADS1232::getRaw()`'s own "changing" flag (a fixed
  0.02%-of-mean per-sample deviation threshold) is not a reliable
  movement signal once something is actually *acting* on it in real
  time. (1) Using the driver's flag directly to decide when to fall
  back to 10 SPS failed because 80 SPS's higher per-sample noise trips
  it almost continuously. (2) Trusting the driver's flag to *escalate*
  at 10 SPS still oscillated, because ordinary noise apparently trips
  it at 10 SPS too, just less often -- harmless when the only consumer
  was auto-tare (a skipped cycle is invisible), not when it drives a
  full speed switch. (3) A fully external, driver-independent detector
  (EWMA-smoothed grams, rolling-range tolerance, dwell-time hysteresis)
  finally stopped the oscillation, but the owner judged the live result
  still not acceptable ("the transitions should not happen") and asked
  to drop the feature rather than continue iterating.
- **Why it matters**: this is a standing finding about the ADS1232
  driver's stability heuristic itself, not just about this feature --
  it should not be assumed reliable as an automatic decision trigger at
  any single sample rate without an external corroborating signal and
  real hysteresis. A future attempt at this feature (or anything else
  reacting to `stable`/`isChanging` in real time) should start from
  that premise rather than re-discovering it.
- **Resolution**: `ScaleTask` reverted to static, settings-controlled
  ADC speed (`applySettingsIfChanged()` calls `setSpeed()` directly
  again, as before this investigation). No dynamic switching in the
  current firmware.

### AR-051 — DisplayTask's boot splash bypassed the main render loop's mode tracking
- **Area**: firmware/display
- **Status**: fixed
- **Found**: 2026-09-11, owner feedback: the boot screen interfered
  with display updates.
- **Why it matters**: `displayTaskFn()` drew the "Eureka" splash via a
  direct call to `drawCentered()` *before* entering the main loop and
  before `last`/`haveLast` existed -- a side channel the loop's
  mode-change detection (`cmd.mode != last.mode`) had no knowledge of,
  duplicating logic `renderMode()`'s own `BOOT` case already covers.
- **Resolution**: the boot splash is now seeded as the loop's initial
  `last` state (`last.mode = DisplayMode::BOOT`) and rendered once
  through the exact same `renderMode()` dispatch every other screen
  uses, removing the special-cased direct draw and the `haveLast` flag
  it existed for.

### AR-052 — TOPUP can structurally never fire a pulse at the cold-start prior
- **Area**: lib/DosingModel, firmware/dosing
- **Status**: fixed
- **Found**: 2026-09-12, live: a session stopped GRINDING at 17.0g
  (correctly, target_grams_corrected for an 18g double dose with
  top_up_margin_double=1.0), then sat in TOPUP with zero pulses fired
  before the safety cutoff forced FINALIZE. Reproduced and confirmed
  with a standalone probe of `TopupModel::computeTopupDecision` against
  a fresh `makeDefaultTopupModel()`: `should_fire` is `false` for every
  gap tried from 0.2g to 3.0g.
- **Why it matters**: `computeTopupDecision`'s overshoot-safety shrink
  (`safe_weight = overshoot_budget_g - overshoot_k_sigma * residual_sd`)
  is gap-independent -- once `overshoot_k_sigma * residual_sd` alone
  reaches `overshoot_budget_g`, `safe_weight <= 0` and every gap clamps
  to "don't fire," permanently, regardless of how large the gap is.
  With the documented cold-start prior (`residual_sd = 0.15`,
  `overshoot_k_sigma = 2.0`) and the value `DosingTask.cpp` actually
  passes as the budget (`0.3`), `2 * 0.15` exactly equals `0.3` --
  zero margin, and the comparison is strict (`>`), so any positive
  pulse fails it. `TopupModelV1` NVS persistence is also still stubbed
  (older finding, see STATUS.md's AR-028 reference), so every boot
  restarts at exactly this prior -- the model can never accumulate the
  real pulse data (`recordPulse` only runs after a pulse fires) that
  might eventually shift `residual_sd`.
- **Partially fixed**: `TopupModel::seedFromPersisted`'s 4-pseudo-
  observation reconstruction was itself inflating the reconstructed
  `residualStdDev()` by ~6% above the documented prior (0.159 measured
  vs. 0.15 intended) -- `residualVariance()`'s `n-2` degrees-of-freedom
  correction was never accounted for when choosing the synthetic
  spread. Fixed by scaling the spread by `sqrt((n0-2)/n0)` so the
  reconstruction reproduces the persisted value exactly. Genuine
  correctness fix, kept regardless of the item below -- but insufficient
  alone, since even the *correct* 0.15 leaves exactly zero margin against
  the 0.3g budget at k_sigma=2.0.
- **Resolution**: owner chose to lower `overshoot_k_sigma` (2.0 -> 1.5,
  ~95% -> ~93% one-sided confidence per pulse) rather than raise the
  0.3g overshoot budget or special-case the zero-margin boundary --
  `2*0.15=0.3` was consuming the entire budget with the pulse's own
  target weight getting none of it; `1.5*0.15=0.225` leaves 0.075g of
  real margin. Accepted on the reasoning that the topup loop
  re-evaluates and re-fires every cycle, so a single pulse doesn't need
  to carry the whole overshoot guarantee alone. New native test
  `test_decision_fires_at_cold_start_with_production_budget` locks in
  the exact production scenario (`computeTopupDecision(1.0, 0.3)` on a
  fresh model) that every prior test's choice of budget/gap had missed.
  Also: `TopupModelV1` NVS persistence (AR-028) now actually lands, so
  once real pulse data accumulates, `residual_sd` is no longer frozen
  at this exact boundary forever. And: the owner asked for the model's
  live parameters to be visible with an explanation of the formulas
  ("I'm not following what happens and what the parameters do") -- see
  the new `TelemetryType::MODEL_STATE` event (sent at boot and after
  every completed session, replayed to newly-connecting WS clients same
  as `settings`) and the SPA's new Model tab
  (`webapp/src/pages/Model.tsx`), which shows the fitted rate/coast/
  topup values alongside the actual stop-time and topup-decision
  formulas with today's numbers substituted in.

### AR-053 — FINALIZE's elapsed-time readout kept counting instead of freezing
- **Area**: firmware/dosing, firmware/display
- **Status**: fixed
- **Found**: 2026-09-12, owner feedback: after a dose finished, the
  displayed timer kept running instead of stopping, while the weight
  readout correctly kept updating live.
- **Resolution**: `transitionTo()` now captures elapsed time into
  `g_frozen_elapsed_ms` the instant FINALIZE is entered;
  `sendDisplayCommand()` reports that frozen value for `elapsed_s`
  while in FINALIZE instead of the live `millis() - grinder_started_ms`
  calculation every other state uses. The weight readout is unaffected.

### AR-054 — Auto-tare fired almost immediately on returning to IDLE after a dose
- **Area**: firmware/scale
- **Status**: fixed
- **Found**: 2026-09-12, owner feedback: wanted time to see the final
  weight before it gets auto-zeroed.
- **Resolution**: `ScaleTask` now detects the falling edge of
  `kDosingActiveBit` (dosing active -> idle) and applies a 10s cooldown
  before auto-tare resumes, instead of the normal 1s inter-tare rate
  limit applying immediately.

### AR-055 — SCREENSAVER was fully implemented but never actually reachable
- **Area**: firmware/dosing, firmware/display
- **Status**: fixed
- **Found**: 2026-09-12, codebase review: `screensaver_timeout_s` had a
  real validator and was exposed in the Settings UI, and `DisplayTask`
  had a complete SCREENSAVER render path, but nothing in `DosingTask`
  ever actually transitioned `g_state` to `SCREENSAVER` -- there was no
  idle timer anywhere. The setting did nothing.
- **Why it matters**: directly relevant to the owner's TFT-burn-in
  concern (screensaver was explicitly one of the two proposed
  mitigations, "blank now, HA later").
- **Resolution**: `DosingTask` now tracks `g_last_activity_ms` (reset on
  every button press and on entering IDLE) and transitions IDLE ->
  SCREENSAVER once `screensaver_timeout_s` has elapsed with none; any
  button press from SCREENSAVER dismisses it back to IDLE. Per the
  owner's earlier decision, SCREENSAVER blanks the panel (backlight off
  via the existing PWM channel) rather than showing an idle clock --
  an always-on clock would just relocate the static-content problem,
  not solve it -- so the previously-implemented digit-clock render path
  (`drawScreensaver`) was removed rather than kept as unreachable
  weight. The HA-integration half of that decision (tie backlight to
  the actual coffee machine's power state) remains a separate, not yet
  started follow-up.

### AR-056 — Owner correction: MainGrindModel/CoastModel shouldn't decay with recency
- **Area**: lib/DosingModel
- **Status**: fixed
- **Found**: 2026-09-12, owner feedback on the Model tab's explanation:
  "No need to have older sessions count less. The grinder is really
  really stable over time. It's 7 years old at this point and still
  performs as on day one." `MainGrindModel`/`CoastModel` both defaulted
  to a 45-day recency half-life; `TopupModel` already had decay
  disabled (`half_life_days = 0.0`) on the same reasoning ("no
  measurable drift... over 17 months"), just never extended to the
  other two models.
- **Resolution**: both now default to `half_life_days = 0.0` (disabled),
  matching `TopupModel`. Required also adding the `<= 0.0` disable guard
  to `MainGrindModel::finalizeSession` and `CoastModel::recordCoast`'s
  decay application (previously only `TopupModel::applyDecay` had it;
  the other two would have divided by zero / produced NaN with a literal
  0.0 half-life otherwise). The decay mechanism itself is untouched and
  still exercised by its own tests via an explicit non-default `Config`
  -- only the default changed.

### AR-057 — No hard floor for the physical relay's minimum actuation time
- **Area**: lib/DosingModel
- **Status**: fixed
- **Found**: 2026-09-12, owner feedback: "pulse_duration, does it
  consider the fact that it's a physical clicky re[lay], not an SSD
  one? It's minimum on duration is about 0.3s, everything below will
  just stall the motor and nothing will happen." `TopupModel::Config`'s
  `hygiene_min_duration_ms` (meant to reject implausibly short pulses)
  defaulted to `0.0` -- a no-op -- relying entirely on the *learned*
  `topup_deadtime_ms` (persisted default ~310ms) happening to already
  sit above the real stall threshold, with no explicit, hardware-derived
  floor of its own. `TopupModel::isPlausible()` has no lower bound on
  deadtime either, so nothing would have caught it drifting below 300ms
  over time.
- **Why it matters**: a commanded pulse below the real actuation floor
  doesn't produce a smaller dose -- it produces *zero* output while the
  model believes a real pulse happened, silently wasting a decision
  cycle at best.
- **Resolution**: `hygiene_min_duration_ms` now defaults to `350.0`
  (a real margin above the owner's stated ~300ms floor), independent of
  whatever `deadtimeMs()` is currently fitted to. Verified the existing
  fitted values (~310ms deadtime, current 0.075g cold-start example
  pulse -> ~381ms) still clear it; the SPA's Model tab now explains and
  checks this floor in its worked example too.

### AR-058 — SCREENSAVER only woke on a button press, not on physical presence
- **Area**: firmware/dosing, firmware/settings, web
- **Status**: fixed
- **Found**: 2026-09-12, owner request: a large weight change while
  SCREENSAVER is active (placing/removing a cup, dosing manually)
  should wake the display, not just an explicit button press.
- **Resolution**: new setting `screensaver_wake_weight_delta_g`
  (default 2.0g, validated/exposed the same as every other field since
  AR-048), plumbed end to end. `DosingTask` captures the scale reading
  at SCREENSAVER entry (`g_screensaver_baseline_grams`) and, on every
  sample while SCREENSAVER is active, wakes to IDLE if the reading has
  moved past that baseline by more than the configured delta -- the
  same per-sample drain loop that already special-cases GRINDING/TOPUP.

### AR-059 — TOPUP's fitted-line model doesn't hold at short pulse durations; replaced with a self-tuning LUT
- **Area**: lib/DosingModel, firmware/dosing, firmware/settings, web
- **Status**: fixed -- a real architecture reversal, see DECISIONS.md D23
- **Found**: 2026-09-12, live: after AR-052/AR-057's fixes let topup
  pulses fire at all, a 4g manual dose stopped GRINDING around 3g (as
  designed), then fired ~10 tiny pulses that "sneaked up" on 3.7g before
  accepting that as done -- a full 0.3g left unclosed. Owner's own
  words: "most of the pulses did nothing, some had a clump that added
  0.05 to 0.15g... nothing intentional about this behavior... it's just
  randomly adding a bunch of clumps until we are at delta 0.3."
  Pulling the live model state confirmed it precisely: real measured
  pulse noise had grown to 0.23g (`topup_residual_sd_g`), at which point
  `1.5 * 0.23 = 0.345g` already exceeds the entire 0.3g overshoot
  budget on its own -- the exact AR-052 deadlock, recurring naturally
  once enough real (noisy) pulse data had actually been folded in.
- **Why it matters**: the fitted-line model (`weight_added = slope *
  (t - deadtime)`) assumes short-pulse output scales smoothly and
  predictably with duration. Real short pulses instead behave like a
  discrete, clumpy release process (grounds either dislodge or don't) --
  a physically different regime than the *main* grind, which runs long
  enough to average over many such events into the smooth, low-noise
  rate the model correctly captures there (0.09 g/s sd, vs. topup's
  0.23g). No amount of re-tuning `overshoot_k_sigma`/`aim_fraction`
  fixes a wrong shape of model.
- **Resolution**: replaced `TopupModel`'s single fitted line with a
  10-bucket (0.1g-wide, 0.0-1.0g) self-tuning lookup table of pulse
  durations -- closer to the pre-rewrite firmware's hand-tuned 6-bucket
  table (recovered from `git show main:src/main.cpp`), but updated from
  real pulses instead of by hand. Each bucket aims for 85% of its own
  upper bound (leaving slack for a smaller-bucket pulse to close the
  remainder) and is nudged by an online rule that corrects overshoot
  faster (0.6) than it grows duration for undershoot (0.3), matching
  the project's standing overshoot-over-undershoot priority. No
  `min_controllable_gap_g` cutoff anymore -- per the owner's request,
  even a 0.1g gap gets a real attempt, gated only by the pre-existing
  `min_topup_grams` "basically zero" setting. `TopupModelV1` bumped to
  schema v3 (`topup_slope`/`topup_deadtime_ms`/`topup_precision`/
  `topup_n_effective` replaced by `topup_lut_duration_ms[10]`/
  `topup_lut_n[10]`) -- **this invalidates any old-version NVS blob
  wholesale, not just the topup fields**, so the accumulated
  `MainGrindModel`/`CoastModel` state (rate_n_effective had reached
  1407 real samples) reset to cold-start priors on this deploy too. The
  point estimates themselves (rate_hat=1.0 g/s) had already converged to
  match the compiled-in prior almost exactly, so this is a confidence
  reset, not a wrong-value regression -- but a real, disclosed side
  effect of the version bump, not a free one. `TopupModel::PulseResult`
  no longer needs `predicted_g`/`residual_g`/`stat_rejected`/`fallback`
  (no per-pulse prediction to check consistency against anymore); the
  hard absolute-bounds rejection (garbage sensor values) is kept as-is.
  The SPA's Model tab now renders the live LUT as a table (bucket,
  duration, aim weight, pulses seen) instead of slope/deadtime/noise
  scalars. See ARS.md AR-052 (superseded for the topup-specific parts)
  and DECISIONS.md D23.

### AR-060 — Ring buffer max size raised 48 -> 96
- **Area**: lib/ADS1232, firmware/settings
- **Status**: fixed
- **Found**: 2026-09-12, owner request after watching the 18g grind
  test: the `speed` setting (default 10 SPS) stays exactly as-is with
  no dynamic switching (already settled by AR-050's revert), but
  `read_samples`' usable range was capped by `RING_BUFFER_MAX_SIZE`,
  which was smaller than the owner wanted room to configure.
- **Resolution**: `ADS1232.h`'s `RING_BUFFER_MAX_SIZE` raised 48 -> 96;
  `SettingsTask.cpp`'s `validReadSamples` upper bound raised to match.

### AR-061 — Model tab had no way to see a bucket's duration change over time
- **Area**: web
- **Status**: fixed
- **Found**: 2026-09-12, owner request after the 18g grind test: "Can
  we historyically plot the 10 bucket sizes in a time series in the
  model tab? That would be very nice to see."
- **Resolution**: Model.tsx now keeps every `model_state` telemetry
  event received since the page connected (not just the latest) and
  renders all 10 buckets' durations as a Chart.js line chart, one
  series per bucket, alongside the existing live LUT table. Session-
  local only -- a page reload or device reboot starts the chart over,
  since nothing is persisted for this beyond the single latest state
  already cached in NetworkTask.cpp.

### AR-062 — History tab had no per-target-dose accuracy distribution
- **Area**: web
- **Status**: fixed
- **Found**: 2026-09-12, owner request: two histograms of final-minus-
  target delta, one per fixed target dose (single/double), x axis
  fixed to +/-0.5g, each fit with a Gaussian.
- **Resolution**: `History.tsx` fetches completed sessions per mode
  from PostgREST and renders a Chart.js bar+line histogram per mode
  (blue for single, orange for double), with a Gaussian curve computed
  from the sample mean/sd overlaid. Verifying this against the real
  device's data (`fetchCompletedDosesForMode`) turned up something
  worth flagging on its own: several `completed` sessions from before
  today's hardware fixes (AR-041/042/043) recorded final weights wildly
  off target (0g, negative, 100g+) -- real rows, not a bug, but not
  representative of current dosing accuracy either. The Gaussian fit is
  computed only from deltas inside the displayed +/-0.5g window (the
  chart couldn't show the rest anyway); the caption discloses how many
  points were excluded so the fit's provenance stays honest rather than
  silently dropping outliers.
- **Follow-up (same day)**: the owner noticed the "Recent sessions"
  table's Target column showed 8.7g/17g for doses they knew were
  9.5g/18g. Root cause: `sessions.target_weight_g` (both the table and
  this histogram's original delta calc) is `target_grams_corrected`
  (`TelemetryTask.cpp:170-171`) -- the margin-reduced internal MAIN_GRIND
  stop threshold (`requested - top_up_margin_single/double`), not what
  was actually asked for. That field measures the wrong thing for both
  the table display and, more importantly, the histogram/accuracy-panel
  deltas: comparing final weight to the corrected threshold instead of
  the true request biased every delta by roughly the margin's size.
  Fixed by switching the table's Target column, the Accuracy panel, and
  both histograms to `requested_weight_g` throughout; `target_weight_g`
  is still shown, now explicitly labeled "main-grind stop", in the
  per-session detail view where both numbers are meaningful side by
  side. Whether `top_up_margin_single/double` itself can now be shrunk
  (since `CoastModel` already predicts and cancels the physical coast
  contribution independently, per `DosingTask.cpp:253-256` -- the margin's
  only remaining job is leaving room for TOPUP, not compensating for
  coast) is a separate open question, not yet acted on.

### AR-063 — Per-dose hardware re-tare could still latch a stale baseline, and its retry loop had no timeout
- **Area**: firmware/dosing, firmware/scale
- **Status**: fixed
- **Found**: 2026-09-13, code review of the (already-tested, already-committed
  but never independently reviewed) calibration-precision/per-dose-retare
  work, as part of a full-codebase review pass.
- **What**: two bugs in the explicit per-dose hardware re-tare added
  alongside the `calibration_factor` double-precision fix. (1)
  `DosingTask`'s TARE state only checked "is there any cached stable
  sample" before latching the grind baseline -- since the fresh re-tare
  request lands on `ScaleTask`'s *next* tick at the earliest, a cup that
  had already been sitting still since before CONFIRM would satisfy that
  check on the very same tick TARE was entered, using a baseline computed
  from the *old* `tareRaw`, silently defeating the whole point of the
  re-tare. (2) `ScaleTask`'s retry loop around the re-tare itself
  (`while (!g_ads.tare())`) had no timeout, unlike the otherwise-identical
  boot-time tare it was modeled on -- a load cell that never settles
  (vibration, EMI, a marginal connection) would livelock the entire Scale
  task forever, with no further samples produced for display/WS/telemetry
  and no recovery short of a power cycle.
- **Why it matters**: (1) would have shipped a feature that looked correct
  (builds, passes tests, "every dose gets a fresh re-tare" per its own
  comment) but silently didn't do its job under a common real condition
  (re-dosing shortly after a previous dose, inside the auto-tare cooldown
  window -- exactly the scenario the feature exists for). (2) is a full,
  unrecoverable subsystem hang triggered by nothing worse than one slow-
  to-settle reading.
- **Resolution**: (1) `DosingTask` now records the newest `ScaleSample::
  sample_seq` already seen at the moment the re-tare is requested, and
  TARE's wait requires a subsequent sample whose `sample_seq` postdates
  it (wraparound-safe signed-subtraction compare) in addition to being
  stable.

  (2) First attempt bounded the retry to 5s and, on timeout, proceeded
  with whatever the last reading was -- **rejected by the owner**: the
  pre-rewrite firmware's own unbounded `while (!tare())` was deliberate
  and compared favorably against the rewrite in practice, and starting a
  dose from an unsettled tare is worse than waiting, not better. Owner's
  actual instruction: keep retrying (no silent fallback to a stale/
  unsettled baseline), but bound it to 20s purely to guarantee the task
  can't hang forever, and on that timeout **abort the dose** rather than
  run it. Implemented as a new `TareResult{bool ok}` reply (Scale ->
  Dosing, `g_tare_result_q`): `ScaleTask` retries for up to 20s exactly
  as before, and on failure sends `ok=false` and leaves `tareRaw`
  untouched (never silently "uses the last reading"); `DosingTask`
  aborts straight to IDLE with a logged reason if that arrives while
  still waiting in TARE. No session/telemetry is ever opened for a dose
  that fails this way -- per the owner, "it's not a valid run in this
  case," so there's nothing to record as aborted, it simply never
  started. Both firmware build and all 21 native tests still pass.

### AR-064 — DisplayTask's boot-splash hold could replay a stale command and skip the screensaver backlight toggle on one code path
- **Area**: firmware/display
- **Status**: fixed
- **Found**: 2026-09-13, same review pass as AR-063.
- **What**: while the boot splash's minimum-display-time hold is active, a
  real command that arrives is stashed as `pendingCmd`/`havePendingCmd`
  rather than rendered immediately. If the hold ends at a moment when a
  *live* command happens to already be waiting in the mailbox, that live
  command was applied directly without ever clearing `havePendingCmd` --
  leaving the stale, boot-era `pendingCmd` primed to overwrite the screen
  again whenever the mailbox next happens to time out (e.g. a tick
  DosingTask skips sending a command). Separately, the code path that
  *did* flush a pending command duplicated the live path's mode-change
  handling but omitted the screensaver backlight on/off toggle the live
  path applies.
- **Why it matters**: an intermittent, self-correcting but real "wrong
  screen flash" bug, and a code shape where the two paths could keep
  drifting further apart the next time either was touched.
- **Resolution**: both paths now go through one shared `applyCommand`
  lambda (mode-change side effects, render, `last`/`havePendingCmd`
  update) so they structurally can't diverge again.

### AR-065 — New Advanced/Calibration debug pages had several real bugs and small UX papercuts
- **Area**: web
- **Status**: fixed
- **Found**: 2026-09-13, code review plus a live walkthrough (against the
  real device, read-only) of the Advanced tab added alongside the
  calibration-precision work.
- **What**: `Calibration.tsx`'s ring-buffer stats reported "stable"/
  "changing" after as few as one sample rather than waiting for the
  buffer to actually fill, contradicting its own stated behavior; its
  poll-rate input snapped back to the 5Hz fallback mid-keystroke for any
  value starting with "0" (e.g. typing "0.5") because it re-parsed and
  clamped a controlled numeric value on every change; its linear-fit
  slope printed raw float noise instead of being rounded like the
  intercept beside it. `TareCalibration.tsx`'s ADC-range filter silently
  mis-handled invalid numeric input -- a garbage min with an empty max
  showed all data while implying a filter was applied, and a garbage min
  with a valid max showed "no samples in range," indistinguishable from a
  genuinely empty result. The Settings page's calibration-factor input
  was stuck at the default 8em width, visually clipping the last 1-2
  digits of the ~10-digit value the precision fix (AR-063's sibling
  commit) specifically exists to preserve. The Model tab's formula code
  blocks were horizontally scrollable with no visual affordance
  indicating that -- discovered live by scrolling a formula box and
  finding real content that had looked simply cut off. A few flex rows
  (Advanced's sub-tab nav, the calibration record-point row, the tare
  filter row) had no `flex-wrap`, risking overflow at phone width.
- **Why it matters**: these are debug/diagnostic tools built specifically
  to validate the calibration-precision work, so their own correctness
  mattered directly; the input-clipping and formula-cutoff bugs in
  particular each undercut the exact feature they were built alongside.
- **Resolution**: ring-buffer stats now return null (shown as "filling
  X%") until the buffer holds `bufferSize` samples; poll rate uses a
  draft-string input like the buffer-size field beside it; the ADC filter
  validates each bound independently and shows an inline "not a valid
  number" hint instead of silently misbehaving; the calibration input
  widened to 12em; the formula display switched from a single scrolling
  line (`white-space: pre` + `overflow-x: auto`) to wrapping
  (`white-space: pre-wrap`), which needs no discovery; the three flex
  rows got `flex-wrap: wrap`.

### AR-066 — D21 comment-style violations in the newest code (two historical references, three long inline comments)
- **Area**: firmware/dosing, firmware/display, lib/DosingModel
- **Status**: fixed
- **Found**: 2026-09-13, comment-style sweep (this review pass) plus
  independent verification -- found one additional violation the sweep
  missed by grepping directly for historical-reference phrasing.
- **What**: `DosingTask.cpp`'s topup-decision comment said "old firmware
  required stability..." and `DosingModel.cpp`'s LUT-seeding comment said
  "the same formula the earlier fitted-line model used" -- both reference
  a since-replaced version of the code (D21 explicitly forbids this: the
  comment must stand on its own for a reader who has never seen any other
  version). Separately, three genuinely-warranted long explanatory
  comments (the delta-space math in `handleGrindingSample`, the FINALIZE
  cup-lift dismiss rationale, and `DisplayTask`'s boot-splash-hold
  rationale) were written as multi-line plain `//` runs rather than the
  `/* ... */` block form the convention calls for once a comment
  genuinely needs more than two lines.
- **Why it matters**: matches the exact pattern D21 was adopted to
  prevent -- comments that rot the moment the thing they compare against
  is gone, or that don't visually signal "this is the rare long one."
- **Resolution**: both historical references rewritten to state the
  underlying physical/behavioral reason directly; the three long
  comments converted to `/* ... */` blocks with their content unchanged.

### AR-067 — No way to abort a running dose from the web app, only the physical BACK button
- **Area**: web, firmware/dosing, firmware/network
- **Status**: open -- needs-owner-input (a control-flow addition touching
  live dosing/relay state, not something to add unilaterally without
  agreeing on the interaction design first)
- **Found**: 2026-09-13, product/UX review ("would a human love to use
  this") -- grepped the webapp and firmware for any abort/cancel WS
  message and found none; `DosingTask.cpp`'s only abort path is
  `press.button == ButtonId::BACK` read from `g_button_press_q`, which
  only a physical button press can populate.
- **What**: the SPA's Live page can *start* a dose remotely (the "Request
  a dose" form, `dose_request`), but there is no symmetric way to stop
  one already running -- if something looks wrong mid-grind (wrong
  weight typed, a jam, wanting to change your mind) while not standing at
  the machine, the only recourse is walking over and pressing BACK.
- **Why it matters**: directly relevant to "would a human love to use
  it" -- a remote start with no remote stop is an asymmetric, slightly
  unsettling interaction, and the gap is more likely to be felt the more
  the web-based manual-dose feature actually gets used.
- **Resolution**: not implemented here -- needs the owner's input on the
  interaction design first (e.g. should a web abort require a confirm
  step, given how much worse an accidental abort mid-grind is than an
  accidental settings write? should it look identical to a physical BACK
  press, or synthesize a slightly different code path?). Mechanically
  straightforward once decided: a new `abort_request` WS message type,
  `NetworkTask` forwarding it into the existing button-press machinery
  (or a small new queue), and a "Stop" button on the Live page shown only
  while a session is active.

### AR-068 — Dev-mode SPA connects directly and silently to the real physical device, with only a small status dot as indication
- **Area**: web, process
- **Status**: open -- needs-owner-input (echoes AR-036's lesson; the risk
  is real but the underlying capability -- testing UI changes against
  live device data -- is legitimate and worth keeping)
- **Found**: 2026-09-13, product/UX review -- `webapp/.env.local` hardcodes
  `VITE_DEVICE_HOST=192.168.0.118` (the device's pre-`eureka.local`
  address, itself stale per AR-020's finding about `platformio.ini`), so
  `npm run dev` connects straight to the real grinder's `/ws`, not a mock.
  Confirmed live during this review: the small green status dot and a
  real weight readout were both genuinely reflecting the physical device.
- **What**: nothing in the dev-mode UI distinguishes "you are looking at
  a real, physical grinder that will actually run" from a disconnected/
  mock state beyond that one small dot -- exactly the ambiguity AR-036
  already flagged once for a "read-only" curl request that turned out to
  trigger a real grind.
- **Why it matters**: a developer (human or agent) casually testing a UI
  change with `npm run dev` is one misclick on "Grind" or a WiFi-reset/
  reboot button away from a real action on the physical machine, with no
  prompt or confirmation gating it in dev mode specifically.
- **Resolution**: not implemented here -- flagging for the owner to decide
  the right mitigation (a persistent on-screen banner when
  `import.meta.env.DEV` is true, a confirm-to-arm step before the first
  write in a dev session, or simply accepting this as a documented risk
  now that it's been named explicitly). This review's own browser-based
  UI checks were done read-only for exactly this reason.

### AR-069 — Tare baseline's raw ADC count shows real session-to-session inconsistency (~8.7% CV on the first 8 logged doses)
- **Area**: firmware/scale, data-infra
- **Status**: confirmed -- needs-owner-input (a physical/data question,
  not a code bug; the new debug page did exactly what it was built for)
- **Found**: 2026-09-13, live reading of the new `v2.tare_debug` table via
  the Advanced > Tare calibration page (built this session, see AR-063's
  sibling commit and `.agent/design/db-schema/002_tare_debug_stats.sql`):
  8 doses so far, mean raw ADC at tare 624428.8, sd 54137.6, min 594990.0,
  max 712171.0, range 117181.0.
- **What**: the page's own stated premise is that the same physical
  dosing cup should tare to roughly the same raw ADC count every time --
  a coefficient of variation around 8.7% on the first real batch of data
  says that's not holding, at least not yet with this few samples.
- **Why it matters**: this is exactly the signal the tare-debug feature
  was built to surface (per its own design-doc comment, investigating "a
  final-weight discrepancy that turned out NOT to be a stability/settling
  issue"), and it's already showing something worth a look -- possibly
  normal thermal/mechanical drift, possibly a cup-seating or mounting
  issue worth a physical check. Too few samples yet to conclude either
  way.
- **Resolution**: none yet -- needs more accumulated data (the page
  supports filtering by raw-ADC range for exactly this kind of ongoing
  monitoring) and the owner's read on whether the spread looks like
  normal drift or a physical issue worth investigating by hand.

### AR-070 — Minor code-quality items from this review pass, not yet acted on
- **Area**: web
- **Status**: open -- low priority, bundled rather than filed separately
- **Found**: 2026-09-13, same review pass as AR-065.
- **What**: (1) `Advanced.tsx`'s `subTabFromHash` and `App.tsx`'s
  `tabFromHash` are near-duplicate hash-parsing helpers; worth a shared
  helper if a third routing level ever appears, not urgent at two call
  sites. (2) `Calibration.tsx` mixes three fairly independent concerns
  (poll loop/ring buffer, chart rendering, calibration-point table/fit)
  in one 390-line component -- no bug, but the one place in the new code
  that diverges from `Settings.tsx`'s flatter, single-purpose-row style.
  (3) `raw_read_request`/`raw_read` carry a `request_id` that nothing
  actually checks on the reply side (`DeviceSocketContext.tsx` just
  stores "whatever arrived most recently") -- harmless on today's
  single-client WS, but the field is generated and sent for no effect.
  (4) `TareCalibration.tsx` has no retry affordance on a PostgREST fetch
  failure beyond switching tabs and back.
- **Why it matters**: none of these are bugs today; logged so they aren't
  silently lost, per this project's practice of tracking real findings
  even at low urgency (cf. AR-020, AR-026, AR-027).
- **Resolution**: —

### AR-071 — Physical TFT panel's color filter is BGR, but the driver was configured for RGB -- every "blue" element likely rendered wrong since the D22 redesign
- **Area**: firmware/display, hardware
- **Status**: fixed
- **Found**: 2026-09-13, owner watching the new OTA liquid-fill screen
  live on the physical device: "the FW flashing starts orange/red, but
  previously we discussed the target was blue, getting more green as it
  progresses... maybe red and blue are swapped."
- **What**: `kColorOtaBlue = 0x0B7F` decodes under standard RGB565 to
  R=1/31, G=27/63, B=31/31 -- low red, moderate green, full blue, i.e.
  unambiguously blue. Swapping only red and blue on that same value gives
  R=31/31, G=27/63, B=1/31 -- full red with moderate green, i.e. orange.
  That is exactly what was reported. `panelBegin()` calls
  `g_tft.initR(INITR_MINI160x80)`, and the Adafruit ST7735 driver library
  sets `MADCTL = ST77XX_MADCTL_RGB` for that exact tab type at rotation 0
  (confirmed by reading the vendored library source) -- so the driver
  believes it configured RGB order, but the actual physical panel's color
  filter is BGR. Confirmed via `git show main:lib/Display/Display.cpp`
  that the pre-rewrite firmware used the identical `initR(INITR_MINI160x80)`
  call -- this is a pre-existing hardware/driver mismatch, not something
  introduced by this session's display work.
- **Why it matters**: every other "blue" UI element added by D22's
  iOS-style redesign (the top-of-screen progress bar, BOOT/CONFIRM's
  accent underlines, `kColorAccentBlue` generally) almost certainly
  rendered as orange/red too, just never noticed -- a thin 2-3px bar or
  underline against a black background is easy to glance past, while a
  full-screen animated liquid fill made the wrong hue undeniable. This
  had been live on the actual device since D22 shipped without anyone
  catching it.
- **Resolution**: `panelBegin()` now re-issues `MADCTL` with the BGR bit
  set, correcting color order at the source for every color drawn rather
  than swapping red/blue in each of the file's hex constants
  individually. **First attempt was wrong**: sent only the BGR bit
  (`0x08`) on the theory that `setRotation(0)` sets no `MX`/`MY`/`MV`
  bits for this tab type -- a misread of the vendored library's
  `setRotation()` switch (confused rotation 2's plain
  `ST77XX_MADCTL_RGB` value with rotation 0's actual
  `MX | MY | ST77XX_MADCTL_RGB`), and OTA-deploying it flipped the whole
  panel upside down, confirmed live by the owner immediately after that
  deploy. Corrected to OR the BGR bit into the same `MX | MY` bits
  rotation 0 actually needs; rebuilt and redeployed in the same session.
  Colors confirmed correct live before the orientation regression was
  caught; awaiting final confirmation that orientation is also back to
  normal after the second deploy.

### AR-072 — Main-grind raw-weight fallback could stop early on a clump-impact spike, causing undershoot
- **Area**: firmware/dosing
- **Status**: fixed
- **Found**: 2026-09-14, owner report: a completed dose measured 17.7g
  against a full 18g requested, asking whether the firmware might be
  reacting to a momentary high reading -- "grounds drop in clumps, they
  have inertia, which reads high for a moment."
- **What**: `handleGrindingSample`'s raw-weight fallback
  (`raw_weight_fallback_fired = delta_weight >= g_target_grams_corrected`)
  had no stability gate, unlike every decision in `handleTopupSample`
  (which explicitly requires `s.stable` -- see that function's own
  comment, "every decision here needs a settled reading"). Tracing
  `ADS1232::getRaw()`: whenever the ring buffer isn't self-consistent
  (`changing == true`, i.e. `isStable == false`), it returns the single
  *latest* raw sample rather than the buffer's mean -- so an unstable
  reading passes a momentary spike straight through, filtered only by
  `getUnits()`'s coarse >200g glitch guard. A falling clump's impact
  registers as extra force for an instant before settling to its true
  mass; if that transient spike happened to cross
  `target_grams_corrected`, the ungated fallback could fire immediately
  and cut the relay a moment early, with the reading then decaying back
  down below target once the clump actually settled.
- **Why it matters**: an early relay-off with no fault in the time-
  estimate model itself, directly costing accuracy on the metric D7/D13
  care about most, and specifically the failure mode the owner correctly
  intuited from physical first principles rather than from reading the
  code.
- **Resolution**: first pass only gated `raw_weight_fallback_fired` on
  `s.stable` -- **incomplete, per the owner's direct pushback**: "Are you
  actually telling me that you are still using an unstable result for a
  decision? You keep making the same mistake. Fix it. Fix it
  everywhere." A full sweep of every scale-reading decision found three
  more instances, including the one most likely responsible for the
  actual reported undershoot:
  - `MainGrindModel::addSample` fed every raw sample unconditionally
    into `m_last_weight_g`, which `predictStopTimeMs` compares directly
    against `target_weight_g` to decide "have we already reached it" --
    on the **primary** (time-estimate) stop path, upstream of the
    fallback the first pass fixed. A clump-impact spike here could stop
    the main grind itself early. Fixed by adding a symmetric
    `max_plausible_rise_g` bound (mirroring the model's existing
    `max_plausible_drop_g` from AR-049), rejecting an implausible
    single-sample rise the same way a sudden drop was already rejected
    -- held at the last known-good value, not folded in. New native test
    `test_main_grind_model_ignores_sudden_weight_spike` locks in the
    exact scenario (22/22 native tests passing).
  - SCREENSAVER's wake-on-weight-change check and FINALIZE's cup-lift
    dismiss check both compared a raw sample against a threshold with no
    stability gate. Both now require `sample.stable`/`g_last_sample.stable`
    -- no bounded-timeout fallback needed for either, since an
    independent unconditional exit already exists (a button press for
    SCREENSAVER, `finalize_timeout_ms` for FINALIZE).
  - Verified `TopupModel` and `CoastModel` already have equivalent
    hard-bound plausibility rejection independent of the driver's
    stability flag (`reject_hard_min/max_g`,
    `min/max_plausible_coast_g`) -- no changes needed there.

  Also recorded as a standing rule in `.agent/AGENTS.md`'s process
  expectations (scale-reading decision discipline) so this class of bug
  can't be reintroduced piecemeal again: gate one-shot decisions on
  `sample.stable` with a bounded fallback where one is actually needed;
  use magnitude/statistical plausibility rejection, not a stability
  gate, for continuous streams feeding a regression model, since real
  continuous flow is essentially never "stable" by the ADC driver's own
  flag (gating `addSample` itself on `stable` would starve the model of
  data during any real grind).

  **Third round, same session -- the second pass's own fix for the
  GRINDING fallback was wrong.** Owner: "will it now run forever
  because during grind, the scale will never report a stable weight? ...
  our transition into topup will be driven by an unstable weight,
  because a running grinder produces by definition an unstable weight.
  Are you capable of auditing the logic with this in mind?" Correct:
  gating `raw_weight_fallback_fired` on `s.stable` doesn't make it
  safer, it makes it fire (if ever) only once flow has nearly stopped --
  GRINDING holds the relay on continuously for its whole duration, so
  the ADC's stability flag is essentially never true there by
  definition. That silently disabled the fallback's actual purpose
  (catching "the primary time estimate is wrong while flow is still
  ongoing") and would have made the *opposite*, worse failure mode
  (overshoot, running all the way to `grinding_timeout_ms`) more likely
  in exchange for fixing a smaller undershoot risk -- backwards, given
  D7's stated priority that overshoot is the worse outcome. Contrast
  with `handleTopupSample`'s DECIDING/SETTLING phases, correctly gated
  on stability because the relay is *off* between pulses -- the scale
  genuinely can and does settle there; GRINDING has no equivalent quiet
  moment. Fixed properly by protecting the *value* instead of gating the
  *decision*: a new `MainGrindModel::currentWeightEstimate()` accessor
  exposes `addSample`'s own last-accepted sample (already protected by
  the second round's `max_plausible_rise_g`/`max_plausible_drop_g`
  rejection), compared with no stability requirement at all, because
  none is ever available while genuinely grinding. The AGENTS.md
  standing rule from the second round was itself rewritten to spell out
  this distinction explicitly (a continuously-active process vs. one
  that can genuinely settle between checks) rather than presenting
  "gate on stable" as a general-purpose fix. All three rounds fixed,
  rebuilt (22/22 native tests), and OTA-deployed in the same session;
  not yet confirmed against a live dose.

### AR-073 — Persistent ~0.3g dosing undershoot vs. an independent reference scale: firmware bug, confirmed NOT physical/calibration

- **Area**: firmware/dosing
- **Status**: probably resolved as a side effect of the rate_hat restore
  below -- **not yet confirmed, do not close.** Two real coffee doses
  after the restore (18g target -> 17.9g, 9.5g target -> 9.5g) both
  landed within the owner's reference-scale accuracy, first time that's
  happened since this AR was opened. Leading theory: repeated dev/test
  sessions over this project's history (of which 2026-09-14's fake-
  calibration-weight runs were the most recent and most acute instance)
  have been slowly corrupting the persisted `MainGrindModel` rate via
  exactly the mechanism found and fixed below -- not a single algorithmic
  bug in the real-dose control loop, which is why AR-023/AR-052/AR-057/
  AR-059/AR-072 (real, correct fixes to real, separate issues) never
  closed it. Needs several more real doses over the coming days to
  confirm before this is called fixed -- **read this whole entry
  (including the update below) before proposing a cause**, and don't
  declare it closed on two data points alone.
- **What the owner has already ruled out, by direct test, more than
  once** — do not re-suggest these:
  - **Not physical/mechanical.** Hardware is unchanged (D1). The
    pre-rewrite firmware landed on target almost exactly, on the same
    hardware, run after run.
  - **Not calibration.** The owner restored the old `calibration_factor`
    and separately re-ran a full recalibration — neither changed the
    ~0.3g gap at all.
  - **Not the load cell's raw accuracy.** A known ~20g calibration
    weight placed on the idle scale (no session running) reads correctly
    -- ~19.9-20.0g, with normal small jump/noise -- confirmed directly on
    the Advanced/raw-read page.
- **The actual repro, and the key clue**: start a normal dose session (a
  real button press or API request, real `TARE`/`GRINDING`/`TOPUP`/
  `FINALIZE` flow) and place that same ~20g calibration weight in the cup
  in place of real coffee. The session's own reported final weight comes
  out ~19.5-19.7g — a ~0.3-0.5g shortfall against the *same physical
  weight*, measured on the *same load cell*, that reads correctly outside
  a session. This isolates the bug to somewhere in the session/FSM's own
  weight bookkeeping (`DosingTask.cpp`'s delta-from-tare-baseline math,
  or a stop condition that latches before settling, or similar) — not
  the ADC, not `calFactor`, not the physical mechanism.
- **Measurement method, since it wasn't logged data before**: the owner
  weighs the finished dose on an independent kitchen/reference scale,
  separate from the grinder's own load cell — not the firmware's
  self-reported `final_weight_g`. `v2.sessions` has no column for this
  independent reading; every fix attempt to date was validated only
  against the firmware's own number, which is exactly the thing in
  question. Getting the true reading logged per-session (even a manual
  entry) is a prerequisite for confirming any future fix actually closes
  the gap, rather than just agreeing with itself again.
- **Next step**: instrument the exact repro above (calibration weight, a
  real session) with `sendLog()`/WS "log"-channel lines at each of: the
  latched `g_grams_on_grind_start` right after `TARE` completes, which
  `GRINDING` stop condition actually fires and
  `MainGrindModel::currentWeightEstimate()`'s value at that instant,
  whether/how many `TOPUP` pulses fire, and the exact `g_last_sample.grams`
  vs. `g_grams_on_grind_start` at the moment `FINALIZE` latches --
  watched live, not guessed at from static reading. Do not propose a fix
  without having traced that specific run.
- **2026-09-14 update -- instrumentation added, first repro run
  inconclusive, and it surfaced a second, separate, real bug.** The
  `sendLog()` lines above were added and deployed. The owner's first
  repro run (an abruptly-placed calibration weight standing in for
  coffee, no dosing cup, several sessions in a row at 9.5g/18g targets)
  showed every `GRINDING` stop firing on `time` or `timeout`, never on
  weight -- `currentWeightEstimate()` read ~0g at the stop instant every
  time. Root cause: an abrupt weight placement is a single-sample jump
  far larger than `max_plausible_rise_g` (1.0g, added earlier this same
  session in AR-072's second round), which `MainGrindModel::addSample()`
  rejected -- and then kept rejecting forever, since every later sample
  was *also* compared against the same now-stale last-accepted value.
  This means the repro method (an abrupt static weight) doesn't exercise
  the same code path real, gradual coffee flow does, so it could not by
  itself diagnose the original gap -- but it is a real bug, introduced
  today, independent of AR-073's original question. Two things followed:
  - **`addSample()` hardened**: an implausible reading that persists past
    a new `max_reject_duration_ms` (500ms) is now accepted as a genuine
    step change rather than rejected forever (`test_main_grind_model_accepts_persistent_step_change_after_reject_window`).
    A momentary clump-impact spike self-reverts well inside that window
    and is still rejected as before.
  - **The persisted main-grind rate got corrupted by the test itself**:
    each stuck session fit a confidently-flat near-zero slope (many
    identical near-zero samples, very low variance) that dominated the
    Bayesian blend into the cross-session prior --
    `rate_hat_g_s` fell from 0.9596 to 0.3759 g/s across 5 sessions,
    live on the device, biasing every real dose's stop-time prediction
    until noticed. Restored via a one-time boot-time patch (added,
    confirmed applied and persisted to NVS across a reboot, then
    removed the same session) back to the pre-test values
    (`rate_hat=0.959576`, `rate_sd=0.000075`, `n_effective=137310`,
    captured live moments before the corrupting runs). A new
    `DoseRequest::discard_training` flag (API-only, `dose_request`'s
    `"discard_training": true` over WS, or `?discard_training=true` over
    HTTP) now lets a dev/test dose run normally without any of it
    reaching NVS or this boot's in-RAM models -- at `FINALIZE`, a
    discard-flagged session's `MainGrindModel`/`TopupModel`/`CoastModel`
    are rebuilt fresh from the untouched persisted blob instead of being
    blended and persisted. Real physical-button doses always train
    normally; the flag only exists over the API.
  - **Real-coffee re-run, same session, after the restore**: two doses
    (18g target -> 17.9g, 9.5g target -> 9.5g) both landed within the
    owner's reference-scale accuracy -- see the status line above. Good
    sign, not yet confirmation; needs more doses over more sessions
    before this AR is actually closed.

### AR-074 — 9.5g dose landed at 10.4g: one topup pulse overshot ~4.5x; the WS-log diagnostics from AR-073 were ephemeral and missed it

- **Area**: firmware/dosing, telemetry
- **Status**: root cause found and confirmed from persisted data alone;
  post-mortem capability gap fixed.
- **Found**: 2026-09-16, owner report: a 9.5g dose finished at 10.4g
  (+0.9g, far outside normal noise), asking what happened and whether it
  could be diagnosed from the device, or whether post-mortem capability
  needed adding.
- **What happened (from `v2.sessions`/`v2.events`/`v2.tare_debug` alone,
  no live listener needed)**: `TARE` baseline was a clean 0g. `MAIN_GRIND`
  behaved normally, stopping at 8.8316g -- appropriately short of the
  9.0g corrected target, anticipating coast. Coast added +0.4346g (9.2662g),
  close to the persisted `coast_weight_hat` (~0.34g), unremarkable. Then
  exactly **one** `TOPUP` pulse fired for a 0.234g gap (bucket 2, aim
  ~0.255g) and delivered **1.1576g** -- 4.5x its own aim. Cross-checked
  against `model_state` broadcasts bracketing that session: bucket 2's
  learned duration was **1335ms**, sitting between neighboring buckets 1
  and 3 at ~650-680ms each -- a clear outlier for a bucket with a healthy
  n=18 samples, not a fresh/undertrained one. TopupModel's only
  safeguards are absolute hygiene bounds (350-5000ms), which 1335ms
  comfortably satisfies -- nothing constrains a bucket's duration
  relative to its neighbors. By the time this was investigated (two days
  later), bucket 2 had already self-corrected down to ~820ms via the
  asymmetric online learning rule (real subsequent pulses in that bucket
  pulling it back toward sane), consistent with a genuine, if slow,
  self-healing learned-value anomaly rather than a permanent bug --
  root cause is data-quality in the learned LUT, not a logic defect.
- **Why the owner couldn't get this from a live listener**: AR-073's
  `sendLog()` diagnostics (TARE baseline / GRIND stop reason / TOPUP
  decision / FINALIZE latch) only ever go out over WS broadcast + Serial
  -- nothing persists them. Confirmed directly: the WS listener used for
  AR-073 was in a "network unreachable" gap (laptop off the LAN) spanning
  exactly this session's runtime, so even the *existing* instrumentation
  was silently lost. Post-mortem was still possible here only because
  `weight_before_g`/`weight_after_g` on the persisted `events` rows
  happened to be enough to localize the overshoot to one specific pulse
  -- it was NOT possible to know from persisted data alone (before this
  fix) *why* that pulse was commanded for 1335ms, only that it was.
- **Fix**: `v2.events` gets five new nullable columns
  (`004_stop_diagnostics.sql`, applied live) -- `stop_reason`/
  `weight_estimate_g` on `MAIN_GRIND` rows (which of the three stop
  conditions fired, and `MainGrindModel::currentWeightEstimate()` at that
  instant), `topup_bucket`/`topup_aim_weight_g`/
  `topup_commanded_duration_ms` on `TOPUP` rows (this pulse's own inputs
  at fire time, captured before `recordPulse()` retunes that bucket for
  the next one). `DosingTask.cpp` now builds these two events directly
  (`sendProgressTelemetry`/`sendTopupPulseTelemetry`) instead of through
  the generic `sendTelemetry()` helper; `TelemetryTask.cpp` posts them to
  PostgREST. This makes every future anomaly diagnosable from the
  database at any time afterward, with no dependency on a live WS
  listener catching it in the moment. Verified end-to-end with a
  synthetic PostgREST round-trip using this exact incident's real
  numbers (not yet confirmed against a live real dose). Firmware builds
  clean, 23/23 native tests pass, OTA-deployed.
- **Not fixed, and not this AR's job**: bucket 2's duration drifting to
  an outlier in the first place. `TopupModel::recordPulse()`'s per-bucket
  independent update has no cross-bucket smoothness constraint; whether
  it should is an open question for whoever next has real multi-bucket
  drift data to look at, not acted on here.

### AR-075 — Falling-clumps animation replaces OTA's liquid fill, shared with GRINDING

- **Area**: firmware/display
- **Status**: implemented, OTA-deployed, not yet visually confirmed (no
  camera/screen access from this side -- needs the owner to look at the
  physical device).
- **What**: owner reported the OTA liquid-fill animation flickered, with
  the topmost (lightest) band's height visibly changing frame to frame.
  Root cause: `bandH = filled / kBands` was recomputed fresh every frame
  from `filled`, which itself changes continuously -- integer truncation
  landed differently often enough to jitter the last band's rendered
  height by a pixel. Rather than patch the banding math, replaced the
  whole effect per the owner's request: small squares ("coffee
  clumps"/snow) fall continuously and disappear into a solid pile rising
  from the bottom, reused as a single shared `drawClumpField()` for both
  `OTA_UPDATE` and the whole GRINDING/TOPUP/STOPPING/FINALIZE family (the
  owner's own ask, "to harmonize things a little"). The pile itself is a
  flat, diffed fill (same technique as `drawProgressBar`) -- no bands, so
  no per-frame recomputed height to jitter.
- **A real bug caught before deploying, not by testing** (no way to
  flash-and-watch a TFT from here): the first draft reset the clump
  field's animation clock on *any* redraw, including one triggered only
  by the pile boundary moving -- and GRINDING's weight display changes
  far more often than the intended ~70ms animation tick. That would have
  let frequent weight updates starve the clump animation almost
  entirely. Fixed to only advance the clock on an actual animation tick.
- **Also changed**: GRINDING's per-field "skip redraw if this string
  didn't change" optimization is gone -- with a continuously-animating
  background, a skipped field could leave stale clump pixels showing
  through it. All three text fields now redraw together whenever the
  clump field drew anything, or a real value changed (still nothing
  redrawn between ticks if truly idle). The pile color changed from
  literal white to a dark roast brown (`#3E2723`) rather than the more
  "coffee-with-milk" tan first drafted -- the existing gray/accent text
  colors were tuned against black and would have washed out against a
  light pile; dark roast is also just what ground coffee actually looks
  like.
- Firmware builds clean, 23/23 native tests pass (unaffected --
  `DisplayTask` isn't part of that suite), OTA-deployed.

### AR-076 — Added a host-side display simulator (tools/display_sim); it immediately caught two real bugs in AR-075

- **Area**: firmware/display, tooling
- **Status**: implemented, two bugs found and fixed, not yet visually
  confirmed on the physical panel.
- **What**: the owner asked how much effort a screen simulation would be,
  agreed to it, and asked specifically for something animated/viewable,
  not a pile of still PNGs. `DisplayTask.cpp` now also compiles host-side
  (`#ifdef ARDUINO` swaps `Adafruit_ST7735`/real SPI for `FakeCanvas`, an
  in-memory RGB565 framebuffer with the same method surface, text
  rendering reusing the real vendored Adafruit_GFX 5x7 font table
  verbatim so layout matches the physical panel exactly) -- the *same*
  rendering source, not a reimplementation. `Messages.h`'s `<Arduino.h>`
  include is now conditional too (nothing else in it is Arduino-specific).
  A small `main()` (native-build only, `DisplaySimMain.h`) scripts three
  scenarios -- BOOT, a full IDLE->CONFIRM->TARE->GRINDING->STOPPING->
  TOPUP->FINALIZE dose, and an OTA_UPDATE 0->100% run -- through the exact
  same `renderMode()` the real Display task calls, capturing frames to a
  tiny custom `.dsim` binary; `tools/display_sim/dsim_to_gif.py` turns one
  into an animated GIF. See `tools/display_sim/README.md`.
- **Immediately paid for itself**: reviewing the very first capture (for
  AR-075's falling-clumps animation) surfaced two real bugs neither code
  review nor reasoning about the pixel math had caught:
  1. `drawOtaLayout`'s percent/label text drew with a transparent
     background (fine against the old smooth liquid fill) -- a falling
     clump caught mid-glyph showed through the gaps between strokes.
     Fixed by clearing each text row's background first via
     `fillGrindBackground`, same as `drawGrindingBlock`'s fields already do.
  2. OTA's pile color shifts continuously with percent (blue -> green),
     but `drawClumpField` only ever repaints the newly-grown band -- so
     each band kept whatever color it was painted with, leaving visible
     "growth ring" stripes as the color moved on, an echo of the exact
     banding flicker AR-075 was meant to eliminate. Fixed with a new
     `pileColorMayShift` flag: true re-floods the whole pile on every
     move (OTA only); GRINDING's pile color never changes, so it keeps
     the cheaper incremental-band fill.
- Both bugs are visible in the published review artifact's before/after
  comparisons (see the session's own record for the link -- not
  duplicated here since artifact URLs aren't guaranteed durable outside
  the conversation that created them).
- Firmware builds clean, 23/23 native tests pass (`DisplayTask`/the sim
  still isn't part of that suite -- this is a visualization tool, not a
  pass/fail check; whether it's worth golden-frame regression assertions
  later is an open question, not attempted here).
