# Topup/dosing model — design proposal (D7)

Analysis-only. No firmware or database changes made. All numbers below come
from live queries against `coffee_grinder.public.topup` (7,647 rows),
`coffee_grinder.public.progress` (121,847 rows), and
`coffee_grinder_raw.public.raw_data` (209,848 rows / 1,058 events), run via
the read-only `claude_agent` role on 2026-09-11. Query text is included
inline so the numbers are reproducible.

**Correction to the task brief**: the tables live in the `public` schema of
each database, not a `coffee_grinder` schema — `coffee_grinder` /
`coffee_grinder_raw` are the *database* names. `\d public.topup` etc.
confirmed the column layout matches what AGENTS.md/DECISIONS.md describe.

---

## 0. Headline finding before anything else: the `topup` table is contaminated

`loopTopUp()`'s very first iteration after the main grind stops always calls
`metrics.sendTopUp(grinder_runtime_millis, delta_grams)` *before* any real
topup pulse has happened — `grinder_runtime_millis` at that point still holds
the duration of the just-finished **main grind**, not a topup pulse (see
`main.cpp` lines 610–612 → 664–686: `grinderOff()` is called at the bottom of
`loopRunning()`, state moves to `TOPUP`, and the first pass through
`loopTopUp()` with `grinder_is_running == false` immediately reports whatever
`grinder_runtime_millis`/`grams_on_grinder_on` were left over from the main
grind, before the gap/lookup-table logic ever runs).

I confirmed this by reconstructing per-grind clusters from `topup.timestamp`
(new cluster whenever the gap to the previous row exceeds 20s) and comparing
row 1 of each cluster (`rn=1`) against later rows:

```sql
-- rn = position within inferred session, using a 20s timestamp-gap heuristic
 rn |  n   | avg_rt(ms) | avg_wi(g) | sd_wi
  1 | 2122 |    11019   |   11.695  | 11.194   -- matches a whole dose, not a pulse
  2 | 1845 |     1670   |    1.369  |  4.958
  3 | 1515 |      571   |    0.247  |  5.550
  4 | 1136 |      578   |    0.102  |  5.248
  5 |  653 |      563   |    0.854  |  8.308
  6 |  210 |      607   |    0.331  |  1.130
```

`rn=1`'s 11-second/11.7g averages are obviously a main-grind tail, not a
topup pulse — it's what produces the misleading "3000ms+ bucket, n=1855,
avg 14.2g" mass I first saw in the raw histogram. **Any topup model must
discard `rn=1` per cluster.** This also means the historical `topup` table
under-represents nothing about topup itself — once filtered, 5,525 rows
(`rn>=2`) are genuine topup-pulse observations, still comfortably enough to
fit a 2-parameter model. I flag this as a firmware bug worth fixing in the
rewrite regardless of the modeling work: tag events by type (`MAIN_GRIND` vs
`TOPUP`) explicitly rather than relying on call order (see §5, new schema).

All topup analysis below uses this `rn>=2` filtered set.

---

## 1. `topup`: runtime → weight-increment relationship

### 1.1 Shape

Bucketing the filtered set by 100ms runtime bins (`runtimemillis <= 2000`)
and using the **median** (means are wrecked by a long tail of outliers, see
§1.2):

| runtime bucket | n | mean Δw (g) | median Δw (g) | sd Δw (g) |
|---|---|---|---|---|
| 0–100ms | 5 | -0.018 | 0.000 | 0.065 |
| 200–300ms | 13 | 0.058 | 0.012 | 0.183 |
| 300–400ms | 149 | 0.047 | 0.021 | 0.074 |
| 400–500ms | 3,863→3,842* | 0.183 | 0.177 | 0.139 |
| 500–600ms | 536 | 0.233 | 0.236 | 0.159 |
| 600–700ms | 278 | 0.470 | 0.354 | 0.149 |
| 700–800ms | 169 | 0.594 | 0.457 | 0.149 |
| 800–900ms | 182 | 0.594 | 0.592 | 0.164 |
| 900–1000ms | 100 | 0.701 | 0.697 | 0.214 |
| 1000–1100ms | 27–29 | 0.781 | 0.825 | 0.188 |
| 1100–1200ms | 11–12 | 0.887 | 0.884 | 0.133 |

(*after clipping the bucket to `weightincrement ∈ [-0.3, 2]` to drop gross
outliers — see §1.2. Un-clipped this bucket's mean is dominated by garbage:
0.050g mean with sd=4.63 because of a handful of 38g/109g/483g entries.)

Two clean physical facts fall out of this:

- **A hard dead zone below ~300–350ms.** Pulses under ~300ms add
  essentially nothing (median ≈0). This is mechanical: the grinder/chute
  needs to spin up and a column of beans needs to reach the outlet before
  any coffee actually lands on the scale.
- **A roughly linear region from ~350ms to ~1300ms** (I didn't see enough
  data past 1300ms to trust it — n drops to single digits).

### 1.2 Fit

Ordinary least squares on the clean, deduplicated region
(`runtimemillis ∈ [400,1300]`, `weightincrement ∈ [-0.3,2]`, n=5,138):

```
slope     = 1.053 g/s
intercept = -0.335 g
R²        = 0.401
```

The negative intercept is exactly the dead-time term: solving
`0 = slope·t + intercept` gives `t ≈ 318ms`, which lines up almost exactly
with the empirically observed dead-zone transition (300–400ms) above. So the
right functional form is **not** `weight = slope · t`, it's:

```
weight_added(t) = max(0, slope · (t - deadtime_ms))
```

with `slope ≈ 1.05 g/s`, `deadtime_ms ≈ 300–320ms` as the fleet-wide fit.

### 1.3 Noise and outliers

Within a fixed 100ms runtime bucket, the **core noise is ~0.14–0.21g
absolute standard deviation** (see sd column above) — this doesn't shrink
much as duration grows, so relative noise is worst at short pulses (the ones
that matter most for fine correction).

On top of that core noise there's a real garbage tail: of the 5,525 filtered
"real topup" rows,

```sql
n_extreme_outlier (|Δw|>5g)  = 199  (3.6%)
n_gt2            (|Δw|>2g)   = 205  (3.7%)
n_negative       (Δw<0)      = 337  (6.1%)
```

Max observed `weightincrement` in the raw table is 483g and min is -129g —
physically impossible for a coffee dose; these are almost certainly weight
sensor glitches, someone bumping the scale, or the portafilter being
lifted/reseated mid-topup. **A production model must use robust fitting
(trimmed / Huber / simple bound-clipping), not plain least squares on raw
data** — plain OLS on the unfiltered set would have the fit dragged around
by a handful of 3-digit-gram entries.

Interestingly, this ~4–6% garbage rate lines up almost exactly with D7's
"remaining 5% outliers tolerated" clause — a reasonable coincidence to note:
the accuracy target's slack band appears to already account for roughly the
rate of genuinely-unmodelable pulses the mechanism produces.

### 1.4 The structural implication for overshoot (important)

At the **shortest usable pulse length** (400–500ms, n=3,911 after clipping),
I checked directly what fraction would already breach the hard overshoot cap
on their own:

```sql
pct_over_0.3g = 20.9%
pct_over_0.2g = 44.9%
p25 = 0.074g, p75 = 0.280g, p90 = 0.360g
```

**One in five of the shortest usable topup pulses in the historical data
added more than 0.3g by itself.** This is a hard physical ceiling, not
something a smarter regression fixes: if the remaining gap before a topup
decision is smaller than roughly the mean output of the shortest controllable
pulse (~0.18g, with a ~0.14g sd), *any* pulse you fire has a meaningful
chance (order 15–20%) of overshooting past the cap on that pulse alone. See
§4 for how this drives the model design directly.

---

## 2. `progress`: flow-rate curve during the main grind

`progress.runtimemillis` is already time-since-that-grind's-start, but rows
from different grinds are interleaved with no session id. I reconstructed
session boundaries by walking rows in insertion order and starting a new
session whenever `runtimemillis` resets close to 0 (`<300ms`) after a
previous value `>2000ms` (verified by inspection — the first rows of the
table show sessions restarting `runtimemillis` from ~20ms each time, with
~100ms sample cadence within a session). This yielded 1,460 plausible
sessions (`n≥10` rows, duration 2–30s) out of 3,503 raw cluster-candidates
before filtering (the extra ones are short/degenerate fragments, e.g.
aborted grinds or single stray rows).

### 2.1 Time-bucketed pooled rate (all sessions overlaid)

Local rate `= Δweight/Δt` computed row-to-row within each session, then
pooled into 500ms time-since-start buckets (median far more informative than
mean here — mean is wrecked by ADC-noise instantaneous-rate spikes when
`Δt` is small):

| time since grind start | median rate (g/s) |
|---|---|
| 0–1000ms | ~0.00 (dead time — chute not primed yet) |
| 1500–2000ms | 0.97–0.98 |
| 3000–8000ms | 0.93–0.98 |
| 8000–15000ms | 0.92–0.98 |
| 15500–17500ms | 0.81–0.92 (tapering — low n, likely hopper-level effects) |
| 18000ms+ | noisy / small n (few sessions run this long) |

So: **~0.8–1.0s dead time** at the start of a full grind (longer than
topup's ~0.3s dead time — makes sense, a full grind starts from an empty
chute, a topup pulse restarts with the chute already primed), then a
**flat plateau around 0.9–1.0 g/s** for the bulk of a normal-length grind,
then a soft taper in the (rare, long) tail.

### 2.2 Session-to-session variance (the part that actually matters for the stop-time estimate)

Per-session flow rate via linear regression of `weight` on `runtimemillis`
restricted to the 2000–15000ms plateau window, one slope per session
(n=1,408 sessions with ≥5 points in that window):

```
mean   = 1.014 g/s
median = 1.018 g/s
p10    = 0.918 g/s
p90    = 1.098 g/s
```

Core session-to-session spread is a **~9% band (p10–p90) around 1.0 g/s** —
real bean/grind-setting variation, not sensor noise (see §3). Note this
**matches the firmware's own `rate_default = 1.0 g/s` and
`rate_min_valid/rate_max_valid = 0.5/2.0`** almost exactly — a good sanity
check that the shipped defaults were already reasonably tuned, and a strong
candidate as the cold-start prior for a new model (§4.3).

### 2.3 Recency trend (main-grind rate)

Monthly median of the per-session plateau-rate fit (filtered to
`0.3 < rate < 2.5` to drop clearly-bad session reconstructions):

```
2025-04: median 1.115 g/s   (sd 0.039, n=13)
2025-07: median 1.024 g/s   (sd 0.083, n=67)
2025-10: median 1.076 g/s   (sd 0.073, n=110)
2026-01: median 0.992 g/s   (sd 0.050, n=100)
2026-04: median 0.995 g/s   (sd 0.046, n=50)
2026-07: median 0.972 g/s   (sd 0.069, n=93)
2026-09: median 0.949 g/s   (sd 0.033, n=53)
```

There **is** a real, gradual drift: roughly **1.11 g/s → 0.95 g/s over ~17
months (~12–15% decline)**, consistent with burr wear and/or bean-supply
changes. It's slow and monotonic-ish, not a step change (one exception:
Sept 2025 shows sd=0.326, an outlier month, probably a real
bean/grind-setting change or a batch of bad session reconstructions — I
didn't chase this further). **This supports D7's recency-weighting
premise for the main-grind rate model** — a half-life on the order of weeks,
not days, is enough to track it without chasing session-to-session noise.

Interestingly, the same before/after split applied to the topup-pulse slope
(§1.2) shows **no comparable drift** — 1.048 g/s pre-Sep-2025 vs 1.062 g/s
post — essentially flat. My read: short topup pulses are dominated by the
chute-already-primed restart transient rather than sustained burr-grinding
physics, so they're less sensitive to whatever's driving the plateau-rate
drift. Practical upshot: **recency-weighting matters more for the
main-grind rate estimate than for the topup-pulse model**, though there's no
harm in applying a (longer) decay to both — it's essentially free.

---

## 3. `raw_data`: sensor noise floor and `stable` flag

`stable = true` on only 16.5% of all 209,848 rows overall — expected, since
most of a grind is *by definition* a changing weight (unstable), and
`stable` is meant to flag the settled tail once the scale finishes moving.

To get a genuine noise-floor estimate I took the last 15 rows of each event
(`event_id`) where `stable=true` (i.e., the settled tail after the grinder
stopped and the reading finished ringing down) and computed within-event
scatter, across 137 events with ≥10 such rows:

```
median sd(filtered_value) = 0.0186g
mean   sd(filtered_value) = 0.0213g
p90    sd(filtered_value) = 0.0445g
```

**Sensor noise floor is ~0.02g (median), ~2.5x tighter than the 0.05g
"spot on" target.** This is a clean, useful result: the ADS1232 + existing
filtering is not the bottleneck for hitting 0.05g. The bottleneck, per §1.4,
is the electromechanical topup-pulse granularity (dead time + ~0.14–0.2g sd
per usable pulse), not the scale.

`raw_value` and `filtered_value` scale check: consecutive samples show
`filtered_value` changes by ~0.0147g per raw ADC count — matches the
`0.014738673` value seen verbatim in `progress.weight` for what's clearly a
near-zero tare reading, confirming both tables use the same calibration.

---

## 4. Proposed model

### 4.1 What NOT to do

Not a neural net, not a lookup table with more buckets, not anything that
needs a training loop or floating tensors on-device. The physics here is a
2–3 parameter affine relationship (`weight = max(0, slope·(t - deadtime))`)
plus a scalar plateau-rate for the main grind. Anything fancier is fitting
noise, not signal — the R²=0.40 ceiling on the topup regression isn't a
modeling failure, it's the actual noise floor of a relay-controlled DC
grinder (§1.3/1.4). An ESP32 with tens of KB of free NVS and negligible RAM
budget for this doesn't need anything more than closed-form linear algebra
that updates in O(1) per new sample.

### 4.2 Recommended approach: two small online-updated linear models, recursive least squares (RLS) with a forgetting factor

**Model A — main-grind plateau rate** (`rate_hat`, g/s). Replaces the
single-point "measure once when weight crosses `rate_calculation_percentage`
of target" logic in `loopRunning()`.

- Fit incrementally *during* the grind itself: once weight is past the
  dead-time region (empirically ~800–1000ms in, or better, once the
  in-session rate estimate has stabilized rather than a hardcoded time),
  keep a running weighted-least-squares fit of `weight` vs. `runtimemillis`
  over the current session's own data, updated every sample or every
  ~250ms.
- Blend that in-session fit with the **persisted, cross-session prior**
  (below) using standard precision-weighted combination (equivalent to
  Bayesian linear regression with an informative prior): early in a grind,
  when the in-session fit has few points and high variance, the persisted
  prior dominates; as more of *this* grind's own data accumulates, the
  estimate shifts toward what this grind is actually doing. This directly
  fixes the real risk the current one-shot design has: committing to a
  rate estimate at exactly 75% of target (`rate_calculation_percentage`)
  bakes in whatever noise happened to be in that one window, when we know
  session-to-session rate has a genuine ~9% (p10–p90) spread (§2.2).
- Recompute `calculated_stop_millis` on every update rather than locking it
  once — cheap (O(1) per update), and lets the stop estimate keep improving
  as the grind progresses instead of freezing early.

**Model B — topup pulse response** (`topup_slope`, `topup_deadtime_ms`).
Replaces `topup_lookup_table[6]`.

- Same recursive/weighted-least-squares update, fit to
  `weight_added = max(0, topup_slope·(runtime - topup_deadtime_ms))`,
  updated after every real topup pulse (only — never on the misattributed
  first-event-per-session; fix the firmware bug from §0 as part of this
  rewrite so the model never has to guess which events are real pulses).
- Robustness: before folding a new observation into the RLS update, reject
  it if it's wildly inconsistent with the current model (e.g.
  `|actual - predicted| > max(0.5g, 3·current_residual_sd)`, or hard bounds
  like `weightincrement ∈ [-1, 10]`) — this is the cheap, on-device
  equivalent of the outlier trimming I had to do by hand in §1.3 (targeting
  the observed ~4–6% garbage rate).

Both models are literally: `Σx`, `Σy`, `Σxy`, `Σx²` (or their
precision-weighted RLS equivalents) plus a handful of scalars — trivial
memory footprint, no matrix inversion beyond 2×2.

### 4.3 NVS persistence

Persist a small versioned struct, e.g.:

```c
struct TopupModelV1 {
  uint8_t  version;              // for future migration
  float    rate_hat;             // main-grind plateau rate, g/s
  float    rate_precision;       // inverse-variance / confidence weight
  float    topup_slope;          // g/s
  float    topup_deadtime_ms;
  float    topup_precision;
  uint32_t rate_n_effective;     // decayed effective sample count
  uint32_t topup_n_effective;
  time_t   last_updated;         // for time-based (not just count-based) decay
};
```

Roughly 40 bytes. Trivial against a default ~20–24KB NVS partition — no
flash/RAM concern at all, even storing a slightly richer model (e.g. a
handful of (bucket, rate, n) triples for a piecewise-linear topup curve
instead of one straight line) would still be well under 200 bytes.

**Recency weighting**: apply the forgetting factor by *elapsed wall-clock
time*, not just session count — usage frequency isn't constant (the data
shows months with 4 sessions and months with 150+), and the drift we
measured (§2.3) is a slow, calendar-time phenomenon (bean/burr wear), not a
grind-count phenomenon. Concretely: on each update, decay the stored
precision/effective-n by a factor `exp(-Δdays / half_life_days)` before
folding in the new observation, with `half_life_days` around 30–60 for
Model A (main-grind rate — where I found the drift) and can be longer (or
even omitted in v1) for Model B (topup slope — where I found no measurable
drift over 17 months, §2.3). This is cheap insurance either way.

### 4.4 Cold start / graceful degradation

Ship firmware defaults equal to the fleet-wide historical fit as the prior,
since they're now empirically grounded rather than guessed:

```
rate_hat_prior         ≈ 1.00 g/s   (matches current rate_default=1.0 almost exactly)
rate_prior_sd          ≈ 0.09 g/s   (from p10–p90 spread, §2.2)
topup_slope_prior       ≈ 1.05 g/s
topup_deadtime_ms_prior ≈ 310 ms
```

Treat the prior as equivalent to some number of "virtual" pseudo-observations
(e.g. `N0 ≈ 15–20` for Model A) so a fresh device already grinds *better
than the old static lookup table from grind #1* (because the prior itself
is the fleet-wide-calibrated version of what that table was hand-tuned to
approximate), and gradually lets real on-device data override the prior as
confidence accumulates — no hard cutover, no "untrained garbage" period.
If the fitted parameters ever land somewhere physically implausible (e.g.
negative slope, deadtime > 1s, R²-equivalent residual variance blowing up),
fall back to the shipped prior outright — this is the same role
`rate_min_valid`/`rate_max_valid` clamping already plays today, just applied
to both models instead of one.

### 4.5 Topup decision logic (`loopTopUp()` replacement)

Given persisted `(topup_slope, topup_deadtime_ms)` and the current gap
`g = target - current_weight`:

1. **If `g` is below a tunable `min_controllable_gap` (recommend starting
   around 0.15–0.2g, calibrated from §1.4's finding that the shortest
   controllable pulse's own output distribution has p25=0.07g/p75=0.28g) —
   do not fire a pulse.** Accept the undershoot and stop. Firing anyway is a
   real gamble: the data shows ~21% of shortest-pulse attempts alone exceed
   the 0.3g hard cap. This directly encodes D7's asymmetric loss using a
   measured number instead of a guess.
2. Otherwise, aim each pulse at **~85–90% of the remaining gap**, not 100%
   — `duration = topup_deadtime_ms + 0.9·g/topup_slope` — so a single
   unlucky high-output pulse (§1.3's noise) doesn't by itself blow past the
   target; convergence happens over 1–3 small pulses instead of one
   large risky one, using `min_topup_interval_ms`/`topup_timeout_ms`-style
   settle-and-repeat exactly as today.
3. Before committing to a duration, check the model's own uncertainty: if
   `predicted_weight + k·residual_sd > overshoot_cap_remaining`, shrink the
   requested duration accordingly (an explicit, computable version of the
   margin that's currently just a hardcoded `target_grams - 0.08` check).
4. Keep a hard hygiene clamp regardless of what the model says (mirroring
   today's `if (top_up_seconds <= 0 || top_up_seconds > 5.0f)` check) —
   cheap insurance against a corrupted/garbage model state.

---

## 5. Accuracy targets (D7) — are they achievable, and what's the limiting factor?

**95% within Δ0.2g: looks achievable, and the data gives a nice sanity
check for it.** A well-calibrated main-grind stop (session rate CV ~9%,
§2.2) plus 1–2 topup pulses (core noise ~0.14–0.2g absolute per pulse,
§1.3) should comfortably land most sessions inside 0.2g. The ~4–6%
genuinely-garbage-pulse rate I measured (§1.3) lines up almost exactly with
D7's own "5% outliers tolerated" clause, which is a reassuring coincidence
— it suggests the target was set with realistic awareness of (or at least
matches) the mechanism's actual noise floor.

**80% within Δ0.05g: achievable, but tight, and it is *not* a sensor
problem.** The sensor noise floor is ~0.02g (§3) — 2.5x better than needed.
The actual constraint is that the smallest reliably-controllable topup
increment is on the order of 0.15–0.2g (dead time ~300ms + ~1g/s flow +
~0.14g sd, §1.2/1.4) — physically coarser than the 0.05g target by roughly
3–4x. **You cannot dose 0.05g with a single relay pulse on this hardware;
the mechanism's minimum quantum is bigger than the target tolerance.**
Hitting 0.05g in 80% of sessions therefore has to come from the *main grind*
landing very close on its own (not from topup fine-tuning it in), aided by
iterative smaller-and-smaller topup pulses only for the cases that need it,
and accepting that sessions needing more than ~0.15g of correction will
mostly land in the 0.05–0.2g band rather than the <0.05g band — which the
80%/95% two-tier target already anticipates.

**Overshoot ≤0.3g hard cap: achievable, with the specific design changes in
§4.5.** The empirical 20.9%-of-shortest-pulses-exceed-0.3g number (§1.4) is
the strongest evidence in this whole analysis that the *current* lookup-table
design is under-protected against overshoot for small gaps — worth flagging
explicitly as a likely real-world failure mode of the existing firmware, not
just a theoretical concern. The "don't pulse below `min_controllable_gap`" +
"aim for 90% of remaining gap, not 100%" + "shrink duration if predicted
upper bound crosses the cap" combination directly targets this.

**Bottom line on limiting factor**: it's the **electromechanical dead
time/output granularity of a relay-switched DC grinder** (~300ms minimum
meaningful pulse, ~1g/s flow, ~0.15g effective minimum controllable
increment with ~0.14g sd around it), not sensor noise (~0.02g, plenty
tight) and not bean-to-bean variability at the session level (~9% CV on
flow rate, which a recency-weighted online model tracks fine). No amount of
better statistics fixes physics smaller than the mechanism's own quantum —
the design has to work *around* that quantum (accept-and-stop below it,
fractional-gap pulses above it) rather than trying to model it away.

---

## 6. What the new `sessions` schema (D8/D9) should capture

This analysis was possible without session linkage, but was noticeably
harder and less precise than it needed to be — I had to reconstruct session
boundaries from timestamp gaps (`topup`, heuristic 20s threshold) and
`runtimemillis` resets (`progress`, heuristic <300ms-after->2000ms), both of
which are estimates, not ground truth (e.g. the raw pre-filter session count
for `progress` was 3,503, and only 1,460 survived a plausibility filter —
the rest were noise-driven false splits or genuinely-degenerate short
fragments I couldn't otherwise distinguish). For the *next* dataset to
support strictly better modeling than this document, add:

- **`session_id`**, and FK it from `topup`, `progress`, and `raw_data` rows
  directly — eliminates all the gap/reset-heuristic reconstruction above.
- **`target_weight_g`** and **`final_weight_g`** (the actual outcome) —
  the single highest-value addition. This analysis is entirely
  target-independent (per D7, correctly, given what's available); with
  these two fields future analysis can directly compute `final_error_g`
  per session and *evaluate* a candidate model against real outcomes
  instead of only fitting the physical relationships and hoping.
- **`event_type` enum** (`MAIN_GRIND` / `TOPUP`) on whatever replaces the
  `topup` table, tagged at write time rather than inferred — this makes
  §0's data-quality bug structurally impossible to reintroduce.
- **`pulse_index`** (sequence number within a session) — lets a future
  model directly study "does pulse 2 behave differently from pulse 1"
  rather than the row-position heuristic I used in §0.
- **`mode`** (single/double/API custom) and the **requested vs. corrected**
  target (i.e. before/after the topup margin) — useful for separating
  "the algorithm chose to undershoot on purpose" from "the algorithm missed".
- **`outcome`** (completed / aborted / timed-out) — so abandoned sessions
  (button-press cancel) don't silently pollute future outcome statistics.
- **A grind-setting/bean identifier** (even a free-text tag bumped manually
  on a bag/burr change) — would let future analysis attribute the ~12–15%
  drift over 17 months (§2.3) to specific events instead of treating it as
  unexplained slow drift, and would make recency-weighting parameters
  tunable against ground truth rather than guessed half-lives.
- **`firmware_version`/`model_version`** — enables clean before/after
  comparison of old vs. new topup algorithm directly from live data once
  this ships.

None of this blocks the model in §4 — it's usable today, cold, with the
existing schema. But every field above closes a specific reconstruction gap
I had to work around in this document.
