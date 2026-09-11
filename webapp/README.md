# Eureka SPA

The web app served from the ESP32's LittleFS partition. Replaces the
old PROGMEM `/console` page and the local-only `dev/graph`/`dev/settings`
mocks with one real app talking to the device's consolidated `/ws`
channel (NetworkTask.cpp) and, for history/analytics, directly to
PostgREST — never through the device.

React + TypeScript + Vite, no CSS framework (plain CSS matching the old
mock pages' dark/monospace aesthetic — see `src/index.css`'s comment for
why this stays dark-only rather than theme-aware). See
`.agent/DECISIONS.md` for the full framework rationale.

## Develop

```
npm install
npm run dev
```

By default the dev server assumes it's running *on* the device's own
origin (`window.location.host`), which is wrong when you're iterating with
`npm run dev` on a laptop against a real device on the LAN. Point it at
the device with a `.env.local` (gitignored):

```
VITE_DEVICE_HOST=eureka.local
```

## Build for the device

```
npm run build
```

Outputs to `dist/`, which `platformio.ini`'s `data_dir` points at — from
the firmware repo root:

```
pio run -t buildfs -e esp_wroom_02
```

builds the LittleFS image locally. **Do not run `pio run -t uploadfs`** —
flashing the physical device is the owner's call only (see `.agent/AGENTS.md`).

## Known gaps (see `.agent/ARS.md`)

- Only 8 of `SettingsSnapshot`'s fields have a write path at all
  (`SettingsFieldId` in `lib/Messaging/Messages.h`) — ADC gain/speed/
  read_samples and the various timeout fields aren't editable from this UI
  yet because the firmware doesn't accept writes for them.
- The Live page's chart is only as granular as the session-level telemetry
  events DosingTask currently emits (target/progress/topup_pulse/finalize/
  complete) — `RAW_SAMPLE` telemetry isn't emitted yet, so there's no
  ~20Hz live curve, just the handful of points per session that already
  exist.
