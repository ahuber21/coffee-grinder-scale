# Status

*Last updated: 2026-09-11 — after planning, before implementation begins.*

## Where things stand

Planning is complete. The full scope, architecture, and constraints for the
rewrite are settled and written down (`AGENTS.md` for the standing rules,
`DECISIONS.md` for the reasoning behind each one). Nothing has been
implemented yet. The new branch `rewrite/rtos-fork` exists and is currently
just the old codebase plus this `.agent/` directory.

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

1. **Audit pass** over the existing firmware — read everything, log real
   findings to `ARS.md` (six seeded already from planning-time reading;
   expect more). This informs the architecture work rather than blocking
   it.
2. **Topup/dosing model design** — analyze the historical `topup`/
   `progress` data properly (distribution of current errors, what a
   recency-weighted fit buys vs. the current static lookup table) and
   propose a concrete model before writing firmware code against it.
3. **FreeRTOS task architecture** — design the task/queue boundaries
   (scale sampling, dosing control, display, network, logging) before
   implementing any of them.
4. **PostgREST deployment plan** — schema for the new `sessions` table,
   INSERT-only role, systemd unit — as a concrete plan before touching the
   live Proxmox host.
5. **Web SPA** — framework choice, LittleFS build pipeline, page/tab
   layout replacing the three old served pages.
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
