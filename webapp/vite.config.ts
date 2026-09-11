import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Builds straight into webapp/dist, which platformio.ini points
// board_build's data_dir at -- `pio run -t buildfs` in the firmware repo
// packages whatever is here into the LittleFS image. No dev-server proxy
// config needed: in dev mode (`npm run dev`) the app talks to the real
// device's IP/hostname directly (see src/lib/config.ts), same as it will
// once served from the device itself.
export default defineConfig({
  plugins: [react()],
  build: {
    outDir: "dist",
    // Keep the bundle boring and cacheable -- content-hashed filenames,
    // no source maps shipped to the device (they'd just eat flash/LittleFS
    // space for no benefit once this is running on real hardware).
    sourcemap: false,
  },
});
