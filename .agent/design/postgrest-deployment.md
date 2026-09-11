# PostgREST deployment (D8/D9)

Deployed and verified end-to-end on 2026-09-11. This documents the running
setup on `192.168.0.111` — the schema it serves is
`.agent/design/db-schema/001_sessions_schema.sql` (read that first for the
table design and rationale; this file is deployment/ops only).

## Where it runs, and why

Host: `192.168.0.111` (the `postgresql-ct` Proxmox LXC container — same
host as Postgres, per the earlier planning discussion and AGENTS.md's
"Data infra" note). Chosen over `192.168.0.112` (the old
`coffee_grinder_api` host) specifically because PostgREST talks to Postgres
constantly and benefits from being on the same host (loopback connection,
no extra network hop, no extra firewall/network config needed) — `.112`
would need a route to `.111`'s Postgres either way, so there's no
advantage to putting PostgREST there instead.

OS: Ubuntu 20.04.6 LTS, x86_64. Postgres 12.20.

## PostgREST version — a real compatibility constraint, not a default choice

**PostgREST 16.3 (the current latest release) refuses to start against
Postgres 12** ("Cannot run in this PostgreSQL version (12.20...), PostgREST
needs at least 14.0" — hit this directly during deployment). PostgREST's
v14+ line requires Postgres 14+.

**Installed: PostgREST 13.0.8** (the latest release on the v13.x line),
which connects to Postgres 12 successfully and was the version actually
verified end-to-end below. This is a real judgment call worth flagging: if
Postgres itself is ever upgraded to 14+, it's worth revisiting whether to
move to a current PostgREST release at that point — nothing in the v2
schema depends on PostgREST 13 specifically, this was purely a server-side
compatibility constraint.

Binary: static Linux x86-64 build from
`https://github.com/PostgREST/postgrest/releases/download/v13.0.8/postgrest-v13.0.8-linux-static-x86-64.tar.xz`,
installed at `/usr/local/bin/postgrest` (owned `root:root`, mode 755).

## Database role

Two roles, per PostgREST's standard authenticator/anon pattern (see the
"Roles for PostgREST's own connection use" section at the bottom of
`001_sessions_schema.sql` for full reasoning):

- `postgrest_authenticator` — `LOGIN NOINHERIT`, the role PostgREST's
  `db-uri` connects as. Privilege-free on its own; can only switch into
  `postgrest_anon`.
- `postgrest_anon` — `NOLOGIN`, holds the actual grants: `SELECT, INSERT`
  on `v2.sessions`/`v2.events`/`v2.raw_samples`, plus column-scoped
  `UPDATE` on just the "finalize" columns (`sessions.final_weight_g`,
  `outcome`, `completed_at`, `relay_off_at_ms`, `stable_at_ms`;
  `events.relay_off_at_ms`, `stable_at_ms`, `weight_after_g`). **No
  privileges of any kind on `public.topup`, `public.progress`, or
  `coffee_grinder_raw.public.raw_data`** — verified directly via
  `information_schema.role_table_grants`/`role_column_grants` after
  applying the migration.

The `postgrest_authenticator` password lives only in
`/etc/postgrest/postgrest.conf` on `192.168.0.111` (root-readable, group
`postgrest`, mode 640) — it is **not** recorded anywhere in this repo. If
it ever needs rotating: `ALTER ROLE postgrest_authenticator PASSWORD
'<new>';` as the `postgres` superuser (`sudo -u postgres psql`), then
update `db-uri` in the conf file and `systemctl restart postgrest`.

A gotcha hit and worth flagging for next time: when applying
`001_sessions_schema.sql`'s role-creation `\gexec` block via `psql -v
pw=...`, do **not** wrap the password in an extra pair of quotes on the
command line (`-v pw="'...'"`) — psql's `:'pw'` substitution already adds
SQL-literal quoting, so double-quoting makes literal quote characters part
of the stored password. Correct invocation: `psql -v pw=<raw password, no
quotes> -f 001_sessions_schema.sql`. (Caught this because the first
`ALTER`/service start failed auth; fixed via `ALTER ROLE ... PASSWORD` with
correct quoting — see git history / this deployment for the exact
sequence.)

## Config file

`/etc/postgrest/postgrest.conf` (root:postgrest, mode 640):

```
db-uri = "postgresql://postgrest_authenticator:<password>@127.0.0.1:5432/coffee_grinder"
db-schemas = "v2"
db-anon-role = "postgrest_anon"
db-pool = 10
db-config = false
server-host = "!4"
server-port = 3000
log-level = "info"
```

Key choices:
- `db-schemas = "v2"` — only the new schema is exposed; PostgREST has no
  visibility into `public` (old tables) or the `coffee_grinder_raw`
  database at all (different database — a single PostgREST process only
  ever talks to one database per `db-uri`).
- `db-anon-role = "postgrest_anon"` — there is no JWT auth configured in
  this v1 deployment. The device posts on the trusted home LAN with no
  bearer token, so every request runs as the anon role, and privilege
  scoping happens entirely at the Postgres grant level described above.
  Adding real JWT-based auth (e.g. to distinguish "device write" from
  "browser analytics read" with different roles per D9) is a reasonable
  future hardening step, not implemented here — flagged as a candidate AR,
  not something this task's scope covers.
- `db-config = false` — disables PostgREST's in-database configuration
  feature (it expects specific schema/function support we haven't set up);
  keeps the deployment simple and fully file-config-driven.
- `server-host = "!4"` — PostgREST's syntax for "listen on all IPv4
  interfaces" (this is also the default), needed so the ESP32 on the LAN
  can reach it, not just localhost.

## systemd service

Unit: `/etc/systemd/system/postgrest.service`, enabled (`WantedBy=multi-user.target`,
survives reboot) and `Restart=always` (mirrors `coffee_grinder_api.service`'s
style on `.112`, which was inspected for local convention before writing
this one):

```ini
[Unit]
Description=PostgREST API for coffee_grinder v2 sessions schema
After=network.target postgresql.service
Wants=postgresql.service

[Service]
User=postgrest
Group=postgrest
ExecStart=/usr/local/bin/postgrest /etc/postgrest/postgrest.conf
Restart=always
RestartSec=5
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true

[Install]
WantedBy=multi-user.target
```

Runs as a dedicated unprivileged system user (`postgrest`, no login shell,
no home directory) created for this purpose — not as root, and not as the
`python` user `coffee_grinder_api` uses on `.112` (different host, no
reason to share an identity). `ProtectSystem=strict`/`ProtectHome=true`/
`NoNewPrivileges=true` are added hardening beyond what `coffee_grinder_api.service`
does, since PostgREST doesn't need filesystem write access anywhere.

**Status/logs**:
```
systemctl status postgrest.service
journalctl -u postgrest.service -f
```

## Endpoint shape (what the future firmware will POST/PATCH)

Base URL: `http://192.168.0.111:3000/`. No auth header required in this v1
deployment (LAN-trusted, see above).

**1. Start a session** (`POST /sessions`) — fire this once, at grind
start, with a firmware-generated UUIDv4 `session_id`:
```
POST /sessions
Content-Type: application/json
Prefer: return=representation

{
  "session_id": "<uuidv4 generated on-device>",
  "mode": "double",
  "requested_weight_g": 18.0,
  "target_weight_g": 17.5,
  "firmware_version": "1.2.3",
  "model_version": "TopupModelV1"
}
```
(`grind_setting` optional/free-text; everything else defaults sensibly —
`outcome` starts `'in_progress'`.)

**2. Log each pulse** (`POST /events`) — one row per relay energize, main
grind first (`pulse_index: 0`, `event_type: "MAIN_GRIND"`), then each topup
pulse (`pulse_index: 1, 2, ...`, `event_type: "TOPUP"`):
```
POST /events
{
  "session_id": "<same uuid>",
  "event_type": "MAIN_GRIND",
  "pulse_index": 0,
  "relay_on_at_ms": 0,
  "relay_off_at_ms": 9124,
  "weight_before_g": 0.0
}
```
Then **PATCH the same row** once the post-pulse settle completes (needs
the row's `event_id` from the POST response, so use
`Prefer: return=representation` on the insert):
```
PATCH /events?event_id=eq.<id>
{ "stable_at_ms": 10502, "weight_after_g": 18.02 }
```

**3. Stream raw samples** (`POST /raw_samples`), continuously, ~20Hz,
tagged with the current phase:
```
POST /raw_samples
{
  "session_id": "<same uuid>",
  "timestamp_ms": 1234,
  "raw_value": 8388608,
  "filtered_value": 12.34,
  "stable": false,
  "grinder_state": "MAIN_GRIND"
}
```
(`grinder_state` one of `MAIN_GRIND`/`TOPUP_PULSE_ON`/`TOPUP_SETTLE`/
`STOPPING`/`FINALIZE`; `event_id` optional, when attributable to a specific
pulse.)

**4. Finalize the session** (`PATCH /sessions?session_id=eq.<uuid>`), once,
at grind end:
```
PATCH /sessions?session_id=eq.<uuid>
{
  "final_weight_g": 18.03,
  "outcome": "completed",
  "completed_at": "2026-09-11T12:00:00Z",
  "relay_off_at_ms": 9124,
  "stable_at_ms": 10502
}
```
Only these columns (plus the equivalent `events` finalize columns above)
are grantable to the device role — attempting to PATCH e.g.
`target_weight_g` correctly fails with `42501 insufficient_privilege`
(verified below).

## End-to-end verification performed

Ran directly against the live deployment on `192.168.0.111`, then cleaned
up:

1. `POST /sessions` with a test UUID, `mode=double`,
   `firmware_version=test-e2e-0.0.0` → `201`, row confirmed via `SELECT`.
2. `POST /events` (`MAIN_GRIND`, `pulse_index=0`) linked to that session →
   `201`, `runtime_ms`/generated column computed correctly
   (`9000 = 9000 - 0`).
3. `POST /raw_samples` linked to that session → `201`.
4. `PATCH /sessions?session_id=eq.<uuid>` setting `final_weight_g`,
   `outcome=completed`, `completed_at` → `200`, confirmed via a follow-up
   `GET` that the row actually updated (`outcome=completed,
   final_weight_g=17.52`).
5. `PATCH /sessions?session_id=eq.<uuid>` attempting to also set
   `target_weight_g` (a column the anon role is deliberately **not**
   granted UPDATE on) → `401`/`42501 insufficient_privilege`, as intended.
6. Cleanup: `DELETE FROM v2.sessions WHERE session_id = '<test uuid>'` as
   the `postgres` superuser — `ON DELETE CASCADE` correctly removed the
   linked `events` and `raw_samples` rows too. Post-cleanup: all three
   `v2` tables confirmed empty (`SELECT count(*)` = 0 on each) — no test
   data left behind.

## What's NOT done (explicitly out of scope for this task)

- No JWT/token auth — the device (and, per D9, the browser analytics
  view) talk to PostgREST unauthenticated on the trusted home LAN. Fine
  for a v1 home deployment; worth an AR if this ever needs to be reachable
  off-LAN.
- No HTTPS/TLS termination in front of PostgREST — plain HTTP on port
  3000, LAN-only. AGENTS.md's "Data infra" note mentions the device
  posting over HTTPS eventually; that would need a reverse proxy (e.g.
  nginx/Caddy) in front of PostgREST, not implemented here.
- `coffee_grinder_api.service` on `.112` was left completely untouched, as
  instructed — still running, still writing to the old tables. Nothing in
  this deployment reads from or writes to it.
