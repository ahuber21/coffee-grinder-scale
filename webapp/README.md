# Eureka SPA

The web app served from the ESP32's LittleFS partition. It talks to the
device over the single `/ws` WebSocket (`lib/NetworkTask`) and, for history
and analytics, directly to PostgREST, never through the device.

React, TypeScript and Vite, with plain CSS in a dark-only style that matches
the device's own display (see `src/index.css`).

## Layout

- `src/pages/` -- one file per tab: Live, Settings, History, Advanced (with
  its Tare, Calibration and Model sub-pages).
- `src/components/` -- shared pieces: the settings rows and the error histogram.
- `src/lib/` -- the WebSocket context, the wire types, the PostgREST client and
  the shared chart styling.

The wire types in `src/lib/types.ts` mirror `lib/NetworkTask/NetworkTask.cpp`
by hand. A new setting needs a row in `lib/Messaging/SettingsSchema.h`, a field
on `SettingsMessage`, and a control on the Settings page.

## Develop

```
npm install
npm run dev
```

The dev server assumes it runs on the device's own origin. To work against a
device on the LAN, set it in the gitignored `.env.local`:

```
VITE_DEVICE_HOST=eureka.local
```

## Build for the device

```
npm run build
```

writes `dist/`, which `platformio.ini`'s `data_dir` points at. From the
repository root, `pio run -t uploadfs -e esp_wroom_02_ota` builds the
LittleFS image and flashes it over WiFi.

## Known gaps

- The firmware does not emit `RAW_SAMPLE` telemetry yet, so the Live chart has
  only the handful of points each session already produces and the History
  session chart stays empty.
- API doses are recorded as `single`, so they appear in the single-dose
  histograms.
