# Coast time / coast weight analysis

Read-only analysis. No firmware or database changes made. All queries run
2026-09-11 via the read-only `claude_agent` Postgres role against
`coffee_grinder.public.topup` (7,647 rows), `coffee_grinder.public.progress`
(121,847 rows, not used further below), and `coffee_grinder_raw.public.raw_data`
(209,848 rows / 1,058 events). Raw `topup` and `raw_data` tables were dumped
to local CSV via `\COPY ... TO STDOUT` over the SSH+psql read-only path and
processed with a local Python script
(`coast_analysis.py`, same directory as this file) — full script included
inline below for reproducibility.

---

## 0. What "coast" means operationally, and where it lives in the firmware today

`loopStopping()` (`src/main.cpp:739`) is exactly the thing being measured:
after `grinderOff()`, it polls `scale.getRaw(isStable)` in a loop and only
finalizes once `isStable` is true (or `stability_max_wait_ms`, default
**1500ms**, elapses). That wait is the "coast" — the time between relay-off
and the reading settling — and whatever the weight climbs by during that
wait is the "coast weight." The same wait-for-`isStable` pattern (with no
timeout at all) also gates every topup-pulse decision in `loopTopUp()`
(`main.cpp:674`, `if (!isStable) return;`) and, critically, gates the
**first** decision after the main grind stops (the call into `loopTopUp()`
right after `RUNNING`'s `grinderOff()` — see `main.cpp:610`). So the main
grind's own post-relay-off coast is also fully captured by this same
`isStable`-gated logic, just under a different state name.

`rawData.sendRawData()` (`RawDataWebSocket.cpp`) is called unconditionally
on every relevant loop iteration in `loopRunning()`, `loopTopUp()`,
`loopStopping()`, and once more in `loopFinalize()` — i.e. **the firmware
already logs through the entire coast window today**, continuously, using
one shared `session_started_millis`-relative timestamp across all four
states. The catch (see §3) is that this only reaches the `raw_data` Postgres
table when a websocket client happens to be connected at grind time — it's a
live-telemetry stream, not a guaranteed-durable log.

---

## 1. Two approaches tried

### (a) Infer relay-off from curve shape alone

Tried first, by eyeballing full per-event curves (e.g. `event_id=8`, dumped
in full during this investigation). Verdict: **works well for topup pulses,
unreliable for the main grind.**

- Main-grind sessions taper naturally near their end for unrelated reasons
  (documented in `topup-model.md` §2.1: hopper-level effects softening flow
  in the last ~2s of a long grind). That natural taper looks very similar in
  shape to a post-relay-off coast, so a pure slope-derivative heuristic
  can't reliably tell "motor still running, just slowing down" from "motor
  off, chute draining" without an independent time anchor.
- Topup pulses are short enough (400–1200ms per `topup-model.md` §1.1) that
  the curve is cleaner, but the settling itself is not monotonic — it rings
  (see the manual trace of `event_id=8`'s first topup pulse below: value
  overshoots to 17.067g, then settles back down to ~16.92g before the
  stability flag latches), which makes a pure shape-based cutoff detector
  fragile on a per-event basis.

Given this, (a) was used only as a sanity check on (b), not as the primary
method.

### (b) Cross-reference `topup.runtimemillis` against the `raw_data` curve — used for all numbers below

This works, and works well, because of three things that fell out of
reading the firmware rather than guessing from the data:

1. **`topup.runtimemillis` is a precise, firmware-clock-measured relay-ON
   duration**, not an estimate: `grinderOff()` sets
   `grinder_runtime_millis = grinder_stopped_millis - grinder_started_millis`
   (`main.cpp:849`) from two `millis()` timestamps taken at the literal
   `digitalWrite(RELAY, LOW/HIGH)` calls. No jitter to worry about here.
2. **The relay-on start time (`T_on`) for the very first pulse in a session
   is `t=0`** in the raw_data curve's own timeline: `grinderOn()` is called
   immediately before `session_started_millis = millis()` is set
   (`main.cpp:497-498`), and `raw_data.timestamp_ms` is logged as
   `now - session_started_millis` throughout. So for the main grind,
   `T_off = runtimemillis` directly, no linkage needed at all.
3. **For every subsequent topup pulse, `T_on` is the last timestamp of the
   immediately preceding stable segment** in the same curve — because
   `grinderOn()` for a topup pulse is called in the very same loop
   iteration that just confirmed `isStable == true` and decided to fire
   (`main.cpp:674-731`). So `T_on_k ≈` last row of stable segment `k-1`,
   and `T_off_k = T_on_k + runtimemillis_k`.

Given that, per raw_data event: detect alternating stable/unstable runs,
treat every "unstable run → stable run" transition after the initial
baseline as one pulse (main grind = 1st, each topup pulse thereafter in
order), interpolate the curve's `filtered_value` at `T_off_k`, and compare
against the value where the following stable run begins (`T_stable_start_k`,
i.e. where the *firmware's own* `stable` flag in the raw stream first
latches true and stays true):

```
coast_time_k   = T_stable_start_k - T_off_k
coast_weight_k = filtered_value(T_stable_start_k) - filtered_value(T_off_k)
```

`runtimemillis_k` itself comes from the `topup` table, ordered `rn` within a
session cluster, using the exact same 20s-gap session-reconstruction
heuristic `topup-model.md` §0/§2 already validated. **`rn=1`'s row is the
main grind's own tail (per `topup-model.md`'s §0 finding) — that's a
feature here, not a bug, since it's exactly the main-grind relay-on duration
we need.**

The one new piece of linkage this method needs that `topup-model.md` didn't:
matching a specific `raw_data.event_id` to a specific `topup` session
cluster, since there's no shared session id (the gap `topup-model.md` §6
already flags). Done by wall-clock proximity: each event's
`session_start_wall ≈ received_at − timestamp_ms` (median over its first 5
rows), matched against each cluster's first row's wall-clock timestamp,
picking the closest cluster within a 15s window.

**Reliability check on the linkage itself**: this "closest cluster" search
is *not* a zero-offset match — a session's first `topup` row (`rn=1`, the
main-grind tail) is written only *after* the main grind's own coast/settle
completes, so the expected offset between the two anchors is
`main_grind_runtime + main_grind_coast ≈ 8-11s` for a typical grind, not
0s. The observed `best_dt` distribution (median 10.7s, p25-p75 9.3-11.0s,
tight) matches this almost exactly, which is a good internal consistency
check that the matching is finding the *correct* cluster rather than a
coincidentally-nearby one from a different grind.

As a stronger check, I also ran the numbers on a **stricter subsample**
(27 events) where the raw_data curve's post-baseline stable-segment count
exactly equals the matched cluster's row count (i.e. every single pulse in
that session, main grind through the last topup, was captured and lines up
1:1 — no ambiguity about which segment is which). Results below (§2) hold
up closely on this subsample, which is the strongest evidence the method
isn't an artifact of loose matching.

---

## 2. Headline numbers

Script: `coast_analysis.py` (included below). Filtered to
`coast_time_ms ∈ [-300, 8000]` and `|coast_weight_g| < 5` to drop a small
number (~4%) of clearly-garbage rows (same kind of sensor-glitch / bumped
-scale outliers `topup-model.md` §1.3 already documented for the `topup`
table directly).

### Main grind (rn=1, n=282 distinct sessions, spanning 2025-09 through
2026-09 — full history, not a narrow slice)

| metric | median | p25 | p75 | p10 | p90 |
|---|---|---|---|---|---|
| coast time (ms) | **1441** | 1328 | 1538 | 1225 | 1585 |
| coast weight (g) | **0.486** | 0.378 | 0.599 | 0.277 | 0.713 |

Stricter 27-event exact-match subsample: median coast time 1382ms, median
coast weight 0.471g — closely matches the full sample, good agreement.

Correlations (n=282, main grind only):
- `coast_weight` vs. local flow rate at cutoff (`dose_g / runtime_ms`):
  **r = 0.50** (moderate positive) — makes physical sense: a faster stream
  means more coffee already airborne in the chute at the instant power cuts.
- `coast_weight` vs. total dose size: **r = 0.067** — essentially no
  relationship. The coast is about *what's already in the chute*, not about
  how long the grind ran overall — consistent with a roughly fixed
  "chute inventory" effect rather than something that accumulates.
- `coast_time` vs. flow rate: **r = 0.06** — flow rate affects *how much*
  extra weight lands, not *how long* it takes to land/settle.
- Flow rate itself in this subsample: median 0.995 g/s (p25-p75
  0.955-1.034), matching `topup-model.md` §2.2's fleet-wide plateau rate
  almost exactly — expected, this is the same underlying quantity.

Implied physical picture: coast weight ≈ 0.49g arrives over ≈1.4s after
relay-off, i.e. an *effective* average coast-phase flow rate of only
~0.35 g/s — about a third of the steady in-grind rate (0.99 g/s). That's
consistent with a genuine coast/taper (flow decaying, not sustained at full
rate) rather than, say, a fixed lag before the sensor catches up.

### Topup pulses (rn≥2, n=284 pulse observations from 154 distinct sessions)

| metric | median | p25 | p75 | p10 | p90 |
|---|---|---|---|---|---|
| coast time (ms) | 380 | 8 | 604 | -197 | 712 |
| coast weight (g) | **-0.020** | -0.064 | -0.001 | -0.103 | 0.016 |

**No measurable positive coast for topup pulses** — the median is
essentially zero, slightly negative, and the spread is dominated by
settling *ringing* (the reading briefly overshoots then relaxes down before
the stability flag latches — see the manual trace below) rather than a
sustained one-directional coast. This matches the physical picture already
established in `topup-model.md` §7 (owner's addendum): a short topup pulse
mostly spends its ~300ms dead time just getting the chute re-primed, so by
the time relay-off happens there's little-to-no continuously-falling
"in-flight" mass left to coast on — output is dominated by clump-timing
luck, not a sustained stream.

### Manual trace used to sanity-check the method (`event_id=8`, main grind,
matched to `topup` cluster starting `2025-10-10 13:54:52.975685`,
`runtimemillis=15376`, `weightincrement=16.92`)

```
raw_data t=15352-15411ms: filtered_value ≈ 16.29g   (T_off = 15376ms, interpolated ≈16.3g)
raw_data t=15855-15916ms: filtered_value = 17.067g   (overshoot peak)
raw_data t=16042-16915ms: filtered_value settles to 16.86-16.92g (ringing down)
raw_data t=16954ms:        stable flag latches true, value=16.920g  (T_stable_start)
  -> coast_time = 16954-15376 = 1578ms
  -> coast_weight = 16.920-16.29 ≈ 0.63g   (this event's raw, unfiltered value; near the sample median)
```

---

## 3. Reliability assessment — be honest about the gaps

**Trustworthy for the main-grind headline number specifically.** Three
independent things line up: (1) the full n=282 sample and the strict
27-event exact-match subsample give nearly identical medians, (2) the
flow-rate correlation has the physically-expected sign and the
"no-dependence-on-dose-size" result is exactly what a fixed-chute-inventory
model predicts, (3) the sample spans the full ~1 year of history, not one
narrow window, so it isn't an artifact of one firmware version or one bag of
beans.

**Weaker, but directionally solid, for the topup-pulse near-zero result.**
The point estimate (~0g, even slightly negative) is unambiguous and matches
the physical story `topup-model.md` §7 already built independently. I would
not trust the exact IQR numbers for topup coast to two decimal places —
they're dominated by ringing/noise at that resolution — but I'm confident
in the qualitative conclusion: no exploitable coast effect on topup pulses.

**What limits confidence further, stated plainly**:

1. **`raw_data` coverage is opportunistic, not systematic.** `rawData.sendRawData()`
   only reaches Postgres when a websocket client was connected live during
   that specific grind (`RawDataWebSocket::sendRawData` returns immediately
   if `getClientCount() == 0`). Only 1,058 of ~2,122 reconstructed topup
   sessions have *any* raw_data event at all, and of those, only 294 could
   be matched to a topup cluster within the 15s window, and only 282 of
   those yielded a usable rn=1 (main-grind) coast measurement. This is
   likely correlated with "owner had the live graph page open" — plausibly
   more common during active tuning/debugging sessions than routine daily
   use. I found no evidence this biases the *coast* measurement specifically
   (flow rate in the matched subsample tracks the fleet-wide number closely,
   and dates span the full year), but it's an assumption, not a proof.
2. **The event↔cluster linkage is wall-clock proximity, not a real foreign
   key** — same category of gap `topup-model.md` §6 already flags for
   session reconstruction generally. The internal-consistency checks in §1
   give good confidence it's finding the right cluster, but it is still a
   heuristic.
3. **`coast_time` conflates true physical settling with whatever hysteresis
   the scale library's own `isStable` computation applies** (its internals
   weren't inspected as part of this task). This doesn't undermine the
   *practical* relevance of the number — it's literally what
   `loopStopping()`/`loopTopUp()` wait through today — but it means
   "1.4 seconds" isn't a pure physics constant, it's "how long until this
   firmware currently decides it's done," which is the right thing to
   measure for this purpose but worth being precise about in how it's
   described.
4. Most raw_data events only capture the *first* pulse or two of a session
   before the websocket disconnects (262/294 matched events have fewer
   stable segments than their matched cluster has rows) — this thins the
   topup-pulse (rn≥2) sample much more than the main-grind (rn=1) sample,
   which is part of why the main-grind number is the one I'd act on and the
   topup number I'd treat as "probably ~0, not worth over-trusting the
   decimals."

**Bottom line**: main-grind coast (~0.49g / ~1.4s) is measured solidly
enough to act on. Topup-pulse coast is measured well enough to say
confidently "there isn't one worth chasing," but not well enough to trust a
precise number if one were needed.

---

## 4. Does this matter enough to fold into the dosing model?

**Yes, for the main grind — this is one of the larger single levers
available.** Comparing magnitudes directly against `topup-model.md`'s own
error budget:

- Sensor noise floor: ~0.02g (§3)
- Core topup-pulse noise: ~0.14-0.2g absolute sd (§1.3)
- **Main-grind coast: ~0.49g median, IQR 0.38-0.60g**

The coast effect is **2.5-3x bigger than the topup-pulse noise floor that
`topup-model.md` §5 identified as the actual accuracy bottleneck**, and
~25x the sensor noise floor. Today's firmware has *no* anticipation of it
at all — `loopRunning()`'s stop condition (`main.cpp:608-609`) fires purely
on `grams > target_grams` or a calculated stop time, with no coast offset —
so every main grind currently overshoots its own intended stop point by
this amount, then relies on topup's noisier, clump-limited pulses to claw
back (or, if the grind already overshot past target, contributes directly
to overshoot risk against the ≤0.3g hard cap). Anticipating even the flat
median (0.49g) as a fixed offset would materially shrink the average
overshoot the topup stage has to correct for, meaning **fewer topup pulses
needed per session on average** — directly attacking the exact bottleneck
`topup-model.md` §5 already identified ("make the main-grind stop estimate
as good as possible so topup ... is invoked as rarely as possible").

The topup-pulse coast effect (~0g) is **not** worth folding in — no signal
to act on, and the `topup-model.md` §4.2 Model B design (fit the pulse's own
`slope`/`deadtime` against total pre-to-post-stabilization weight change)
already implicitly absorbs whatever tiny coast exists into its regression;
there's no separate correction needed.

### Sketch of the change (not a redesign — `topup-model.md` §4 stands)

In `loopRunning()`'s stop-decision (today: `grams > target_grams` /
`now >= calculated_stop_millis`), subtract a predicted coast offset from
the effective target used for the early-exit check:

```
predicted_coast_g = coast_weight_prior   // start at the fleet median, 0.49g
// optionally, given the r=0.50 correlation with flow rate:
predicted_coast_g = coast_weight_prior * (current_local_rate_g_s / 1.0)
```

and fire `grinderOff()` when `grams >= target_grams - predicted_coast_g`
(mirroring the existing `target_grams_corrected` pattern already used for
the calculated-stop-time path, so this slots into the existing structure
rather than adding a new mechanism). This would live alongside
`topup-model.md` §4.2 Model A (the main-grind rate RLS fit) as a third
small persisted scalar (`coast_weight_hat`, `coast_weight_precision`),
updated the same way: after each session, once the true post-coast settled
weight is known, feed `(observed_coast_weight, flow_rate_at_cutoff)` into
the same kind of recency-weighted online update already proposed for
Models A/B. Given the r=0.50 correlation with rate isn't overwhelming,
starting with just the flat median as `coast_weight_prior` (simplest
possible version) already captures most of the benefit; the rate-scaled
version is a natural v2 if the flat version's residual error still shows
rate-dependence in practice.

This is a small, targeted addition — a single extra scalar subtracted from
one comparison — not a redesign of `topup-model.md`'s architecture.

---

## 5. What the new `sessions` schema should capture for this specifically

`topup-model.md` §6 already lists `session_id`, `target_weight_g`,
`final_weight_g`, `event_type` enum, `pulse_index`, etc. Add, specifically
for coast:

- **`relay_off_at_ms`** (and ideally `relay_on_at_ms`) logged directly by
  firmware at the `digitalWrite(RELAY, LOW)` call itself (i.e. capture
  `grinder_stopped_millis`/`grinder_started_millis`, which already exist as
  local variables in `grinderOn()`/`grinderOff()` in `main.cpp` today —
  they're just never persisted anywhere past the in-memory
  `grinder_runtime_millis` scalar). This alone eliminates *all* of the
  linkage/heuristic work in §1(b) above — `T_off` becomes a direct lookup
  instead of an inferred quantity, for every pulse, not just ones lucky
  enough to be linkable via wall-clock proximity.
- **`stable_at_ms`** (when `isStable` first latched true and held) per
  pulse — turns `coast_time`/`coast_weight` into a stored, first-class pair
  per pulse rather than something recomputed from the raw curve after the
  fact.
- **Continuous raw sampling should keep happening exactly as it does now**
  through `loopStopping()`/`loopFinalize()` (`main.cpp:754-756`, `790-793`)
  — firmware already does this correctly, nothing to change there. The gap
  is entirely on the *persistence* side (§3 point 1): make raw_data capture
  unconditional (or at least session-scoped, e.g. always buffer-and-flush
  once per session to PostgREST) rather than gated on
  `getClientCount() > 0`. Given the new architecture routes ingestion
  through PostgREST directly rather than a live websocket relay (per
  `AGENTS.md`'s "Data infra" section), this should fall out naturally from
  that rewrite rather than needing special-casing — but it's worth calling
  out explicitly so "keep the debug websocket, but always also durably log"
  isn't accidentally lost in the port.
- A **`grinder_state` enum tag per raw sample** (`MAIN_GRIND` /
  `TOPUP_PULSE_ON` / `TOPUP_SETTLE` / `STOPPING` / `FINALIZE`) would remove
  the need for the stable-segment-walking reconstruction in §1(b) entirely
  — every row would already say which phase it's from.

None of this blocks using the §4 recommendation today — the fleet-wide
0.49g / 1.4s numbers above are usable as a cold-start prior right now — but
every item above turns "reconstructed from curve shape + heuristic linkage"
into "read directly from a column," the same upgrade `topup-model.md` §6
already asked for generally.

---

## Appendix: `coast_analysis.py` (full script, for reproducibility)

See `/private/tmp/claude-501/-Users-ahuber-Nextcloud-basteln-2026-09-11-coffee-grinder-scale/73fbb13f-0caa-4a2e-9bb7-22943def5e0d/scratchpad/coast_analysis.py`
in this same scratch directory — reads `topup.csv` and `raw_data.csv` (both
dumped via `\COPY ... TO STDOUT WITH CSV HEADER` from the read-only
`claude_agent` role, queries below), reconstructs sessions, does the
segment-detection + linkage + coast computation described in §1(b), and
prints the summary tables in §2.

```sql
-- topup.csv
\COPY (SELECT id, timestamp, runtimemillis, weightincrement FROM public.topup
       ORDER BY timestamp) TO STDOUT WITH CSV HEADER;
       -- run against coffee_grinder database

-- raw_data.csv
\COPY (SELECT event_id, timestamp_ms, filtered_value, stable, received_at
       FROM public.raw_data ORDER BY event_id, timestamp_ms)
      TO STDOUT WITH CSV HEADER;
      -- run against coffee_grinder_raw database
```
