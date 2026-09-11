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
  | "log";

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

export type TelemetryMessage =
  | TelemetryGrams
  | TelemetryTopupPulse
  | TelemetryRawSample
  | TelemetryLog;

// buildSettingsJson's field list. Deliberately NOT every SettingsSnapshot
// field -- only what the firmware currently broadcasts.
export interface SettingsMessage {
  type: "settings";
  version: number;
  calibration_factor: number;
  target_dose_single: number;
  target_dose_double: number;
  top_up_margin_single: number;
  top_up_margin_double: number;
  min_topup_grams: number;
  button_debounce_ms: number;
  screensaver_timeout_s: number;
}

export interface ErrorMessage {
  type: "error";
  message: string;
  request_id: number;
}

export type InboundMessage = TelemetryMessage | SettingsMessage | ErrorMessage;

// handleWsMessage's settingsFieldFromName -- the exact set of fields the
// firmware currently accepts a write for. Extending this requires a
// matching change in lib/Messaging/Messages.h's SettingsFieldId and
// NetworkTask.cpp's settingsFieldFromName first.
export type WritableSettingsField =
  | "calibration_factor"
  | "target_dose_single"
  | "target_dose_double"
  | "top_up_margin_single"
  | "top_up_margin_double"
  | "button_debounce_ms"
  | "wifi_reset_flag"
  | "wifi_reboot_flag";

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

export type OutboundMessage = SettingsWriteOutbound | DoseRequestOutbound;
