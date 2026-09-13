import { useEffect, useMemo, useRef, useState } from "react";
import {
  Chart,
  LineController,
  LineElement,
  PointElement,
  BarController,
  BarElement,
  LinearScale,
  Tooltip,
  Legend,
  Filler,
} from "chart.js";
import {
  fetchRecentSessions,
  fetchSessionEvents,
  fetchSessionRawSamples,
  fetchCompletedDosesForMode,
  type Session,
  type SessionEvent,
} from "../lib/postgrest";

Chart.register(
  LineController,
  LineElement,
  PointElement,
  BarController,
  BarElement,
  LinearScale,
  Tooltip,
  Legend,
  Filler
);

const HISTOGRAM_RANGE_G = 0.5;
const HISTOGRAM_BIN_WIDTH_G = 0.05;

function fmt(n: number, digits = 3): string {
  return n.toFixed(digits);
}

// Delta-from-target histogram with a fitted Gaussian overlay, x axis fixed
// to +/-HISTOGRAM_RANGE_G regardless of what the data actually spans -- so
// the single/double panels stay visually comparable to each other.
function DeltaHistogram({
  label,
  deltas,
  color,
}: {
  label: string;
  deltas: number[];
  color: string;
}) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  // Fit only over the displayed +/-HISTOGRAM_RANGE_G window: a handful of
  // sessions predate hardware fixes (AR-041/042/043) and have final weights
  // wildly off target (0g, negative, 100g+) -- real numbers, but not
  // representative of current dosing accuracy, and the chart can't show
  // them anyway. Folding them into mean/sd would silently distort a stat
  // that's supposed to describe how well the scale doses today.
  const fit = useMemo(() => {
    const inRange = deltas.filter((d) => d >= -HISTOGRAM_RANGE_G && d < HISTOGRAM_RANGE_G);
    const n = inRange.length;
    if (n === 0) return null;
    const mean = inRange.reduce((a, b) => a + b, 0) / n;
    const variance =
      n > 1 ? inRange.reduce((a, b) => a + (b - mean) ** 2, 0) / (n - 1) : 0;
    const sd = Math.sqrt(variance);
    return { n, mean, sd, excluded: deltas.length - n };
  }, [deltas]);

  useEffect(() => {
    if (!canvasRef.current) return;

    const binCount = Math.round((2 * HISTOGRAM_RANGE_G) / HISTOGRAM_BIN_WIDTH_G);
    const bins = Array.from({ length: binCount }, (_, i) => ({
      center: -HISTOGRAM_RANGE_G + (i + 0.5) * HISTOGRAM_BIN_WIDTH_G,
      count: 0,
    }));
    for (const d of deltas) {
      if (d < -HISTOGRAM_RANGE_G || d >= HISTOGRAM_RANGE_G) continue;
      const idx = Math.min(
        binCount - 1,
        Math.floor((d + HISTOGRAM_RANGE_G) / HISTOGRAM_BIN_WIDTH_G)
      );
      bins[idx].count++;
    }

    const curve: { x: number; y: number }[] = [];
    if (fit && fit.sd > 0) {
      const steps = 120;
      for (let i = 0; i <= steps; i++) {
        const x = -HISTOGRAM_RANGE_G + (i / steps) * 2 * HISTOGRAM_RANGE_G;
        const z = (x - fit.mean) / fit.sd;
        const density = Math.exp(-0.5 * z * z) / (fit.sd * Math.sqrt(2 * Math.PI));
        curve.push({ x, y: fit.n * HISTOGRAM_BIN_WIDTH_G * density });
      }
    }

    chartRef.current?.destroy();
    chartRef.current = new Chart(canvasRef.current, {
      data: {
        datasets: [
          {
            type: "bar",
            label: `${label} (n=${fit?.n ?? 0})`,
            data: bins.map((b) => ({ x: b.center, y: b.count })),
            backgroundColor: `${color}99`,
            borderWidth: 0,
            borderRadius: { topLeft: 3, topRight: 3, bottomLeft: 0, bottomRight: 0 },
            borderSkipped: false,
            parsing: false,
          },
          ...(curve.length > 0
            ? [
                {
                  type: "line" as const,
                  label: "Gaussian fit",
                  data: curve,
                  borderColor: color,
                  borderWidth: 2,
                  pointRadius: 0,
                  fill: false,
                  tension: 0.25,
                  parsing: false as const,
                },
              ]
            : []),
        ],
      },
      options: {
        responsive: true,
        animation: false,
        scales: {
          x: {
            type: "linear",
            min: -HISTOGRAM_RANGE_G,
            max: HISTOGRAM_RANGE_G,
            title: { display: true, text: "Δ from target (g)", color: "rgba(235, 235, 245, 0.45)" },
            ticks: { color: "rgba(235, 235, 245, 0.45)" },
            grid: { color: "rgba(84, 84, 88, 0.2)" },
            border: { display: false },
          },
          y: {
            type: "linear",
            beginAtZero: true,
            title: { display: true, text: "Count", color: "rgba(235, 235, 245, 0.45)" },
            ticks: { color: "rgba(235, 235, 245, 0.45)" },
            grid: { color: "rgba(84, 84, 88, 0.2)" },
            border: { display: false },
          },
        },
        plugins: {
          legend: {
            labels: { color: "rgba(235, 235, 245, 0.75)", boxWidth: 14, boxHeight: 10 },
          },
          tooltip: {
            backgroundColor: "#1c1c1e",
            titleColor: "rgba(235, 235, 245, 0.6)",
            bodyColor: "#ffffff",
            borderColor: "rgba(84, 84, 88, 0.65)",
            borderWidth: 1,
            padding: 10,
            cornerRadius: 8,
            callbacks: {
              title: (items) => `Δ ${(items[0]?.parsed.x ?? 0).toFixed(2)}g`,
            },
          },
        },
      },
    });
    return () => chartRef.current?.destroy();
  }, [deltas, fit, label, color]);

  return (
    <div>
      <h4 style={{ margin: "0 0 0.5rem" }}>{label}</h4>
      {deltas.length === 0 ? (
        <p className="muted">No completed sessions yet.</p>
      ) : (
        <>
          <canvas ref={canvasRef} height={200} />
          {fit && (
            <p className="muted" style={{ fontSize: "0.85em" }}>
              μ = {fmt(fit.mean)}g, σ = {fmt(fit.sd)}g, n = {fit.n}
              {fit.excluded > 0 &&
                ` (${fit.excluded} outside ±${HISTOGRAM_RANGE_G}g excluded from the fit)`}
            </p>
          )}
        </>
      )}
    </div>
  );
}

function formatTimestamp(iso: string): string {
  return new Date(iso).toLocaleString();
}

function SessionDetail({ session }: { session: Session }) {
  const [events, setEvents] = useState<SessionEvent[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  useEffect(() => {
    let cancelled = false;
    setEvents(null);
    setError(null);
    Promise.all([
      fetchSessionEvents(session.session_id),
      fetchSessionRawSamples(session.session_id),
    ])
      .then(([ev, samples]) => {
        if (cancelled) return;
        setEvents(ev);
        if (canvasRef.current) {
          chartRef.current?.destroy();
          chartRef.current = new Chart(canvasRef.current, {
            type: "line",
            data: {
              datasets: [
                {
                  label: "Weight (g)",
                  data: samples.map((s) => ({ x: s.timestamp_ms / 1000, y: s.filtered_value })),
                  borderColor: "#0a84ff",
                  backgroundColor: "rgba(10, 132, 255, 0.12)",
                  borderWidth: 2,
                  pointRadius: 0,
                  pointHoverRadius: 4,
                  pointHoverBackgroundColor: "#0a84ff",
                  pointHoverBorderColor: "#0b0b0c",
                  pointHoverBorderWidth: 2,
                  tension: 0.15,
                  fill: "origin",
                  parsing: false,
                },
              ],
            },
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
                  title: { display: true, text: "Weight (g)", color: "rgba(235, 235, 245, 0.45)" },
                  ticks: { color: "rgba(235, 235, 245, 0.45)" },
                  grid: { color: "rgba(84, 84, 88, 0.2)" },
                  border: { display: false },
                },
              },
              plugins: {
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
                    label: (item) => `${(item.parsed.y ?? 0).toFixed(2)} g`,
                  },
                },
              },
            },
          });
        }
      })
      .catch((e) => !cancelled && setError(String(e)));
    return () => {
      cancelled = true;
      chartRef.current?.destroy();
      chartRef.current = null;
    };
  }, [session.session_id]);

  return (
    <div className="panel">
      <h3>Session {session.session_id.slice(0, 8)}</h3>
      <p className="muted">
        {formatTimestamp(session.started_at)} · {session.mode} · requested{" "}
        {session.requested_weight_g}g, main-grind stop {session.target_weight_g}g
        {session.final_weight_g !== null && `, final ${session.final_weight_g}g`}
      </p>
      {error && <p style={{ color: "var(--red)" }}>{error}</p>}
      {/* raw_samples is only populated once DosingTask starts emitting
          RAW_SAMPLE telemetry -- the canvas renders an empty chart until
          then, which is honest about current backend capability rather
          than a bug here. */}
      <canvas ref={canvasRef} height={180} />
      {events && events.length > 0 && (
        <div className="table-scroll" style={{ marginTop: "1rem" }}>
          <table>
            <thead>
              <tr>
                <th>#</th>
                <th>type</th>
                <th>on (ms)</th>
                <th>off (ms)</th>
                <th>runtime (ms)</th>
                <th>before (g)</th>
                <th>after (g)</th>
                <th>Δ (g)</th>
              </tr>
            </thead>
            <tbody>
              {events.map((e) => (
                <tr key={e.event_id}>
                  <td>{e.pulse_index}</td>
                  <td>{e.event_type}</td>
                  <td>{e.relay_on_at_ms}</td>
                  <td>{e.relay_off_at_ms}</td>
                  <td>{e.runtime_ms}</td>
                  <td>{e.weight_before_g.toFixed(2)}</td>
                  <td>{e.weight_after_g?.toFixed(2) ?? "--"}</td>
                  <td>{e.weight_increment_g?.toFixed(2) ?? "--"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}

export default function HistoryPage() {
  const [sessions, setSessions] = useState<Session[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [selected, setSelected] = useState<Session | null>(null);
  const [singleDeltas, setSingleDeltas] = useState<number[] | null>(null);
  const [doubleDeltas, setDoubleDeltas] = useState<number[] | null>(null);

  useEffect(() => {
    fetchRecentSessions(50)
      .then(setSessions)
      .catch((e) => setError(String(e)));
  }, []);

  useEffect(() => {
    fetchCompletedDosesForMode("single")
      .then((doses) => setSingleDeltas(doses.map((d) => d.final_weight_g - d.requested_weight_g)))
      .catch(() => setSingleDeltas([]));
    fetchCompletedDosesForMode("double")
      .then((doses) => setDoubleDeltas(doses.map((d) => d.final_weight_g - d.requested_weight_g)))
      .catch(() => setDoubleDeltas([]));
  }, []);

  const stats = useMemo(() => {
    if (!sessions || sessions.length === 0) return null;
    const completed = sessions.filter((s) => s.outcome === "completed" && s.final_weight_g !== null);
    if (completed.length === 0) return null;
    const errors = completed.map((s) => Math.abs(s.final_weight_g! - s.requested_weight_g));
    const withinSpot = errors.filter((e) => e < 0.05).length;
    const within02 = errors.filter((e) => e < 0.2).length;
    return {
      n: completed.length,
      spotOnPct: (100 * withinSpot) / completed.length,
      within02Pct: (100 * within02) / completed.length,
    };
  }, [sessions]);

  return (
    <>
      {/* The project's accuracy targets (80% within 0.05g, 95% within 0.2g),
          computed live from whatever real sessions exist so far. */}
      {stats && (
        <div className="panel">
          <h3>Accuracy (last {stats.n} completed sessions)</h3>
          <p>
            <strong>{stats.spotOnPct.toFixed(0)}%</strong> spot on (Δ&lt;0.05g, target 80%) ·{" "}
            <strong>{stats.within02Pct.toFixed(0)}%</strong> within 0.2g (target 95%)
          </p>
        </div>
      )}

      <div className="panel">
        <h3>Dose accuracy by target</h3>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Final weight minus target, one histogram per fixed target dose (single/double), fit
          with a Gaussian. A narrower, more centered curve means tighter, less biased dosing.
        </p>
        <div
          style={{
            display: "grid",
            gridTemplateColumns: "repeat(auto-fit, minmax(280px, 1fr))",
            gap: "1.5rem",
          }}
        >
          <DeltaHistogram label="Single dose" deltas={singleDeltas ?? []} color="#0a84ff" />
          <DeltaHistogram label="Double dose" deltas={doubleDeltas ?? []} color="#ff9f0a" />
        </div>
      </div>

      <div className="panel">
        <h3>Recent sessions</h3>
        {error && <p style={{ color: "var(--red)" }}>{error}</p>}
        {!sessions && !error && <p className="muted">Loading from PostgREST...</p>}
        {sessions && sessions.length === 0 && <p className="muted">No sessions recorded yet.</p>}
        {sessions && sessions.length > 0 && (
          <div className="table-scroll">
            <table>
              <thead>
                <tr>
                  <th>Started</th>
                  <th>Mode</th>
                  <th>Target</th>
                  <th>Final</th>
                  <th>Outcome</th>
                </tr>
              </thead>
              <tbody>
                {sessions.map((s) => (
                  <tr key={s.session_id} className="session-row" onClick={() => setSelected(s)}>
                    <td>{formatTimestamp(s.started_at)}</td>
                    <td>{s.mode}</td>
                    <td>{s.requested_weight_g.toFixed(2)}g</td>
                    <td>{s.final_weight_g !== null ? `${s.final_weight_g.toFixed(2)}g` : "--"}</td>
                    <td>
                      <span className={`outcome ${s.outcome}`}>{s.outcome}</span>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>

      {selected && <SessionDetail session={selected} />}
    </>
  );
}
