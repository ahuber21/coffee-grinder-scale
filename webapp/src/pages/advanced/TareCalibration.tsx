import { useEffect, useMemo, useRef, useState } from "react";
import {
  Chart,
  BarController,
  BarElement,
  LinearScale,
  Tooltip,
  Legend,
} from "chart.js";
import { ACCENT_BLUE, legendLabels, linearAxis, tooltipTheme } from "../../lib/chartTheme";
import { fetchTareDebugSamples, type TareDebugSample } from "../../lib/postgrest";
import { mean, sampleStdDev } from "../../lib/stats";

Chart.register(BarController, BarElement, LinearScale, Tooltip, Legend);


interface Stats {
  n: number;
  mean: number;
  sd: number;
  min: number;
  max: number;
  range: number;
}

function computeStats(values: number[]): Stats | null {
  if (values.length === 0) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  return { n: values.length, mean: mean(values), sd: sampleStdDev(values), min, max, range: max - min };
}

interface Bucket {
  lower: number;
  upper: number;
  count: number;
}

/** Bins the values; bin count and width come from the data, as raw ADC counts have no natural range. */
function computeBuckets(values: number[]): Bucket[] | null {
  const n = values.length;
  if (n < 2) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  if (max === min) return null;

  const binCount = Math.min(25, Math.max(5, Math.ceil(Math.sqrt(n))));
  const width = (max - min) / binCount;
  const buckets: Bucket[] = Array.from({ length: binCount }, (_, i) => ({
    lower: min + i * width,
    upper: min + (i + 1) * width,
    count: 0,
  }));
  for (const v of values) {
    const idx = Math.min(binCount - 1, Math.floor((v - min) / width));
    buckets[idx].count++;
  }
  return buckets;
}

function TareHistogram({ buckets }: { buckets: Bucket[] }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  useEffect(() => {
    if (!canvasRef.current) return;
    chartRef.current?.destroy();
    chartRef.current = new Chart(canvasRef.current, {
      type: "bar",
      data: {
        datasets: [
          {
            label: `raw_adc (n=${buckets.reduce((a, b) => a + b.count, 0)})`,
            data: buckets.map((b) => ({ x: (b.lower + b.upper) / 2, y: b.count })),
            backgroundColor: `${ACCENT_BLUE}99`,
            borderWidth: 0,
            borderRadius: { topLeft: 3, topRight: 3, bottomLeft: 0, bottomRight: 0 },
            borderSkipped: false,
            parsing: false,
          },
        ],
      },
      options: {
        responsive: true,
        animation: false,
        scales: {
          x: linearAxis("Raw ADC count"),
          y: linearAxis("Count", { beginAtZero: true }),
        },
        plugins: {
          legend: legendLabels(14, { boxHeight: 10 }),
          tooltip: tooltipTheme({
            title: (items: { parsed: { x: number | null } }[]) => `ADC ${(items[0]?.parsed.x ?? 0).toFixed(0)}`,
          }),
        },
      },
    });
    return () => chartRef.current?.destroy();
  }, [buckets]);

  return <canvas ref={canvasRef} height={220} />;
}

export default function TareCalibrationPage() {
  const [samples, setSamples] = useState<TareDebugSample[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [minFilter, setMinFilter] = useState("");
  const [maxFilter, setMaxFilter] = useState("");

  useEffect(() => {
    fetchTareDebugSamples()
      .then(setSamples)
      .catch((e) => setError(String(e)));
  }, []);

  const allRawAdc = useMemo(() => samples?.map((s) => s.raw_adc) ?? [], [samples]);

  // An empty field means no bound on that side. Text that doesn't parse is flagged instead of
  // silently showing unfiltered data or an always-empty result (NaN compares false to everything).
  const minParsed = minFilter === "" ? null : Number(minFilter);
  const maxParsed = maxFilter === "" ? null : Number(maxFilter);
  const minInvalid = minParsed !== null && !Number.isFinite(minParsed);
  const maxInvalid = maxParsed !== null && !Number.isFinite(maxParsed);

  const rawAdc = useMemo(() => {
    const min = minParsed !== null && Number.isFinite(minParsed) ? minParsed : -Infinity;
    const max = maxParsed !== null && Number.isFinite(maxParsed) ? maxParsed : Infinity;
    return allRawAdc.filter((v) => v >= min && v <= max);
  }, [allRawAdc, minParsed, maxParsed]);

  const filterActive = (minFilter !== "" && !minInvalid) || (maxFilter !== "" && !maxInvalid);
  const stats = useMemo(() => computeStats(rawAdc), [rawAdc]);
  const buckets = useMemo(() => computeBuckets(rawAdc), [rawAdc]);
  const filledBuckets = useMemo(() => buckets?.filter((b) => b.count > 0) ?? [], [buckets]);

  return (
    <>
      <div className="panel">
        <h3>Tare consistency (raw ADC at tare)</h3>
        {error && <p style={{ color: "var(--red)" }}>{error}</p>}
        {!samples && !error && <p className="muted">Loading from PostgREST...</p>}
        {samples && samples.length === 0 && <p className="muted">No data recorded yet.</p>}
        {samples && samples.length > 0 && (
          <div className="setting-row">
            <div className="label">Filter (raw ADC range)</div>
            <div style={{ display: "flex", alignItems: "center", gap: "0.5em", flexWrap: "wrap" }}>
              <input
                type="number"
                placeholder="min"
                value={minFilter}
                onChange={(e) => setMinFilter(e.target.value)}
                style={{ width: "8em" }}
              />
              <span className="muted">to</span>
              <input
                type="number"
                placeholder="max"
                value={maxFilter}
                onChange={(e) => setMaxFilter(e.target.value)}
                style={{ width: "8em" }}
              />
              <button
                onClick={() => {
                  setMinFilter("");
                  setMaxFilter("");
                }}
                disabled={minFilter === "" && maxFilter === ""}
              >
                Clear
              </button>
            </div>
          </div>
        )}
        {(minInvalid || maxInvalid) && (
          <p style={{ color: "var(--red)" }}>
            {minInvalid && maxInvalid
              ? "Min and max are not valid numbers -- ignored."
              : minInvalid
                ? "Min is not a valid number -- ignored."
                : "Max is not a valid number -- ignored."}
          </p>
        )}
        {filterActive && (
          <p className="muted">
            Showing {rawAdc.length} of {allRawAdc.length} samples in range.
          </p>
        )}
        {stats && (
          <p>
            <strong>{stats.n}</strong> doses · mean <strong>{stats.mean.toFixed(1)}</strong> · sd{" "}
            <strong>{stats.sd.toFixed(1)}</strong> · min <strong>{stats.min.toFixed(1)}</strong> · max{" "}
            <strong>{stats.max.toFixed(1)}</strong> · range <strong>{stats.range.toFixed(1)}</strong>
          </p>
        )}
        {samples && samples.length > 0 && rawAdc.length === 0 && (
          <p className="muted">No samples fall within the filter range.</p>
        )}
      </div>

      {rawAdc.length > 0 && (
        <div className="panel">
          <h3>Distribution</h3>
          {buckets ? (
            <>
              <TareHistogram buckets={buckets} />
              <div className="table-scroll" style={{ marginTop: "1rem" }}>
                <table>
                  <thead>
                    <tr>
                      <th>Bucket</th>
                      <th>Count</th>
                    </tr>
                  </thead>
                  <tbody>
                    {filledBuckets.map((b) => (
                      <tr key={b.lower}>
                        <td>
                          ({b.lower.toFixed(0)}, {b.upper.toFixed(0)}]
                        </td>
                        <td>{b.count}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </>
          ) : (
            <p className="muted">Not enough spread to histogram yet.</p>
          )}
        </div>
      )}
    </>
  );
}
