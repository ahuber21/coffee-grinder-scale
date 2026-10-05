#pragma once

/**
 * Message shapes shared by the FreeRTOS tasks. Every struct here is
 * plain and self-contained, so it can be copied across task boundaries
 * by value, which keeps the queue and mailbox patterns in Queues.h lock-free.
 */

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>  // uint8_t/uint32_t/etc for a native build (e.g. tools/display_sim)
#endif

#include "DosingModel.h"  // DosingModelState

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
 * Dosing task's session FSM state and Display task's render mode. One
 * enum serves both because every state maps to exactly one layout.
 * OTA_UPDATE is never assigned by Dosing task; Network task sets it
 * during an OTA flash.
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
  TARE_DEBUG,  ///< The tare baseline, one per session, for checking tare consistency across sessions.
  LANDING,     ///< Main-grind landing measurements, once per session after the post-grind settle.
};

/** Which of GRINDING's three stop conditions actually fired -- PROGRESS only. */
enum class GrindStopReason : uint8_t { TIME_ESTIMATE, RAW_WEIGHT_FALLBACK, SAFETY_TIMEOUT };

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
   * TARGET only: whether this is a double dose, and the margin-corrected
   * stop target Dosing aims for (target_grams stays the requested dose).
   */
  bool is_double;
  float target_grams_corrected;

  /// PROGRESS only: which stop condition fired, and the model's weight estimate then (delta space).
  GrindStopReason stop_reason;
  float weight_estimate_g;

  /// TOPUP_PULSE only: the pulse's inputs at fire time, which later retuning makes unrecoverable.
  uint8_t topup_bucket;
  float topup_aim_weight_g;
  uint32_t topup_commanded_duration_ms;

  /// MODEL_STATE only: the persisted model parameters as standard deviations, sent at boot and after each session.
  float rate_hat_g_s;
  float rate_sd_g_s;
  float coast_weight_g;
  float coast_weight_sd_g;
  uint32_t rate_n_effective;
  /// Per-gap-bucket tuned pulse duration and observation count (see kTopupLutBuckets).
  float topup_lut_duration_ms[kTopupLutBuckets];
  uint32_t topup_lut_n[kTopupLutBuckets];

  /// LANDING only (the settled weight itself travels in `grams`): coast, margin and learner shift.
  float landing_coast_g;
  float landing_margin_g;
  float landing_correction_g;  ///< Applied stop-point shift, positive = stopped earlier.
  bool landing_clamped;
};

/// Largest dose, in grams, an API request may ask for.
constexpr float kMaxDoseGrams = 50.0f;

/** A manual/API dose request, sent from Network task to Dosing task. */
struct DoseRequest {
  float requested_grams;
  uint32_t request_id;
  /// Runs the dose normally but discards its model updates, for test doses that would skew the estimates.
  bool discard_training;
};

/**
 * A hardware re-tare request from Dosing task to Scale task, so every
 * dose starts from a fresh zero instead of a possibly stale idle auto-tare.
 * The request itself is the whole message.
 */
struct TareRequest {};

/**
 * Scale task's reply to a TareRequest. `ok == false` means no stable zero
 * was found in time, and Dosing task must abort the dose.
 */
struct TareResult {
  bool ok;
};

/**
 * The single source of truth for tunable settings, distributed to every
 * task through one overwrite mailbox per subscriber. SettingsSchema.h
 * describes each member's name, NVS key and validator.
 */
struct SettingsSnapshot {
  uint32_t version = 0;  ///< Bumped by Settings task on every accepted write.

  // Scale/ADC config (Scale task's slice).
  uint8_t read_samples = 12;
  uint8_t speed = 10;
  uint8_t gain = 128;
  /*
   * Defaults to 1.0 so an uncalibrated scale reads raw counts (visibly
   * wrong) rather than a silent 0.0g. A double, because a realistic
   * factor (~0.000924583895) has more significant digits than a float
   * keeps once it multiplies ADC deltas in the tens of thousands.
   */
  double calibration_factor = 1.0;

  // Dosing config (Dosing task's slice).
  float target_dose_single = 18.0f;
  float target_dose_double = 36.0f;
  float top_up_margin_single = 0.3f;
  float top_up_margin_double = 0.5f;
  float min_topup_grams = 0.08f;
  uint32_t topup_timeout_ms = 1000;
  uint32_t grinding_timeout_ms = 30000;
  uint32_t finalize_timeout_ms = 8000;
  uint32_t confirm_timeout_ms = 2000;
  uint32_t stability_min_wait_ms = 500;
  uint32_t stability_max_wait_ms = 1500;
  uint32_t screensaver_timeout_s = 60;
  /// A settled weight change past this wakes the display from SCREENSAVER, no button press needed.
  float screensaver_wake_weight_delta_g = 2.0f;

  // Input task's slice.
  uint32_t button_debounce_ms = 150;
  uint32_t button_min_hold_ms = 20;

  // Display task's slice: multipliers on the falling-clumps animation, where 1.0 is the default look.
  float display_clump_density = 1.0f;  ///< Scales how many clumps fall at once.
  float display_clump_gravity = 1.0f;  ///< Scales how fast each clump falls.

  // Dosing task's landing learner, which shifts the main-grind stop so the settled weight hits the offset target.
  bool landing_learner_enabled = true;
  float landing_learner_rate = 0.05f;  ///< Per-session forgetting; evidence window ~1/rate sessions.
  uint32_t landing_learner_clamp_ms = 500;  ///< Max stop-time shift either way vs the unlearned stop.

  // Network task's slice.
  bool wifi_reset_flag = false;
  bool wifi_reboot_flag = false;
};

/** One field write, sent from Network task to Settings task, which alone validates it. */
struct SettingsWriteRequest {
  uint16_t field_index;  ///< Index into settingsTable() (SettingsSchema.h).
  double value;          ///< The stored value; booleans are 0 or 1.
  uint32_t request_id;   ///< For an ack/error reply back to the WS client.
};

/** Identifies which persisted blob a PersistRequest carries. */
enum class PersistBlobId : uint16_t {
  DOSING_MODEL,
  LANDING_LEARNER,
};

/** A persistence request from Dosing task to Settings task, sent once when a session finishes. */
struct PersistRequest {
  PersistBlobId blob_id;
  DosingModelState payload;                ///< Valid when blob_id == DOSING_MODEL.
  LandingLearnerState learner_payload;  ///< Valid when blob_id == LANDING_LEARNER.
  uint32_t request_id;
};
