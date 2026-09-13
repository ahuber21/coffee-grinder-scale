import { useEffect, useMemo, useRef, useState } from "react";
import {
  Chart,
  LineController,
  LineElement,
  PointElement,
  LinearScale,
  Tooltip,
} from "chart.js";
import { useDeviceSocket } from "../../lib/DeviceSocketContext";
import { CALIBRATION_FACTOR_WIRE_SCALE } from "../../lib/types";
import { SelectSettingRow } from "../Settings";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip);

const SCATTER_WINDOW_S = 10;
const CHART_REDRAW_MS = 150; // decoupled from poll rate, so the window slides smoothly regardless

interface RingStats {
  mean: number;
  stable: boolean;
}

// Mirrors lib/ADS1232/ADS1232.cpp's getRaw() exactly: mean over the whole
// buffer, "changing" (unstable) if any sample deviates from that mean by
// more than 0.02% of the mean. Math.trunc, not JS's default float
// division, to match the firmware's int32_t rawSum / ringBufferSize.
function computeRingStats(buffer: number[]): RingStats | null {
  const n = buffer.length;
  if (n === 0) return null;
  const sum = buffer.reduce((a, b) => a + b, 0);
  const mean = Math.trunc(sum / n);
  const maxDelta = 0.0002 * mean;
  const changing = buffer.some((v) => Math.abs(mean - v) > maxDelta);
  return { mean, stable: !changing };
}

interface CalibrationPoint {
  id: number;
  weightG: number;
  rawAdc: number;
  source: "buffer mean" | "single sample";
}

// Least-squares fit of weight (g) against raw ADC count -- slope is a
// candidate calibration_factor (g per raw count), independent of
// whatever's currently configured on the device.
function linearFit(points: CalibrationPoint[]): { slope: number; intercept: number } | null {
  const n = points.length;
  if (n < 2) return null;
  const xs = points.map((p) => p.rawAdc);
  const ys = points.map((p) => p.weightG);
  const meanX = xs.reduce((a, b) => a + b, 0) / n;
  const meanY = ys.reduce((a, b) => a + b, 0) / n;
  let num = 0;
  let den = 0;
  for (let i = 0; i < n; i++) {
    num += (xs[i] - meanX) * (ys[i] - meanY);
    den += (xs[i] - meanX) ** 2;
  }
  if (den === 0) return null;
  const slope = num / den;
  return { slope, intercept: meanY - slope * meanX };
}

export default function CalibrationPage() {
  const { status, settings, send, nextRequestId, lastRawRead } = useDeviceSocket();

  const [pollHz, setPollHz] = useState(5);
  const [bufferSizeInput, setBufferSizeInput] = useState("12");
  const bufferSize = Math.max(1, Math.round(parseFloat(bufferSizeInput)) || 12);

  const [weightInput, setWeightInput] = useState("");
  const [points, setPoints] = useState<CalibrationPoint[]>([]);
  const nextPointId = useRef(1);

  const bufferRef = useRef<number[]>([]);
  const [ringStats, setRingStats] = useState<RingStats | null>(null);
  const [latestRaw, setLatestRaw] = useState<number | null>(null);
  const [latestGrams, setLatestGrams] = useState<number | null>(null);

  const scatterRef = useRef<{ tSec: number; rawAdc: number }[]>([]);
  const startRef = useRef(performance.now());
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  // Poll loop -- fires independently of everything else on the socket, at
  // whatever rate the field below is set to.
  useEffect(() => {
    const intervalMs = 1000 / Math.max(0.1, pollHz);
    const id = setInterval(() => {
      send({ type: "raw_read_request", request_id: nextRequestId() });
    }, intervalMs);
    return () => clearInterval(id);
  }, [pollHz, send, nextRequestId]);

  // A resized ring buffer is a fresh experiment, not a continuation of
  // the old one -- old samples at the wrong window size would corrupt
  // the mean/stability computation if kept.
  useEffect(() => {
    bufferRef.current = [];
    setRingStats(null);
  }, [bufferSize]);

  // Every reply feeds both the ring-buffer replica and the scatter
  // history. Chart rendering itself is decoupled (see the redraw timer
  // below) so the plotted window keeps sliding even between replies.
  useEffect(() => {
    if (!lastRawRead) return;
    const raw = lastRawRead.raw_adc;
    setLatestRaw(raw);
    setLatestGrams(lastRawRead.grams);

    const buf = bufferRef.current;
    buf.push(raw);
    if (buf.length > bufferSize) buf.splice(0, buf.length - bufferSize);
    setRingStats(computeRingStats(buf));

    scatterRef.current.push({ tSec: (performance.now() - startRef.current) / 1000, rawAdc: raw });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [lastRawRead]);

  // Chart setup, once.
  useEffect(() => {
    if (!canvasRef.current) return;
    chartRef.current = new Chart(canvasRef.current, {
      type: "line",
      data: {
        datasets: [
          {
            label: "Raw ADC",
            data: [],
            borderColor: "transparent",
            backgroundColor: "#0a84ff",
            pointRadius: 3,
            pointHoverRadius: 4,
            showLine: false,
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
            title: { display: true, text: "Time (s)", color: "rgba(235, 235, 245, 0.45)" },
            ticks: { color: "rgba(235, 235, 245, 0.45)" },
            grid: { color: "rgba(84, 84, 88, 0.2)" },
            border: { display: false },
          },
          y: {
            type: "linear",
            title: { display: true, text: "Raw ADC count", color: "rgba(235, 235, 245, 0.45)" },
            ticks: { color: "rgba(235, 235, 245, 0.45)" },
            grid: { color: "rgba(84, 84, 88, 0.2)" },
            border: { display: false },
          },
        },
        plugins: {
          legend: { display: false },
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
              label: (item) => `ADC ${(item.parsed.y ?? 0).toFixed(0)}`,
            },
          },
        },
      },
    });
    return () => chartRef.current?.destroy();
  }, []);

  // Redraw on a steady timer, not on each reply -- this is what makes the
  // window actually *slide* in real time (old points age out visually)
  // even when the poll rate is slow. No explicit y min/max is set, so
  // Chart.js re-fits it to whatever's currently in the window on every
  // update() -- that's the auto-resize.
  useEffect(() => {
    const id = setInterval(() => {
      const chart = chartRef.current;
      if (!chart) return;
      const nowSec = (performance.now() - startRef.current) / 1000;
      const cutoff = nowSec - SCATTER_WINDOW_S;
      const buf = scatterRef.current;
      while (buf.length > 0 && buf[0].tSec < cutoff) buf.shift();
      chart.data.datasets[0].data = buf.map((p) => ({ x: p.tSec, y: p.rawAdc }));
      const xScale = chart.options.scales?.x;
      if (xScale) {
        xScale.min = cutoff;
        xScale.max = nowSec;
      }
      chart.update("none");
    }, CHART_REDRAW_MS);
    return () => clearInterval(id);
  }, []);

  const bufferFillPct = Math.min(100, (bufferRef.current.length / bufferSize) * 100);
  const recordValue = ringStats?.mean ?? latestRaw;
  const fit = useMemo(() => linearFit(points), [points]);

  function recordPoint() {
    const weight = parseFloat(weightInput);
    if (!Number.isFinite(weight) || recordValue === null) return;
    setPoints((prev) => [
      ...prev,
      {
        id: nextPointId.current++,
        weightG: weight,
        rawAdc: recordValue,
        source: ringStats ? "buffer mean" : "single sample",
      },
    ]);
    setWeightInput("");
  }

  function removePoint(id: number) {
    setPoints((prev) => prev.filter((p) => p.id !== id));
  }

  return (
    <>
      <div className="panel">
        <h3>Live reading</h3>
        {status !== "open" && <p className="muted">Not connected -- polling will resume once reconnected.</p>}
        <div style={{ display: "flex", gap: "2rem", flexWrap: "wrap", alignItems: "baseline" }}>
          <div>
            <div className="muted" style={{ fontSize: "0.78rem", textTransform: "uppercase" }}>
              Latest sample
            </div>
            <div className="big-number" style={{ fontSize: "1.8rem" }}>
              {latestRaw ?? "--"}
            </div>
            <div className="muted" style={{ fontSize: "0.82rem" }}>
              {latestGrams !== null ? `${latestGrams.toFixed(3)} g` : " "}
            </div>
          </div>
          <div>
            <div className="muted" style={{ fontSize: "0.78rem", textTransform: "uppercase" }}>
              Ring buffer mean ({bufferRef.current.length}/{bufferSize})
            </div>
            <div className="big-number" style={{ fontSize: "1.8rem" }}>
              {ringStats ? ringStats.mean : "--"}
            </div>
            <div className="muted" style={{ fontSize: "0.82rem" }}>
              {ringStats ? (ringStats.stable ? "stable" : "changing") : `filling (${bufferFillPct.toFixed(0)}%)`}
            </div>
          </div>
        </div>
      </div>

      <div className="panel">
        <h3>Polling</h3>
        <div className="setting-row">
          <div className="label">Poll rate</div>
          <div style={{ display: "flex", alignItems: "center" }}>
            <span className="current">{pollHz} Hz</span>
            <input
              type="number"
              min="0.2"
              max="20"
              step="0.5"
              value={pollHz}
              onChange={(e) => setPollHz(Math.max(0.2, parseFloat(e.target.value) || 5))}
            />
          </div>
        </div>
        <div className="setting-row">
          <div className="label">Ring buffer size (frontend replica)</div>
          <div style={{ display: "flex", alignItems: "center" }}>
            <span className="current">{bufferSize} samples</span>
            <input
              type="number"
              min="1"
              max="200"
              step="1"
              value={bufferSizeInput}
              onChange={(e) => setBufferSizeInput(e.target.value)}
            />
          </div>
        </div>
        <SelectSettingRow
          field="speed"
          label="ADC speed (device setting)"
          currentValue={settings?.speed ?? null}
          options={[10, 80]}
          unit=" SPS"
        />
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Poll rate and buffer size are purely a frontend experiment -- they never touch the
          device. ADC speed is the one real hardware setting here, written the same way the
          Settings page does.
        </p>
      </div>

      <div className="panel">
        <h3>Raw ADC, last {SCATTER_WINDOW_S}s</h3>
        <canvas ref={canvasRef} height={200} />
      </div>

      <div className="panel">
        <h3>Record a calibration point</h3>
        <div style={{ display: "flex", gap: "0.75rem", alignItems: "center" }}>
          <input
            type="number"
            step="0.01"
            placeholder="weight (g)"
            value={weightInput}
            onChange={(e) => setWeightInput(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && recordPoint()}
          />
          <button className="primary" onClick={recordPoint} disabled={weightInput === "" || recordValue === null}>
            Record at {recordValue ?? "--"}
          </button>
        </div>
        <p className="muted" style={{ fontSize: "0.85em" }}>
          Uses the ring buffer mean once it's full, otherwise the latest single sample -- place a
          known weight, wait for it to settle, then record.
        </p>

        {points.length > 0 && (
          <div className="table-scroll" style={{ marginTop: "1rem" }}>
            <table>
              <thead>
                <tr>
                  <th>Weight (g)</th>
                  <th>Raw ADC</th>
                  <th>Source</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {points.map((p) => (
                  <tr key={p.id}>
                    <td>{p.weightG.toFixed(2)}</td>
                    <td>{p.rawAdc}</td>
                    <td>{p.source}</td>
                    <td>
                      <button onClick={() => removePoint(p.id)}>Remove</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}

        <p className="muted" style={{ marginTop: "1rem", fontSize: "0.85em" }}>
          Current device calibration_factor:{" "}
          <strong>
            {settings
              ? settings.calibration_factor_x1e6 / CALIBRATION_FACTOR_WIRE_SCALE
              : "not reported yet"}
          </strong>
        </p>

        {fit && (
          <div style={{ marginTop: "0.5rem" }}>
            <p>
              Linear fit (weight vs raw ADC): <strong>{fit.slope}</strong> g/count, intercept{" "}
              <strong>{fit.intercept.toFixed(3)}</strong> g -- a candidate calibration_factor
              from these points alone.
            </p>
            <button
              onClick={() =>
                send({
                  type: "settings_write",
                  field: "calibration_factor_x1e6",
                  value: fit.slope * CALIBRATION_FACTOR_WIRE_SCALE,
                  request_id: nextRequestId(),
                })
              }
            >
              Apply fit as calibration_factor
            </button>
          </div>
        )}
      </div>
    </>
  );
}
