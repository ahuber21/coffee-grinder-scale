import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./index.css";
import App from "./App";
import { DeviceSocketProvider } from "./lib/DeviceSocketContext";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <DeviceSocketProvider>
      <App />
    </DeviceSocketProvider>
  </StrictMode>
);
