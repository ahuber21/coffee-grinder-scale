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

**Topup/dosing model designed.** Full analysis and proposal in
`.agent/design/topup-model.md`, built from live queries against the real
historical data (5,525 genuine topup pulses, 1,460 reconstructed
main-grind sessions, 209,848 raw sensor rows). Headline results:
- Sensor noise floor (~0.02g) is not the bottleneck for anything.
- The 95%/Δ0.2g and overshoot-≤0.3g targets from D7 both look achievable
  with the proposed design (two tiny recursive-least-squares linear
  models — main-grind rate, topup pulse response — persisted to NVS,
  replacing the single-point rate estimate and the static lookup table).
- Found a real, quantified problem with the *current* live system: the
  shortest lookup-table topup pulse already exceeds the 0.3g overshoot cap
  ~21% of the time, on its own (AR-023) — matches the owner's stated pain
  point directly.
- Found and traced a firmware bug that corrupted ~28% of the historical
  topup log (main-grind tail misreported as a topup event — AR-021),
  harmless to live behavior but worth fixing.
- **Flagged, not decided**: the 80%/Δ0.05g target may not be physically
  achievable — the grinder's electromechanical minimum controllable dose
  increment (~0.15-0.2g) is coarser than the 0.05g tolerance itself. See
  AR-022, `needs-owner-input`. This is the one thing blocking moving from
  "model designed" to "model locked in" for implementation.

**FreeRTOS task architecture designed.** Full proposal in
`.agent/design/rtos-architecture.md`: 7 tasks (Scale, Dosing/Session
Control, Input, Display, Settings/NVS, Network, Telemetry), strict
single-writer ownership per piece of state, two message primitives chosen
per link (queues where every item matters, length-1 overwrite mailboxes
where only the latest value matters). Closes 15 of the audit's findings by
construction (full traceability table in the doc §9) — most notably both
shared-state races (AR-001, AR-009), the button-handling asymmetry
(AR-007/008), and AR-011's overshoot-margin bug. Surfaced one new minor
open question (**AR-024**, low urgency): should an OTA update mid-grind
abort the grind, or should OTA be refused while a grind is in progress?
Has a stated safe default (abort-and-stop), not blocking.

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
2. ~~Topup/dosing model design~~ — done, see below and
   `.agent/design/topup-model.md`. **One open question for the owner.**
3. ~~FreeRTOS task architecture~~ — done, see below and
   `.agent/design/rtos-architecture.md`.
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

- **AR-022** (blocking model lock-in): is the 80%-of-sessions-within-Δ0.05g
  accuracy target still the goal as stated, knowing the grinder's own
  minimum controllable dose increment (~0.15-0.2g) is physically coarser
  than that tolerance? The 95%/Δ0.2g and overshoot-≤0.3g targets both look
  solidly achievable either way. See `.agent/design/topup-model.md` §5.
- **AR-024** (not blocking, low urgency): should an OTA update mid-grind
  abort the grind, or should OTA be refused while a grind is in progress?
  Recommended default (abort-and-stop) is fine to proceed with unless the
  owner prefers otherwise.

## Decisions made

See `DECISIONS.md` for the full log (D1–D11 so far, covering hardware
scope, RTOS approach, display ambition, web app architecture, OTA identity,
the ADS1232 driver, the topup model approach, the `coffee_grinder_api`
retirement, the browser-direct analytics query pattern, the audit-as-you-go
process, and the no-autonomous-deploy rule).
