import { useEffect, useMemo, useState } from "react";
import ParamChart, { type ParamSeries } from "../../components/ParamChart";
import { ACCENT_BLUE, ACCENT_GREEN, ACCENT_ORANGE } from "../../lib/chartTheme";
import { useDeviceSocket } from "../../lib/DeviceSocketContext";
import { formatTimestamp } from "../../lib/format";
import { fetchModelSnapshots, type ModelSnapshot } from "../../lib/postgrest";
import type { LearnerParams, TelemetryModelState } from "../../lib/types";

// LandingLearner::Config and MainGrindModel::Config values (lib/DosingModel/DosingModel.h),
// repeated here for the explanation because no channel reports them. Keep in step by hand.
const LEARNER_PRIOR_WEIGHT = 3;
const LEARNER_RIDGE = 0.02;
const LEARNER_MAX_ABS_ERROR_G = 1.0;
const RATE_SESSION_WINDOW = 30;
const RATE_SESSION_SD = 0.09;
const RATE_DEADTIME_S = 0.9;

function Formula({ children }: { children: string }) {
  return <div className="formula">{children}</div>;
}

function Stat({ label, value, unit, note }: { label: string; value: string; unit?: string; note?: string }) {
  return (
    <div>
      <div className="muted" style={{ fontSize: "0.8em" }}>
        {label}
      </div>
      <div className="big-number" style={{ fontSize: "1.5rem" }}>
        {value}
        {unit && (
          <span className="muted" style={{ fontSize: "0.55em" }}>
            {" "}
            {unit}
          </span>
        )}
      </div>
      {note && (
        <div className="muted" style={{ fontSize: "0.8em" }}>
          {note}
        </div>
      )}
    </div>
  );
}

const statGrid = {
  display: "grid",
  gridTemplateColumns: "repeat(auto-fit, minmax(150px, 1fr))",
  gap: "1rem",
  margin: "0.8rem 0",
};

/** A snapshot's learner columns for one dose mode, in the shape the live model_state uses. */
function learnerOf(s: ModelSnapshot, mode: "single" | "double"): LearnerParams {
  return {
    sessions: s[`${mode}_sessions`],
    effective_sessions: s[`${mode}_effective_sessions`],
    mean_rate_g_s: s[`${mode}_mean_rate_g_s`],
    intercept_g: s[`${mode}_intercept_g`],
    slope_g_per_g_s: s[`${mode}_slope_g_per_g_s`],
    residual_sd_g: s[`${mode}_residual_sd_g`],
  };
}

interface CurrentModel {
  rate_hat_g_s: number;
  rate_sd_g_s: number;
  rate_n_effective: number;
  coast_g: number;
  coast_sd_g: number;
  learner: [LearnerParams, LearnerParams];
  source: string;
}

function fromSnapshot(s: ModelSnapshot): CurrentModel {
  return {
    rate_hat_g_s: s.rate_hat_g_s,
    rate_sd_g_s: s.rate_sd_g_s,
    rate_n_effective: s.rate_n_effective,
    coast_g: s.coast_g,
    coast_sd_g: s.coast_sd_g,
    learner: [learnerOf(s, "single"), learnerOf(s, "double")],
    source: `last session snapshot, ${formatTimestamp(s.taken_at)}`,
  };
}

function fromLive(m: TelemetryModelState): CurrentModel {
  return {
    rate_hat_g_s: m.rate_hat_g_s,
    rate_sd_g_s: m.rate_sd_g_s,
    rate_n_effective: m.rate_n_effective,
    coast_g: m.coast_weight_g,
    coast_sd_g: m.coast_weight_sd_g,
    learner: [m.learner[0], m.learner[1]],
    source: "live from the device",
  };
}

const fmt = (v: number, digits = 3) => v.toFixed(digits);
const signed = (v: number, digits = 3) => `${v >= 0 ? "+" : ""}${v.toFixed(digits)}`;

export default function ModelPage() {
  const { telemetryHistory, settings } = useDeviceSocket();
  const [snapshots, setSnapshots] = useState<ModelSnapshot[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    fetchModelSnapshots()
      .then(setSnapshots)
      .catch((e) => setError(String(e)));
  }, []);

  const live = useMemo<TelemetryModelState | null>(() => {
    for (let i = telemetryHistory.length - 1; i >= 0; i--) {
      const m = telemetryHistory[i];
      if (m.type === "model_state") return m;
    }
    return null;
  }, [telemetryHistory]);

  const current: CurrentModel | null = live
    ? fromLive(live)
    : snapshots && snapshots.length > 0
      ? fromSnapshot(snapshots[snapshots.length - 1])
      : null;

  const captions = useMemo(
    () =>
      (snapshots ?? []).map(
        (s, i) => `#${i} · ${formatTimestamp(s.taken_at)}${s.note ? ` · ${s.note}` : ""}`
      ),
    [snapshots]
  );

  const charts = useMemo(() => {
    const rows = snapshots ?? [];
    const col = (pick: (s: ModelSnapshot) => number) => rows.map(pick);
    const band = (center: number[], sd: number[], sign: 1 | -1) =>
      center.map((c, i) => c + sign * sd[i]);
    const rate = col((s) => s.rate_hat_g_s);
    const rateSd = col((s) => s.rate_sd_g_s);
    const coast = col((s) => s.coast_g);
    const coastSd = col((s) => s.coast_sd_g);
    const pair = (label: string, pick: (s: ModelSnapshot, mode: "single" | "double") => number): ParamSeries[] => [
      { label: `Single ${label}`, color: ACCENT_BLUE, values: col((s) => pick(s, "single")) },
      { label: `Double ${label}`, color: ACCENT_ORANGE, values: col((s) => pick(s, "double")) },
    ];
    const rateSeries: ParamSeries[] = [
      { label: "Rate", color: ACCENT_GREEN, values: rate },
      { label: "± 1 sd", color: ACCENT_GREEN, values: band(rate, rateSd, 1), band: true },
      { label: "", color: ACCENT_GREEN, values: band(rate, rateSd, -1), band: true },
    ];
    const coastSeries: ParamSeries[] = [
      { label: "Coast", color: ACCENT_GREEN, values: coast },
      { label: "± 1 sd", color: ACCENT_GREEN, values: band(coast, coastSd, 1), band: true },
      { label: "", color: ACCENT_GREEN, values: band(coast, coastSd, -1), band: true },
    ];
    return {
      rateSeries,
      coastSeries,
      intercept: pair("shift", (s, m) => s[`${m}_intercept_g`]),
      slope: pair("slope", (s, m) => s[`${m}_slope_g_per_g_s`]),
      residual: pair("residual sd", (s, m) => s[`${m}_residual_sd_g`]),
      effective: pair("effective sessions", (s, m) => s[`${m}_effective_sessions`]),
      lifetime: pair("lifetime sessions", (s, m) => s[`${m}_sessions`]),
    };
  }, [snapshots]);

  return (
    <>
      <div className="panel">
        <h3>How the stop point is chosen</h3>
        <p>
          The grinder runs once, with no top-up afterwards, so the stop time has to be right.
          Three small models cooperate: the <strong>rate</strong> of the grind, the
          <strong> coast</strong> that lands after the relay switches off, and the
          <strong> landing learner</strong>, which corrects whatever systematic error is left,
          separately for single and double doses.
        </p>
        <Formula>{"stop_time = elapsed + (target − already_ground − coast − learner_shift) / rate"}</Formula>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Every scale sample during the grind re-solves this. The grind stops as soon as elapsed
          time reaches the prediction, the raw weight reaches the target directly, or the
          grinding safety timeout elapses, whichever comes first. The learner's shift never moves
          the stop by more than the clamp
          {settings ? ` (currently ±${settings.landing_learner_clamp_ms} ms)` : ""} from the
          unlearned stop time.
        </p>
      </div>

      {!current && (
        <div className="panel">
          <p className="muted">
            {error
              ? `Could not load the model history: ${error}`
              : "No model state yet -- the device reports it at boot and after every dose, and the history fills in as doses complete."}
          </p>
        </div>
      )}

      {current && (
        <>
          <div className="panel">
            <h3>Current values</h3>
            <p className="muted" style={{ fontSize: "0.85em" }}>
              Source: {current.source}.
            </p>
            <div style={statGrid}>
              <Stat
                label="Grind rate"
                value={fmt(current.rate_hat_g_s)}
                unit="g/s"
                note={`± ${fmt(current.rate_sd_g_s)} · ${current.rate_n_effective} sessions`}
              />
              <Stat
                label="Coast after relay off"
                value={fmt(current.coast_g)}
                unit="g"
                note={`± ${fmt(current.coast_sd_g)}`}
              />
            </div>
            <div className="table-scroll">
              <table>
                <thead>
                  <tr>
                    <th>Landing learner</th>
                    <th>Sessions</th>
                    <th>Effective</th>
                    <th>Mean rate</th>
                    <th>Shift at mean rate</th>
                    <th>Slope</th>
                    <th>Residual sd</th>
                  </tr>
                </thead>
                <tbody>
                  {(["Single", "Double"] as const).map((name, i) => {
                    const p = current.learner[i];
                    return (
                      <tr key={name}>
                        <td>{name}</td>
                        <td>{p.sessions}</td>
                        <td>{fmt(p.effective_sessions, 1)}</td>
                        <td>{fmt(p.mean_rate_g_s)} g/s</td>
                        <td>{signed(p.intercept_g)} g</td>
                        <td>{signed(p.slope_g_per_g_s)} g per g/s</td>
                        <td>{fmt(p.residual_sd_g)} g</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </div>
        </>
      )}

      <div className="panel">
        <h3>Landing learner</h3>
        <p className="muted">
          After each grind the weight settles a little above or below the target. The learner
          fits that landing error against the grind's flow rate at the stop, separately for
          single and double doses, and moves the stop point to cancel what it expects. It trains
          only on genuine settled weights from time-estimate stops, and always on the error the
          session would have had without its own correction, so it never trains on its own action.
        </p>
        <Formula>{"excess(rate) = intercept + slope × (rate − mean_rate)\nshift = excess(rate at stop), moved into stop time via the coast term"}</Formula>
        <Formula>{`each session: every sum is multiplied by (1 − learning_rate), then the new (rate, error) pair is added
intercept = Σwy / (Σw + prior),  prior = ${LEARNER_PRIOR_WEIGHT}² / (${LEARNER_PRIOR_WEIGHT} + lifetime_sessions)
slope = Sxy / (Sxx + ${LEARNER_RIDGE})   (centred sums; the ridge keeps it near zero until the rate varies)`}</Formula>
        <div className="table-scroll">
          <table>
            <tbody>
              <tr>
                <td>Active</td>
                <td>{settings ? (settings.landing_learner_enabled ? "yes" : "no") : "--"}</td>
                <td className="muted">Settings page</td>
              </tr>
              <tr>
                <td>Learning rate</td>
                <td>{settings ? settings.landing_learner_rate : "--"}</td>
                <td className="muted">
                  per-session forgetting; evidence window is about 1 / rate sessions
                </td>
              </tr>
              <tr>
                <td>Stop-time clamp</td>
                <td>{settings ? `±${settings.landing_learner_clamp_ms} ms` : "--"}</td>
                <td className="muted">Settings page</td>
              </tr>
              <tr>
                <td>Prior weight</td>
                <td>{LEARNER_PRIOR_WEIGHT} sessions</td>
                <td className="muted">shrinks the intercept toward 0 early on</td>
              </tr>
              <tr>
                <td>Slope ridge</td>
                <td>{LEARNER_RIDGE}</td>
                <td className="muted">shrinks the slope toward 0</td>
              </tr>
              <tr>
                <td>Glitch limit</td>
                <td>±{LEARNER_MAX_ABS_ERROR_G} g</td>
                <td className="muted">larger landing errors are not used for training</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <div className="panel">
        <h3>Grind rate and coast</h3>
        <p className="muted">
          <strong>Rate</strong>: how fast coffee falls once the chute is primed (after about{" "}
          {RATE_DEADTIME_S} s). Every session fits its own line and counts as one observation of
          quality ±{RATE_SESSION_SD} g/s; older sessions fade over a window of{" "}
          {RATE_SESSION_WINDOW} sessions, which also caps how certain the estimate can become.
          During a grind the stored rate is blended with the live fit, weighted by their
          certainty.
        </p>
        <p className="muted">
          <strong>Coast</strong>: coffee keeps landing for a moment after the relay opens. It
          is one number per session (settled weight minus the weight at relay-off) blended into
          the stored estimate, and the stop formula subtracts it from the target.
        </p>
      </div>

      <div className="panel">
        <h3>Parameter history</h3>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          One point per finished session, from the database
          {snapshots ? ` (${snapshots.length} snapshots)` : ""}. The first point is the device
          state read back just before the top-up was removed; earlier history was not recorded.
          Hover a point for its date.
        </p>
        {error && <p style={{ color: "var(--red)" }}>{error}</p>}
        {!snapshots && !error && <p className="muted">Loading from PostgREST...</p>}
        {snapshots && snapshots.length === 0 && (
          <p className="muted">No snapshots recorded yet.</p>
        )}
        {snapshots && snapshots.length > 0 && (
          <>
            <h4>Grind rate (g/s)</h4>
            <ParamChart yLabel="g/s" captions={captions} series={charts.rateSeries} digits={4} />
            <h4>Coast (g)</h4>
            <ParamChart yLabel="g" captions={captions} series={charts.coastSeries} digits={4} />
            <h4>Learner shift at the mean rate (g, positive = stops earlier)</h4>
            <ParamChart yLabel="g" captions={captions} series={charts.intercept} digits={4} />
            <h4>Learner slope (g per g/s)</h4>
            <ParamChart yLabel="g per g/s" captions={captions} series={charts.slope} digits={4} />
            <h4>Learner residual sd (g)</h4>
            <ParamChart yLabel="g" captions={captions} series={charts.residual} digits={4} />
            <h4>Learner evidence (sessions)</h4>
            <ParamChart
              yLabel="sessions"
              captions={captions}
              series={[...charts.effective, ...charts.lifetime]}
              digits={1}
            />
          </>
        )}
      </div>
    </>
  );
}
