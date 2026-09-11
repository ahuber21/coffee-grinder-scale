// Once built and served from the device's own LittleFS, this page's origin
// *is* the device -- window.location.host is always right. The only case
// that needs an override is `npm run dev` on a developer's laptop, where
// the page is served from Vite's dev server but should still talk to a
// real device on the LAN. VITE_DEVICE_HOST covers that (set it in
// webapp/.env.local, gitignored, never committed -- see webapp/README.md).
const override = import.meta.env.VITE_DEVICE_HOST;

export const deviceHost = override && override.length > 0 ? override : window.location.host;

export const wsUrl = `ws://${deviceHost}/ws`;

// The browser queries PostgREST directly, bypassing the device --
// hardcoded to the known LAN host/port. Not device-relative, so no
// override needed the way deviceHost has one.
export const postgrestBase = "http://192.168.0.111:3000";
