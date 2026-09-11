# Status

*Last updated: 2026-09-11 — audit pass complete, topup data analysis in
progress, implementation not yet started.*

## Where things stand

Planning is complete (`AGENTS.md` for standing rules, `DECISIONS.md` D1-D11
for the reasoning). Nothing has been implemented yet — this is still design/
audit phase. The new branch `rewrite/rtos-fork` exists with the old codebase
plus this `.agent/` directory.

**Audit pass done.** A full read-only review of the existing firmware
(`src/`, `lib/`, `include/`, `platformio.ini`) is complete, cross-checked
against commit history. 20 findings logged in `ARS.md` (AR-001 through
AR-020 — 6 from initial planning-time reading, 14 from the dedicated audit
pass). Most consequential:
- **AR-009**: the settings struct (calibration, timing constants, the
  topup lookup table) is read from ISR context and written from the
  web-server task with zero synchronization — a second shared-state race
  beyond the one already known (AR-001).
- **AR-011**: the weight-based fallback stop check compares against the
  *full* target rather than the margin-reduced one, meaning if the primary
  time-estimate never engages, the grinder can pour straight to full
  target in one continuous run — bypassing the topup-margin strategy the
  whole overshoot-avoidance goal (D7) depends on. Needs to be addressed
  deliberately in the new dosing design, not just ported forward.
- **AR-013/014**: none of the five WebSocket endpoints ever call
  `cleanupClients()`, and their connection caps are inconsistent (1/3/
  unlimited) — a real slow heap leak over long uptime. The new consolidated
  single-channel design (D4) should get this right from the start.

None of these are surprising given the project's organic-growth history,
and none contradict "mostly bug-free in day-to-day use" — they're latent/
edge-case issues, not things misbehaving right now. Full detail in
`ARS.md`.

Infrastructure groundwork done during planning:
- Read-only Postgres role (`claude_agent`) created on `192.168.0.111` for
  exploring the historical topup/progress/raw-data tables (credentials in
  `.agent/secrets/pg_agent.env`, gitignored).
- Historical data confirmed usable: 7,647 `topup` rows, 121,847 `progress`
  rows, 209,848 `raw_data` rows — enough to fit a real dosing model without
  needing the missing target-weight linkage (see `DECISIONS.md` D7).
- Discovered and scoped in a whole extra component: `coffee_grinder_api`,
  a Python service on `192.168.0.112` that will be retired in favor of the
  device posting directly to PostgREST (D8).

## What's next (in rough order)

1. ~~Audit pass over the existing firmware~~ — done, see above.
2. **Topup/dosing model design** — in progress. Analyzing the historical
   `topup`/`progress` data (distribution of current errors, what a
   recency-weighted fit buys vs. the current static lookup table) to
   propose a concrete model before writing firmware code against it.
3. **FreeRTOS task architecture** — design the task/queue boundaries
   (scale sampling, dosing control, display, network, logging) before
   implementing any of them. Should account for AR-001/AR-007/AR-008/AR-009
   (the input and settings race conditions) by construction.
4. **PostgREST deployment plan** — schema for the new `sessions` table,
   INSERT-only role, systemd unit — as a concrete plan before touching the
   live Proxmox host.
5. **Web SPA** — framework choice, LittleFS build pipeline, page/tab
   layout replacing the three old served pages. Should account for
   AR-012/013/014 (display indicator bug, WebSocket cleanup/connection-cap
   inconsistency) by construction rather than porting them forward.
6. Implementation, in whatever order the above design work suggests makes
   sense — almost certainly scale + dosing core first (the part with real
   behavioral stakes), display and web app after.

## Standing constraints (see `AGENTS.md` for full detail)

- **No OTA deploy to the physical device without the owner's explicit
  go-ahead, every time.** Building and compiling freely is fine and
  expected.
- Hardware is fixed — firmware/software rewrite only.
- Cheapest model suitable for each task.

## Open questions for the owner

None right now — everything from planning is resolved. This section will
list anything genuinely blocking as it comes up (flagged in `ARS.md` as
`needs-owner-input`).

## Decisions made

See `DECISIONS.md` for the full log (D1–D11 so far, covering hardware
scope, RTOS approach, display ambition, web app architecture, OTA identity,
the ADS1232 driver, the topup model approach, the `coffee_grinder_api`
retirement, the browser-direct analytics query pattern, the audit-as-you-go
process, and the no-autonomous-deploy rule).
