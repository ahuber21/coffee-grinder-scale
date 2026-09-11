#pragma once

// Message-shape definitions for the FreeRTOS task architecture, per
// .agent/design/rtos-architecture.md §3. Every struct here is plain,
// self-contained, and safe to copy across task boundaries by value (no
// pointers) -- that's what makes both the queue and mailbox patterns in
// Queues.h safe without any additional locking.
//
// This header is intentionally Arduino/ESP32-light (only pulls in
// <Arduino.h> for basic integer typedefs already used elsewhere in this
// codebase) so it stays easy to reason about from any task's .cpp.

#include <Arduino.h>

#include "DosingModel.h"  // TopupModelV1 -- lib/DosingModel, reused as-is (§5)

// ---------------------------------------------------------------------------
// §3.3 -- Buttons: ISR -> Input task -> Dosing task
// ---------------------------------------------------------------------------

enum class ButtonId : uint8_t { LEFT, RIGHT, BACK };

struct ButtonEdge {
  ButtonId button;
  bool level;     // pin level at the edge
  uint32_t millis;  // ISR-local millis(), nothing else touched
};

struct ButtonPress {
  ButtonId button;
  uint32_t pressed_at_ms;  // when the debounced press was confirmed
};

// ---------------------------------------------------------------------------
// §3.1 -- Scale task -> Dosing task
// ---------------------------------------------------------------------------

struct ScaleSample {
  float grams;       // filtered/calibrated weight
  int32_t raw_adc;    // raw ADC code, for telemetry/debug
  bool stable;        // ADS1232 stability flag (ring-buffer settled)
  uint32_t sample_seq;  // monotonic, wraps at ~4B -- for drop detection
  uint32_t millis;      // producer-side millis() at sample time
};

// ---------------------------------------------------------------------------
// §3.2 -- Dosing task -> Display task (mailbox)
//
// DisplayMode also stands in as Dosing task's own session FSM state (the
// "state" row of §2's ownership table) -- the design doc keeps them
// conceptually distinct, but they are a 1:1 mapping in this codebase (every
// FSM state has exactly one corresponding display layout), so this skeleton
// uses one enum for both rather than maintaining two enums that must be kept
// in permanent lockstep. OTA_UPDATE is the one value Dosing task itself
// never assigns -- see §7, Network task is the second/narrow writer for that
// one mode only.
// ---------------------------------------------------------------------------

enum class DisplayMode : uint8_t {
  BOOT,
  IDLE,
  CONFIRM,
  TARE,
  GRINDING,
  TOPUP,
  STOPPING,
  FINALIZE,
  SCREENSAVER,
  DEBUG,
  OTA_UPDATE,
};

struct DisplayCommand {
  uint32_t seq;  // monotonic frame id
  DisplayMode mode;
  float current_grams;
  float target_grams;
  float elapsed_s;
  uint16_t current_color;
  uint16_t target_color;
  uint16_t time_color;
  uint16_t connection_indicator_color;  // 0 = none; always part of this struct (AR-012 fix)
  // screensaver-only
  uint32_t idle_h, idle_m, idle_s, idle_ms;
  bool idle_is_uptime;  // vs. time-since-last-coffee
  // debug-only
  int32_t debug_raw_adc;
  bool debug_stable;
  char debug_ip[16];
  // ota-only
  uint8_t ota_percent;
};

// ---------------------------------------------------------------------------
// §3.4 -- Dosing task -> Telemetry task
// ---------------------------------------------------------------------------

enum class TelemetryType : uint8_t {
  TARGET,
  PROGRESS,
  RAW_SAMPLE,
  TOPUP_PULSE,
  FINALIZE,
  COMPLETE,
  LOG_LINE,
};

struct TelemetryEvent {
  TelemetryType type;
  uint32_t session_id;  // assigned by dosing_task at CONFIGURED entry
  uint32_t runtime_ms;   // session-relative
  float grams;
  float target_grams;
  float delta_grams;  // TOPUP_PULSE only
  int32_t raw_adc;
  bool stable;
  char log_line[96];  // LOG_LINE only
  // TARGET only -- carried so Telemetry task can populate PostgREST
  // v2.sessions' mode/target_weight_g columns (db-schema/001_sessions_schema.sql)
  // without needing its own SettingsSnapshot subscription or a second
  // message hop back to Dosing task. is_double mirrors Dosing task's
  // g_is_double; target_grams_corrected mirrors g_target_grams_corrected
  // (the coast/margin-corrected stop target -- target_grams above stays the
  // *requested*, uncorrected value, matching topup-model.md §6's requested-
  // vs-target distinction).
  bool is_double;
  float target_grams_corrected;
};

// ---------------------------------------------------------------------------
// §3.6 -- Network task -> Dosing task
// ---------------------------------------------------------------------------

struct DoseRequest {
  float requested_grams;
  uint32_t request_id;
};

// ---------------------------------------------------------------------------
// §4 -- Settings/NVS. SettingsSnapshot mirrors today's
// WebSocketSettings::Scale field set (lib/WebSocketSettings/WebSocketSettings.h),
// minus `is_changed` (unnecessary once writes go through the request/ack
// path below) and `magic`/`topup_lookup_table[6]` (D7 replaces the static
// lookup table with lib/DosingModel entirely, so it has no successor field
// here). Plain, self-contained, no pointers -- safe to copy across tasks.
// ---------------------------------------------------------------------------

struct SettingsSnapshot {
  uint32_t version = 0;  // bumped by Settings task on every accepted write

  // Scale/ADC config (Scale task's slice)
  uint8_t read_samples = 8;
  uint8_t speed = 10;
  uint8_t gain = 1;
  float calibration_factor = 0.0f;

  // Dosing config (Dosing task's slice)
  float target_dose_single = 18.0f;
  float target_dose_double = 36.0f;
  float top_up_margin_single = 0.3f;
  float top_up_margin_double = 0.5f;
  float min_topup_grams = 0.08f;
  float rate_calculation_percentage = 0.75f;
  uint32_t topup_timeout_ms = 1000;
  uint32_t grinding_timeout_ms = 30000;
  uint32_t finalize_timeout_ms = 8000;
  uint32_t confirm_timeout_ms = 2000;
  uint32_t stability_min_wait_ms = 500;
  uint32_t stability_max_wait_ms = 1500;
  uint32_t min_topup_runtime_ms = 500;
  uint32_t min_topup_interval_ms = 1000;
  uint32_t screensaver_timeout_s = 60;

  // Input task's slice
  uint32_t button_debounce_ms = 150;
  uint32_t button_min_hold_ms = 20;

  // Network task's slice
  bool wifi_reset_flag = false;
  bool wifi_reboot_flag = false;
};

// Field identifiers for §4's write path -- one enum value per field a
// settings-write request can target. Deliberately a small subset for this
// skeleton (enough to demonstrate the validated single-writer pattern), not
// exhaustive coverage of every SettingsSnapshot field yet.
enum class SettingsFieldId : uint16_t {
  CALIBRATION_FACTOR,
  TARGET_DOSE_SINGLE,
  TARGET_DOSE_DOUBLE,
  TOP_UP_MARGIN_SINGLE,
  TOP_UP_MARGIN_DOUBLE,
  BUTTON_DEBOUNCE_MS,
  WIFI_RESET_FLAG,
  WIFI_REBOOT_FLAG,
};

struct SettingsWriteRequest {
  SettingsFieldId field_id;
  union {
    float f;
    uint32_t u;
    bool b;
  } value;
  uint32_t request_id;  // for an ack/error reply back to the WS client
};

// ---------------------------------------------------------------------------
// §5 -- Topup model persistence round trip (Dosing task -> Settings task,
// session-boundary event only, never per-sample).
// ---------------------------------------------------------------------------

enum class PersistBlobId : uint16_t {
  TOPUP_MODEL_V1,
};

struct PersistRequest {
  PersistBlobId blob_id;
  TopupModelV1 payload;
  uint32_t request_id;
};
