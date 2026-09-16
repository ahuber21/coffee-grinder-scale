// Mirrors the JSON envelope NetworkTask.cpp actually builds/parses
// (lib/NetworkTask/NetworkTask.cpp: buildTelemetryJson, buildSettingsJson,
// handleWsMessage) -- not the full TelemetryEvent/SettingsSnapshot C++
// structs, just the subset of fields each envelope actually serializes.
// Keep this in lockstep with that file by hand; there's no shared
// schema-generation between firmware and SPA in this project.

export type TelemetryType =
  | "target"
  | "progress"
  | "raw_sample"
  | "topup_pulse"
  | "finalize"
  | "complete"
  | "log"
  | "model_state";

interface TelemetryBase {
  type: TelemetryType;
  session_id: number;
  runtime_ms: number;
}

export interface TelemetryGrams extends TelemetryBase {
  type: "target" | "progress" | "finalize" | "complete";
  grams: number;
  target_grams: number;
}

export interface TelemetryTopupPulse extends TelemetryBase {
  type: "topup_pulse";
  grams: number;
  delta_grams: number;
  target_grams: number;
}

export interface TelemetryRawSample extends TelemetryBase {
  type: "raw_sample";
  raw_adc: number;
  grams: number;
  stable: boolean;
}

export interface TelemetryLog extends TelemetryBase {
  type: "log";
  line: string;
}

export interface TelemetryModelState extends TelemetryBase {
  type: "model_state";
  rate_hat_g_s: number;
  rate_sd_g_s: number;
  rate_n_effective: number;
  coast_weight_g: number;
  coast_weight_sd_g: number;
  // Per-gap-bucket tuned pulse duration/observation count: index i covers
  // gap in (i*0.1g, (i+1)*0.1g], e.g. index 0 = (0, 0.1g], index 9 = (0.9, 1.0g].
  topup_lut_duration_ms: number[];
  topup_lut_n: number[];
}

export type TelemetryMessage =
  | TelemetryGrams
  | TelemetryTopupPulse
  | TelemetryRawSample
  | TelemetryLog
  | TelemetryModelState;

// Matches NetworkTask.cpp's CALIBRATION_FACTOR_WIRE_SCALE exactly --
// divide calibration_factor_x1e6 by this to get the true factor, and
// multiply a true factor by this before writing it back.
export const CALIBRATION_FACTOR_WIRE_SCALE = 1e6;

// Mirrors buildSettingsJson's field list, which now covers every
// SettingsSnapshot field except the two write-only WiFi action flags.
export interface SettingsMessage {
  type: "settings";
  version: number;
  // Scaled ×1e6 on the wire, not the true calibration factor -- see
  // NetworkTask.cpp's CALIBRATION_FACTOR_WIRE_SCALE comment.
  // ArduinoJson's double serializer prints a fixed 9 places after the
  // decimal point (not 9 significant digits), which loses most of a
  // value this small (~1e-4) to leading zeros; the ×1e6 scaling is what
  // keeps a round-trip through Settings/Calibration from silently
  // truncating a carefully-measured factor to ~6 significant digits.
  calibration_factor_x1e6: number;
  target_dose_single: number;
  target_dose_double: number;
  top_up_margin_single: number;
  top_up_margin_double: number;
  min_topup_grams: number;
  button_debounce_ms: number;
  screensaver_timeout_s: number;
  screensaver_wake_weight_delta_g: number;
  read_samples: number;
  speed: number;
  gain: number;
  rate_calculation_percentage: number;
  topup_timeout_ms: number;
  grinding_timeout_ms: number;
  finalize_timeout_ms: number;
  confirm_timeout_ms: number;
  stability_min_wait_ms: number;
  stability_max_wait_ms: number;
  min_topup_runtime_ms: number;
  min_topup_interval_ms: number;
  button_min_hold_ms: number;
  display_clump_density: number;
  display_clump_gravity: number;
}

export interface ErrorMessage {
  type: "error";
  message: string;
  request_id: number;
}

export type InboundMessage = TelemetryMessage | SettingsMessage | ErrorMessage | RawReadMessage;

// handleWsMessage's settingsFieldFromName -- the exact set of fields the
// firmware currently accepts a write for. Extending this requires a
// matching change in lib/Messaging/Messages.h's SettingsFieldId and
// NetworkTask.cpp's settingsFieldFromName first.
export type WritableSettingsField =
  | "calibration_factor_x1e6"
  | "target_dose_single"
  | "target_dose_double"
  | "top_up_margin_single"
  | "top_up_margin_double"
  | "button_debounce_ms"
  | "wifi_reset_flag"
  | "wifi_reboot_flag"
  | "read_samples"
  | "speed"
  | "gain"
  | "min_topup_grams"
  | "rate_calculation_percentage"
  | "topup_timeout_ms"
  | "grinding_timeout_ms"
  | "finalize_timeout_ms"
  | "confirm_timeout_ms"
  | "stability_min_wait_ms"
  | "stability_max_wait_ms"
  | "min_topup_runtime_ms"
  | "min_topup_interval_ms"
  | "screensaver_timeout_s"
  | "screensaver_wake_weight_delta_g"
  | "button_min_hold_ms"
  | "display_clump_density"
  | "display_clump_gravity";

export interface SettingsWriteOutbound {
  type: "settings_write";
  field: WritableSettingsField;
  value: number | boolean;
  request_id: number;
}

export interface DoseRequestOutbound {
  type: "dose_request";
  grams: number;
  request_id: number;
}

// Advanced/Calibration page's live polling -- answered directly from
// ScaleTask's latest-sample mailbox (NetworkTask.cpp's sendRawRead()),
// not through the session/telemetry machinery, since it's an ephemeral
// probe with nothing to persist.
export interface RawReadRequestOutbound {
  type: "raw_read_request";
  request_id: number;
}

export interface RawReadMessage {
  type: "raw_read";
  request_id: number;
  raw_adc: number;
  grams: number;
  stable: boolean;
}

export type OutboundMessage = SettingsWriteOutbound | DoseRequestOutbound | RawReadRequestOutbound;
