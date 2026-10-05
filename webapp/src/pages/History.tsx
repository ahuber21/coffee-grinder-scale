import { useEffect, useMemo, useState } from "react";
import DeltaHistogram from "../components/DeltaHistogram";
import { ACCENT_BLUE, ACCENT_ORANGE } from "../lib/chartTheme";
import { formatTimestamp } from "../lib/format";
import {
  fetchCompletedDosesForMode,
  fetchLandingForMode,
  fetchRecentSessions,
  type Session,
} from "../lib/postgrest";
import ReferenceWeightCell from "./history/ReferenceWeightCell";
import SessionDetail from "./history/SessionDetail";

/// "Spot on" is within this many grams of the request; the second goal is within 0.2g.
const SPOT_ON_THRESHOLD_G = 0.1;
const WITHIN_GOAL_G = 0.2;

/** Loads a list of errors in grams, treating a failed request as an empty list. */
function useDeltas(load: () => Promise<number[]>): number[] | null {
  const [deltas, setDeltas] = useState<number[] | null>(null);
  useEffect(() => {
    load()
      .then(setDeltas)
      .catch(() => setDeltas([]));
    // `load` is a fresh closure each render; the data is fetched once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return deltas;
}

/**
 * Final weight minus the requested weight. The requested weight, not the
 * main-grind stop target, because topup's job is closing that margin.
 */
const finalError = (mode: "single" | "double") => () =>
  fetchCompletedDosesForMode(mode).then((doses) =>
    doses.map((d) => d.final_weight_g - d.requested_weight_g)
  );

/** Settled main-grind weight minus the stop target, before any topup. */
const landingError = (mode: "single" | "double") => () =>
  fetchLandingForMode(mode).then((rows) => rows.map((r) => r.settled_weight_g - r.target_weight_g));

const twoColumnGrid = {
  display: "grid",
  gridTemplateColumns: "repeat(auto-fit, minmax(280px, 1fr))",
  gap: "1.5rem",
};

export default function HistoryPage() {
  const [sessions, setSessions] = useState<Session[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [selected, setSelected] = useState<Session | null>(null);
  const singleDeltas = useDeltas(finalError("single"));
  const doubleDeltas = useDeltas(finalError("double"));
  const singleLanding = useDeltas(landingError("single"));
  const doubleLanding = useDeltas(landingError("double"));

  useEffect(() => {
    fetchRecentSessions(50)
      .then(setSessions)
      .catch((e) => setError(String(e)));
  }, []);

  const stats = useMemo(() => {
    const completed = (sessions ?? []).filter(
      (s) => s.outcome === "completed" && s.final_weight_g !== null
    );
    if (completed.length === 0) return null;
    const errors = completed.map((s) => Math.abs(s.final_weight_g! - s.requested_weight_g));
    const share = (limit: number) => (100 * errors.filter((e) => e < limit).length) / errors.length;
    return {
      n: completed.length,
      spotOnPct: share(SPOT_ON_THRESHOLD_G),
      within02Pct: share(WITHIN_GOAL_G),
    };
  }, [sessions]);

  return (
    <>
      {stats && (
        <div className="panel">
          <h3>Accuracy (last {stats.n} completed sessions)</h3>
          <p>
            <strong>{stats.spotOnPct.toFixed(0)}%</strong> spot on (Δ&lt;
            {SPOT_ON_THRESHOLD_G.toFixed(2)}g, target 80%) ·{" "}
            <strong>{stats.within02Pct.toFixed(0)}%</strong> within {WITHIN_GOAL_G}g (target 95%)
          </p>
        </div>
      )}

      <div className="panel">
        <h3>Dose accuracy by target</h3>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Final weight minus target, one histogram per fixed target dose (single/double), fit
          with a Gaussian. A narrower, more centered curve means tighter, less biased dosing.
        </p>
        <div style={twoColumnGrid}>
          <DeltaHistogram label="Single dose" deltas={singleDeltas ?? []} color={ACCENT_BLUE} />
          <DeltaHistogram label="Double dose" deltas={doubleDeltas ?? []} color={ACCENT_ORANGE} />
        </div>
      </div>

      <div className="panel">
        <h3>Main-grind landing (landing learner)</h3>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Settled weight right after the main grind, before any top-up, minus the stop target
          (requested dose minus the top-up margin). The learner's job is to pull this curve
          narrower and onto zero. Only grinds that recorded a settled weight appear here, i.e.
          the learner era onward.
        </p>
        <div style={twoColumnGrid}>
          <DeltaHistogram
            label="Single dose"
            deltas={singleLanding ?? []}
            color={ACCENT_BLUE}
            xLabel="Δ from stop target (g)"
            emptyText="No landing data yet."
          />
          <DeltaHistogram
            label="Double dose"
            deltas={doubleLanding ?? []}
            color={ACCENT_ORANGE}
            xLabel="Δ from stop target (g)"
            emptyText="No landing data yet."
          />
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
                  <th>Reference</th>
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
                      <ReferenceWeightCell
                        session={s}
                        onSaved={(value) =>
                          setSessions((prev) =>
                            prev
                              ? prev.map((row) =>
                                  row.session_id === s.session_id
                                    ? { ...row, reference_weight_g: value }
                                    : row
                                )
                              : prev
                          )
                        }
                      />
                    </td>
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
