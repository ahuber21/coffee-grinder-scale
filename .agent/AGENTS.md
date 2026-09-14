# Agent instructions — coffee grinder firmware rewrite

Read this first. It is the canonical, up-to-date source of truth for this
effort — more current than any single conversation transcript. If you are a
subagent picked up mid-project, start here, then check `STATUS.md` for where
things currently stand and `ARS.md` for open findings before touching code.

## What this project is

A full rewrite of the firmware for a smart coffee-grinder scale (ESP32 +
load cell, controls a Eureka Mignon grinder by weight, tops up to hit a
target dose). The existing codebase grew organically — started hand-written,
increasingly LLM-authored over time — and is being rewritten from scratch on
a new branch (`rewrite/rtos-fork`) as effectively a permanent fork of
`main`. It will likely never merge back.

The owner (Andreas) wants this to be a **showcase of embedded ESP32 best
practices** — code he can read and learn from, not just code that works.
When porting or regenerating any piece of the existing logic, question
whether what it's doing is actually right before carrying it forward. Don't
transliterate blindly. Log anything questionable in `ARS.md`.

## Hard constraints — do not violate

- **The standing ~0.3g dosing-accuracy gap (`ARS.md` AR-073) is a
  confirmed firmware/session-logic bug, not physical, not calibration,
  not hardware.** The owner has directly ruled out all three (unchanged
  hardware that used to be accurate, a restored + freshly recalibrated
  `calibration_factor`, and a static reference weight reading correctly
  outside a session) — read AR-073 in full before proposing a cause, and
  do not re-propose physical/mechanical/calibration explanations; that
  ground has already been covered and rejected, repeatedly, across
  several sessions.
- **OTA-deploying to the physical device is always allowed**, using this
  repo's own toolchain (`pio run -t upload`/`uploadfs`, targeting
  `eureka.local` / its IP). The owner lifted the earlier "ask first every
  time" rule (2026-09-14) — no per-deploy confirmation needed.
- **Hardware is fixed.** Same ESP32 (`az-delivery-devkit-v4` board id,
  ESP32-WROOM), same 80×160 ST7735 color TFT, same ADS1232 load-cell ADC,
  same 3 buttons + relay wiring. No hardware/case changes.
- **PlatformIO + VS Code stays** as the toolchain.
- Use the cheapest model suitable for a given task. Reserve stronger
  models for genuine design/judgment work (architecture, the topup
  algorithm, audit synthesis); use cheaper ones for mechanical work
  (boilerplate, inventory/grep-style exploration, repetitive porting).

## Architecture decisions (see `DECISIONS.md` for full rationale)

- **RTOS pattern**: stay on Arduino-ESP32 (not Zephyr) — it already runs on
  FreeRTOS. Replace the current single `loop()` + `switch(state)` machine
  with explicit FreeRTOS tasks (scale sampling, dosing/grinder control,
  display rendering, network/web server, logging/telemetry) communicating
  via queues/notifications — not shared globals touched from ISR context
  like the current code does.
- **Display**: push the existing 80×160 ST7735 to its limit — smooth,
  flicker-free, high-framerate animation. No resolution upgrade available.
- **Web app**: one SPA, built with a real framework/build pipeline, served
  from an ESP32 LittleFS partition (OTA-updatable filesystem), reachable at
  `eureka.local`. Replaces the current three separate PROGMEM-embedded
  pages (`/console`, `/settings`, `/graph`) and consolidates the multiple
  websockets (`WebSocketLogger`, `WebSocketGraph`, `WebSocketMetrics`,
  `RawDataWebSocket`, `WebSocketSettings`) into one multiplexed channel.
  A history/analytics tab in the SPA queries PostgREST **directly from the
  browser**, bypassing the device — the ESP32 has no business aggregating
  200k+ historical rows itself.
- **OTA**: mDNS hostname `eureka.local`. No OTA password (accepted risk on
  a home LAN) — but see the hard constraint above, this is orthogonal to
  who is *allowed* to trigger a deploy.
- **ADS1232 driver**: keep the existing implementation (ring buffer,
  non-blocking) — it's genuinely good. Vendor it into the new tree
  directly; don't keep it as a git submodule.
- **Data infra**: kill `coffee_grinder_api` (the bespoke Python trampoline
  on `192.168.0.112`). The ESP32 posts directly to **PostgREST** (deployed
  on `192.168.0.111`, next to Postgres) over HTTPS, using an INSERT-only
  Postgres role — the device must never be able to read or alter history.
  New `sessions`-style schema needed (target weight, mode, linked
  topup/progress/raw-data) — the old schema silently discarded target
  weight, which is being fixed.
- **Topup algorithm**: replace the static 6-bucket lookup table with a
  fitted, recency-weighted model built from `topup(runtime_ms →
  weight_increment)` and `progress(runtime_ms → weight)` history (7,647 /
  121,847 rows respectively as of the audit). Neither needs the missing
  target linkage to be useful. Targets: "spot on" = |error| < 0.05g for
  80% of sessions, 95% within Δ0.2g, remaining 5% outliers tolerated;
  **overshoot hard-capped at ≤0.3g** and weighted as more critical to avoid
  than undershoot (undershoot → annoying-but-fine topup loop; overshoot →
  manually discarding ground coffee).

## Infrastructure access

- Device: `eureka.local` (was `192.168.0.118` / mDNS
  `esp32-98cdac595620` before rename).
- Build offload: try `pio remote agent` / `pio remote run --force-remote`
  against `dev-ct.local` first; if that needs a paid PlatformIO tier, fall
  back to a plain SSH build-and-fetch script. Non-blocking, nice-to-have.
- Postgres host: `192.168.0.111` (SSH as root via `~/.ssh/id_proxmox`,
  granted for read/admin setup only — see rule below).
- Old trampoline host: `192.168.0.112` (`coffee_grinder_api`, to be
  decommissioned after PostgREST cutover is verified — still running
  unchanged, do not touch it until then).
- **PostgREST is deployed and running**: `http://192.168.0.111:3000`,
  systemd service `postgrest` (active, enabled). Serves the new `v2`
  schema (`sessions`/`events`/`raw_samples`) in the `coffee_grinder`
  database — see `.agent/design/db-schema/001_sessions_schema.sql` and
  `.agent/design/postgrest-deployment.md` for the full shape, the
  `postgrest_anon`/`postgrest_authenticator` role setup, and the
  column-scoped update permissions (a session's `target_weight_g` etc.
  can't be rewritten after creation, only "finalize" fields like
  `final_weight_g`/`outcome`). Old `public.topup`/`public.progress`/
  `coffee_grinder_raw.public.raw_data` are untouched and still hold all
  historical data — nothing currently writes to the new schema yet
  (firmware doesn't exist to post to it; `coffee_grinder_api` still writes
  to the old tables). Note: PostgREST is pinned to v13.0.8, not latest —
  this Postgres instance runs 12.20, and PostgREST 16+ requires PG14+.
- **Database credentials**: see `.agent/secrets/pg_agent.env`
  (gitignored, not committed — read-only `claude_agent` Postgres role,
  SELECT-only on `coffee_grinder` and `coffee_grinder_raw`). **Only use
  the root SSH account for administrative read operations (e.g. creating
  scoped roles); use a purpose-scoped role for actual data access, and
  create a new one if the task needs a different privilege shape (e.g. the
  PostgREST INSERT-only role) rather than reusing root.**

## Sister repo

`../2023-12-10-espresso-scale-eureka` is a second checkout of the *same*
GitHub repo (same remote, same history) — not a fork, not a divergent
codebase. It's reference-only (e.g. it has the `ADS1232` submodule
actually checked out). Never push to it, never treat it as a separate
source of truth.

## Process expectations

- Track open findings/questions as ARs in `ARS.md` — see that file for the
  format.
- Keep `STATUS.md` current (≤2 pages) after any meaningful chunk of work —
  it's the owner's fast way to catch up on where things stand.
- Record real architectural decisions (not routine implementation choices)
  in `DECISIONS.md`.
- Tests aren't mandatory but are welcome for pure-logic modules (topup
  model, rate calculation, settings serialization) — PlatformIO's native
  test environment is the right tool. Static asserts are welcome wherever
  they catch a real invariant (struct sizes vs. EEPROM/NVS budget, pin
  conflicts, enum bounds) cheaply.
- **Comment style**: code should be mostly self-explanatory; comments
  exist for what isn't. Every class/struct/function/enum gets a short
  doxygen-style (`/** ... */` or `///<`) doc comment. Anywhere else, an
  inline comment is explanatory, procedural, or a findings note — plain
  `//`, at most two lines; something genuinely complex enough to need
  more goes in a `/* ... */` block, but that's the rare exception, not
  the default whenever a comment runs long. Never cite a bare decision/
  section/finding code (`D12`, `§4.5`, `AR-016`) as the explanation
  itself — the comment must stand on its own without the reader needing
  to open `DECISIONS.md`/the design doc/`ARS.md` to understand it. Citing
  a real file path (`.agent/design/topup-model.md`) is fine since that's
  directly openable, not an opaque code.
- **No comments that only make sense in light of history.** A comment
  must explain the code that is there, for a reader who has never seen
  any other version of it — never "the old code did X", "previously Y",
  "no longer Z", "mirrors the legacy firmware's W", or a reference to a
  label/function/file that this same change just removed. That framing
  rots the moment the comparison point is gone (the old code, once
  deleted, leaves the comment meaningless) and adds nothing a reader
  standing in front of only the current code needs. If a numeric
  constant or a design choice has real, non-obvious justification,
  state the justification itself (the physical/behavioral reason),
  not where the number was previously copied from. That kind of
  before/after narrative belongs in the commit message, not the file.
- **Never let a single raw/unstable scale reading directly trigger a
  decision — but "gate on `sample.stable`" is not a universal fix, and
  applying it blindly is its own bug.** A falling clump of grounds has
  inertia — its impact reads heavier than its true settled mass for an
  instant — and `ADS1232::getRaw()` passes a single unfiltered sample
  straight through (not the ring-buffer mean) whenever it isn't stable.
  AR-072 went through two rounds on this: round one gated every
  offending check on `sample.stable` and shipped it as fixed; the owner
  caught that this was wrong for the main-grind case specifically ("of
  course our transition into topup will be driven by an unstable
  weight, because a running grinder produces by definition an unstable
  weight") and made the actual failure mode worse, not better. The
  distinction that matters is **whether the physical process is
  continuously active or can genuinely settle between checks**:
  - **A continuously active process** (GRINDING: relay held on, coffee
    constantly falling, for the whole duration of the state) is, by the
    ADS1232's own stability definition, essentially *never* stable —
    the flow rate alone exceeds the ring buffer's self-consistency
    threshold. Gating a decision on `sample.stable` here doesn't make it
    safer, it makes it fire (at best) only once flow has nearly stopped
    — silently disabling whatever the check existed to catch during
    normal, ongoing operation. GRINDING's raw-weight fallback exists
    specifically to catch "the primary time estimate is wrong while
    flow is still ongoing"; gating it on stability meant it could no
    longer catch that at all, leaving grinding_timeout_ms (tens of
    seconds) as the only remaining backstop — trading a rare, small
    undershoot for a much worse rare, large overshoot (this project's
    stated worse failure mode, D7). The correct fix here is protecting
    the *value* being compared, not gating the *decision*:
    `MainGrindModel::currentWeightEstimate()` is `addSample`'s own
    last-accepted sample, already rejecting an implausible single-
    sample rise or drop (`max_plausible_rise_g`/`max_plausible_drop_g`)
    before it's compared against anything — no stability requirement,
    because none is available while genuinely grinding. The same
    reasoning applies to `TopupModel`'s hard bounds and `CoastModel`'s
    plausibility bounds: independent, magnitude/statistical rejection,
    not a stability gate.
  - **A process that is idle or between discrete pulses** (TOPUP's
    DECIDING/SETTLING phases, with the relay *off* between pulses;
    SCREENSAVER waiting for a cup to be placed/removed; FINALIZE waiting
    to see if the cup was lifted) genuinely can and does settle once the
    one-time disturbance passes, because nothing is continuously acting
    on the scale. Gating on `sample.stable` here is correct and doesn't
    starve anything — see `handleTopupSample`'s DECIDING/SETTLING
    phases for the reference pattern (`stable || elapsed >=
    stability_max_wait_ms` when the wait must also be bounded; no bound
    needed when an independent unconditional exit already exists, like
    a button press or an overall timeout).

  Before gating anything on `sample.stable`, ask: is the relay
  continuously on / is something continuously changing the reading for
  the whole state this check runs in? If yes, that gate will rarely or
  never pass — protect the value instead. Never assume the ADS1232
  driver's own `stable`/`isChanging` flag is a sufficient, trustworthy
  signal by itself in a tight real-time loop either way — see AR-050,
  which found the same flag unreliable as a direct switching trigger
  for a different feature; treat it as one input to corroborate, not as
  proof.
