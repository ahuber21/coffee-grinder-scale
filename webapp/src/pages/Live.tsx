import { useEffect, useMemo, useRef, useState } from "react";
import {
  Chart,
  LineController,
  LineElement,
  PointElement,
  LinearScale,
  Tooltip,
  Legend,
  Filler,
  type ChartDataset,
} from "chart.js";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type { TelemetryMessage } from "../lib/types";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip, Legend, Filler);

// Sessions are identified by session_id (Messages.h: "assigned by
// dosing_task at CONFIGURED entry"), not by a dedicated "session started"
// boolean over the wire -- a "target" event is always the first message of
// a new session, so that's the reset signal for the chart below.
//
// Requires target_grams too, not just grams -- raw_sample also carries
// grams but has no target_grams field, and every call site below reads
// both off the narrowed type.
type TelemetryWithTarget = Extract<TelemetryMessage, { grams: number; target_grams: number }>;

function hasTargetGrams(m: TelemetryMessage): m is TelemetryWithTarget {
  return "grams" in m && "target_grams" in m;
}

/** Linear interpolation between two hex colors ("#rrggbb"), t clamped to 0..1. */
function lerpHex(a: string, b: string, t: number): string {
  const c = Math.max(0, Math.min(1, t));
  const ar = parseInt(a.slice(1, 3), 16);
  const ag = parseInt(a.slice(3, 5), 16);
  const ab = parseInt(a.slice(5, 7), 16);
  const br = parseInt(b.slice(1, 3), 16);
  const bg = parseInt(b.slice(3, 5), 16);
  const bb = parseInt(b.slice(5, 7), 16);
  const r = Math.round(ar + (br - ar) * c);
  const g = Math.round(ag + (bg - ag) * c);
  const bl = Math.round(ab + (bb - ab) * c);
  return `#${r.toString(16).padStart(2, "0")}${g.toString(16).padStart(2, "0")}${bl
    .toString(16)
    .padStart(2, "0")}`;
}

type Phase = "idle" | "grinding" | "topping-up" | "done";

const PHASE_LABEL: Record<Phase, string> = {
  idle: "Ready",
  grinding: "Grinding",
  "topping-up": "Topping up",
  done: "Done",
};

export default function LivePage() {
  const { status, telemetryHistory, send, nextRequestId } = useDeviceSocket();
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);
  const [doseInput, setDoseInput] = useState("18");

  const currentSessionId = useMemo(() => {
    for (let i = telemetryHistory.length - 1; i >= 0; i--) {
      const m = telemetryHistory[i];
      if (m.type === "target") return m.session_id;
    }
    return null;
  }, [telemetryHistory]);

  const sessionMessages = useMemo(
    () =>
      currentSessionId === null
        ? []
        : telemetryHistory.filter((m) => m.session_id === currentSessionId),
    [telemetryHistory, currentSessionId]
  );

  const latestGrams = useMemo(() => {
    for (let i = sessionMessages.length - 1; i >= 0; i--) {
      const m = sessionMessages[i];
      if (hasTargetGrams(m)) return m.grams;
    }
    return null;
  }, [sessionMessages]);

  const targetGrams = useMemo(() => {
    for (let i = sessionMessages.length - 1; i >= 0; i--) {
      const m = sessionMessages[i];
      if (hasTargetGrams(m)) return m.target_grams;
    }
    return null;
  }, [sessionMessages]);

  const isComplete = sessionMessages.some((m) => m.type === "complete");

  // Derived purely from which telemetry types this session has seen so
  // far -- there's no explicit FSM-state message over the wire, but the
  // message sequence maps 1:1 to it: target opens GRINDING, progress marks
  // the main grind's stop (entering STOPPING/TOPUP), complete closes it out.
  const phase: Phase = useMemo(() => {
    if (currentSessionId === null) return "idle";
    if (isComplete) return "done";
    if (sessionMessages.some((m) => m.type === "progress")) return "topping-up";
    return "grinding";
  }, [currentSessionId, isComplete, sessionMessages]);

  const progressFrac =
    latestGrams !== null && targetGrams !== null && targetGrams > 0
      ? Math.max(0, Math.min(1, latestGrams / targetGrams))
      : 0;
  const meterColor = lerpHex("#0a84ff", "#30d158", progressFrac);

  const recentLogs = useMemo(
    () => telemetryHistory.filter((m) => m.type === "log").slice(-8),
    [telemetryHistory]
  );

  useEffect(() => {
    if (!canvasRef.current) return;
    const dataset: ChartDataset<"line"> = {
      label: "Weight (g)",
      data: [],
      borderColor: "#0a84ff",
      backgroundColor: "rgba(10, 132, 255, 0.12)",
      borderWidth: 2,
      pointRadius: 0,
      pointHoverRadius: 4,
      pointHoverBackgroundColor: "#0a84ff",
      pointHoverBorderColor: "#0b0b0c",
      pointHoverBorderWidth: 2,
      tension: 0.2,
      fill: "origin",
      parsing: false,
    };
    const targetDataset: ChartDataset<"line"> = {
      label: "Target",
      data: [],
      borderColor: "rgba(255, 159, 10, 0.65)",
      borderDash: [4, 4],
      borderWidth: 1.5,
      pointRadius: 0,
      fill: false,
      parsing: false,
    };
    chartRef.current = new Chart(canvasRef.current, {
      type: "line",
      data: { datasets: [dataset, targetDataset] },
      options: {
        responsive: true,
        animation: false,
        interaction: { mode: "index", intersect: false },
        scales: {
          x: {
            type: "linear",
            title: { display: true, text: "Time (s)", color: "rgba(235, 235, 245, 0.45)" },
            ticks: { color: "rgba(235, 235, 245, 0.45)" },
            grid: { color: "rgba(84, 84, 88, 0.2)" },
            border: { display: false },
          },
          y: {
            type: "linear",
            beginAtZero: true,
            title: { display: true, text: "Weight (g)", color: "rgba(235, 235, 245, 0.45)" },
            ticks: { color: "rgba(235, 235, 245, 0.45)" },
            grid: { color: "rgba(84, 84, 88, 0.2)" },
            border: { display: false },
          },
        },
        plugins: {
          legend: { labels: { color: "rgba(235, 235, 245, 0.75)", boxWidth: 14, boxHeight: 2 } },
          tooltip: {
            backgroundColor: "#1c1c1e",
            titleColor: "rgba(235, 235, 245, 0.6)",
            bodyColor: "#ffffff",
            borderColor: "rgba(84, 84, 88, 0.65)",
            borderWidth: 1,
            padding: 10,
            cornerRadius: 8,
            displayColors: false,
            callbacks: {
              title: (items) => `t = ${(items[0]?.parsed.x ?? 0).toFixed(2)}s`,
              label: (item) => `${item.dataset.label}: ${(item.parsed.y ?? 0).toFixed(2)} g`,
            },
          },
        },
      },
    });
    return () => chartRef.current?.destroy();
  }, []);

  useEffect(() => {
    const chart = chartRef.current;
    if (!chart) return;
    const points = sessionMessages
      .filter(hasTargetGrams)
      .map((m) => ({ x: m.runtime_ms / 1000, y: m.grams }));
    chart.data.datasets[0].data = points;
    if (targetGrams !== null && points.length > 0) {
      const maxX = points[points.length - 1].x;
      chart.data.datasets[1].data = [
        { x: 0, y: targetGrams },
        { x: maxX, y: targetGrams },
      ];
    } else {
      chart.data.datasets[1].data = [];
    }
    const lineColor = isComplete ? "#30d158" : "#0a84ff";
    const lineDataset = chart.data.datasets[0] as ChartDataset<"line">;
    lineDataset.borderColor = lineColor;
    lineDataset.backgroundColor = isComplete
      ? "rgba(48, 209, 88, 0.12)"
      : "rgba(10, 132, 255, 0.12)";
    lineDataset.pointHoverBackgroundColor = lineColor;
    chart.update();
  }, [sessionMessages, targetGrams, isComplete]);

  function requestDose() {
    const grams = parseFloat(doseInput);
    if (!Number.isFinite(grams) || grams <= 0) return;
    send({ type: "dose_request", grams, request_id: nextRequestId() });
  }

  return (
    <>
      <div className="panel hero">
        <div className="hero-top">
          <span className={`phase-pill phase-${phase}`}>{PHASE_LABEL[phase]}</span>
          {currentSessionId !== null && (
            <span className="muted hero-session">session #{currentSessionId}</span>
          )}
        </div>

        <div className="hero-number">
          {latestGrams !== null ? latestGrams.toFixed(2) : "0.00"}
          <span className="hero-unit"> g</span>
        </div>

        {targetGrams !== null ? (
          <>
            <div className="meter-track">
              <div
                className="meter-fill"
                style={{ width: `${progressFrac * 100}%`, background: meterColor }}
              />
            </div>
            <div className="muted hero-target">target {targetGrams.toFixed(2)} g</div>
          </>
        ) : (
          <div className="muted hero-target">Place a cup and press a dose button to begin</div>
        )}
      </div>

      <div className="panel">
        <canvas ref={canvasRef} height={200} />
      </div>

      <div className="panel">
        <h3>Request a dose</h3>
        <div style={{ display: "flex", gap: "0.75rem", alignItems: "center" }}>
          <input
            type="number"
            step="0.1"
            min="0.1"
            max="50"
            value={doseInput}
            onChange={(e) => setDoseInput(e.target.value)}
          />
          <button className="primary" onClick={requestDose} disabled={status !== "open"}>
            Grind
          </button>
        </div>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Same effect as the physical single/double buttons, with a custom
          target weight -- routed through DosingTask's normal correction
          logic (mode "api_custom" in the session history).
        </p>
      </div>

      {recentLogs.length > 0 && (
        <div className="panel">
          <h3>Recent log lines</h3>
          <div className="log-lines">
            {recentLogs.map((m, i) =>
              m.type === "log" ? (
                <div key={i} className="log-line">
                  <span className="log-time">{m.runtime_ms}ms</span> {m.line}
                </div>
              ) : null
            )}
          </div>
        </div>
      )}
    </>
  );
}
