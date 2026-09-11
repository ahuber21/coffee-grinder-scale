# Status

*Last updated: 2026-09-11 — design phase complete, implementation started.*

## Where things stand

Planning and design are done; implementation has begun. Branch
`rewrite/rtos-fork`. Standing rules in `AGENTS.md`, full decision log in
`DECISIONS.md` (D1-D14), all findings in `ARS.md` (AR-001-025, all
resolved or non-blocking), design docs in `.agent/design/`.

**Design work completed:**
- **Audit** of the existing firmware (20 findings, `ARS.md`). Nothing
  contradicts "mostly bug-free day-to-day" — latent/edge-case issues, not
  live misbehavior. Most consequential: a second ISR/task shared-state
  race (AR-009), a fallback stop path that silently bypasses the
  topup-margin strategy (AR-011), a WebSocket client-cleanup gap
  (AR-013/014).
- **Topup/dosing model** (`design/topup-model.md`), built from live
  queries against real historical data. Replaces the static lookup table
  and single-point rate estimate with small recursive-least-squares
  models. Found and fixed in design: a firmware bug corrupting ~28% of
  historical topup logs (AR-021), a quantified real overshoot problem in
  the *current* system (~21% of shortest pulses breach the 0.3g cap,
  AR-023), and — the largest single finding — a ~0.49g "coast" effect
  (coffee still landing for ~1.4s after relay-off, 2.5-3x bigger than the
  topup-pulse noise floor) that the firmware currently doesn't anticipate
  at all (AR-025/D14, `design/coast-effect.md`). D13: the 80%/Δ0.05g
  accuracy target stands unchanged — owner confirmed the physical
  mechanism (clumping within the relay's minimum on-time) and the path to
  it is a better main-grind stop estimate (Models A + C), not a more
  precise topup pulse (capped by clumping).
- **FreeRTOS task architecture** (`design/rtos-architecture.md`): 7 tasks,
  strict single-writer state ownership, queues vs. overwrite-mailboxes
  chosen per link. Closes 15 audit findings by construction. D12: OTA is
  refused outright during a grind, not aborted.

**Implemented:**
- `lib/DosingModel/` — all three models (main-grind rate, topup pulse
  response, coast anticipation) from `topup-model.md` §4/§8, natively
  unit-tested (22/22 passing, independently re-verified). Not yet wired
  into any actual control loop — that happens once the FreeRTOS task
  implementation exists.

**In progress:** PostgREST schema + deployment on `192.168.0.111`
(explicitly approved by the owner, including the systemd service).

**Not yet started:** web SPA, FreeRTOS task implementation
(scale/display/network/settings), integration of the dosing model into an
actual control loop.

## Infrastructure on hand

- Read-only Postgres role `claude_agent` on `192.168.0.111`
  (`.agent/secrets/pg_agent.env`, gitignored) — used for all the analysis
  above. A separate INSERT-only role will be needed for the PostgREST
  ingestion path (not created yet).
- `coffee_grinder_api` (Python trampoline on `192.168.0.112`) confirmed
  in scope for retirement in favor of direct-to-PostgREST posting (D8).

## Standing constraints (full detail in `AGENTS.md`)

- **No OTA deploy to the physical device without the owner's explicit
  go-ahead, every time.** Building/testing freely is fine and expected.
- Hardware is fixed — firmware/software rewrite only.
- Cheapest model suitable for each task.

## Open questions for the owner

None right now.

## Parked ideas (out of scope for this rewrite)

See `.agent/ideas.md` — currently: a possible future grinder/chute
modification to reduce topup-pulse clumping.
