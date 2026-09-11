import { useEffect, useMemo, useRef, useState } from "react";
import { Chart, LineController, LineElement, PointElement, LinearScale, Tooltip } from "chart.js";
import {
  fetchRecentSessions,
  fetchSessionEvents,
  fetchSessionRawSamples,
  type Session,
  type SessionEvent,
} from "../lib/postgrest";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip);

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
                  borderWidth: 2.5,
                  pointRadius: 0,
                  tension: 0.15,
                  parsing: false,
                },
              ],
            },
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
        {session.requested_weight_g}g, target {session.target_weight_g}g
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

  useEffect(() => {
    fetchRecentSessions(50)
      .then(setSessions)
      .catch((e) => setError(String(e)));
  }, []);

  const stats = useMemo(() => {
    if (!sessions || sessions.length === 0) return null;
    const completed = sessions.filter((s) => s.outcome === "completed" && s.final_weight_g !== null);
    if (completed.length === 0) return null;
    const errors = completed.map((s) => Math.abs(s.final_weight_g! - s.target_weight_g));
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
                    <td>{s.target_weight_g.toFixed(2)}g</td>
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
