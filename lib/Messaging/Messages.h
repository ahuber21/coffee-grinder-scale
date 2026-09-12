#pragma once

/**
 * Message-shape definitions shared by the FreeRTOS tasks. Every struct
 * here is plain, self-contained, and safe to copy across task
 * boundaries by value -- that is what makes both the queue and mailbox
 * patterns in Queues.h safe without additional locking.
 */

#include <Arduino.h>

#include "DosingModel.h"  // TopupModelV1

/** Physical button identity, from ISR up through Input and Dosing tasks. */
enum class ButtonId : uint8_t { LEFT, RIGHT, BACK };

/** Raw pin transition, pushed from ISR context with no shared-state writes. */
struct ButtonEdge {
  ButtonId button;
  bool level;       ///< Pin level at the edge.
  uint32_t millis;  ///< ISR-local millis(), nothing else touched.
};

/** A debounced button press, confirmed by Input task. */
struct ButtonPress {
  ButtonId button;
  uint32_t pressed_at_ms;  ///< When the debounced press was confirmed.
};

/** One filtered scale reading, sent from Scale task to Dosing task. */
struct ScaleSample {
  float grams;          ///< Filtered/calibrated weight.
  int32_t raw_adc;      ///< Raw ADC code, for telemetry/debug.
  bool stable;          ///< ADS1232 stability flag (ring buffer settled).
  uint32_t sample_seq;  ///< Monotonic, wraps at ~4B -- for drop detection.
  uint32_t millis;      ///< Producer-side millis() at sample time.
};

/**
 * Dosing task's session FSM state and Display task's render mode --
 * one enum for both, since every FSM state maps to exactly one display
 * layout and a second parallel enum would just be duplication that
 * could silently drift out of sync. OTA_UPDATE is the one value Dosing
 * task never assigns; only Network task sets it, while an OTA flash is
 * in progress.
 */
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

/** What to render, sent to Display task's single overwrite mailbox. */
struct DisplayCommand {
  uint32_t seq;  ///< Monotonic frame id.
  DisplayMode mode;
  float current_grams;
  float target_grams;
  float elapsed_s;
  uint16_t current_color;
  uint16_t target_color;
  uint16_t time_color;
  uint16_t connection_indicator_color;  ///< 0 = none; always present, never omitted.
  // SCREENSAVER-only fields.
  uint32_t idle_h, idle_m, idle_s, idle_ms;
  bool idle_is_uptime;  ///< vs. time-since-last-coffee.
  // DEBUG-only fields.
  int32_t debug_raw_adc;
  bool debug_stable;
  char debug_ip[16];
  // OTA_UPDATE-only field.
  uint8_t ota_percent;
};

/** Discriminates TelemetryEvent's meaning. */
enum class TelemetryType : uint8_t {
  TARGET,
  PROGRESS,
  RAW_SAMPLE,
  TOPUP_PULSE,
  FINALIZE,
  COMPLETE,
  LOG_LINE,
  MODEL_STATE,
};

/**
 * One telemetry event, forwarded to PostgREST and the WS broadcast
 * queue. Field usage varies by `type` -- see the per-field comments
 * below.
 */
struct TelemetryEvent {
  TelemetryType type;
  uint32_t session_id;  ///< Assigned by Dosing task when a session starts.
  uint32_t runtime_ms;  ///< Session-relative.
  float grams;
  float target_grams;
  float delta_grams;  ///< TOPUP_PULSE only.
  int32_t raw_adc;
  bool stable;
  char log_line[96];  ///< LOG_LINE only.
  /*
   * TARGET only, so Telemetry task can populate PostgREST's session
   * row without a second round trip back to Dosing task. is_double
   * mirrors whether this session is a double dose; target_grams_corrected
   * is the coast/margin-corrected stop target Dosing task actually aims
   * for internally -- target_grams above stays the requested,
   * uncorrected value the user or caller asked for.
   */
  bool is_double;
  float target_grams_corrected;

  /*
   * MODEL_STATE only -- lib/DosingModel's current persisted parameters
   * (standard deviations, not raw precisions, since that's what a
   * reader actually wants), sent once at boot and again after every
   * completed session. Powers the SPA's "how does this work" view.
   */
  float rate_hat_g_s;
  float rate_sd_g_s;
  float topup_slope_g_s;
  float topup_deadtime_ms;
  float topup_residual_sd_g;
  float coast_weight_g;
  float coast_weight_sd_g;
  uint32_t rate_n_effective;
  uint32_t topup_n_effective;
};

/** A manual/API dose request, sent from Network task to Dosing task. */
struct DoseRequest {
  float requested_grams;
  uint32_t request_id;
};

/**
 * The single source of truth for tunable settings, distributed to
 * every task via one overwrite mailbox per subscriber. Persisted via
 * the write-through path below rather than a dirty flag, and carries
 * no static topup lookup table -- lib/DosingModel's fitted models
 * cover that entirely.
 */
struct SettingsSnapshot {
  uint32_t version = 0;  ///< Bumped by Settings task on every accepted write.

  // Scale/ADC config (Scale task's slice). 128/10/12 are this device's
  // actual hardware calibration, not placeholders.
  uint8_t read_samples = 12;
  uint8_t speed = 10;
  uint8_t gain = 128;
  /*
   * 1.0f, not 0.0f: ScaleTask forwards this straight into
   * ADS1232::setCalFactor (units = raw * calFactor), and the driver's
   * own constructor already defaults calFactor to 1.0f for exactly this
   * reason -- a fresh/uncalibrated scale should read raw counts
   * (visibly wrong, but a real number), not silently read 0.0g forever.
   */
  float calibration_factor = 1.0f;

  // Dosing config (Dosing task's slice).
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

  // Input task's slice.
  uint32_t button_debounce_ms = 150;
  uint32_t button_min_hold_ms = 20;

  // Network task's slice.
  bool wifi_reset_flag = false;
  bool wifi_reboot_flag = false;
};

/**
 * One value per SettingsSnapshot field a write request can target.
 * Deliberately a subset, not exhaustive coverage -- extend alongside
 * SettingsTask.cpp's applyWrite() and NetworkTask.cpp's
 * settingsFieldFromName() when a new field needs a live write path.
 */
enum class SettingsFieldId : uint16_t {
  CALIBRATION_FACTOR,
  TARGET_DOSE_SINGLE,
  TARGET_DOSE_DOUBLE,
  TOP_UP_MARGIN_SINGLE,
  TOP_UP_MARGIN_DOUBLE,
  BUTTON_DEBOUNCE_MS,
  WIFI_RESET_FLAG,
  WIFI_REBOOT_FLAG,
  READ_SAMPLES,
  SPEED,
  GAIN,
  MIN_TOPUP_GRAMS,
  RATE_CALCULATION_PERCENTAGE,
  TOPUP_TIMEOUT_MS,
  GRINDING_TIMEOUT_MS,
  FINALIZE_TIMEOUT_MS,
  CONFIRM_TIMEOUT_MS,
  STABILITY_MIN_WAIT_MS,
  STABILITY_MAX_WAIT_MS,
  MIN_TOPUP_RUNTIME_MS,
  MIN_TOPUP_INTERVAL_MS,
  SCREENSAVER_TIMEOUT_S,
  BUTTON_MIN_HOLD_MS,
};

/** A single validated field write, sent from Network task to Settings task. */
struct SettingsWriteRequest {
  SettingsFieldId field_id;
  union {
    float f;
    uint32_t u;
    bool b;
  } value;
  uint32_t request_id;  ///< For an ack/error reply back to the WS client.
};

/** Identifies which persisted blob a PersistRequest carries. */
enum class PersistBlobId : uint16_t {
  TOPUP_MODEL_V1,
};

/**
 * A session-boundary persistence request from Dosing task to Settings
 * task, fired once when a session finishes -- never per-sample.
 */
struct PersistRequest {
  PersistBlobId blob_id;
  TopupModelV1 payload;
  uint32_t request_id;
};
