import { useEffect, useMemo, useRef } from "react";
import {
  Chart,
  BarController,
  BarElement,
  LineController,
  LineElement,
  PointElement,
  LinearScale,
  Tooltip,
  Legend,
} from "chart.js";
import { legendLabels, linearAxis, tooltipTheme } from "../lib/chartTheme";
import { mean, sampleStdDev } from "../lib/stats";

Chart.register(BarController, BarElement, LineController, LineElement, PointElement, LinearScale, Tooltip, Legend);

/// The x axis is fixed to this window so the single and double panels stay comparable.
const RANGE_G = 0.5;
const BIN_WIDTH_G = 0.05;

interface Props {
  label: string;
  deltas: number[];
  color: string;
  xLabel?: string;
  emptyText?: string;
}

/** A histogram of errors in grams with a fitted Gaussian overlay. */
export default function DeltaHistogram({
  label,
  deltas,
  color,
  xLabel = "Δ from target (g)",
  emptyText = "No completed sessions yet.",
}: Props) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  // Early hardware-fix sessions sit far outside the window (0g, negative, 100g+). They are real
  // numbers but not today's accuracy, so only the window feeds the mean and sd.
  const fit = useMemo(() => {
    const inRange = deltas.filter((d) => d >= -RANGE_G && d < RANGE_G);
    if (inRange.length === 0) return null;
    return {
      n: inRange.length,
      mean: mean(inRange),
      sd: sampleStdDev(inRange),
      excluded: deltas.length - inRange.length,
    };
  }, [deltas]);

  useEffect(() => {
    if (!canvasRef.current) return;

    const binCount = Math.round((2 * RANGE_G) / BIN_WIDTH_G);
    const bins = Array.from({ length: binCount }, (_, i) => ({
      center: -RANGE_G + (i + 0.5) * BIN_WIDTH_G,
      count: 0,
    }));
    for (const d of deltas) {
      if (d < -RANGE_G || d >= RANGE_G) continue;
      bins[Math.min(binCount - 1, Math.floor((d + RANGE_G) / BIN_WIDTH_G))].count++;
    }

    const curve: { x: number; y: number }[] = [];
    if (fit && fit.sd > 0) {
      const steps = 120;
      for (let i = 0; i <= steps; i++) {
        const x = -RANGE_G + (i / steps) * 2 * RANGE_G;
        const z = (x - fit.mean) / fit.sd;
        const density = Math.exp(-0.5 * z * z) / (fit.sd * Math.sqrt(2 * Math.PI));
        curve.push({ x, y: fit.n * BIN_WIDTH_G * density });
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
          x: linearAxis(xLabel, { min: -RANGE_G, max: RANGE_G }),
          y: linearAxis("Count", { beginAtZero: true }),
        },
        plugins: {
          legend: legendLabels(14, { boxHeight: 10 }),
          tooltip: tooltipTheme({ title: (items: { parsed: { x: number | null } }[]) => `Δ ${(items[0]?.parsed.x ?? 0).toFixed(2)}g` }),
        },
      },
    });
    return () => chartRef.current?.destroy();
  }, [deltas, fit, label, color, xLabel]);

  return (
    <div>
      <h4 style={{ margin: "0 0 0.5rem" }}>{label}</h4>
      {deltas.length === 0 ? (
        <p className="muted">{emptyText}</p>
      ) : (
        <>
          <canvas ref={canvasRef} height={200} />
          {fit && (
            <p className="muted" style={{ fontSize: "0.85em" }}>
              μ = {fit.mean.toFixed(3)}g, σ = {fit.sd.toFixed(3)}g, n = {fit.n}
              {fit.excluded > 0 && ` (${fit.excluded} outside ±${RANGE_G}g excluded from the fit)`}
            </p>
          )}
        </>
      )}
    </div>
  );
}
