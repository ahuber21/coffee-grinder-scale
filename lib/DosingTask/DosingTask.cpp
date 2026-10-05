#include "DosingTask.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include "DosingModel.h"
#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"

namespace {

/** Dosing task's own session FSM state -- reuses DisplayMode, since every FSM
 *  state maps to exactly one display layout (OTA_UPDATE is never assigned here). */
using DosingState = DisplayMode;

/** Sub-state within DosingState::TOPUP -- one pulse's decide/fire/settle cycle. */
enum class TopupPhase { DECIDING, PULSING, SETTLING };

/// A cup-lift swing this large in the delta weight dismisses the finished-dose screen.
constexpr float kCupLiftDeltaG = 3.0f;
/// Last-resort cap on any non-idle state, for gaps the per-state timeouts miss.
constexpr uint32_t kMaxStateDurationMs = 60000;
/// The topup loop as a whole gives up after this many topup_timeout_ms.
constexpr uint32_t kTopupLoopTimeoutFactor = 10;

// Session-scoped state, written by this task only.

DosingState g_state = DosingState::BOOT;
uint32_t g_display_seq = 0;
uint32_t g_next_session_id = 1;
uint32_t g_session_id = 0;

SettingsSnapshot g_settings;  ///< Latest snapshot seen from Settings task.
uint32_t g_settings_version_seen = 0;

ScaleSample g_last_sample{};
bool g_have_sample = false;

bool g_is_double = false;
bool g_session_discard_training = false;  ///< This session's model updates are thrown away at FINALIZE.
ButtonId g_pending_confirm_button = ButtonId::LEFT;

float g_target_grams = 0.0f;            ///< Full requested target.
float g_target_grams_corrected = 0.0f;  ///< Target minus topup margin -- the actual stop threshold.
float g_grams_on_grind_start = 0.0f;    ///< Software tare baseline for this session.
uint32_t g_grinder_started_ms = 0;
uint32_t g_grinder_stopped_ms = 0;
uint32_t g_frozen_elapsed_ms = 0;  ///< Elapsed time at FINALIZE entry -- the timer stops here.
float g_weight_at_relay_off = 0.0f;

uint32_t g_state_entered_ms = 0;

TopupPhase g_topup_phase = TopupPhase::DECIDING;
uint32_t g_topup_deciding_entered_ms = 0;  ///< DECIDING's stable-or-timeout timer; SETTLING uses g_state_entered_ms.
uint32_t g_topup_pulse_start_ms = 0;
uint32_t g_topup_pulse_duration_ms = 0;
float g_topup_weight_before_pulse = 0.0f;
float g_topup_gap_at_fire = 0.0f;           ///< Gap at DECIDING time, for recordPulse's bucket lookup.
int g_topup_bucket_at_fire = -1;            ///< This pulse's bucket, for TOPUP_PULSE telemetry.
double g_topup_aim_weight_at_fire_g = 0.0;  ///< This pulse's aim, for TOPUP_PULSE telemetry.

uint32_t g_tare_requested_after_seq = 0;  ///< sample_seq at TareRequest send.

bool g_finalize_done = false;
float g_finalize_latched_delta = 0.0f;  ///< Delta weight at the moment FINALIZE latched.

uint32_t g_last_activity_ms = 0;            ///< Last button press or IDLE entry -- drives the SCREENSAVER timer.
float g_screensaver_baseline_grams = 0.0f;  ///< Weight at SCREENSAVER entry, for the wake check.
uint32_t g_last_coffee_ms = 0;              ///< When the last session completed; 0 == none yet this boot.

// Models: hot state is task-local; the persisted copies are loaded at BOOT
// and written back at FINALIZE.
DosingModelState g_persisted_model;
MainGrindModel *g_main_grind_model = nullptr;
TopupModel *g_topup_model = nullptr;
CoastModel *g_coast_model = nullptr;

LandingLearnerState g_persisted_learner;
LandingLearner *g_landing_learner = nullptr;
float g_learner_applied_g = 0.0f;  ///< Stop-point shift applied at the latest GRINDING sample (g, positive = earlier).
bool g_learner_clamped = false;    ///< Whether that shift was cut short by the stop-time clamp.
double g_last_rate_g_s = 0.0;      ///< Blended grind rate at the latest GRINDING sample.
double g_stop_rate_g_s = 0.0;      ///< Grind rate captured at the stop instant.
GrindStopReason g_stop_reason = GrindStopReason::TIME_ESTIMATE;

/** Current time as seconds since epoch, for the models' recency decay. */
int64_t nowEpochS() { return static_cast<int64_t>(time(nullptr)); }

/** True in every state that counts as a session in progress. */
bool isSessionActive(DosingState s) {
  return s != DosingState::IDLE && s != DosingState::SCREENSAVER && s != DosingState::BOOT;
}

/** Sets/clears kDosingActiveBit -- true whenever a grind is in progress. */
void setDosingActiveBit() {
  if (isSessionActive(g_state)) {
    xEventGroupSetBits(g_sys_events, kDosingActiveBit);
  } else {
    xEventGroupClearBits(g_sys_events, kDosingActiveBit);
  }
}

/** Human-readable name for a DosingState, for diagnostic log lines. */
const char *dosingStateName(DosingState s) {
  switch (s) {
    case DosingState::BOOT: return "BOOT";
    case DosingState::IDLE: return "IDLE";
    case DosingState::CONFIRM: return "CONFIRM";
    case DosingState::TARE: return "TARE";
    case DosingState::GRINDING: return "GRINDING";
    case DosingState::TOPUP: return "TOPUP";
    case DosingState::STOPPING: return "STOPPING";
    case DosingState::FINALIZE: return "FINALIZE";
    case DosingState::SCREENSAVER: return "SCREENSAVER";
    case DosingState::DEBUG: return "DEBUG";
    case DosingState::OTA_UPDATE: return "OTA_UPDATE";
  }
  return "?";
}

/** Returns a telemetry event with the fields every session event carries already filled in. */
TelemetryEvent makeEvent(TelemetryType type) {
  TelemetryEvent ev{};
  ev.type = type;
  ev.session_id = g_session_id;
  ev.runtime_ms = g_grinder_started_ms ? (millis() - g_grinder_started_ms) : 0;
  ev.raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  ev.stable = g_have_sample ? g_last_sample.stable : false;
  return ev;
}

/** Zero-timeout send, so Dosing never blocks on Telemetry; a full queue drops the event. */
void sendEvent(const TelemetryEvent &ev) { xQueueSend(g_telemetry_q, &ev, 0); }

/**
 * printf-style diagnostic line over the WS "log" channel, so state
 * transitions and button presses can be watched without a serial cable.
 * Output is truncated to TelemetryEvent::log_line.
 */
void logLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char *fmt, ...) {
  TelemetryEvent ev = makeEvent(TelemetryType::LOG_LINE);
  va_list args;
  va_start(args, fmt);
  vsnprintf(ev.log_line, sizeof(ev.log_line), fmt, args);
  va_end(args);
  sendEvent(ev);
}

/** Moves the FSM to `next`, resetting the state-entry timer and active bit. */
void transitionTo(DosingState next) {
  bool state_changing = next != g_state;
  if (state_changing) {
    logLine("state %s -> %s", dosingStateName(g_state), dosingStateName(next));
  }
  if (next == DosingState::FINALIZE) {
    // Only the weight readout keeps moving on the finished-dose screen.
    g_frozen_elapsed_ms = g_grinder_started_ms ? millis() - g_grinder_started_ms : 0;
  }
  if (next == DosingState::TARE && state_changing) {
    /*
     * Every dose gets a fresh hardware re-tare, which lands on Scale
     * task's next tick at the earliest. Remembering the newest sample_seq
     * lets TARE require a sample produced after the request, not a stale
     * stable one that would latch the old baseline.
     */
    g_tare_requested_after_seq = g_have_sample ? g_last_sample.sample_seq : 0;
    TareRequest req{};
    xQueueSend(g_tare_request_q, &req, 0);
  }
  if (next == DosingState::SCREENSAVER) {
    g_screensaver_baseline_grams = g_have_sample ? g_last_sample.grams : 0.0f;
  }
  g_state = next;
  g_state_entered_ms = millis();
  if (next == DosingState::IDLE) {
    g_last_activity_ms = g_state_entered_ms;
  }
  setDosingActiveBit();
}

/** Sets the requested and margin-corrected targets for a dose of `base_grams`, by g_is_double's margin. */
void setSessionTarget(float base_grams) {
  float margin =
      g_is_double ? g_settings.top_up_margin_double : g_settings.top_up_margin_single;
  g_target_grams = base_grams;
  g_target_grams_corrected = base_grams - margin;
}

/** The configured button-dose target for g_is_double. */
float configuredTargetGrams() {
  return g_is_double ? g_settings.target_dose_double : g_settings.target_dose_single;
}

/** Refreshes g_settings from the mailbox if Settings task published a new version. */
void pollSettings() {
  SettingsSnapshot snap;
  if (xQueuePeek(g_settings_mailbox_dosing, &snap, 0) == pdTRUE &&
      snap.version != g_settings_version_seen) {
    g_settings = snap;
    g_settings_version_seen = snap.version;
  }
}

/** Publishes the current state/weight/target to Display task's mailbox. */
void sendDisplayCommand() {
  DisplayCommand cmd{};
  cmd.seq = g_display_seq++;
  cmd.mode = g_state;
  // Session screens show the delta from the tare baseline, so the cup's own
  // weight never leaks onto a layout sized for 0..target_grams.
  bool shows_session_delta = g_state == DosingState::GRINDING || g_state == DosingState::TOPUP ||
                             g_state == DosingState::STOPPING || g_state == DosingState::FINALIZE;
  cmd.current_grams =
      g_have_sample ? (shows_session_delta ? g_last_sample.grams - g_grams_on_grind_start
                                           : g_last_sample.grams)
                    : 0.0f;
  cmd.target_grams = g_target_grams;
  cmd.elapsed_s = g_state == DosingState::FINALIZE
                      ? g_frozen_elapsed_ms / 1000.0f
                      : (g_grinder_started_ms ? (millis() - g_grinder_started_ms) / 1000.0f : 0.0f);
  cmd.debug_raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  cmd.debug_stable = g_have_sample ? g_last_sample.stable : false;
  if (g_state == DosingState::SCREENSAVER) {
    // Time since the last completed coffee, or plain uptime before the first one.
    uint32_t elapsed_ms;
    if (g_last_coffee_ms != 0) {
      cmd.idle_is_uptime = false;
      elapsed_ms = millis() - g_last_coffee_ms;
    } else {
      cmd.idle_is_uptime = true;
      elapsed_ms = millis();
    }
    cmd.idle_h = elapsed_ms / 3600000UL;
    cmd.idle_m = (elapsed_ms / 60000UL) % 60;
    cmd.idle_s = (elapsed_ms / 1000UL) % 60;
    cmd.idle_ms = elapsed_ms % 1000UL;
  }
  xQueueOverwrite(g_display_mailbox, &cmd);
}

/** Pushes a plain session event (TARGET, FINALIZE, COMPLETE, ...) to Telemetry task. */
void sendTelemetry(TelemetryType type, float grams = 0, float target = 0, float delta = 0) {
  TelemetryEvent ev = makeEvent(type);
  ev.grams = grams;
  ev.target_grams = target;
  ev.delta_grams = delta;
  // Only the TARGET handler reads these two.
  ev.is_double = g_is_double;
  ev.target_grams_corrected = g_target_grams_corrected;
  sendEvent(ev);
}

/**
 * Sends PROGRESS telemetry: which stop condition fired and the model's
 * weight estimate at that instant, neither of which a session's final
 * weight can reveal afterwards.
 */
void sendProgressTelemetry(GrindStopReason reason, float delta_weight, double weight_estimate_g) {
  TelemetryEvent ev = makeEvent(TelemetryType::PROGRESS);
  ev.grams = delta_weight;
  ev.target_grams = g_target_grams;
  ev.stop_reason = reason;
  ev.weight_estimate_g = static_cast<float>(weight_estimate_g);
  sendEvent(ev);
}

/**
 * Sends TOPUP_PULSE telemetry with the pulse's decision inputs as they
 * were at fire time; TopupModel keeps retuning its per-bucket durations,
 * so they can't be recovered later.
 */
void sendTopupPulseTelemetry(float delta_weight, float weight_added, int bucket,
                             double aim_weight_g, uint32_t commanded_duration_ms) {
  TelemetryEvent ev = makeEvent(TelemetryType::TOPUP_PULSE);
  ev.grams = delta_weight;
  ev.target_grams = g_target_grams;
  ev.delta_grams = weight_added;
  ev.topup_bucket = static_cast<uint8_t>(bucket);
  ev.topup_aim_weight_g = static_cast<float>(aim_weight_g);
  ev.topup_commanded_duration_ms = commanded_duration_ms;
  sendEvent(ev);
}

/** Converts a precision (inverse variance) to a standard deviation, 0 if unset. */
float sdFromPrecision(float precision) {
  return precision > 0.0f ? 1.0f / sqrtf(precision) : 0.0f;
}

/** Sends the current persisted model's parameters, for the SPA's algorithm view. */
void sendModelStateTelemetry(const DosingModelState &model) {
  TelemetryEvent ev = makeEvent(TelemetryType::MODEL_STATE);
  ev.rate_hat_g_s = model.rate_hat;
  ev.rate_sd_g_s = sdFromPrecision(model.rate_precision);
  ev.coast_weight_g = model.coast_weight_hat;
  ev.coast_weight_sd_g = sdFromPrecision(model.coast_weight_precision);
  ev.rate_n_effective = model.rate_n_effective;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    ev.topup_lut_duration_ms[i] = model.topup_lut_duration_ms[i];
    ev.topup_lut_n[i] = model.topup_lut_n[i];
  }
  sendEvent(ev);
}

/** Energizes/de-energizes the grinder relay -- the sole writer of this pin. */
void grinderRelay(bool on) { digitalWrite(GRINDER_RELAY_PIN, on ? HIGH : LOW); }

/** True once the latest sample is stable, or `stability_max_wait_ms` has passed since g_state_entered_ms. */
bool stateSettledOrTimedOut() {
  return (g_have_sample && g_last_sample.stable) ||
         millis() - g_state_entered_ms >= g_settings.stability_max_wait_ms;
}

/** True once `s` is stable, or `stability_max_wait_ms` has passed since `since_ms`. */
bool sampleSettledOrTimedOut(const ScaleSample &s, uint32_t since_ms) {
  return s.stable || s.millis - since_ms >= g_settings.stability_max_wait_ms;
}

/** Builds the models from the persisted blobs, discarding any in-RAM state. */
void rebuildModels() {
  delete g_main_grind_model;
  delete g_topup_model;
  delete g_coast_model;
  delete g_landing_learner;
  g_main_grind_model = new MainGrindModel(g_persisted_model);
  g_topup_model = new TopupModel(g_persisted_model);
  g_coast_model = new CoastModel(g_persisted_model);
  g_landing_learner = new LandingLearner(g_persisted_learner);
}

/** Reads the persisted model blobs Settings task published at boot and builds the models. */
void loadModels() {
  if (xQueueReceive(g_dosing_model_mailbox, &g_persisted_model, portMAX_DELAY) != pdTRUE) {
    g_persisted_model = makeDefaultDosingModelState();
  }
  if (xQueueReceive(g_landing_learner_mailbox, &g_persisted_learner, portMAX_DELAY) != pdTRUE) {
    g_persisted_learner = makeDefaultLandingLearnerState();
  }
  // Put them back so any later reader can still peek them.
  xQueueOverwrite(g_dosing_model_mailbox, &g_persisted_model);
  xQueueOverwrite(g_landing_learner_mailbox, &g_persisted_learner);
  rebuildModels();
  sendModelStateTelemetry(g_persisted_model);
}

/** Hands the in-RAM models' state to Settings task for NVS, once per session. */
void persistModels() {
  DosingModelState blob = g_persisted_model;
  blob = g_main_grind_model->dumpPersisted(blob);
  blob = g_topup_model->dumpPersisted(blob);
  blob = g_coast_model->dumpPersisted(blob);
  g_persisted_model = blob;

  PersistRequest req{};
  req.blob_id = PersistBlobId::DOSING_MODEL;
  req.payload = blob;
  req.request_id = g_session_id;
  xQueueSend(g_persist_request_q, &req, 0);

  g_persisted_learner = g_landing_learner->dumpPersisted();
  PersistRequest learner_req{};
  learner_req.blob_id = PersistBlobId::LANDING_LEARNER;
  learner_req.learner_payload = g_persisted_learner;
  learner_req.request_id = g_session_id;
  xQueueSend(g_persist_request_q, &learner_req, 0);
}

/**
 * Records this session's settled main-grind landing: sends it to
 * telemetry and, when the settle was genuine and the time estimate
 * ended the grind, trains the landing learner on it. The training
 * target is the error the session would have had without the learner's
 * own shift. A settle that merely timed out is not a measurement.
 */
void recordLanding(double observed_coast_g, bool settled_stable) {
  float settled_delta = g_last_sample.grams - g_grams_on_grind_start;
  float landing_error_g = settled_delta - g_target_grams_corrected;

  bool trained = false;
  if (settled_stable && g_stop_reason == GrindStopReason::TIME_ESTIMATE) {
    g_landing_learner->setRate(g_settings.landing_learner_rate);
    trained = g_landing_learner->record(g_is_double, g_stop_rate_g_s,
                                        landing_error_g + g_learner_applied_g);
  }

  TelemetryEvent ev = makeEvent(TelemetryType::LANDING);
  ev.grams = settled_delta;
  ev.target_grams = g_target_grams;
  ev.landing_coast_g = static_cast<float>(observed_coast_g);
  ev.landing_margin_g = g_target_grams - g_target_grams_corrected;
  ev.landing_correction_g = g_learner_applied_g;
  ev.landing_clamped = g_learner_clamped;
  sendEvent(ev);

  logLine("LAND %s err=%+.3fg corr=%+.3fg%s n=%u sd=%.3fg%s", g_is_double ? "dbl" : "sgl",
       landing_error_g, g_learner_applied_g, g_learner_clamped ? " CLAMP" : "",
       static_cast<unsigned>(g_landing_learner->sessions(g_is_double)),
       g_landing_learner->residualSd(g_is_double), trained ? "" : " (not trained)");
}

/**
 * Returns the stop time with the landing learner's shift applied, never
 * further than the clamp from `unlearned_stop_ms`, and records the shift
 * actually applied.
 */
double learnedStopMs(double unlearned_stop_ms, double coast_estimate) {
  g_learner_applied_g = 0.0f;
  g_learner_clamped = false;
  if (!g_settings.landing_learner_enabled || !std::isfinite(unlearned_stop_ms) ||
      g_last_rate_g_s <= 0.0) {
    return unlearned_stop_ms;
  }
  double shift_g = g_landing_learner->predict(g_is_double, g_last_rate_g_s);
  double learned_ms = g_main_grind_model->predictStopTimeMsWithCoast(
      g_target_grams_corrected, coast_estimate + shift_g);
  double clamp_ms = g_settings.landing_learner_clamp_ms;
  double clamped_ms = std::max(unlearned_stop_ms - clamp_ms,
                               std::min(learned_ms, unlearned_stop_ms + clamp_ms));
  g_learner_clamped = clamped_ms != learned_ms;
  g_learner_applied_g =
      static_cast<float>((unlearned_stop_ms - clamped_ms) * g_last_rate_g_s / 1000.0);
  return clamped_ms;
}

/** Cuts the grinder and moves to STOPPING, recording which condition fired and the model's state. */
void stopGrinding(const ScaleSample &s, uint32_t runtime_ms, GrindStopReason reason) {
  grinderRelay(false);
  g_grinder_stopped_ms = s.millis;

  // Anything past TelemetryEvent::log_line is truncated, so keep this short.
  static const char *const kReasonNames[] = {"time", "raw_fallback", "timeout"};
  double weight_estimate = g_main_grind_model->currentWeightEstimate();
  logLine("GRIND stop:%s rt=%ums est=%.3fg tgt=%.3fg base=%.3fg",
       kReasonNames[static_cast<int>(reason)], static_cast<unsigned>(runtime_ms), weight_estimate,
       g_target_grams_corrected, g_grams_on_grind_start);

  // The plausibility-filtered estimate, not s.grams; this value trains CoastModel.
  g_weight_at_relay_off = g_grams_on_grind_start + weight_estimate;
  g_stop_reason = reason;
  g_stop_rate_g_s = g_last_rate_g_s;
  g_main_grind_model->finalizeSession(nowEpochS());
  sendProgressTelemetry(reason, static_cast<float>(s.grams - g_grams_on_grind_start),
                        weight_estimate);
  transitionTo(DosingState::STOPPING);
}

/** Per-sample GRINDING update: feeds the rate/coast models and checks every stop condition. */
void handleGrindingSample(const ScaleSample &s) {
  uint32_t runtime_ms = s.millis - g_grinder_started_ms;
  g_main_grind_model->addSample(runtime_ms, s.grams - g_grams_on_grind_start);

  // The model works in delta space, so the corrected target is used as-is.
  double coast_estimate = g_coast_model->currentCoastEstimate();
  double unlearned_stop_ms =
      g_main_grind_model->predictStopTimeMsWithCoast(g_target_grams_corrected, coast_estimate);
  g_last_rate_g_s = g_main_grind_model->currentRateEstimate();
  double predicted_stop_ms = learnedStopMs(unlearned_stop_ms, coast_estimate);

  // The fallback deliberately ignores s.stable: the relay is always on here.
  bool time_estimate_fired = runtime_ms >= predicted_stop_ms;
  bool raw_weight_fallback_fired =
      g_main_grind_model->currentWeightEstimate() >= g_target_grams_corrected;
  bool safety_timeout_fired = runtime_ms >= g_settings.grinding_timeout_ms;

  if (time_estimate_fired) {
    stopGrinding(s, runtime_ms, GrindStopReason::TIME_ESTIMATE);
  } else if (raw_weight_fallback_fired) {
    stopGrinding(s, runtime_ms, GrindStopReason::RAW_WEIGHT_FALLBACK);
  } else if (safety_timeout_fired) {
    stopGrinding(s, runtime_ms, GrindStopReason::SAFETY_TIMEOUT);
  }
}

/** Ends the topup loop and moves to FINALIZE, announcing the finalize to Telemetry. */
void finishTopup(float delta_weight) {
  sendTelemetry(TelemetryType::FINALIZE, delta_weight, g_target_grams);
  transitionTo(DosingState::FINALIZE);
}

/** Per-sample TOPUP update: drives the DECIDING/PULSING/SETTLING pulse cycle. */
void handleTopupSample(const ScaleSample &s) {
  float delta_weight = s.grams - g_grams_on_grind_start;
  float gap = g_target_grams - delta_weight;

  switch (g_topup_phase) {
    case TopupPhase::DECIDING: {
      // Decide only on a settled reading, bounded so a noisy sensor can't stall every decision.
      if (!sampleSettledOrTimedOut(s, g_topup_deciding_entered_ms)) {
        break;
      }
      if (gap <= g_settings.min_topup_grams) {
        logLine("TOPUP: gap=%.4fg <= min_topup_grams, finalizing without a pulse", gap);
        finishTopup(delta_weight);
        return;
      }
      TopupModel::Decision decision = g_topup_model->computeTopupDecision(gap);
      if (!decision.should_fire) {
        // Safety net: every bucket normally has a clamped, in-bounds duration.
        finishTopup(delta_weight);
        return;
      }
      g_topup_gap_at_fire = gap;
      g_topup_bucket_at_fire = decision.bucket;
      g_topup_aim_weight_at_fire_g = decision.aim_weight_g;
      g_topup_weight_before_pulse = s.grams;
      g_topup_pulse_start_ms = s.millis;
      g_topup_pulse_duration_ms = decision.duration_ms;
      grinderRelay(true);
      g_topup_phase = TopupPhase::PULSING;
      break;
    }
    case TopupPhase::PULSING: {
      if (s.millis - g_topup_pulse_start_ms >= g_topup_pulse_duration_ms) {
        grinderRelay(false);
        g_topup_phase = TopupPhase::SETTLING;
        g_state_entered_ms = s.millis;  // Doubles as the settle-wait start.
      }
      break;
    }
    case TopupPhase::SETTLING: {
      // The min wait lets the pulse's bounce damp out before a lucky quiet window can pass as stable.
      bool min_wait_elapsed = s.millis - g_state_entered_ms >= g_settings.stability_min_wait_ms;
      if (min_wait_elapsed && sampleSettledOrTimedOut(s, g_state_entered_ms)) {
        double weight_added = s.grams - g_topup_weight_before_pulse;
        g_topup_model->recordPulse(g_topup_gap_at_fire, weight_added);
        sendTopupPulseTelemetry(s.grams - g_grams_on_grind_start, static_cast<float>(weight_added),
                                g_topup_bucket_at_fire, g_topup_aim_weight_at_fire_g,
                                g_topup_pulse_duration_ms);
        g_topup_phase = TopupPhase::DECIDING;
        g_topup_deciding_entered_ms = s.millis;
      }
      break;
    }
  }

  // Overall safety cutoff so a misbehaving topup loop can't run forever.
  if (millis() - g_grinder_stopped_ms >= g_settings.topup_timeout_ms * kTopupLoopTimeoutFactor) {
    grinderRelay(false);
    transitionTo(DosingState::FINALIZE);
  }
}

/** Wakes from SCREENSAVER on a settled weight change, e.g. a cup placed or removed. */
void wakeScreensaverOnWeightChange(const ScaleSample &s) {
  // Needs a settled reading so a passing vibration can't wake it; a button press wakes it regardless.
  float delta = fabsf(s.grams - g_screensaver_baseline_grams);
  if (s.stable && delta > g_settings.screensaver_wake_weight_delta_g) {
    transitionTo(DosingState::IDLE);
  }
}

/** Drains every pending scale sample, so Dosing always has a current weight; true if any arrived. */
bool drainScaleSamples() {
  ScaleSample sample;
  bool got_sample = false;
  while (xQueueReceive(g_scale_sample_q, &sample, 0) == pdTRUE) {
    g_last_sample = sample;
    g_have_sample = true;
    got_sample = true;

    if (g_state == DosingState::GRINDING) {
      handleGrindingSample(sample);
    } else if (g_state == DosingState::TOPUP) {
      handleTopupSample(sample);
    } else if (g_state == DosingState::SCREENSAVER) {
      wakeScreensaverOnWeightChange(sample);
    }
  }
  return got_sample;
}

/** Starts a manual/API dose; only actionable while idle. */
void handleDoseRequests() {
  DoseRequest dose;
  while (xQueueReceive(g_dose_request_q, &dose, 0) == pdTRUE) {
    if (g_state == DosingState::IDLE && dose.requested_grams > 0.0f &&
        dose.requested_grams <= kMaxDoseGrams) {
      g_is_double = false;
      g_session_discard_training = dose.discard_training;
      setSessionTarget(dose.requested_grams);
      transitionTo(DosingState::TARE);
    }
  }
}

/**
 * Handles Scale task's reply to the re-tare TARE's entry requested. A
 * failure already exhausted its retries inside Scale task, so the dose
 * is aborted rather than started from an unproven baseline.
 */
void handleTareResults() {
  TareResult tare_result;
  while (xQueueReceive(g_tare_result_q, &tare_result, 0) == pdTRUE) {
    if (g_state == DosingState::TARE && !tare_result.ok) {
      logLine("pre-dose tare never stabilized -- aborting dose");
      transitionTo(DosingState::IDLE);
    }
  }
}

/** Interprets one debounced button press according to the current state. */
void handleButtonPress(const ButtonPress &press) {
  g_last_activity_ms = millis();
  logLine("button %d received in state %s", static_cast<int>(press.button),
       dosingStateName(g_state));
  switch (g_state) {
    case DosingState::IDLE:
    case DosingState::SCREENSAVER:
      if (press.button == ButtonId::LEFT || press.button == ButtonId::RIGHT) {
        g_is_double = press.button == ButtonId::RIGHT;
        g_pending_confirm_button = press.button;
        // Preview the real target now so CONFIRM never shows a stale value.
        setSessionTarget(configuredTargetGrams());
        transitionTo(DosingState::CONFIRM);
      } else if (g_state == DosingState::SCREENSAVER) {
        transitionTo(DosingState::IDLE);
      }
      break;
    case DosingState::CONFIRM:
      if (press.button == g_pending_confirm_button) {
        g_session_discard_training = false;  // A real physical dose always trains the models.
        setSessionTarget(configuredTargetGrams());
        transitionTo(DosingState::TARE);
      } else {
        transitionTo(DosingState::IDLE);
      }
      break;
    case DosingState::GRINDING:
    case DosingState::TOPUP:
      if (press.button == ButtonId::BACK) {
        // Stop at once regardless of sub-phase; a cancel never queues behind anything.
        grinderRelay(false);
        transitionTo(DosingState::FINALIZE);
      }
      break;
    default:
      break;
  }
}

/** TARE: latches the baseline on a settled reading and starts the grind. */
void updateTare() {
  /*
   * The sample must postdate the re-tare request (wraparound-safe signed
   * compare), or a cup already sitting stable would latch a baseline from
   * before the hardware re-tare landed.
   */
  bool sample_postdates_retare =
      g_have_sample &&
      static_cast<int32_t>(g_last_sample.sample_seq - g_tare_requested_after_seq) > 0;
  if (!sample_postdates_retare || !g_last_sample.stable) {
    return;
  }
  g_grams_on_grind_start = g_last_sample.grams;
  logLine("TARE baseline latched: %.4fg (raw_adc=%ld)", g_grams_on_grind_start,
       static_cast<long>(g_last_sample.raw_adc));
  g_session_id = g_next_session_id++;
  g_main_grind_model->startSession();
  g_grinder_started_ms = millis();
  grinderRelay(true);
  // Delta from the baseline is 0 by definition here; the baseline itself is never "current weight".
  sendTelemetry(TelemetryType::TARGET, 0.0f, g_target_grams);
  // Sent after TARGET: the session row this references is only created on that event.
  sendTelemetry(TelemetryType::TARE_DEBUG, g_grams_on_grind_start);
  transitionTo(DosingState::GRINDING);
}

/** STOPPING: waits for the post-grind coast to settle, then records it and starts topup. */
void updateStopping() {
  // Bounded wait: the coast trains the stop-time model, so a transient reading would bias it.
  if (!stateSettledOrTimedOut() || !g_have_sample) {
    return;
  }
  double observed_coast = g_last_sample.grams - g_weight_at_relay_off;
  g_coast_model->recordCoast(observed_coast, nowEpochS());
  recordLanding(observed_coast, g_last_sample.stable);
  g_topup_phase = TopupPhase::DECIDING;
  g_topup_deciding_entered_ms = millis();
  transitionTo(DosingState::TOPUP);
}

/** Latches the final weight, then keeps or discards the session's model updates. */
void latchFinalWeight() {
  if (g_session_discard_training) {
    // Rebuilding from the untouched blobs drops every in-RAM mutation, not just the NVS write.
    rebuildModels();
    logLine("FINALIZE: discard_training set, model left untouched");
  } else {
    persistModels();
  }
  sendModelStateTelemetry(g_persisted_model);

  // Delta, not the absolute reading, so it is comparable to the requested weight.
  g_finalize_latched_delta =
      g_have_sample ? g_last_sample.grams - g_grams_on_grind_start : 0;
  logLine("FINALIZE latch: sample=%.4fg baseline=%.4fg delta=%.4fg",
       g_have_sample ? g_last_sample.grams : 0.0f, g_grams_on_grind_start,
       g_finalize_latched_delta);
  sendTelemetry(TelemetryType::COMPLETE, g_finalize_latched_delta, g_target_grams);
  g_last_coffee_ms = millis();
  g_finalize_done = true;
}

/** FINALIZE: latches the result once settled, then dismisses on a cup lift or the screen timeout. */
void updateFinalize() {
  if (!g_finalize_done) {
    if (!stateSettledOrTimedOut()) {
      return;
    }
    // The screen timeout counts from the latch, so the result shows its full duration.
    g_state_entered_ms = millis();
    latchFinalWeight();
  }

  /*
   * Lifting the cup swings the delta by about the cup's weight, which the
   * fixed-width layout can't show, so a swing this large dismisses to
   * IDLE. It needs a settled reading, as a mid-lift transient must not
   * trigger it; finalize_timeout_ms is the backstop if it never settles.
   */
  bool cup_lifted =
      g_have_sample && g_last_sample.stable &&
      fabsf((g_last_sample.grams - g_grams_on_grind_start) - g_finalize_latched_delta) >=
          kCupLiftDeltaG;
  if (cup_lifted || millis() - g_state_entered_ms >= g_settings.finalize_timeout_ms) {
    g_finalize_done = false;
    transitionTo(DosingState::IDLE);
  }
}

/** State transitions driven by time or settled readings rather than by a sample or button. */
void updateTimedStates() {
  switch (g_state) {
    case DosingState::IDLE:
      if (millis() - g_last_activity_ms >= g_settings.screensaver_timeout_s * 1000UL) {
        transitionTo(DosingState::SCREENSAVER);
      }
      break;
    case DosingState::CONFIRM:
      // Lapses to IDLE if nobody confirms or cancels in time.
      if (millis() - g_state_entered_ms >= g_settings.confirm_timeout_ms) {
        transitionTo(DosingState::IDLE);
      }
      break;
    case DosingState::TARE:
      updateTare();
      break;
    case DosingState::STOPPING:
      updateStopping();
      break;
    case DosingState::FINALIZE:
      updateFinalize();
      break;
    default:
      break;
  }
}

/**
 * Last-resort net for any non-idle state without its own timeout (e.g.
 * TARE waiting on a scale that died). Without it the device would stay
 * stuck, and OTA blocked, until a power cycle.
 */
void enforceStateBackstop() {
  if (isSessionActive(g_state) && millis() - g_state_entered_ms >= kMaxStateDurationMs) {
    logLine("backstop: forcing IDLE from %s after %lums", dosingStateName(g_state),
         static_cast<unsigned long>(kMaxStateDurationMs));
    grinderRelay(false);
    g_finalize_done = false;
    transitionTo(DosingState::IDLE);
  }
}

/** Dosing task entry point: the session FSM's whole lifetime lives in this loop. */
void dosingTaskFn(void *) {
  pinMode(GRINDER_RELAY_PIN, OUTPUT);
  grinderRelay(false);

  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE, portMAX_DELAY);
  pollSettings();
  loadModels();

  transitionTo(DosingState::IDLE);
  sendDisplayCommand();

  for (;;) {
    pollSettings();
    bool got_sample = drainScaleSamples();
    handleDoseRequests();
    handleTareResults();

    ButtonPress press;
    while (xQueueReceive(g_button_press_q, &press, 0) == pdTRUE) {
      handleButtonPress(press);
    }

    updateTimedStates();
    enforceStateBackstop();

    // Network task owns the display mailbox during an OTA flash; writing here too would race it.
    bool ota_in_progress = (xEventGroupGetBits(g_sys_events) & kOtaInProgressBit) != 0;
    if (!ota_in_progress && (got_sample || g_state != DosingState::GRINDING)) {
      sendDisplayCommand();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void createDosingTask() {
  xTaskCreatePinnedToCore(dosingTaskFn, "Dosing", TaskConfig::kDosingStackBytes, nullptr,
                          TaskConfig::kDosingPriority, nullptr, TaskConfig::kDosingCore);
}
