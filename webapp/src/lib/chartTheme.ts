// The dark-theme Chart.js styling shared by every chart, so each chart only states what is unique to it.

const AXIS_TEXT = "rgba(235, 235, 245, 0.45)";
const GRID = "rgba(84, 84, 88, 0.2)";
const LEGEND_TEXT = "rgba(235, 235, 245, 0.75)";

export const ACCENT_BLUE = "#0a84ff";
export const ACCENT_GREEN = "#30d158";
export const ACCENT_ORANGE = "#ff9f0a";

interface AxisOptions {
  beginAtZero?: boolean;
  min?: number;
  max?: number;
  stepSize?: number;
}

/** A linear axis with a title, in the shared dark styling. */
export function linearAxis(title: string, options: AxisOptions = {}) {
  const { stepSize, ...scale } = options;
  return {
    type: "linear" as const,
    ...scale,
    title: { display: true, text: title, color: AXIS_TEXT },
    ticks: stepSize === undefined ? { color: AXIS_TEXT } : { color: AXIS_TEXT, stepSize },
    grid: { color: GRID },
    border: { display: false },
  };
}

/** Tooltip styling; pass `callbacks` for per-chart titles and labels. */
export function tooltipTheme<T extends object>(callbacks?: T, displayColors = true) {
  return {
    backgroundColor: "#1c1c1e",
    titleColor: "rgba(235, 235, 245, 0.6)",
    bodyColor: "#ffffff",
    borderColor: "rgba(84, 84, 88, 0.65)",
    borderWidth: 1,
    padding: 10,
    cornerRadius: 8,
    displayColors,
    ...(callbacks ? { callbacks } : {}),
  };
}

/** Legend label styling. */
export function legendLabels(boxWidth = 14, extra: { boxHeight?: number; font?: { size: number } } = {}) {
  return { labels: { color: LEGEND_TEXT, boxWidth, ...extra } };
}

/** Style shared by the line charts that plot weight over time. */
export function weightLineDataset(color: string, fill: string, tension: number) {
  return {
    borderColor: color,
    backgroundColor: fill,
    borderWidth: 2,
    pointRadius: 0,
    pointHoverRadius: 4,
    pointHoverBackgroundColor: color,
    pointHoverBorderColor: "#0b0b0c",
    pointHoverBorderWidth: 2,
    tension,
    fill: "origin" as const,
    parsing: false as const,
  };
}
