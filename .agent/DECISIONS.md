# Decisions log

Condensed record of the decisions reached during the initial planning
interview (2026-09-11), in the order they were settled. Each entry: the
decision, why, and what it forecloses. Add new entries at the bottom as
real architectural decisions get made during the rewrite; don't log routine
implementation choices here — this is for things a future reader would
otherwise have to reverse-engineer from the code or ask about again.

---

**D1 — Firmware-only rewrite, hardware fixed.**
Same ESP32 board, display, ADC, buttons, relay as today. No case or wiring
changes. Rationale: it's a hobby project already built into a real grinder;
the ambition is code quality and behavior, not a hardware revision.

**D2 — RTOS = explicit FreeRTOS tasks inside Arduino-ESP32, not a framework
switch.** Arduino-ESP32 already runs on FreeRTOS (the current `loop()` is
itself one task). "RTOS pattern instead of a state machine" means
decomposing into multiple tasks (scale sampling, dosing control, display,
network, logging) communicating via queues, replacing the single
`switch(state)` loop and its racy ISR-writes-global-variable pattern.
Zephyr was considered and rejected: it would force rewriting WiFi
provisioning (WiFiManager), the web server (ESPAsyncWebServer), and the
display driver (Adafruit GFX/ST7735) from scratch, none of which have
mature Zephyr equivalents — too much budget spent on an OS migration
instead of on the parts that actually matter to the owner.

**D3 — Display ambition is bounded by the real hardware.** The physical
display is a 80×160 color TFT (ST7735) — small by modern standards. "Cool,
high-framerate, high-resolution graphics" means making that specific
hardware look as good as it possibly can (smooth animation, zero flicker,
crisp typography), not swapping in a bigger panel.

**D4 — Web app: full SPA, LittleFS-served, single consolidated realtime
channel.** Contrary to the README's framing, no page was actually being
served live and unified before — `/console` was real (PROGMEM HTML), but
`dev/graph` and `dev/settings` were local-only tooling talking to a mock
Python server with fake data, not the device. The rewrite builds a real SPA
(framework TBD by whoever implements it — "use libraries, go all in," the
owner is happy to learn the pattern), built to a LittleFS partition,
OTA-updatable as its own filesystem image, served at `eureka.local`. The
five current separate WebSocket endpoints collapse into one multiplexed
channel — there was never a principled reason for them being separate,
just organic growth.

**D5 — mDNS hostname `eureka.local`, no OTA password.** Accepted risk,
home LAN only. Orthogonal to the separate hard rule that only the owner
may trigger an actual OTA deploy (see `AGENTS.md`).

**D6 — Keep the existing ADS1232 driver, vendor it in (drop the
submodule).** It's a hand-tuned, non-blocking, ring-buffered driver the
owner considers genuinely superior to generic alternatives (HX711-style
libraries, ADS123X). "Use libraries where possible" applies to the parts
the project isn't already expert in, not to a working, accuracy-critical
ADC driver.

**D7 — Topup/dosing: data-driven, recency-weighted model, not a bigger
lookup table.** ~7,647 rows in `topup(runtime_ms, weight_increment)` and
~121,847 rows in `progress(runtime_ms, weight)` are available and directly
useful without needing the (missing) target-weight linkage — both describe
target-independent physical relationships (pulse duration → weight added;
elapsed time → flow curve). Recency weighting matters because bean/grind
variation drifts the underlying rates over time. Accuracy target: 80% of
sessions "spot on" (|error| < 0.05g, i.e. rounds to the same 1-decimal
display value as the target), 95% within Δ0.2g, remaining 5% may be
outliers. Overshoot is separately hard-capped at ≤0.3g and weighted as
worse than undershoot (undershoot → mildly annoying extra topup cycle;
overshoot → physically discarding ground coffee).

**D8 — `coffee_grinder_api` (external Python/Postgres logging trampoline)
is in scope, and gets replaced, not preserved.** Discovered mid-planning:
a FastAPI service on `192.168.0.112` connects *out* to the device's old
websockets and logs into Postgres on `192.168.0.111`, with its own small
web UI for browsing history. It silently discards the `target` event
(never persisted a target weight against a session — a real data-quality
gap). Renaming the device's hostname/protocol would have broken it anyway.
Decision: retire it. Device now posts directly to **PostgREST**
(auto-generated REST-over-Postgres, no custom app code) deployed on
`192.168.0.111`, using an INSERT-only Postgres role. `.112` gets
decommissioned once the cutover is verified. New schema adds a proper
`sessions` table (target, mode, linked topup/progress/raw rows) to close
the data-quality gap. A hardcoded plaintext DB password was found in that
service's `main.py` — flagged as an AR, not rotated unilaterally since
it's a live credential on a running service.

**D9 — History/analytics UI queries PostgREST directly from the browser,
not through the device.** The SPA's live view still goes through the
ESP32's websocket; anything historical (200k+ rows) is fetched
client-side straight from PostgREST on the LAN. The ESP32 has no business
aggregating that volume of data itself.

**D10 — No standalone audit report; audit-as-you-go instead.** Findings
get logged as ARs (`ARS.md`) as they're discovered during the actual
rewrite/porting work, rather than front-loaded as a separate deliverable.
The instruction that matters more than the artifact: whenever porting or
regenerating a chunk of existing logic, actively question whether it's
actually the right approach before carrying it forward.

**D11 — Never OTA-deploy to the physical device without explicit
owner sign-off.** Added after the rest of the plan was confirmed. Building
and verifying compilation is always fine and expected; flashing the real
grinder (or its filesystem image) is the owner's call exclusively, every
time, until he says otherwise.

**D12 — OTA is refused outright while a grind is in progress, rather than
aborting an active grind.** Raised by the FreeRTOS task design (AR-024):
decoupling Network (OTA handling) from Dosing (grind state) reopened
whether an OTA flash beginning mid-grind should force an immediate stop,
or simply not be allowed to start. Owner chose the latter — Network task
rejects the OTA begin while Dosing task is outside `IDLE`/`SCREENSAVER`,
rather than the design doc's originally-recommended abort-and-stop
default. See `.agent/design/rtos-architecture.md` §7 for the mechanism
this replaces.

**D13 — D7's accuracy targets stand unchanged, including 80%/Δ0.05g.**
Data analysis (AR-022) found the grinder's minimum controllable topup
increment (~0.15-0.2g, driven by clumping within the relay's ~0.3-0.4s
minimum on-time) is coarser than the 0.05g "spot on" tolerance — raised as
a question of whether the target itself needed revising. Owner reviewed
the physics and declined to relax the number: clumping during a short
topup pulse is a real, unpredictable limitation, but the *main grind*
itself is a continuous stream, not discrete clumped bursts, and isn't
subject to the same granularity floor. The path to 80%/0.05g is therefore
a much better main-grind stop estimate (so topup is rarely needed at all),
not a more precise topup pulse (which can't get better than the mechanism
allows). The owner independently found a naive `total_grams_out /
time_running` rate estimate insufficient before this was discussed —
consistent with, and validating, `topup-model.md` §4.2's proposal to fit a
continuous regression over the post-dead-time plateau rather than a single
division. No change to the 95%/Δ0.2g or ≤0.3g overshoot-cap targets either
— both were already assessed as achievable.

**D14 — Add a third small model ("Model C") anticipating post-relay-off
"coast" weight in the main-grind stop calculation.** Raised by the owner
independently of D13's discussion (AR-025): does coffee still land after
the relay switches off, and is it measurable? Investigation found yes,
solidly — median 0.49g over ~1.4s per main grind, 2.5-3x bigger than the
topup-pulse noise floor that was the previously-identified bottleneck, and
currently completely unanticipated by the firmware's stop check. Design:
a third persisted scalar (prior: flat fleet median 0.49g, recency-weighted
online update like Models A/B), subtracted from the effective stop
threshold so the relay cuts power in anticipation of what's still coming.
Purely additive to `topup-model.md` §4's design, not a redesign — see
§8 there. This is one of the more promising concrete levers for D13's
80%/Δ0.05g target specifically, since — unlike topup-pulse precision — it
isn't capped by the clumping mechanism D13 identified as a hard physical
ceiling.

**D15 — Task-per-lib layout for the FreeRTOS implementation, plus a shared
`lib/Messaging/` for message structs/queues/task-config, rather than one
large `src/` file or folding messaging into each task's own lib.**
Implementing `design/rtos-architecture.md`: house style already puts each
logical unit in its own `lib/<Name>/<Name>.h/.cpp` (`Display`, `API`,
`WebSocketSettings`, ...), so each of the 7 tasks got the same treatment
(`lib/ScaleTask`, `lib/DosingTask`, `lib/InputTask`, `lib/DisplayTask`,
`lib/SettingsTask`, `lib/NetworkTask`, `lib/TelemetryTask`), each exposing
one `createXTask()` entry point that `src/main.cpp` calls after
`initQueuesAndEvents()`. The §3 message structs, the actual
queue/mailbox `QueueHandle_t`s, the §8 event-group bits, and the §1/§6.7
priority/core/stack-size table live in one `lib/Messaging/` (`Messages.h`,
`Queues.h/.cpp`, `TaskConfig.h`) rather than duplicated per-task or bolted
onto whichever task happened to be implemented first — every task lib
includes it, none of them owns it. `src/main.cpp` itself shrinks to just
`initQueuesAndEvents()` + 7 `createXTask()` calls, matching the design
doc's spirit that no task's *implementation* belongs in `main.cpp`.

**D16 — Vendor the ADS1232 driver instead of running
`git submodule update --init` (executing D6, not superseding it).** The
submodule was never checked out in this repo (empty `lib/ADS1232/`,
gitlink present). Since D6 had already decided to vendor and drop the
submodule, the straightforward move was to do that now rather than
initialize a submodule this repo was about to delete anyway: the driver's
two source files were copied in directly from
`../2023-12-10-espresso-scale-eureka` (a second checkout of the *same* repo
history, per `AGENTS.md` — not a separate/divergent source), `.gitmodules`
and the `lib/ADS1232` gitlink were removed, and the two files were added as
ordinary tracked files. No driver code was changed.

**D17 — Dosing task's session FSM reuses the `DisplayMode` enum instead of
a second parallel state enum.** `rtos-architecture.md` §2 lists `state`
(session FSM) and treats `DisplayCommand.mode` (§3.2) as a separate field
Dosing task fills in from that state — but the two are a 1:1 mapping in
this codebase (every FSM state has exactly one corresponding display
layout; confirmed against the old `main.cpp`'s `State` enum, whose
`BUTTON_FILTER`/`BUTTON_PRESSED`/`CONFIGURED` values are absorbed into
Input task's debounce state machine and Dosing task's transient
TARE→GRINDING step respectively under the new design, leaving exactly the
same set as `DisplayMode`). Maintaining two enums that must be kept in
permanent lockstep for zero behavioral benefit seemed like exactly the kind
of "organic duplication that silently desyncs" pattern flagged elsewhere in
this project (see AR-019's lookup-table/label duplication finding for the
general shape of that risk) — so Dosing task's FSM state *is*
`DisplayMode` (`DosingState` is a type alias for it in `DosingTask.cpp`),
with `OTA_UPDATE` simply a value Dosing task never assigns (Network task's
narrow second-writer case per §7 is unaffected). If a future FSM state ever
needs to exist without a corresponding display layout (or vice versa), this
should be revisited — nothing here prevents splitting them later.

**D18 — WiFi-provisioning (AP portal) status is log-only, not shown on the
ST7735, under the new task architecture.** A side effect of the FreeRTOS
rewrite's single-writer-per-resource rule (closes the original audit's
shared-SPI-bus class of findings), not a deliberate UX decision: the old
code drew AP-portal status directly to the display from WiFiManager's own
callback, which ran outside Display's control. Under the new design,
DisplayTask exclusively owns the SPI bus, so NetworkTask can no longer draw
to the screen itself and currently just logs. Left as AR-033 rather than
fully resolved here, since it's arguably the single moment on-screen
status is most useful (no other UI exists during initial setup) — flagged
for the owner to decide whether it's worth a small `DisplayMode`/
`DisplayCommand` addition (NetworkTask sends, DisplayTask renders) or
whether log-only is acceptable given this is a one-time setup step.

**D19 — SPA framework: React + TypeScript + Vite, no CSS framework.**
D4 left "framework TBD by whoever implements it." Picked React/TS/Vite as
a genuinely modern, widely-legible stack (readable by someone learning
the pattern, per D4's framing) with a real build pipeline (`npm run
build` → `webapp/dist`, which `platformio.ini`'s `data_dir` now points
`pio run -t buildfs` at directly — see `webapp/README.md`). No Tailwind/
CSS framework: the old `dev/graph`/`dev/settings` mock pages already
established a specific dark/monospace visual language (matches the
physical ST7735 display's own aesthetic, D3) that's simpler to carry
forward as plain CSS (`webapp/src/index.css`) than to re-derive through a
utility-class system. Client-side routing is a minimal custom hash router
(`#/live`, `#/settings`, `#/history`) rather than a routing library —
three flat tabs don't need one, and hash routing sidesteps needing a
server-side catch-all fallback route for `AsyncWebServer::serveStatic`.
Chart.js was kept from the old `dev/graph` mock (bundled via npm now,
not a CDN script tag — the app must work fully offline/LAN-only, matching
the rest of this project's no-internet-dependency stance).
`board_build.filesystem = littlefs` (default.csv's `spiffs`-labeled
partition subtype is legacy naming; PlatformIO's LittleFS support mounts
the same partition region under that label regardless — confirmed
working via `pio run -t buildfs`).

**D20 — `SettingsSnapshot`'s scale-config defaults (`gain=128`,
`speed=10`, `read_samples=12`, `calibration_factor`) are this device's
real, confirmed hardware calibration, hardcoded directly — not a
migration path kept around indefinitely.** The first OTA deploy of the
rewritten firmware to the physical device exposed two stacked bugs
(AR-037, AR-038): `calibration_factor`'s own default was `0.0f`
(silently zeroing the scale), and a one-time migration added to pull
the pre-rewrite firmware's still-present EEPROM-emulated settings
forward omitted `gain`/`speed`/`read_samples` — breaking the migrated
calibration factor, since it's only meaningful relative to the ADC gain
it was measured under. Once both were fixed and the correct values
confirmed live on the device, the migration code was deleted rather than
kept: both OTA and serial reflashing leave NVS untouched, so the
migration could only ever do its job once, on this one physical unit,
and this project is a single fixed device, not a fleet — there is no
future device that would ever take that code path again. The values
themselves are now the compiled-in defaults directly. Removing completed
one-off code promptly (rather than leaving it as permanent dead weight,
coupled to an already-deleted reference module's struct layout) matches
this project's general stance on not keeping unused machinery around.

**D21 — Standing comment-style convention: self-explanatory code first,
doxygen for every declared class/struct/function/enum, plain inline
comments capped at two lines, and no bare `D`/`§`/`AR-` citations used
as the explanation itself.** Raised directly by the owner reviewing the
FreeRTOS rewrite's comments: many carried "(D12)"/"(§4.5)"/"(AR-016)"-
style tags that require opening `DECISIONS.md`, a design doc, or
`ARS.md` to understand — the opposite of the "code should speak for
itself" standard the rewrite is meant to showcase. A citation is fine
when it points to something directly openable (a real file path); a
one-letter-plus-number code that only resolves inside this project's own
internal tracking docs is not. Recorded in `AGENTS.md`'s process
expectations so it's followed automatically going forward, not just
applied retroactively once and forgotten.

**D22 — Visual design direction: iOS/macOS system colors and the
`-apple-system` font stack, everywhere, on both the TFT and the SPA.**
Owner feedback: the inherited look (flat yellow/cyan/green on the TFT,
a monospace-terminal dark theme on the SPA) reads dated ("straight out
of 1970") for a project meant to be a showcase. Rather than a bespoke
palette, both surfaces now use the same real iOS/macOS dark-mode system
colors (`systemBlue #0A84FF`, `systemGreen #30D158`, label/secondary-
label opacities, `systemGray` elevation levels) and, on the SPA, the
`-apple-system, BlinkMacSystemFont, ...` font stack -- which renders as
actual San Francisco on Apple hardware with zero font download, and a
sensible native-UI fallback everywhere else, rather than a webfont
fetched over the network (the device's SPA should keep working with no
internet access, LAN-only). The TFT's font itself is unchanged (a
custom Adafruit GFX bitmap font was considered and rejected: flash
headroom is down to ~85KB after AR-034's/AR-048's Settings-exposure
growth, and even a narrow-character-set custom font risks that budget
for a typography win the existing dirty-rect layouts can't safely
absorb without live-device visual verification of every redrawn
region). The TFT's actual improvements are the color/hierarchy change
plus new, geometrically-isolated elements added at zero flash cost: a
thin top-of-screen progress bar (grinding/topup/finalize and OTA), and
small accent underlines on BOOT/CONFIRM -- deliberately kept clear of
the tightly-fitted, already-live-verified number layouts (IDLE's fixed
decimal-point anchoring, the grinding block's dynamically centered
digit layout) rather than risk a pixel-overflow regression that
couldn't be caught without the physical device in hand.
