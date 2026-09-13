import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { wsUrl } from "./config";
import type {
  ErrorMessage,
  InboundMessage,
  OutboundMessage,
  RawReadMessage,
  SettingsMessage,
  TelemetryMessage,
} from "./types";

export type ConnectionStatus = "connecting" | "open" | "closed";

// How many telemetry messages the Live view's chart can plot before older
// points fall off -- generous enough for one full grind+topup session at
// the ~20Hz raw_sample rate design/postgrest-deployment.md describes,
// without letting a long-idle tab's memory grow unbounded.
const TELEMETRY_HISTORY_LIMIT = 2000;

interface DeviceSocketValue {
  status: ConnectionStatus;
  settings: SettingsMessage | null;
  telemetryHistory: TelemetryMessage[];
  lastError: ErrorMessage | null;
  // Latest reply only -- Advanced/Calibration page's own polling loop
  // watches this and accumulates its own local sample history from it;
  // there's no reason for every other page to carry that stream too.
  lastRawRead: RawReadMessage | null;
  send: (message: OutboundMessage) => void;
  nextRequestId: () => number;
}

const DeviceSocketContext = createContext<DeviceSocketValue | null>(null);

export function DeviceSocketProvider({ children }: { children: ReactNode }) {
  const [status, setStatus] = useState<ConnectionStatus>("connecting");
  const [settings, setSettings] = useState<SettingsMessage | null>(null);
  const [telemetryHistory, setTelemetryHistory] = useState<TelemetryMessage[]>([]);
  const [lastError, setLastError] = useState<ErrorMessage | null>(null);
  const [lastRawRead, setLastRawRead] = useState<RawReadMessage | null>(null);
  const socketRef = useRef<WebSocket | null>(null);
  const requestIdRef = useRef(1);

  useEffect(() => {
    let cancelled = false;
    let retryTimer: ReturnType<typeof setTimeout> | undefined;

    function connect() {
      if (cancelled) return;
      setStatus("connecting");
      const socket = new WebSocket(wsUrl);
      socketRef.current = socket;

      socket.onopen = () => setStatus("open");

      socket.onclose = () => {
        setStatus("closed");
        // The device reboots on its own after a WiFi reset/reboot flag, an
        // OTA update, or just a flaky home-LAN WiFi association -- reconnect
        // rather than making the user reload the page.
        retryTimer = setTimeout(connect, 2000);
      };

      socket.onerror = () => socket.close();

      socket.onmessage = (event) => {
        let parsed: InboundMessage;
        try {
          parsed = JSON.parse(event.data);
        } catch {
          return;
        }
        if (parsed.type === "settings") {
          setSettings(parsed);
        } else if (parsed.type === "error") {
          setLastError(parsed);
        } else if (parsed.type === "raw_read") {
          setLastRawRead(parsed);
        } else {
          setTelemetryHistory((prev) => {
            const next = [...prev, parsed];
            return next.length > TELEMETRY_HISTORY_LIMIT
              ? next.slice(next.length - TELEMETRY_HISTORY_LIMIT)
              : next;
          });
        }
      };
    }

    connect();
    return () => {
      cancelled = true;
      clearTimeout(retryTimer);
      socketRef.current?.close();
    };
  }, []);

  const nextRequestId = useCallback(() => requestIdRef.current++, []);

  const send = useCallback((message: OutboundMessage) => {
    const socket = socketRef.current;
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify(message));
    }
  }, []);

  return (
    <DeviceSocketContext.Provider
      value={{ status, settings, telemetryHistory, lastError, lastRawRead, send, nextRequestId }}
    >
      {children}
    </DeviceSocketContext.Provider>
  );
}

export function useDeviceSocket(): DeviceSocketValue {
  const ctx = useContext(DeviceSocketContext);
  if (!ctx) {
    throw new Error("useDeviceSocket must be used within a DeviceSocketProvider");
  }
  return ctx;
}
