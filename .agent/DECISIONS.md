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
