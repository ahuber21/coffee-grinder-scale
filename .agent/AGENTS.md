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

- **Never OTA-deploy to the physical device.** Building is always fine.
  Flashing/deploying to the real grinder is the owner's call exclusively,
  until he explicitly says otherwise. This applies to `pio run -t upload`,
  `pio run -t uploadfs`, or anything that writes to the device at
  `eureka.local` / its IP — CI-style "build and verify it compiles" is
  fine and encouraged; touching the live device is not.
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
  decommissioned after PostgREST cutover is verified).
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
