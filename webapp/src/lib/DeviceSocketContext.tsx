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

/// Telemetry kept for the Live chart: enough for a full session at ~20Hz, without unbounded growth.
const TELEMETRY_HISTORY_LIMIT = 2000;

interface DeviceSocketValue {
  status: ConnectionStatus;
  settings: SettingsMessage | null;
  telemetryHistory: TelemetryMessage[];
  lastError: ErrorMessage | null;
  /** Only the latest reply; the Calibration page accumulates its own history from it. */
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
        // The device reboots after OTA or a WiFi action, so reconnect rather than make the user reload.
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
