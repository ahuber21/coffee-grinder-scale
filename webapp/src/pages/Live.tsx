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
import {
  ACCENT_BLUE,
  ACCENT_GREEN,
  legendLabels,
  linearAxis,
  tooltipTheme,
  weightLineDataset,
} from "../lib/chartTheme";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type { TelemetryMessage } from "../lib/types";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip, Legend, Filler);

// A "target" event always opens a new session, which is what resets the chart below.
// The guard needs target_grams as well as grams, because raw_sample carries grams alone.
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

  // No state message exists, but the event sequence implies it: target opens GRINDING,
  // progress marks the main grind's stop, and complete closes the session.
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
  const meterColor = lerpHex(ACCENT_BLUE, ACCENT_GREEN, progressFrac);

  const recentLogs = useMemo(
    () => telemetryHistory.filter((m) => m.type === "log").slice(-8),
    [telemetryHistory]
  );

  useEffect(() => {
    if (!canvasRef.current) return;
    const dataset: ChartDataset<"line"> = {
      label: "Weight (g)",
      data: [],
      ...weightLineDataset(ACCENT_BLUE, "rgba(10, 132, 255, 0.12)", 0.2),
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
          x: linearAxis("Time (s)"),
          y: linearAxis("Weight (g)", { beginAtZero: true }),
        },
        plugins: {
          legend: legendLabels(14, { boxHeight: 2 }),
          tooltip: tooltipTheme(
            {
              title: (items: { parsed: { x: number | null } }[]) => `t = ${(items[0]?.parsed.x ?? 0).toFixed(2)}s`,
              label: (item: { dataset: { label?: string }; parsed: { y: number | null } }) =>
                `${item.dataset.label}: ${(item.parsed.y ?? 0).toFixed(2)} g`,
            },
            false
          ),
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
    const lineColor = isComplete ? ACCENT_GREEN : ACCENT_BLUE;
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
          Same effect as the physical buttons, with a custom target weight; the usual top-up
          margin applies, and the session is recorded as a single dose.
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
