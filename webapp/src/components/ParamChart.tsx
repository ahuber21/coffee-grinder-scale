import { useEffect, useRef } from "react";
import {
  Chart,
  LineController,
  LineElement,
  PointElement,
  LinearScale,
  Tooltip,
  Legend,
} from "chart.js";
import { legendLabels, linearAxis, tooltipTheme } from "../lib/chartTheme";

Chart.register(LineController, LineElement, PointElement, LinearScale, Tooltip, Legend);

export interface ParamSeries {
  label: string;
  color: string;
  values: (number | null)[];
  /** Drawn thin and dashed, for the uncertainty bands around a main series. */
  band?: boolean;
}

interface Props {
  yLabel: string;
  /** One caption per x position, shown in the tooltip title (e.g. the snapshot's date). */
  captions: string[];
  series: ParamSeries[];
  /** Decimals in the tooltip values. */
  digits?: number;
}

/** A parameter plotted against snapshot number, one point per session. */
export default function ParamChart({ yLabel, captions, series, digits = 3 }: Props) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const chartRef = useRef<Chart | null>(null);

  useEffect(() => {
    if (!canvasRef.current) return;
    chartRef.current = new Chart(canvasRef.current, {
      type: "line",
      data: { datasets: [] },
      options: {
        responsive: true,
        animation: false,
        interaction: { mode: "index", intersect: false },
        scales: { x: linearAxis("Session snapshot #"), y: linearAxis(yLabel) },
        plugins: {
          legend: {
            ...legendLabels(12, { font: { size: 11 } }),
            labels: {
              ...legendLabels(12, { font: { size: 11 } }).labels,
              // The lower edge of a band shares the upper edge's legend entry.
              filter: (item) => item.text !== "",
            },
          },
          tooltip: tooltipTheme({}, true),
        },
      },
    });
    return () => {
      chartRef.current?.destroy();
      chartRef.current = null;
    };
    // The chart is created once; the effect below keeps its data current.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const chart = chartRef.current;
    if (!chart) return;
    chart.data.datasets = series.map((s) => ({
      label: s.label,
      data: s.values.map((y, x) => ({ x, y })),
      borderColor: s.color,
      backgroundColor: s.color,
      borderWidth: s.band ? 1 : 2,
      borderDash: s.band ? [4, 4] : [],
      pointRadius: s.band || s.values.length > 80 ? 0 : 2,
      pointHoverRadius: 4,
      tension: 0,
      spanGaps: true,
      parsing: false as const,
    }));
    const tooltip = chart.options.plugins!.tooltip!;
    tooltip.callbacks = {
      title: (items) => captions[items[0]?.parsed.x ?? 0] ?? "",
      label: (item) =>
        `${item.dataset.label}: ${(item.parsed.y ?? 0).toFixed(digits)}`,
    };
    chart.update();
  }, [series, captions, digits]);

  return <canvas ref={canvasRef} height={200} />;
}
