// Served from the device's own LittleFS, the page's origin is the device. Only `npm run dev` needs
// an override: set VITE_DEVICE_HOST in the gitignored webapp/.env.local to reach a device on the LAN.
const override = import.meta.env.VITE_DEVICE_HOST;

export const deviceHost = override && override.length > 0 ? override : window.location.host;

export const wsUrl = `ws://${deviceHost}/ws`;

// The browser queries PostgREST directly, never through the device.
export const postgrestBase = "http://192.168.0.111:3000";
