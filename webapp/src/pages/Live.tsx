import { useEffect, useMemo, useRef, useState } from "react";
import {
  Chart,
  LineController,
  LineElement,
  PointElement,
  LinearScale,
  Tooltip,
  Legend,
  type ChartDataset,
} from "chart.js";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type { TelemetryMessage } from "../lib/types";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip, Legend);

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
      backgroundColor: "#0a84ff",
      borderWidth: 2.5,
      pointRadius: 0,
      tension: 0.15,
      fill: false,
      parsing: false,
    };
    const targetDataset: ChartDataset<"line"> = {
      label: "Target",
      data: [],
      borderColor: "rgba(255, 159, 10, 0.7)",
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
        scales: {
          x: {
            type: "linear",
            title: { display: true, text: "Time (s)", color: "rgba(235, 235, 245, 0.6)" },
            ticks: { color: "rgba(235, 235, 245, 0.6)" },
            grid: { color: "rgba(84, 84, 88, 0.3)" },
          },
          y: {
            type: "linear",
            title: { display: true, text: "Weight (g)", color: "rgba(235, 235, 245, 0.6)" },
            ticks: { color: "rgba(235, 235, 245, 0.6)" },
            grid: { color: "rgba(84, 84, 88, 0.3)" },
          },
        },
        plugins: { legend: { labels: { color: "#ffffff" } } },
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
    (chart.data.datasets[0] as ChartDataset<"line">).borderColor = isComplete
      ? "#30d158"
      : "#0a84ff";
    chart.update();
  }, [sessionMessages, targetGrams, isComplete]);

  function requestDose() {
    const grams = parseFloat(doseInput);
    if (!Number.isFinite(grams) || grams <= 0) return;
    send({ type: "dose_request", grams, request_id: nextRequestId() });
  }

  return (
    <>
      <div className="panel">
        <div className="big-number">
          {latestGrams !== null ? latestGrams.toFixed(2) : "--"}
          <span style={{ fontSize: "1.5rem", color: "var(--text-dim)" }}> g</span>
        </div>
        <div className="muted">
          {targetGrams !== null ? `target ${targetGrams.toFixed(2)} g` : "no active session"}
          {currentSessionId !== null && ` · session #${currentSessionId}`}
        </div>
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
            max="40"
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
          {recentLogs.map((m, i) =>
            m.type === "log" ? (
              <div key={i} className="muted" style={{ fontSize: "0.85em" }}>
                [{m.runtime_ms}ms] {m.line}
              </div>
            ) : null
          )}
        </div>
      )}
    </>
  );
}
