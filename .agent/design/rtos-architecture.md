# FreeRTOS task/queue architecture — design proposal

Status: design proposal, no implementation. Target: `rewrite/rtos-fork`,
Arduino-ESP32 (D2 — explicit FreeRTOS tasks, not a framework switch).
Read against `.agent/AGENTS.md`, `.agent/DECISIONS.md` (D2, D4, D6, D7),
`.agent/ARS.md` (all entries, esp. AR-001/007/008/009/011/012/013/014/016/021),
`.agent/design/topup-model.md`, `src/main.cpp`, `lib/Display/Display.h`,
`lib/API/API.h`, `lib/WebSocketSettings/WebSocketSettings.h`.

---

## 0. Design principles (apply throughout)

1. **Single-writer rule.** Every piece of mutable state has exactly one task
   that ever writes it. Everyone else either (a) receives a copy via a
   message, or (b) reads a "latest snapshot" mailbox that only the owner
   writes into. No struct is ever written from more than one task's context,
   and — critically — **never from ISR context**. This is the one discipline
   that eliminates AR-001 and AR-009's entire bug class by construction, not
   by convention.
2. **ISRs do the absolute minimum**: read a GPIO/timer, `xQueueSendFromISR`
   a small POD struct, `portYIELD_FROM_ISR` if needed. No settings reads, no
   state-machine logic, no globals mutated, in *any* ISR — this is the
   concrete fix for AR-001/007/008 and the ISR-reads-settings part of AR-009.
3. **Two message shapes, chosen deliberately per link**:
   - **Queue** (`xQueueSend`/`Receive`, depth > 1) when every item matters
     (a value stream that must not lose entries — e.g. scale samples feeding
     the topup model, telemetry events).
   - **Mailbox** (`xQueueOverwrite` on a length-1 queue) when only the
     *latest* value matters and staleness is fine — e.g. "what should the
     display show right now", "what are the current settings". Mailboxes
     never block on send, which is what lets a fast/critical producer push
     to a slow/optional consumer without ever waiting on it.
4. **The dosing/session task must never block on anything downstream of it**
   (display, network, settings, telemetry). All sends *from* dosing control
   are either mailbox overwrites (non-blocking by definition) or queue sends
   with a zero timeout and a drop-and-count policy on full. This is the
   direct guard against a slow web client or a flash write stalling the
   relay-control loop.
5. **NVS is touched by exactly one task.** ESP-IDF's NVS API is not
   documented as safe for concurrent access to the same namespace from
   multiple tasks; centralizing it also gives one place to rate-limit flash
   wear and validate everything that gets persisted.
6. **Validate at the authority, not at the door.** The settings task is the
   single authority on "is this a legal value" (AR-016) — the network task
   may do cheap syntactic rejection (malformed JSON, wrong type) as a fast
   fail, but semantic bounds-checking (non-zero divisors, timeout ranges,
   lookup-table bounds) happens exactly once, in the settings task, so a
   second future write path (e.g. a batch-import feature) can't bypass it.

---

## 1. Task list

| # | Task | Replaces (from `main.cpp`) | Core | Prio | Stack |
|---|------|------------------------------|------|------|-------|
| 1 | **Scale Sampling** (`scale_task`) | `scale.readADCIfReady()` calls scattered through every `loop*()` | 1 | 9 | 3072 B |
| 2 | **Dosing/Session Control** (`dosing_task`) | the whole `switch(state)` machine: `loopIdle`…`loopScreensaver`/`loopDebug`, `grinderOn/Off` | 1 | 8 | 4096 B |
| 3 | **Input** (`input_task`) | `button_interrupt<pin>`, `back_button_isr`, `loopButtonFilter` | 1 | 7 | 2048 B |
| 4 | **Display** (`display_task`) | `Display` class render calls, `display.clear()`/`displayString` scattered through loop | 1 | 4 | 4096 B |
| 5 | **Settings/NVS** (`settings_task`) | `WebSocketSettings` struct + EEPROM load/save | 0 | 3 | 4096 B |
| 6 | **Network** (`network_task`) | `WiFiManager`, `ESPAsyncWebServer`, `ArduinoOTA`, the future consolidated WS (D4) | 0 | 3 | 8192 B |
| 7 | **Telemetry** (`telemetry_task`) | `WebSocketLogger`, `WebSocketGraph`, `WebSocketMetrics`, `RawDataWebSocket`, future PostgREST posting (D8/D9) | 0 | 2 | 6144 B |

Not a separate task: **API** (`/api/getDosage`). It's a thin HTTP handler
living inside the Network task's AsyncWebServer instance — it does cheap
syntactic validation and forwards a `DoseRequest` to `dosing_task`; it owns
no state of its own (see §3.6, fixes AR-015).

### Why this decomposition (deltas from the AGENTS.md sketch)

AGENTS.md's D2 sketch names "scale sampling, dosing control, display,
network, logging/telemetry" as likely candidates. This design keeps all of
those but splits two things out further, and merges one:

- **Input is its own task**, not folded into dosing control. The current
  code's bug (AR-001/007/008) is specifically that *interrupt* context does
  debounce/gating logic that's inconsistent between buttons. Giving debounce
  a dedicated task with one code path for all three buttons is what makes
  "every button gets the same treatment" structurally true rather than
  something you have to remember to keep in sync across three ISRs.
- **Settings/NVS is its own task**, not folded into dosing control or
  network. It's the direct fix for AR-009: the thing being raced today
  (`settings.scale.*`) needs exactly one owner that is neither the task
  reading it fastest (dosing) nor the task writing it most often externally
  (network/web).
- **Telemetry is split from Network.** Network owns the AsyncTCP-backed
  WS/HTTP/OTA surface, which must stay responsive. Telemetry owns anything
  that can legitimately block for a while — in particular the future direct
  HTTPS POST to PostgREST (D8/D9), which involves a TLS handshake. Blocking
  TLS work in the same task that's servicing WS frames would reintroduce a
  responsiveness hazard in a different shape. The old five separate loggers
  (`WebSocketLogger`, `WebSocketGraph`, `WebSocketMetrics`, `RawDataWebSocket`)
  collapse into one `TelemetryEvent` stream (§3.5) consumed by this task,
  which forwards a subset to Network for the single multiplexed WS (D4) and
  separately buffers session data for the PostgREST POST — no dedicated
  "logging" task, because log lines have no different timing/reliability
  requirement from progress/topup events; they're just one more
  `TelemetryEvent` variant.

---

## 2. Ownership map — single writer per piece of state

| State | Owner (sole writer) | Readers, and how they read it |
|---|---|---|
| Raw ADC / filtered weight (ring buffer) | Scale task | Dosing task, via `ScaleSample` queue (§3.1) |
| `state` (session FSM: IDLE…DEBUG) | Dosing task | Display task, via `DisplayCommand` mailbox (§3.2); nobody else needs it |
| `target_grams` / `target_grams_corrected` | Dosing task | Display (via `DisplayCommand`), Telemetry (via `TelemetryEvent`) |
| Topup model (`rate_hat`, `topup_slope`, …) — **hot, in-RAM, updated per-sample during a grind** | Dosing task | nobody reads it live except Dosing itself; persisted snapshot pushed to Settings task at session boundaries only (§5) |
| Topup model — **cold, persisted NVS copy** | Settings task | Dosing task, once at boot/session-start via a mailbox (§5) |
| Button raw GPIO edges | (hardware / ISR, no software "owner") | Input task, via `ButtonEdge` queue (§3.3) |
| Debounced button presses | Input task | Dosing task, via `ButtonPress` queue (§3.3) |
| `Settings.Scale` struct (canonical, in RAM) | Settings task | Every other task, via a **per-subscriber `SettingsSnapshot` mailbox** (§4) — never the shared struct itself |
| `Settings.WiFi` (reset/reboot flags) | Settings task | Network task, via its `SettingsSnapshot` mailbox |
| NVS flash contents | Settings task | n/a — nobody else touches NVS |
| Grinder relay GPIO | Dosing task | n/a — no other task ever writes `GRINDER_RELAY_PIN` |
| Display framebuffer / SPI bus to ST7735 | Display task | n/a — no other task touches `Adafruit_ST7735` |
| WiFi connection state, AsyncWebServer, WS client list, OTA state | Network task | Telemetry task reads connection/client-count via a small mailbox if needed for UI (e.g. connection indicator, see §6) |
| Session-scoped telemetry buffer (for PostgREST POST) | Telemetry task | n/a |

Every row above has exactly one writer. That's the whole trick.

---

## 3. Communication — concrete message shapes

### 3.1 Scale task → Dosing task: `ScaleSample` (queue, depth 8)

```c
struct ScaleSample {
  float    grams;            // filtered/calibrated weight
  int32_t  raw_adc;           // raw ADC code, for telemetry/debug
  bool     stable;            // ADS1232 stability flag (ring-buffer settled)
  uint32_t sample_seq;        // monotonic, wraps at ~4B — for drop detection
  uint32_t millis;             // producer-side millis() at sample time
};
```

- Scale task's loop: poll `scale.readADCIfReady()` (already non-blocking,
  D6 — keep as-is) on a tight period (see §6.1 for the rate justification),
  and on every new conversion, `xQueueSend(scale_q, &sample, 0)` — **zero
  timeout**. If the queue is full (dosing task somehow stalled), drop the
  oldest sample rather than block the sampler: `xQueueReceive(scale_q, &discard, 0)`
  then retry the send once. Increment a `dropped_samples` counter (exposed
  via telemetry) so a stall is visible instead of silent.
- Dosing task drains this queue with `xQueueReceive(scale_q, &sample, 0)`
  in a tight non-blocking loop at the top of every FSM tick, processing
  *every* sample (cheap: FSM/topup-model update is O(1) per sample), so
  under normal operation the queue never approaches full. Depth 8 is
  generous headroom (≈8 sample-periods, see §6.1) against a transient
  preemption by a higher-priority... there is none above scale sampling in
  our band, so 8 is pure margin, not a load-bearing number.
- Dosing task always drains this queue regardless of FSM state (IDLE,
  SCREENSAVER, DEBUG included) — so it always has a current weight for
  every display mode without Scale needing a second output channel.

### 3.2 Dosing task → Display task: `DisplayCommand` (mailbox, length 1)

```c
enum class DisplayMode : uint8_t {
  BOOT, IDLE, CONFIRM, TARE, GRINDING, TOPUP, STOPPING,
  FINALIZE, SCREENSAVER, DEBUG, OTA_UPDATE,
};

struct DisplayCommand {
  uint32_t     seq;                       // monotonic frame id
  DisplayMode  mode;
  float        current_grams;
  float        target_grams;
  float        elapsed_s;
  uint16_t     current_color;
  uint16_t     target_color;
  uint16_t     time_color;
  uint16_t     connection_indicator_color; // 0 = none; ALWAYS part of this struct
  // screensaver-only
  uint32_t     idle_h, idle_m, idle_s, idle_ms;
  bool         idle_is_uptime;             // vs. time-since-last-coffee
  // debug-only
  int32_t      debug_raw_adc;
  bool         debug_stable;
  char         debug_ip[16];
  // ota-only
  uint8_t      ota_percent;
};
```

- `xQueueOverwrite(display_mailbox, &cmd)` — never blocks, always "latest
  wins". Dosing task constructs a new `DisplayCommand` on every FSM tick
  where something render-relevant changed, but does **not** need to
  rate-limit its own send rate to protect itself (overwrite is O(1) and
  non-blocking) — rate-limiting for the ST7735's own sake happens in
  Display task (§6.2), not here. This keeps the producer dead simple.
- **This is the direct fix for AR-012.** The connection-indicator color is
  a first-class field of the one struct the whole display is redrawn from
  — there is no separate side channel that "forgot" to be part of
  change-detection, because there's only one channel and it always carries
  full state. Display task's diffing compares the *whole* incoming
  `DisplayCommand` against the last one it rendered, symmetrically
  (erase-on-change is automatic if every field, including the indicator, is
  in the same diffed struct) — **this also directly fixes AR-018**: there
  is one rendering discipline, applied uniformly to every `DisplayMode`,
  including what is today the `CONFIRM` layout's full-clear holdout.
- Network task also writes into this mailbox for `OTA_UPDATE` frames (it's
  the one narrow case where a second task legitimately needs to drive the
  display — see §7 for why this doesn't violate the single-writer rule).

### 3.3 Buttons: ISR → Input task → Dosing task

**ISR (all three buttons, symmetric — this is the concrete AR-001/007/008 fix):**

```c
struct ButtonEdge {
  ButtonId  button;   // LEFT, RIGHT, BACK
  bool      level;    // pin level at the edge
  uint32_t  millis;   // ISR-local millis(), nothing else touched
};

template <ButtonId B, bool ACTIVE_LEVEL>
void IRAM_ATTR button_isr() {
  ButtonEdge e{ B, digitalRead(PIN_FOR<B>), millis() };
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(button_edge_q, &e, &woken);
  portYIELD_FROM_ISR(woken);
}
```

Every button — including `back` — is wired through the *identical*
template instantiation, attached on `CHANGE` (both edges), pushing into one
shared queue (depth 8 — a few bounce edges per press, comfortably bounded).
**No global `state`/`button` is touched. No settings are read.** This alone
closes AR-001's core hazard (ISR-writes-shared-global) and removes the
structural asymmetry AR-007 found between `back` and `left`/`right` (today
`back` is the *only* one with no debounce and no state gating in the ISR —
in this design no ISR does gating at all, so there is nothing to be
asymmetric).

**Input task** owns a small per-button debounce state machine (mirrors
today's `loopButtonFilter`: resample after a minimum hold time before
treating a press as real), driven off its own `SettingsSnapshot` mailbox
(§4) for `button_debounce_ms` / the 20ms min-hold constant — **not** read
from ISR context, closing the ISR-reads-settings half of AR-009 too. It
consumes `ButtonEdge`s and emits, only for edges that survive debounce +
hold-time verification:

```c
struct ButtonPress {
  ButtonId button;
  uint32_t pressed_at_ms;   // when the debounced press was confirmed
};
```

`xQueueSend(button_press_q, &press, 0)` (depth 4, human-timescale events —
never expected to fill). **Every button press, `back` included, goes
through the same hold-time verification before Dosing ever sees it** — this
is the direct fix for AR-008: there is exactly one debounce/filter code
path, used for every press regardless of current FSM state, so "the second
press in CONFIRM skips the filter the first press got" is no longer
possible — there is no per-state special case in the Input task at all,
only in what Dosing *does* with a press once it arrives (see next).

**Dosing task** interprets `ButtonPress` according to its *own* current FSM
state (this is where CONFIRM's "same button = confirm, different = cancel"
logic and BUTTON_PRESSED's debug-mode dispatch belong — state-dependent
interpretation lives in the one task that owns the state, not smeared
across ISR/Input/loop like today). This also resolves AR-017: since Input
no longer special-cases `back` vs `left`/`right` at the signaling layer,
implementing (or deliberately not implementing) left/right behavior in
debug mode is a one-line decision in Dosing's `DEBUG` case, not dead code
inherited from an ISR wiring accident.

### 3.4 Dosing task → Telemetry task: `TelemetryEvent` (queue, depth 32, drop-oldest-on-full, zero-timeout send)

```c
enum class TelemetryType : uint8_t {
  TARGET, PROGRESS, RAW_SAMPLE, TOPUP_PULSE, FINALIZE, COMPLETE, LOG_LINE,
};

struct TelemetryEvent {
  TelemetryType type;
  uint32_t      session_id;      // assigned by dosing_task at CONFIGURED entry
  uint32_t      runtime_ms;      // session-relative
  float         grams;
  float         target_grams;
  float         delta_grams;     // TOPUP_PULSE only
  int32_t       raw_adc;
  bool          stable;
  char          log_line[96];    // LOG_LINE only
};
```

- Send is **always** `xQueueSend(telemetry_q, &ev, 0)`; on failure
  (queue full), drop the event and increment a counter — dosing control
  must never stall waiting on telemetry, per principle #4. A queue depth of
  32 at the current telemetry cadence (progress/raw-data roughly per FSM
  tick during RUNNING, i.e. every few ms, plus occasional log lines) gives
  Telemetry task a few tens of ms of slack — generous relative to its own
  priority-2 scheduling, since it just needs to drain faster than it fills
  on average, not guarantee zero loss under sustained overload.
- **This directly fixes AR-021.** `TelemetryType::TOPUP_PULSE` is only ever
  constructed in the exact code path that follows a real topup pulse
  (grinder ran, then stopped, then a stable post-pulse reading was taken)
  — there is no "first iteration of TOPUP state, before any pulse has
  fired" code path that can accidentally emit one, because that transitional
  moment doesn't correspond to any `TelemetryType` variant at all in this
  design (today's bug is literally that `metrics.sendTopUp()` is called
  once too early; here, the event doesn't exist to send until the real
  pulse has happened). `session_id` + explicit `type` also directly satisfy
  topup-model.md §6's schema recommendation (`session_id` FK,
  `event_type` enum tagged at write time) — the firmware and the eventual
  DB schema agree from day one instead of needing the DB layer to
  reconstruct sessions/event-types heuristically the way the topup-model
  analysis had to.
- Telemetry task forwards a subset (PROGRESS/TOPUP_PULSE/LOG_LINE etc., at
  whatever throttle it wants — mirroring today's ~6-7Hz metrics cadence) to
  Network task's WS-broadcast inbox (a small queue Network drains and calls
  `ws.textAll()` on, per §7), and separately accumulates a per-session
  buffer keyed by `session_id` for the eventual PostgREST POST fired on
  `COMPLETE`.

### 3.5 Dosing task ↔ Settings task

Covered fully in §4/§5 (settings snapshot broadcast, and the topup-model
persistence round trip) since it's one coherent mechanism, not a single
message.

### 3.6 Network task → Dosing task: `DoseRequest` (queue, depth 2)

```c
struct DoseRequest {
  float    requested_grams;
  uint32_t request_id;
};
```

`/api/getDosage`'s handler (running in AsyncWebServer's callback context,
effectively "another task" per principle #2/#6) does **only** cheap
syntactic validation before enqueueing — reject (HTTP 400, proper JSON
error body) anything that isn't a finite, positive number in a sane range
(e.g. `0 < grams <= 40`), rather than `toFloat()`'s silent-zero-on-garbage
behavior. This is the direct fix for the input-validation half of AR-015;
the unescaped-JSON-concatenation half is fixed by using a real JSON
serializer (ArduinoJson, already a reasonable dependency for the settings
snapshot wire format) for every HTTP/WS response instead of string
concatenation — a general rule for the Network task's whole surface, not
just this endpoint.

Dosing task, on receiving a `DoseRequest`, computes the corrected target
using the **same** `computeCorrectedTarget(mode, requested_grams)` function
the button path uses (single/double dose margins, or an API-specific
margin setting if the owner wants one — but *one* function, one code path,
not a second hardcoded constant). This is the direct fix for AR-004. Dosing
task re-validates the range defensively (defense in depth — Network task
validating doesn't remove Dosing's own responsibility to never act on a
nonsensical target).

---

## 4. Settings/NVS access pattern (the AR-009 fix, concretely)

**Settings task owns the canonical `Settings` struct in its own task's
memory.** It is the only task that ever assigns into its fields, and the
only task that calls into the NVS/`Preferences` API.

```c
struct SettingsSnapshot {          // plain, self-contained, no pointers —
  uint32_t version;                 // safe to copy across tasks
  Scale    scale;                   // same fields as today's Scale struct,
  WiFiCfg  wifi;                    // see WebSocketSettings.h, minus `is_changed`
};                                  // (that flag becomes unnecessary — see below)
```

**Distribution — one length-1 overwrite mailbox per subscriber**, not one
shared mailbox everyone peeks (peeking a shared mailbox from multiple
readers is safe for the data itself, but gives no way to know "have I seen
this version yet" without extra bookkeeping per reader; a private mailbox
per subscriber sidesteps that, and costs nothing extra — ~150-200 bytes of
RAM per subscriber, trivial on ESP32):

- `settings_mailbox_scale` → Scale task (needs `calibration_factor`,
  `read_samples`, `speed`, `gain` — replaces the current `is_changed` +
  `setupScale()` re-init dance: Scale task simply notices `version` changed
  since its last read and re-applies calibration/ring-buffer config).
- `settings_mailbox_dosing` → Dosing task (needs essentially every `Scale`
  field: dose targets, margins, timeouts, rate bounds, topup-model priors).
- `settings_mailbox_input` → Input task (needs only `button_debounce_ms`
  and the hold-time constant).
- `settings_mailbox_network` → Network task (needs `WiFiCfg` reset/reboot
  flags, and re-broadcasts the snapshot to WS clients for the live settings
  UI).

On boot, Settings task loads from NVS (with the versioned-migration
discipline recommended below), then pushes the initial snapshot into every
mailbox before signaling `SETTINGS_LOADED` on the readiness event group
(§8) — every subscriber's first read is guaranteed to be a valid, fully
loaded snapshot, never a default-constructed placeholder racing against
`setup()` the way `settings.scale.*` can be read mid-load today.

**Write path (AR-009's actual fix):** Network task's WS/HTTP settings
handler parses the incoming payload (cheap syntactic checks only) and sends
a `SettingsWriteRequest` to Settings task:

```c
struct SettingsWriteRequest {
  uint16_t field_id;      // enum identifying one Scale/WiFi field
  union { float f; uint32_t u; bool b; } value;
  uint32_t request_id;    // for an ack/error reply back to the WS client
};
```

Settings task applies the write to its **own private copy** of the struct
(nobody else has ever seen this memory), running the authoritative
validation for that `field_id` (AR-016's fix lives exactly here: `rate_default`,
`rate_min_valid`, any topup-table entry, any timeout — anything used as a
divisor or a timing bound gets an explicit range/non-zero check; an invalid
write is rejected with an error reply, not silently accepted). On success:
increment `version`, call `saveScaleToEEPROM()`'s NVS-based successor, and
`xQueueOverwrite` the new snapshot into every subscriber mailbox. **At no
point does any other task's context touch the struct being written** — this
is the structural difference from today's `handleWebSocketText()` writing
`scale.*` directly while `loop()`/an ISR reads it.

**Settings versioning (closes AR-005/AR-010):** the NVS-persisted struct
gets a real `schema_version` field plus small explicit per-version upgrade
functions (`upgrade_v1_to_v2(...)`, etc.), not a byte-prefix-compatibility
assumption. On a version mismatch with no upgrade path registered, or on a
sanity-check failure (e.g. `isfinite(calibration_factor)` false, matching
the AR-010 blank-EEPROM NaN scenario), Settings task falls back to
compiled-in defaults and logs a `LOG_LINE` telemetry event flagging it —
never feeds a NaN/garbage value into `scale.setCalFactor()` etc.

---

## 5. Topup model ↔ task design

topup-model.md §4.2's two RLS models need (a) a per-sample update during a
grind and (b) small, infrequent NVS persistence between sessions. Given the
ownership rules above, the natural home is:

- **The hot, running model state lives inside Dosing task**, as plain local
  state (not behind any queue) — because it's updated on *every*
  `ScaleSample` Dosing task already receives (§3.1) while `RUNNING`/`TOPUP`,
  and Dosing task is already the sole owner of the grinder relay and the
  gap/duration decision it feeds. There is no cross-task hop on the hot
  path at all: sample arrives → RLS update (a handful of float multiplies,
  O(1)) → (if in TOPUP) recompute the next pulse duration → write relay
  GPIO directly. This is deliberately the *shortest possible* path for
  exactly the logic that has real-time stakes (AR-011's bug lived here).
- **The cold, persisted copy (`TopupModelV1`, per topup-model.md §4.3) is
  owned by Settings task**, loaded from NVS at boot and pushed to Dosing
  task once via a dedicated mailbox (`topup_model_mailbox`) before Dosing
  leaves its `BOOT` state (gated on the same readiness event group, §8).
  Dosing task seeds its in-RAM RLS state from that snapshot (the
  cold-start/prior blending topup-model.md §4.4 describes) and never reads
  the mailbox again mid-session.
- **Write-back is a session-boundary event, not a per-sample one.** At
  `FINALIZE` (or periodically, e.g. every N sessions, if the owner wants
  faster convergence to be persisted sooner — a tunable, not a hardcoded
  choice), Dosing task sends one `PersistRequest` to Settings task:

  ```c
  struct PersistRequest {
    uint16_t blob_id;              // TOPUP_MODEL_V1
    TopupModelV1 payload;          // ~40 bytes, per topup-model.md §4.3
    uint32_t request_id;
  };
  ```

  Settings task validates (`isfinite()`, plausibility bounds per
  topup-model.md §4.4 — negative slope, deadtime > 1s, etc. get rejected
  rather than persisted, mirroring the same discipline as settings writes)
  and commits to NVS. This keeps NVS write frequency at "once per grind
  session" (seconds-to-minutes cadence in practice), nowhere near flash
  wear-leveling concerns, and keeps every flash write behind the one task
  that's allowed to do them.

This gives the model exactly what topup-model.md asks for — per-sample
in-session updates, cross-session persistence — without ever putting a
queue hop or a flash write on the per-sample critical path.

---

## 6. Timing/priority reasoning

### 6.1 Scale sampling

The ADS1232 in this design is assumed run in its faster conversion mode
(the `ADC_SPEED_PIN` wiring already present in `include/defines.h` implies
this is a deliberate choice, not a default) — datasheet-class ADS1232 fast
mode is commonly cited around 80 SPS, i.e. a new conversion roughly every
12.5ms. **This number should be confirmed against the actual vendored
driver once it's ported in per D6**; treat it as the working assumption
behind the figures below, not a verified constant. `read_samples = 8`
ring-buffers roughly 100ms of settled averaging on top of that.

Scale task polls `readADCIfReady()` (already non-blocking) on a short fixed
period — recommend `vTaskDelay` of ~2ms (i.e. ~500Hz poll rate), which
oversamples the ~80Hz conversion rate by >6x, so the worst-case latency
between "a conversion becomes ready" and "the task notices it" is ~2ms,
negligible against every downstream timing budget in this design. This is
cheap: the poll itself is a fast register/SPI check, not a blocking read,
so a 2ms period costs essentially nothing in CPU even at the app's highest
priority band.

**Priority 9 (highest in our app band).** Missing a ready conversion isn't
catastrophic (the ring buffer just has one fewer sample that cycle), but
there's no reason to risk it: nothing else in the system has a legitimate
claim to preempt a sub-millisecond hardware poll, and giving it the top
slot means its timing is never a variable anyone else has to reason about.

### 6.2 Dosing/session control

**Priority 8**, immediately below Scale. This is the task AR-011's bug
lived in, and the one with actual safety stakes (a relay driving a motor).
Its job per tick is bounded and cheap (drain the scale queue, O(1) RLS
update, a handful of comparisons, at most one GPIO write) — the timing
concern isn't its own execution time, it's making sure nothing lower-value
(display redraw, a WS broadcast, a settings write) can delay it. Priority 8
guarantees it preempts Display(4)/Settings(3)/Network(3)/Telemetry(2)
immediately whenever it has work, while still yielding cleanly to Scale(9)
and Input(7) — button presses (e.g. an emergency "back to cancel") should
also be able to interrupt a display refresh promptly, hence Input sitting
above Display too.

**AR-011's specific fix, restated as a design rule**: the RUNNING-state stop
check compares against exactly one variable — `target_grams_corrected` —
for *both* the rate/time estimate and the raw-weight fallback safety check.
There is no second "fallback" variable to accidentally diverge from the
first, because the design has only one canonical "what weight means stop"
value in scope at the comparison site. (Whether that value should include
the topup margin needs the owner's confirmation per AR-022's tolerance
discussion, but *whichever* value is chosen, it's used everywhere a stop
decision is made — the bug class, not just the specific instance, is closed.)

### 6.3 Input

**Priority 7.** Button response should feel immediate to a human (sub-50ms
is imperceptible as a delay; the existing 20ms min-hold-time filter is
itself the dominant term in perceived latency, not scheduling jitter), and
a `back`-to-cancel press during a grind is a safety-relevant interaction
that shouldn't be able to queue up behind a display frame. Below Dosing
because Dosing's own relay-timing correctness always wins a scheduling
conflict; above Display/Network/Settings/Telemetry because none of those
have any business delaying a button response.

### 6.4 Display

**Priority 4.** D3 wants "smooth, flicker-free, high-framerate" on an
80×160 ST7735 — but this is a small panel with small partial-redraw
regions (a few tens of pixels per changed field, per the existing
`fillRect`-only-what-changed discipline this design keeps and makes
uniform, §3.2/AR-018). Human motion-smoothness perception for this kind of
numeric/counter animation plateaus around 24-30fps; even a conservative
SPI clock comfortably redraws these small regions well inside a 33ms frame
budget. Recommend Display task render "as fast as new `DisplayCommand`s
arrive, but no more often than every ~33ms" (a simple `vTaskDelayUntil`
floor) — this bounds needless SPI traffic without capping perceived
smoothness, and its own low priority means a burst of Dosing/Input activity
never gets stuck behind a slow frame.

### 6.5 Settings/NVS and Network

**Priority 3 each.** Both are inherently bursty/event-driven (a settings
save, a WS message, OTA progress) rather than periodic, and neither has any
timing relationship to the grinder relay. Kept above Telemetry because a
settings write or a WS response should still feel responsive to a human at
the browser, just not responsive enough to matter next to the control loop.
A flash write's occasional multi-millisecond stall is fully contained at
this priority — it can never preempt or delay Dosing/Input/Scale, and even
Display only loses a few ms of its 33ms budget in the rare case they
coincide.

### 6.6 Telemetry

**Priority 2, lowest of the app tasks.** Explicitly best-effort per
principle #4 — progress/raw-data logging and the eventual PostgREST POST
are valuable but never load-bearing for correct grinder behavior; the
drop-oldest-on-full queue policy (§3.4) means a slow HTTPS POST backs up
telemetry, not the control loop.

### 6.7 Core assignment

**Core 1 (APP_CPU): Scale, Dosing, Input, Display.** These four are the
tasks with genuine timing sensitivity (safety-critical relay control,
hardware polling, human-perceptible responsiveness). Arduino-ESP32's own
`loopTask` defaults to core 1 for exactly this reason — core 0 hosts the
WiFi/BT radio stack's own internal tasks, which can have occasional latency
spikes servicing radio interrupts. Isolating the control/display quartet
from that keeps their jitter independent of network activity.

**Core 0 (PRO_CPU): Network, Telemetry, Settings.** Network and Telemetry
are inherently coupled to the WiFi/TCP stack already resident on core 0
(fewer cross-core handoffs for the high-volume WS traffic). Settings isn't
timing-critical either way — it's grouped here mainly to free core 1
entirely for the real-time quartet; correctness doesn't depend on this
choice since FreeRTOS queues/mailboxes are core-agnostic and the priority
scheme already guarantees Settings can never preempt Dosing regardless of
core.

---

## 7. Network/web seam (bounded, not designed here per the task brief)

Per the brief, the SPA/single-multiplexed-WebSocket consolidation (D4) is
separate work. What this design commits to as the seam Network task must
honor:

- **Inbound to Network task**: a `ws_broadcast_q` (queue, depth ~16) that
  Telemetry task pushes pre-formatted broadcast payloads into; Network
  task's job is just "drain this queue, `ws.textAll()` each entry, and call
  `ws.cleanupClients()` once per its own tick" — **this is the direct fix
  for AR-013** (today, no socket ever calls it). With D4 collapsing five
  sockets into one, there is by construction only **one** connection-limit
  policy to pick (recommend something like 4 concurrent clients — covers a
  live-view tab, a settings tab, and headroom for a second device — enforced
  in the single socket's `onEvent` handler), closing AR-014's
  inconsistent-caps problem structurally rather than by picking one number
  and hoping the next socket added remembers to match it.
- **Outbound from Network task**: `SettingsWriteRequest` (§4) and
  `DoseRequest` (§3.6) to their respective owners; Network task itself never
  reaches into another task's memory. This is the rule that has to hold for
  AR-009's fix to survive the D4 rewrite: **AsyncWebServer/WS callback
  context may only read from a mailbox it's a registered subscriber of, and
  enqueue request messages — never write into another task's state
  directly**, no matter how the SPA's endpoints get reorganized later.
- **OTA**: Network task pumps `ArduinoOTA.handle()` on its own schedule
  (decoupled from Dosing's FSM, unlike today's `loopIdle()`-only pumping),
  and drives `DisplayCommand{mode=OTA_UPDATE, ota_percent=...}` directly
  into the display mailbox during a flash (the one deliberate second writer
  to that mailbox — see below). **Safety recommendation**: Network task
  sets a bit on the shared readiness/status event group (§8) when an OTA
  flash begins; Dosing task checks that bit every tick and, if a grind is
  in progress, immediately forces `STOPPING` (relay off) rather than
  letting an OTA apply interrupt a live grind uncontrolled. This doesn't
  change who's *allowed* to trigger OTA (that's the human hard-constraint
  in AGENTS.md, orthogonal) — it's about what the firmware does if one
  happens to land mid-grind.

  On the "two writers to the display mailbox" point: this isn't a
  single-writer violation in the sense that matters, because the two
  writers never write concurrently by construction — Dosing task doesn't
  drive `OTA_UPDATE` frames (it's not a state in its own FSM) and Network
  task only writes during the narrow, rare OTA-flash window signaled by the
  same event-group bit Dosing is watching. If tighter guarantees are
  wanted, this can be made a true single-writer channel by having Network
  task send an `OtaProgress{percent}` message to Dosing task instead (small
  queue, depth 2), and Dosing task translates that into the
  `DisplayCommand` it already exclusively owns — recommend this as the
  cleaner variant unless the two-writer version proves simpler in practice.

---

## 8. Startup sequencing (event group)

Tasks now start concurrently rather than the fully synchronous `setup()`
sequence today's code relies on. One `EventGroupHandle_t sys_events`
with bits:

```
SETTINGS_LOADED   — Settings task has loaded NVS and pushed initial
                     snapshots to every subscriber mailbox
SCALE_READY       — Scale task has completed scale.begin()/tare()
DISPLAY_READY     — Display task has completed panel init
WIFI_CONNECTED    — set/cleared by Network task (not a startup gate —
                     the device must be usable offline)
OTA_IN_PROGRESS   — set by Network task around an active flash write
```

Dosing task blocks on `xEventGroupWaitBits(sys_events, SETTINGS_LOADED |
SCALE_READY | DISPLAY_READY, ...)` before leaving its initial `BOOT` state
for `IDLE` — this replaces today's implicit ordering guarantee (`setup()`
runs everything serially before `loop()` starts) with an explicit one that
still holds once initialization is spread across concurrently-starting
tasks. `WIFI_CONNECTED` is deliberately *not* part of the startup gate —
the grinder must work with no network present, matching current behavior
(WiFiManager's portal is the one case the device blocks on, and that's
already handled inside Network task's own startup, independent of Dosing).

---

## 9. AR traceability summary

| AR | Closed by |
|---|---|
| AR-001 (ISR writes shared `state`) | ISRs only `xQueueSendFromISR` a POD `ButtonEdge`; no global state written from ISR context anywhere (§3.3) |
| AR-004 (hardcoded API correction) | `DoseRequest` path uses the same `computeCorrectedTarget()` as button path (§3.6) |
| AR-005 / AR-010 (no real settings migration) | `schema_version` + explicit per-version upgrade functions in Settings task; unrecognized/implausible data falls back to defaults, never raw-copied (§4) |
| AR-007 (back-button ISR has no debounce/gating) | All three buttons go through the identical ISR template + Input task debounce pipeline; no ISR does gating at all (§3.3) |
| AR-008 (CONFIRM's 2nd press skips debounce) | One debounce/hold-time code path in Input task, applied to every press before Dosing ever sees it — no per-state exception exists at the signaling layer (§3.3) |
| AR-009 (settings struct raced ISR/loop vs. web task) | Settings task is sole writer; every reader gets a private `SettingsSnapshot` mailbox, never the shared struct (§4) |
| AR-011 (fallback stop uses uncorrected target) | Single canonical stop-comparison value used at every stop-check site in Dosing task (§6.2) |
| AR-012 (connection indicator never erased / excluded from diffing) | `connection_indicator_color` is a first-class field of the one `DisplayCommand` struct Display always diffs in full (§3.2) |
| AR-013 (no socket calls `cleanupClients()`) | Network task calls it once per tick on the single consolidated socket (§7) |
| AR-014 (inconsistent per-socket caps) | One socket (post-D4), one cap policy, enforced in one place (§7) |
| AR-015 (unvalidated API input, unescaped JSON) | Network task rejects non-numeric/out-of-range `grams` with HTTP 400 before enqueueing; JSON built with a real serializer, not string concatenation (§3.6) |
| AR-016 (unvalidated `rate_default` → div-by-zero/UB cast) | Settings task is the sole authority on field validation (non-zero divisors, bounds) before ever committing/broadcasting a value; Dosing task additionally floors any settings-sourced divisor and checks `isfinite()` before any float→int cast, as defense in depth (§4, §6.2) |
| AR-017 (unreachable debug left/right cases) | Input no longer special-cases any button at the signaling layer; debug-mode left/right becomes one explicit, reachable decision in Dosing's own `DEBUG` case (§3.3) |
| AR-018 (CONFIRM layout full-clears, inconsistent with other layouts) | One diffing discipline in Display task, applied uniformly to every `DisplayMode` because every mode's state flows through the same `DisplayCommand` struct (§3.2) |
| AR-021 (spurious first TOPUP telemetry event) | `TelemetryType::TOPUP_PULSE` is only constructible from the real-pulse code path; the mislabeled transitional moment has no corresponding event at all in this design (§3.4) |

Not closed by this document (needs owner input / separate work, noted for
completeness): **AR-002** (rotate the leaked DB password when
`coffee_grinder_api` is decommissioned — infra, not task architecture),
**AR-006** (pin-sharing verification — hardware wiring fact, not a
task-boundary issue), **AR-019** (topup lookup-table bucket duplication —
moot once D7's model replaces the lookup table entirely), **AR-020** (stale
OTA IP in `platformio.ini` — trivial config fix, unrelated to tasking),
**AR-022/AR-023** (accuracy-target achievability — topup-model.md's
concern, not this document's; this design just gives that model an
unblocked per-sample update path and a safe persistence path, per §5).

---

## 10. Open questions / left to the implementer

- Exact `min_topup_gap`, pulse-aim-fraction, and other topup-model tunables
  are topup-model.md's concern (§4.5 there), not this document's — Dosing
  task just needs to host whatever the final formula is; the architecture
  doesn't constrain it beyond "O(1) per sample, no blocking calls."
- Whether the API-triggered dose gets its own configurable correction
  margin or reuses single/double margins is a product decision (flagged in
  AR-004's original note) — this document only guarantees there's one
  function to change, not what the function should compute.
- The exact stack sizes and queue depths above are starting points with
  stated reasoning, not final — tune with `uxTaskGetStackHighWaterMark()`
  and observed queue high-water marks during bring-up.
- Whether OTA-during-grind should abort the grind (§7) or simply be
  disallowed while `dosing_task` is outside `IDLE`/`SCREENSAVER` (reject
  the OTA start instead of aborting a grind in progress) is worth the
  owner's input — this document recommends abort-and-stop as the safer
  default given a live relay is involved, but "refuse to start OTA during
  a grind" is a defensible alternative with different tradeoffs.
