import { useEffect, useRef, useState } from "react";
import { Chart, LineController, LineElement, PointElement, LinearScale, Tooltip, Filler } from "chart.js";
import { ACCENT_BLUE, linearAxis, tooltipTheme, weightLineDataset } from "../../lib/chartTheme";
import { formatTimestamp } from "../../lib/format";
import {
  fetchSessionEvents,
  fetchSessionRawSamples,
  type Session,
  type SessionEvent,
} from "../../lib/postgrest";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip, Filler);

/** One session's weight curve and its main-grind and topup pulse events. */
export default function SessionDetail({ session }: { session: Session }) {
  const [events, setEvents] = useState<SessionEvent[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  useEffect(() => {
    let cancelled = false;
    setEvents(null);
    setError(null);
    Promise.all([fetchSessionEvents(session.session_id), fetchSessionRawSamples(session.session_id)])
      .then(([sessionEvents, samples]) => {
        if (cancelled) return;
        setEvents(sessionEvents);
        if (!canvasRef.current) return;
        chartRef.current?.destroy();
        chartRef.current = new Chart(canvasRef.current, {
          type: "line",
          data: {
            datasets: [
              {
                label: "Weight (g)",
                data: samples.map((s) => ({ x: s.timestamp_ms / 1000, y: s.filtered_value })),
                ...weightLineDataset(ACCENT_BLUE, "rgba(10, 132, 255, 0.12)", 0.15),
              },
            ],
          },
          options: {
            responsive: true,
            animation: false,
            interaction: { mode: "index", intersect: false },
            scales: { x: linearAxis("Time (s)"), y: linearAxis("Weight (g)") },
            plugins: {
              tooltip: tooltipTheme(
                {
                  title: (items: { parsed: { x: number | null } }[]) => `t = ${(items[0]?.parsed.x ?? 0).toFixed(2)}s`,
                  label: (item: { parsed: { y: number | null } }) => `${(item.parsed.y ?? 0).toFixed(2)} g`,
                },
                false
              ),
            },
          },
        });
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
      {/* The firmware does not emit raw samples yet, so this chart stays empty. */}
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
