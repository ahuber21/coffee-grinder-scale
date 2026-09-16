#pragma once

/**
 * Message-shape definitions shared by the FreeRTOS tasks. Every struct
 * here is plain, self-contained, and safe to copy across task
 * boundaries by value -- that is what makes both the queue and mailbox
 * patterns in Queues.h safe without additional locking.
 */

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>  // uint8_t/uint32_t/etc for a native build (e.g. tools/display_sim)
#endif

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
  // Debug-only: the tare baseline's raw ADC count and converted grams, one
  // per session, so the same physical dosing cup's tare can be checked for
  // consistency across sessions -- not a calibration input, just stats.
  // See .agent/design/db-schema/002_tare_debug_stats.sql.
  TARE_DEBUG,
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
   * PROGRESS only -- what a completed session's own final_weight_g can
   * never reveal after the fact: which stop condition actually fired,
   * and MainGrindModel's plausibility-protected weight estimate (delta
   * space, matching `grams` above) at that exact instant. See AR-074.
   */
  GrindStopReason stop_reason;
  float weight_estimate_g;

  /*
   * TOPUP_PULSE only -- this specific pulse's own inputs, captured at
   * fire time. Not reconstructable after the fact the way gap_at_fire_g
   * is (from weight_before_g/target_grams): topup_commanded_duration_ms
   * reflects TopupModel's online-learned per-bucket duration at that
   * exact moment, which keeps changing as later pulses retrain it. See
   * AR-074.
   */
  uint8_t topup_bucket;
  float topup_aim_weight_g;
  uint32_t topup_commanded_duration_ms;

  /*
   * MODEL_STATE only -- lib/DosingModel's current persisted parameters
   * (standard deviations, not raw precisions, since that's what a
   * reader actually wants), sent once at boot and again after every
   * completed session. Powers the SPA's "how does this work" view.
   */
  float rate_hat_g_s;
  float rate_sd_g_s;
  float coast_weight_g;
  float coast_weight_sd_g;
  uint32_t rate_n_effective;
  /// Per-gap-bucket tuned pulse duration/observation count -- see
  /// kTopupLutBuckets/kTopupLutBucketWidthG in DosingModel.h.
  float topup_lut_duration_ms[kTopupLutBuckets];
  uint32_t topup_lut_n[kTopupLutBuckets];
};

/** A manual/API dose request, sent from Network task to Dosing task. */
struct DoseRequest {
  float requested_grams;
  uint32_t request_id;
  // Runs the dose normally, but the session's model updates (main-grind
  // rate, topup LUT, coast) never reach NVS or this boot's in-RAM models --
  // for dev-time doses (a placed test weight, a deliberately abnormal
  // grind) that would otherwise poison the estimates every real dose
  // relies on.
  bool discard_training;
};

/**
 * An explicit hardware re-tare request, sent from Dosing task to Scale
 * task once the confirm button is pressed -- guarantees every dose starts
 * from a freshly-zeroed ADC baseline rather than whatever the last
 * opportunistic idle auto-tare happened to leave behind (up to
 * kAutoTareMinIntervalMs/kAutoTareIdleReturnCooldownMs stale). No payload
 * needed; the request itself is the whole message.
 */
struct TareRequest {};

/**
 * Scale task's reply to a TareRequest, sent once the retry loop either
 * settles or gives up. `ok == false` means no trustworthy zero point was
 * found within the bounded retry window -- Dosing task must abort the
 * dose rather than start it from an unsettled (or stale) baseline; a
 * dose that never had a valid tare is not a valid run.
 */
struct TareResult {
  bool ok;
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
   * 1.0, not 0.0: ScaleTask forwards this straight into
   * ADS1232::setCalFactor (units = raw * calFactor), and the driver's
   * own constructor already defaults calFactor to 1.0 for exactly this
   * reason -- a fresh/uncalibrated scale should read raw counts
   * (visibly wrong, but a real number), not silently read 0.0g forever.
   *
   * double, not float: this gets multiplied against raw ADC deltas that
   * can run into the tens of thousands of counts, and a realistic
   * calibration factor for this hardware has ~6-7 significant digits of
   * its own (e.g. 0.000924583895...) -- float32's ~7 significant digits
   * total leaves it with essentially nothing to spare once combined with
   * that multiplication, silently rounding away real precision on every
   * write. double carries the extra headroom through storage (NVS),
   * the WS wire format, and the multiplication itself.
   */
  double calibration_factor = 1.0;

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
  /// A weight change past this, while SCREENSAVER is active, wakes the display --
  /// someone approaching/using the machine shouldn't have to press a button first.
  float screensaver_wake_weight_delta_g = 2.0f;

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
  SCREENSAVER_WAKE_WEIGHT_DELTA_G,
  BUTTON_MIN_HOLD_MS,
};

/** A single validated field write, sent from Network task to Settings task. */
struct SettingsWriteRequest {
  SettingsFieldId field_id;
  union {
    // double, not float -- calibration_factor needs the extra precision
    // (see SettingsSnapshot's own comment); every other field that reads
    // this member is unaffected, since a double carries any float value
    // through unchanged and narrows back losslessly for those.
    double f;
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
