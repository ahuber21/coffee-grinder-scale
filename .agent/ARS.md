# ARs — findings & open questions log

"AR" here = a finding worth tracking: a bug, a design smell, a piece of
code whose rationale is unclear, or a question that needs the owner's
input before proceeding. Log one as soon as you notice it — don't wait
until a dedicated audit pass. Update its status as it's resolved. Keep
entries short; link to `DECISIONS.md` if resolving one produces a real
architectural decision.

Format per entry:

```
### AR-NNN — short title
- **Area**: e.g. firmware/dosing, firmware/display, web, data-infra
- **Status**: open | confirmed | fixed | wontfix | needs-owner-input
- **Found**: date, by what (file/commit/observation)
- **What**: the finding, concretely
- **Why it matters**: concrete consequence if left as-is
- **Resolution**: (fill in once acted on)
```

---

### AR-001 — Button ISR mutates shared `state` without synchronization
- **Area**: firmware/architecture
- **Status**: open
- **Found**: 2026-09-11, reading `src/main.cpp` (`button_interrupt<pin>`,
  the `back` button ISR, and the main `switch(state)` loop)
- **What**: `state`, `button`, `last_button` are `volatile` globals written
  directly from interrupt context (`IRAM_ATTR` handlers) and read/written
  from the main loop with no critical section, mutex, or atomic. `volatile`
  prevents compiler reordering but does not make this safe against a
  genuine race between ISR and loop context on a dual-core part.
- **Why it matters**: plausible source of the "sporadic glitches and
  self-triggering" the README already admits to needing manual filtering
  for. The FreeRTOS task rewrite should replace this with a proper queue
  (ISR → `xQueueSendFromISR` → dosing/input task) instead of shared-memory
  signaling.
- **Resolution**: —

### AR-002 — `coffee_grinder_api/main.py` hardcodes a live DB password
- **Area**: data-infra
- **Status**: open — flagged only, not rotated (live credential on a
  running service; coordinate before changing)
- **Found**: 2026-09-11, `DB_PASSWORD = "..."` (plaintext literal) in
  `main.py` on `192.168.0.112` — value intentionally omitted here, don't
  re-add it to this repo
- **What**: plaintext Postgres password committed to that service's own
  git repo.
- **Why it matters**: moot once `coffee_grinder_api` is retired (D8), but
  worth confirming the credential gets rotated/removed as part of
  decommissioning `.112`, not just abandoned in a still-readable repo.
- **Resolution**: —

### AR-003 — `dev/graph` and `dev/settings` are disconnected mock tooling, not real dev tools
- **Area**: web
- **Status**: open
- **Found**: 2026-09-11, `dev/graph/websockets_server.py` and
  `dev/settings/websockets_server.py` serve canned/fake data on
  `localhost:8765`, unrelated to the real device.
- **What**: these look like live development tools from the README/repo
  layout but don't talk to the device at all.
- **Why it matters**: dead weight that could mislead a future contributor
  (or agent) into thinking they're a real integration point. Drop them
  once the new SPA replaces the three separate served pages, rather than
  porting them forward.
- **Resolution**: —

### AR-004 — Hardcoded dose correction constant in the API path
- **Area**: firmware/dosing
- **Status**: open
- **Found**: 2026-09-11, `src/main.cpp` `loopIdle()`: `float correction =
  requested_grams > 1.5f ? 1.5f : 0.0f;` with the comment "correction
  hardcoded for now"
- **What**: the HTTP API dosing path (`/api/getDosage`) applies a
  different, hardcoded correction margin instead of using the configurable
  `top_up_margin_single`/`top_up_margin_double` settings the button paths
  use.
- **Why it matters**: inconsistent behavior between button-triggered and
  API-triggered grinds; author's own comment marks it as unfinished. The
  new topup model should have one consistent way of deriving the
  correction margin regardless of trigger source.
- **Resolution**: —

### AR-005 — EEPROM settings versioning has no real migration path
- **Area**: firmware/settings
- **Status**: open
- **Found**: 2026-09-11, `WebSocketSettings.h`: single `magic = 0xCAFEBABE`
  field on the `Scale` struct, no version field, no migration logic
  observed alongside it.
- **What**: any change to the `Scale` struct's layout risks silently
  reading garbage (or the magic mismatching and falling back to defaults,
  losing calibration) rather than migrating old data forward.
- **Why it matters**: worth deciding deliberately whether the rewrite's
  settings storage (NVS is the more idiomatic ESP32 choice over raw
  EEPROM) gets real schema versioning, given calibration data is
  operationally important (re-calibrating is annoying).
- **Resolution**: —

### AR-006 — Pin sharing between `BUTTON_RIGHT` and `DISPLAY_MISO_PIN`
- **Area**: firmware/hardware
- **Status**: open — plausibly intentional, needs verification during
  the port, not a blocking concern
- **Found**: 2026-09-11, `include/defines.h`: `BUTTON_RIGHT` and
  `DISPLAY_MISO_PIN` are both pin 12.
- **What**: likely intentional and harmless — the ST7735 display is
  write-only from the MCU's perspective (no MISO read-back needed), so the
  pin is probably free to reuse for a button. Should be confirmed rather
  than assumed once the display driver is re-examined, since getting this
  wrong would produce very confusing intermittent bugs.
- **Resolution**: —
