// The JSON envelopes lib/NetworkTask/NetworkTask.cpp builds and parses, limited to the
// fields each one carries. There is no shared schema, so keep this in step with that file.

export type TelemetryType =
  | "target"
  | "progress"
  | "raw_sample"
  | "topup_pulse"
  | "finalize"
  | "complete"
  | "log"
  | "model_state"
  | "tare_debug"
  | "landing";

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

export interface TelemetryTareDebug extends TelemetryBase {
  type: "tare_debug";
  raw_adc: number;
  grams: number;
}

/** Main-grind landing: the settled weight before topup, and what the landing learner did. */
export interface TelemetryLanding extends TelemetryBase {
  type: "landing";
  grams: number;
  target_grams: number;
  coast_g: number;
  margin_g: number;
  correction_g: number;
  clamped: boolean;
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
  /** Per-gap-bucket tuned pulse duration and observation count; index i covers (i*0.1g, (i+1)*0.1g]. */
  topup_lut_duration_ms: number[];
  topup_lut_n: number[];
}

export type TelemetryMessage =
  | TelemetryGrams
  | TelemetryTopupPulse
  | TelemetryRawSample
  | TelemetryTareDebug
  | TelemetryLanding
  | TelemetryLog
  | TelemetryModelState;

/** Matches the firmware's calibration_factor wire scale: divide the wire value by it for the true factor. */
export const CALIBRATION_FACTOR_WIRE_SCALE = 1e6;

/** Every setting the firmware reports except the write-only WiFi action flags. */
export interface SettingsMessage {
  type: "settings";
  version: number;
  /** Scaled by CALIBRATION_FACTOR_WIRE_SCALE: the JSON serializer prints a fixed number of decimals, which would truncate a factor near 1e-4. */
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
  topup_timeout_ms: number;
  grinding_timeout_ms: number;
  finalize_timeout_ms: number;
  confirm_timeout_ms: number;
  stability_min_wait_ms: number;
  stability_max_wait_ms: number;
  button_min_hold_ms: number;
  display_clump_density: number;
  display_clump_gravity: number;
  landing_learner_enabled: boolean;
  landing_learner_rate: number;
  landing_learner_clamp_ms: number;
}

export interface ErrorMessage {
  type: "error";
  message: string;
  request_id: number;
}

export type InboundMessage = TelemetryMessage | SettingsMessage | ErrorMessage | RawReadMessage;

type SettingFieldsOfType<T> = {
  [K in keyof SettingsMessage]: SettingsMessage[K] extends T ? K : never;
}[keyof SettingsMessage];

/** Settings that hold a number; `version` is the snapshot counter, not a setting. */
export type NumericSettingField = Exclude<SettingFieldsOfType<number>, "version">;
export type BooleanSettingField = SettingFieldsOfType<boolean>;
/** One-shot action flags the firmware accepts but never reports back. */
export type ActionSettingField = "wifi_reset_flag" | "wifi_reboot_flag";

/** Every field the firmware accepts a write for. */
export type WritableSettingsField = NumericSettingField | BooleanSettingField | ActionSettingField;

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

/** The Calibration page's live poll, answered straight from the latest scale sample. */
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
