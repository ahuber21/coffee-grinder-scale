import { useEffect, useMemo, useRef } from "react";
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
import { legendLabels, linearAxis } from "../../lib/chartTheme";
import { useDeviceSocket } from "../../lib/DeviceSocketContext";
import type { TelemetryModelState } from "../../lib/types";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip, Legend);

// TopupModel::Config values (lib/DosingModel/DosingModel.h), repeated here for the explanation
// because no channel reports them. Keep in step by hand.
const AIM_FRACTION = 0.85;
const LEARN_RATE_UNDERSHOOT = 0.3;
const LEARN_RATE_OVERSHOOT = 0.6;
const HYGIENE_MIN_DURATION_MS = 350;
const HYGIENE_MAX_DURATION_MS = 5000;
const BUCKET_WIDTH_G = 0.1;

function Formula({ children }: { children: string }) {
  return <div className="formula">{children}</div>;
}

// Buckets are ordered by gap size, so they get one hue from light to dark rather than a rainbow.
function bucketColor(i: number): string {
  const lightness = 74 - i * 4.2;
  return `hsl(210, 85%, ${lightness}%)`;
}

export default function ModelPage() {
  const { telemetryHistory } = useDeviceSocket();
  const lutCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const lutChartRef = useRef<Chart | null>(null);

  const model = useMemo<TelemetryModelState | null>(() => {
    for (let i = telemetryHistory.length - 1; i >= 0; i--) {
      const m = telemetryHistory[i];
      if (m.type === "model_state") return m;
    }
    return null;
  }, [telemetryHistory]);

  // Every model_state seen since this page connected, so the chart can show how each bucket moved.
  const modelHistory = useMemo<TelemetryModelState[]>(
    () => telemetryHistory.filter((m): m is TelemetryModelState => m.type === "model_state"),
    [telemetryHistory]
  );

  useEffect(() => {
    if (!lutCanvasRef.current) return;
    const datasets: ChartDataset<"line">[] = Array.from({ length: 10 }, (_, i) => ({
      label: `(${(i * BUCKET_WIDTH_G).toFixed(1)}, ${((i + 1) * BUCKET_WIDTH_G).toFixed(1)}]g`,
      data: [],
      borderColor: bucketColor(i),
      backgroundColor: bucketColor(i),
      borderWidth: 2,
      pointRadius: 2,
      fill: false,
      parsing: false,
    }));
    lutChartRef.current = new Chart(lutCanvasRef.current, {
      type: "line",
      data: { datasets },
      options: {
        responsive: true,
        animation: false,
        scales: {
          x: linearAxis("Reading # (this session)", { stepSize: 1 }),
          y: linearAxis("Duration (ms)"),
        },
        plugins: { legend: legendLabels(12, { font: { size: 10 } }) },
      },
    });
    return () => lutChartRef.current?.destroy();
  }, []);

  useEffect(() => {
    const chart = lutChartRef.current;
    if (!chart) return;
    for (let bucket = 0; bucket < 10; bucket++) {
      chart.data.datasets[bucket].data = modelHistory.map((m, i) => ({
        x: i,
        y: m.topup_lut_duration_ms[bucket],
      }));
    }
    chart.update();
  }, [modelHistory]);

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
          sessions, and a landing learner then corrects where it stops. The final top-up is a
          self-tuning lookup table: one pulse duration per remaining-gap size, each nudged toward
          whatever has worked in real pulses, because short relay pulses are too clumpy for a
          single formula to predict. This page shows the models' current values and the logic
          that uses them.
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
          {model.rate_hat_g_s.toFixed(3)} <span className="muted" style={{ fontSize: "0.55em" }}>g/s</span>
        </div>
        <p className="muted">
          ±{model.rate_sd_g_s.toFixed(3)} g/s uncertainty · {model.rate_n_effective} effective
          sessions
        </p>
        <Formula>{"stop_time = elapsed + (target − already_ground − coast − learner_shift) / rate"}</Formula>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Every scale sample during the grind re-solves this for the predicted stop time. The
          grind stops as soon as elapsed time reaches that prediction, the raw weight reaches the
          target directly, or the grinding safety timeout elapses, whichever comes first.
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
          {model.coast_weight_g.toFixed(3)} <span className="muted" style={{ fontSize: "0.55em" }}>g</span>
        </div>
        <p className="muted">±{model.coast_weight_sd_g.toFixed(3)} g uncertainty</p>
      </div>

      <div className="panel">
        <h3>Landing learner</h3>
        <p className="muted">
          After each grind the weight settles a little above or below the offset target. The
          learner predicts that landing error from the grind's flow rate, separately for single
          and double doses, and moves the stop point to cancel it (bounded by the stop-time
          clamp on the Settings page). Each session's error, applied shift and running spread
          appear in the console as a LAND line, and the History page plots the landing error.
        </p>
      </div>

      <div className="panel">
        <h3>Top-up pulses</h3>
        <p className="muted">
          Once the main grind stops (deliberately a little short of target), short relay pulses
          close the remaining gap. Which duration to fire is looked up by the current gap's
          bucket, not computed from a formula -- each bucket covers a {BUCKET_WIDTH_G.toFixed(1)}g
          range and is independently self-tuning.
        </p>
        <Formula>{`bucket = ceil(gap / ${BUCKET_WIDTH_G.toFixed(1)}g)  -- e.g. a 0.45g gap uses bucket 5's duration`}</Formula>
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
                      ({bucketLower.toFixed(1)}, {bucketUpper.toFixed(1)}]g
                    </td>
                    <td>{duration.toFixed(0)}ms</td>
                    <td>{(AIM_FRACTION * bucketUpper).toFixed(3)}g</td>
                    <td>{model.topup_lut_n[i]}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      </div>

      <div className="panel">
        <h3>Top-up duration history</h3>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Every bucket's tuned duration, once per reading (boot, then after every completed
          session) seen since this page connected -- not a persisted log, so reloading the page
          or a device reboot starts a fresh chart. Useful for watching a bucket settle in after a
          few real doses land in it.
        </p>
        <canvas ref={lutCanvasRef} height={220} />
      </div>
    </>
  );
}
