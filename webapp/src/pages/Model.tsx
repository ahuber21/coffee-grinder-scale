import { useMemo } from "react";
import { useDeviceSocket } from "../lib/DeviceSocketContext";
import type { TelemetryModelState } from "../lib/types";

// The topup decision constants below (aim fraction, k-sigma, overshoot
// budget, min controllable gap) live in lib/DosingModel/DosingModel.h's
// TopupModel::Config and lib/DosingTask/DosingTask.cpp, not in
// SettingsSnapshot -- there's no live channel for them, so they're
// hardcoded here for the explanation. Keep in sync by hand if they change.
const AIM_FRACTION = 0.9;
const OVERSHOOT_K_SIGMA = 1.5;
const OVERSHOOT_BUDGET_G = 0.3;
const MIN_CONTROLLABLE_GAP_G = 0.18;
const HYGIENE_MIN_DURATION_MS = 350;
const HYGIENE_MAX_DURATION_MS = 5000;

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

  const safeWeight = Math.max(
    0,
    OVERSHOOT_BUDGET_G - OVERSHOOT_K_SIGMA * model.topup_residual_sd_g
  );
  const exampleGap = 1.0;
  const exampleTarget = Math.min(AIM_FRACTION * exampleGap, safeWeight);
  const exampleDurationMs =
    exampleTarget > 0
      ? model.topup_deadtime_ms + (1000 * exampleTarget) / model.topup_slope_g_s
      : 0;
  const exampleFires = exampleTarget > 0 && exampleDurationMs > HYGIENE_MIN_DURATION_MS;

  return (
    <>
      <div className="panel">
        <h3>What this page is</h3>
        <p>
          The dosing logic doesn't use a fixed lookup table -- it fits two small statistical
          models from real sessions and uses them to decide when to stop the main grind and how
          long to run each top-up pulse. This page shows the models' current fitted values and
          the formulas that use them.
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
          close the remaining gap. Each pulse re-runs this decision from scratch against the
          weight actually measured after the previous pulse settled.
        </p>
        <p>
          Slope <strong>{fmt(model.topup_slope_g_s, 3)} g/s</strong> · dead time{" "}
          <strong>{fmt(model.topup_deadtime_ms, 0)} ms</strong> · pulse noise{" "}
          <strong>±{fmt(model.topup_residual_sd_g, 3)} g</strong> ·{" "}
          {model.topup_n_effective} effective pulses
        </p>
        <Formula>{`target_weight = ${AIM_FRACTION} × gap  (aim for 90% of what's left)`}</Formula>
        <Formula>{`safe_weight = ${OVERSHOOT_BUDGET_G}g − ${OVERSHOOT_K_SIGMA} × pulse_noise  (shrink to stay inside the overshoot budget)`}</Formula>
        <Formula>{"pulse_duration = dead_time + 1000 × min(target_weight, safe_weight) / slope"}</Formula>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          If the gap is below {MIN_CONTROLLABLE_GAP_G}g, or the shrunk target weight is ≤ 0, no
          pulse fires -- the remaining gap is accepted as undershoot rather than risk an
          overshoot. Right now that shrink allows up to <strong>{fmt(safeWeight, 3)}g</strong>{" "}
          per pulse. The duration is also clamped to {HYGIENE_MIN_DURATION_MS}-
          {HYGIENE_MAX_DURATION_MS}ms: the relay is a physical, clicky switch, not
          solid-state -- below ~{HYGIENE_MIN_DURATION_MS}ms it just stalls the motor and produces
          zero output rather than a smaller pulse, so a duration that short is refused rather
          than wasted.
        </p>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Worked example with today's fitted values, a {exampleGap}g gap:{" "}
          {exampleFires
            ? `aim for ${fmt(exampleTarget, 3)}g → a ${fmt(exampleDurationMs, 0)}ms pulse.`
            : exampleTarget > 0
              ? `aim for ${fmt(exampleTarget, 3)}g → a ${fmt(exampleDurationMs, 0)}ms pulse, but that's below the ${HYGIENE_MIN_DURATION_MS}ms relay floor -- no pulse fires, undershoot accepted.`
              : "the shrink reduces the target to 0 -- no pulse would fire, undershoot accepted."}
        </p>
      </div>
    </>
  );
}
