import { useMemo } from "react";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type { TelemetryModelState } from "../lib/types";

// These live in lib/DosingModel/DosingModel.h's TopupModel::Config, not
// in SettingsSnapshot -- there's no live channel for them, so they're
// hardcoded here for the explanation. Keep in sync by hand if they change.
const AIM_FRACTION = 0.85;
const LEARN_RATE_UNDERSHOOT = 0.3;
const LEARN_RATE_OVERSHOOT = 0.6;
const HYGIENE_MIN_DURATION_MS = 350;
const HYGIENE_MAX_DURATION_MS = 5000;
const BUCKET_WIDTH_G = 0.1;

function Formula({ children }: { children: string }) {
  return <div className="formula">{children}</div>;
}

function fmt(n: number, digits = 3): string {
  return n.toFixed(digits);
}

export default function ModelPage() {
  const { telemetryHistory } = useDeviceSocket();

  const model = useMemo<TelemetryModelState | null>(() => {
    for (let i = telemetryHistory.length - 1; i >= 0; i--) {
      const m = telemetryHistory[i];
      if (m.type === "model_state") return m;
    }
    return null;
  }, [telemetryHistory]);

  if (!model) {
    return (
      <div className="panel">
        <p className="muted">
          No model state reported yet -- this arrives once at boot and again after every
          completed dose.
        </p>
      </div>
    );
  }

  return (
    <>
      <div className="panel">
        <h3>What this page is</h3>
        <p>
          The main grind stop time is predicted from a small statistical model fitted from real
          sessions. The final top-up, by contrast, is a self-tuning lookup table -- a table of
          pulse durations, one per remaining-gap size, each nudged toward whatever has actually
          worked from real pulses -- because short relay pulses turned out to be too clumpy and
          inconsistent for a single formula to predict reliably. This page shows both models'
          current values and the logic that uses them.
        </p>
      </div>

      <div className="panel">
        <h3>Main grind rate</h3>
        <p className="muted">
          How fast coffee falls once the grinder has been running long enough for the chute to
          be primed (~0.9s). Fitted per-session as a running average -- every session counts
          equally, with no recency weighting, since this grinder's mechanical behavior doesn't
          drift with age.
        </p>
        <div className="big-number" style={{ fontSize: "1.8rem" }}>
          {fmt(model.rate_hat_g_s, 3)} <span className="muted" style={{ fontSize: "0.55em" }}>g/s</span>
        </div>
        <p className="muted">
          ±{fmt(model.rate_sd_g_s, 3)} g/s uncertainty · {model.rate_n_effective} effective
          sessions
        </p>
        <Formula>{"stop_time = elapsed + (target − already_ground − coast) / rate"}</Formula>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Every scale sample during the grind re-solves this for the predicted stop time. The
          grind actually stops as soon as elapsed time reaches that prediction, the raw weight
          reaches the target directly, or a 30s safety timeout elapses -- whichever comes first.
        </p>
      </div>

      <div className="panel">
        <h3>Post-relay-off coast</h3>
        <p className="muted">
          Coffee keeps landing on the scale for a moment after the grinder relay switches off.
          The stop-time formula above subtracts this from the target so the coast lands you on
          target instead of past it.
        </p>
        <div className="big-number" style={{ fontSize: "1.8rem" }}>
          {fmt(model.coast_weight_g, 3)} <span className="muted" style={{ fontSize: "0.55em" }}>g</span>
        </div>
        <p className="muted">±{fmt(model.coast_weight_sd_g, 3)} g uncertainty</p>
      </div>

      <div className="panel">
        <h3>Top-up pulses</h3>
        <p className="muted">
          Once the main grind stops (deliberately a little short of target), short relay pulses
          close the remaining gap. Which duration to fire is looked up by the current gap's
          bucket, not computed from a formula -- each bucket covers a {fmt(BUCKET_WIDTH_G, 1)}g
          range and is independently self-tuning.
        </p>
        <Formula>{`bucket = ceil(gap / ${fmt(BUCKET_WIDTH_G, 1)}g)  -- e.g. a 0.45g gap uses bucket 5's duration`}</Formula>
        <Formula>{`aim_weight = ${AIM_FRACTION} × bucket_upper_bound  (leaves slack for the remainder to land in a smaller bucket)`}</Formula>
        <Formula>{`error = aim_weight − weight_this_pulse_added
duration += (error > 0 ? ${LEARN_RATE_UNDERSHOOT} : ${LEARN_RATE_OVERSHOOT}) × error × 1000 / slope`}</Formula>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Overshoot is corrected faster ({LEARN_RATE_OVERSHOOT}) than undershoot is grown (
          {LEARN_RATE_UNDERSHOOT}) after every real pulse -- overshoot is the worse outcome, so
          the table is quicker to back off than to push a duration back up. Every duration is
          also clamped to {HYGIENE_MIN_DURATION_MS}-{HYGIENE_MAX_DURATION_MS}ms regardless of
          what the update rule alone would produce: the relay is a physical, clicky switch, not
          solid-state -- below ~{HYGIENE_MIN_DURATION_MS}ms it just stalls the motor and produces
          zero output rather than a smaller pulse.
        </p>
        <div className="table-scroll">
          <table>
            <thead>
              <tr>
                <th>Gap bucket</th>
                <th>Duration</th>
                <th>Aim weight</th>
                <th>Pulses seen</th>
              </tr>
            </thead>
            <tbody>
              {model.topup_lut_duration_ms.map((duration, i) => {
                const bucketUpper = (i + 1) * BUCKET_WIDTH_G;
                const bucketLower = i * BUCKET_WIDTH_G;
                return (
                  <tr key={i}>
                    <td>
                      ({fmt(bucketLower, 1)}, {fmt(bucketUpper, 1)}]g
                    </td>
                    <td>{fmt(duration, 0)}ms</td>
                    <td>{fmt(AIM_FRACTION * bucketUpper, 3)}g</td>
                    <td>{model.topup_lut_n[i]}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      </div>
    </>
  );
}
