#include "DosingTask.h"

#include <Arduino.h>
#include <cmath>
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

// Session-scoped state (this task's alone -- single-writer rule).

DosingState g_state = DosingState::BOOT;
uint32_t g_display_seq = 0;
uint32_t g_next_session_id = 1;
uint32_t g_session_id = 0;

SettingsSnapshot g_settings;  // latest snapshot seen from Settings task
uint32_t g_settings_version_seen = 0;

ScaleSample g_last_sample{};
bool g_have_sample = false;

bool g_is_double = false;
bool g_session_discard_training = false;  ///< This session's model updates never reach NVS/in-RAM models.
ButtonId g_pending_confirm_button = ButtonId::LEFT;

float g_target_grams = 0.0f;  ///< Full requested target.
float g_target_grams_corrected = 0.0f;  ///< Target minus topup margin -- the actual stop threshold.
float g_grams_on_grind_start = 0.0f;    ///< Software tare baseline for this session.
uint32_t g_grinder_started_ms = 0;
uint32_t g_grinder_stopped_ms = 0;
uint32_t g_frozen_elapsed_ms = 0;  ///< Elapsed time at FINALIZE entry -- the timer stops here.
float g_weight_at_relay_off = 0.0f;

uint32_t g_state_entered_ms = 0;

/** Sub-state within DosingState::TOPUP -- one pulse's decide/fire/settle cycle. */
enum class TopupPhase { DECIDING, PULSING, SETTLING };
TopupPhase g_topup_phase = TopupPhase::DECIDING;
uint32_t g_topup_deciding_entered_ms = 0;  ///< DECIDING's own stable-or-timeout timer -- SETTLING reuses g_state_entered_ms for its own.
uint32_t g_topup_pulse_start_ms = 0;
uint32_t g_topup_pulse_duration_ms = 0;
float g_topup_weight_before_pulse = 0.0f;
float g_topup_gap_at_fire = 0.0f;  ///< Gap at DECIDING time, for recordPulse's bucket lookup.
int g_topup_bucket_at_fire = -1;          ///< This pulse's bucket, for TOPUP_PULSE telemetry.
double g_topup_aim_weight_at_fire_g = 0.0;  ///< This pulse's aim, for TOPUP_PULSE telemetry.

uint32_t g_tare_requested_after_seq = 0;  ///< sample_seq at TareRequest send -- see TARE's own wait.

bool g_finalize_done = false;
float g_finalize_latched_delta = 0.0f;  ///< Delta weight at the moment FINALIZE latched -- see the cup-lift dismiss check.

uint32_t g_last_activity_ms = 0;  ///< Last button press or IDLE entry -- drives the SCREENSAVER timer.
float g_screensaver_baseline_grams = 0.0f;  ///< Weight at SCREENSAVER entry, for the wake check.
uint32_t g_last_coffee_ms = 0;    ///< When the last session completed; 0 == none yet this boot.

// DosingModel: hot state is task-local, updated per-sample while GRINDING/TOPUP.
// Persisted copy loaded from Settings at BOOT, written back when session finishes.

TopupModelV1 g_persisted_model;
MainGrindModel *g_main_grind_model = nullptr;
TopupModel *g_topup_model = nullptr;
CoastModel *g_coast_model = nullptr;

/** Current time as seconds since epoch, for the models' recency decay. */
int64_t nowEpochS() { return static_cast<int64_t>(time(nullptr)); }

/** Sets/clears kDosingActiveBit -- true whenever a grind is in progress. */
void setDosingActiveBit() {
  bool active = g_state != DosingState::IDLE && g_state != DosingState::SCREENSAVER &&
                g_state != DosingState::BOOT;
  if (active) {
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

/**
 * Sends a free-text diagnostic line over the WS "log" channel (see
 * NetworkTask.cpp's buildTelemetryJson and the SPA's Live page) --
 * lets state transitions and button presses be watched remotely
 * without a serial cable.
 */
void sendLog(const char *line) {
  TelemetryEvent ev{};
  ev.type = TelemetryType::LOG_LINE;
  ev.session_id = g_session_id;
  ev.runtime_ms = g_grinder_started_ms ? (millis() - g_grinder_started_ms) : 0;
  strncpy(ev.log_line, line, sizeof(ev.log_line) - 1);
  ev.log_line[sizeof(ev.log_line) - 1] = '\0';
  xQueueSend(g_telemetry_q, &ev, 0);
}

/** Moves the FSM to `next`, resetting the state-entry timer and active bit. */
void transitionTo(DosingState next) {
  bool state_changing = next != g_state;
  if (state_changing) {
    char line[96];
    snprintf(line, sizeof(line), "state %s -> %s", dosingStateName(g_state),
              dosingStateName(next));
    sendLog(line);
  }
  if (next == DosingState::FINALIZE) {
    // Freezes the displayed elapsed time once dispensing is actually
    // done -- only the weight readout should keep moving on this screen.
    g_frozen_elapsed_ms = g_grinder_started_ms ? millis() - g_grinder_started_ms : 0;
  }
  if (next == DosingState::TARE && state_changing) {
    // Every dose gets a fresh hardware re-tare, not just whatever the
    // idle auto-tare last happened to leave behind (which can be
    // several seconds stale). The retare itself lands on Scale task's
    // next tick at the earliest -- recording the newest sample_seq
    // already seen lets TARE's own wait below require a sample produced
    // after this request, not a stale one already sitting in
    // g_last_sample from before the retare (which would otherwise still
    // pass the stability check and silently latch the old baseline).
    g_tare_requested_after_seq = g_have_sample ? g_last_sample.sample_seq : 0;
    TareRequest req{};
    xQueueSend(g_tare_request_q, &req, 0);
  }
  if (next == DosingState::SCREENSAVER) {
    // Reference point for the wake-on-weight-change check below.
    g_screensaver_baseline_grams = g_have_sample ? g_last_sample.grams : 0.0f;
  }
  g_state = next;
  g_state_entered_ms = millis();
  if (next == DosingState::IDLE) {
    g_last_activity_ms = g_state_entered_ms;
  }
  setDosingActiveBit();
}

/** Applies the topup-margin correction to a requested dose -- one function for
 *  both the button path and the API path, so there is no second correction constant. */
void computeCorrectedTarget(float base_grams, bool is_double, float &target,
                             float &target_corrected) {
  float margin = is_double ? g_settings.top_up_margin_double
                            : g_settings.top_up_margin_single;
  target = base_grams;
  target_corrected = base_grams - margin;
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
  // GRINDING/TOPUP/STOPPING/FINALIZE show progress toward a dose, so the
  // cup's own weight (whatever the tare baseline happened to capture) must
  // never leak onto the screen -- otherwise a heavier cup shows up as a
  // three-digit reading instead of the 0..target_grams the layout expects.
  bool session_active = g_state == DosingState::GRINDING || g_state == DosingState::TOPUP ||
                        g_state == DosingState::STOPPING || g_state == DosingState::FINALIZE;
  cmd.current_grams =
      g_have_sample ? (session_active ? g_last_sample.grams - g_grams_on_grind_start
                                       : g_last_sample.grams)
                    : 0.0f;
  cmd.target_grams = g_target_grams;
  cmd.elapsed_s = g_state == DosingState::FINALIZE
                      ? g_frozen_elapsed_ms / 1000.0f
                      : (g_grinder_started_ms ? (millis() - g_grinder_started_ms) / 1000.0f : 0.0f);
  cmd.debug_raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  cmd.debug_stable = g_have_sample ? g_last_sample.stable : false;
  if (g_state == DosingState::SCREENSAVER) {
    // Shows time since the last completed coffee once there's been one
    // this boot; falls back to plain uptime before that.
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

/** Pushes one TelemetryEvent of `type` to Telemetry task's queue. */
void sendTelemetry(TelemetryType type, float grams = 0, float target = 0,
                    float delta = 0) {
  TelemetryEvent ev{};
  ev.type = type;
  ev.session_id = g_session_id;
  ev.runtime_ms = g_grinder_started_ms ? (millis() - g_grinder_started_ms) : 0;
  ev.grams = grams;
  ev.target_grams = target;
  ev.delta_grams = delta;
  ev.raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  ev.stable = g_have_sample ? g_last_sample.stable : false;
  // TARGET-only fields; harmless to set unconditionally since only the
  // TARGET handler on the receiving end reads them.
  ev.is_double = g_is_double;
  ev.target_grams_corrected = g_target_grams_corrected;
  // Zero-timeout send, drop-and-count on full -- Dosing never blocks on
  // Telemetry.
  xQueueSend(g_telemetry_q, &ev, 0);
}

/**
 * Sends PROGRESS telemetry with the two fields no other event carries:
 * which GRINDING stop condition fired, and the model's own weight
 * estimate at that instant -- see AR-074, the post-mortem gap a session's
 * final_weight_g alone can never fill in after the fact.
 */
void sendProgressTelemetry(GrindStopReason reason, float delta_weight, double weight_estimate_g) {
  TelemetryEvent ev{};
  ev.type = TelemetryType::PROGRESS;
  ev.session_id = g_session_id;
  ev.runtime_ms = g_grinder_started_ms ? (millis() - g_grinder_started_ms) : 0;
  ev.grams = delta_weight;
  ev.target_grams = g_target_grams;
  ev.raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  ev.stable = g_have_sample ? g_last_sample.stable : false;
  ev.stop_reason = reason;
  ev.weight_estimate_g = static_cast<float>(weight_estimate_g);
  xQueueSend(g_telemetry_q, &ev, 0);
}

/**
 * Sends TOPUP_PULSE telemetry with this pulse's own decision inputs,
 * captured at fire time -- see AR-074. topup_commanded_duration_ms
 * especially can't be recovered later: TopupModel keeps retuning each
 * bucket's duration from every pulse that lands in it.
 */
void sendTopupPulseTelemetry(float delta_weight, float weight_added, int bucket,
                              double aim_weight_g, uint32_t commanded_duration_ms) {
  TelemetryEvent ev{};
  ev.type = TelemetryType::TOPUP_PULSE;
  ev.session_id = g_session_id;
  ev.runtime_ms = g_grinder_started_ms ? (millis() - g_grinder_started_ms) : 0;
  ev.grams = delta_weight;
  ev.target_grams = g_target_grams;
  ev.delta_grams = weight_added;
  ev.raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  ev.stable = g_have_sample ? g_last_sample.stable : false;
  ev.topup_bucket = static_cast<uint8_t>(bucket);
  ev.topup_aim_weight_g = static_cast<float>(aim_weight_g);
  ev.topup_commanded_duration_ms = commanded_duration_ms;
  xQueueSend(g_telemetry_q, &ev, 0);
}

/** Converts a precision (inverse variance) to a standard deviation, 0 if unset. */
float sdFromPrecision(float precision) {
  return precision > 0.0f ? 1.0f / sqrtf(precision) : 0.0f;
}

/** Sends the current persisted model's parameters, for the SPA's algorithm view. */
void sendModelStateTelemetry(const TopupModelV1 &model) {
  TelemetryEvent ev{};
  ev.type = TelemetryType::MODEL_STATE;
  ev.session_id = g_session_id;
  ev.rate_hat_g_s = model.rate_hat;
  ev.rate_sd_g_s = sdFromPrecision(model.rate_precision);
  ev.coast_weight_g = model.coast_weight_hat;
  ev.coast_weight_sd_g = sdFromPrecision(model.coast_weight_precision);
  ev.rate_n_effective = model.rate_n_effective;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    ev.topup_lut_duration_ms[i] = model.topup_lut_duration_ms[i];
    ev.topup_lut_n[i] = model.topup_lut_n[i];
  }
  xQueueSend(g_telemetry_q, &ev, 0);
}

/** Energizes/de-energizes the grinder relay -- the sole writer of this pin. */
void grinderRelay(bool on) { digitalWrite(GRINDER_RELAY_PIN, on ? HIGH : LOW); }

/** Per-sample GRINDING update: feeds the rate/coast models and checks every stop condition. */
void handleGrindingSample(const ScaleSample &s) {
  uint32_t runtime_ms = s.millis - g_grinder_started_ms;
  double delta_weight = s.grams - g_grams_on_grind_start;
  g_main_grind_model->addSample(runtime_ms, delta_weight);

  // addSample is fed delta_weight (relative to baseline), so the target
  // must also be in delta space (g_target_grams_corrected, not subtracted again).
  double coast_estimate = g_coast_model->currentCoastEstimate();
  double predicted_stop_ms = g_main_grind_model->predictStopTimeMsWithCoast(
      g_target_grams_corrected, coast_estimate);

  // Both primary and fallback stops use the same canonical target value;
  // currentWeightEstimate() is already filtered by addSample's plausibility check.
  // Fallback deliberately does NOT gate on s.stable (relay is always on here).
  bool time_estimate_fired = runtime_ms >= predicted_stop_ms;
  bool raw_weight_fallback_fired =
      g_main_grind_model->currentWeightEstimate() >= g_target_grams_corrected;
  bool safety_timeout_fired = runtime_ms >= g_settings.grinding_timeout_ms;

  if (time_estimate_fired || raw_weight_fallback_fired || safety_timeout_fired) {
    grinderRelay(false);
    g_grinder_stopped_ms = s.millis;
    {
      // AR-073 diagnostic: which condition fired, and the model's state at
      // that instant -- currently the only place this is ever visible.
      // Sized to fit TelemetryEvent::log_line[96] -- sendLog() silently
      // truncates anything longer, so this must stay short, not just fit
      // this one call's worst case.
      char line[96];
      const char *reason = time_estimate_fired  ? "time"
                            : raw_weight_fallback_fired ? "raw_fallback"
                                                          : "timeout";
      snprintf(line, sizeof(line), "GRIND stop:%s rt=%ums est=%.3fg tgt=%.3fg base=%.3fg",
                reason, static_cast<unsigned>(runtime_ms),
                g_main_grind_model->currentWeightEstimate(), g_target_grams_corrected,
                g_grams_on_grind_start);
      sendLog(line);
    }
    // currentWeightEstimate() (converted back to absolute), not s.grams --
    // same plausibility protection as the fallback check above; this value trains CoastModel.
    g_weight_at_relay_off = g_grams_on_grind_start + g_main_grind_model->currentWeightEstimate();
    GrindStopReason stop_reason = time_estimate_fired  ? GrindStopReason::TIME_ESTIMATE
                                   : raw_weight_fallback_fired ? GrindStopReason::RAW_WEIGHT_FALLBACK
                                                                 : GrindStopReason::SAFETY_TIMEOUT;
    double weight_estimate = g_main_grind_model->currentWeightEstimate();
    g_main_grind_model->finalizeSession(nowEpochS());
    sendProgressTelemetry(stop_reason, static_cast<float>(delta_weight), weight_estimate);
    transitionTo(DosingState::STOPPING);
  }
}

/** Per-sample TOPUP update: drives the DECIDING/PULSING/SETTLING pulse cycle. */
void handleTopupSample(const ScaleSample &s) {
  float delta_weight = s.grams - g_grams_on_grind_start;
  float gap = g_target_grams - delta_weight;

  switch (g_topup_phase) {
    case TopupPhase::DECIDING: {
      // Every decision here needs a settled reading -- a mid-transient
      // sample could fire a pulse (or declare the dose done) on a value
      // that's about to move. Bounded so a persistently noisy sensor
      // can't stall every decision until the overall topup cutoff fires.
      bool settled = s.stable ||
                    s.millis - g_topup_deciding_entered_ms >= g_settings.stability_max_wait_ms;
      if (!settled) {
        break;
      }
      if (gap <= g_settings.min_topup_grams) {
        // AR-073 diagnostic: distinguishes "closed the gap" from
        // "declared close enough without ever firing a pulse".
        char line[96];
        snprintf(line, sizeof(line), "TOPUP: gap=%.4fg <= min_topup_grams, finalizing without a pulse",
                  gap);
        sendLog(line);
        sendTelemetry(TelemetryType::FINALIZE, delta_weight, g_target_grams);
        transitionTo(DosingState::FINALIZE);
        return;
      }
      TopupModel::Decision decision = g_topup_model->computeTopupDecision(gap);
      if (!decision.should_fire) {
        // Shouldn't normally happen (every bucket always has a clamped,
        // in-bounds duration) -- a safety net, not the everyday path.
        sendTelemetry(TelemetryType::FINALIZE, delta_weight, g_target_grams);
        transitionTo(DosingState::FINALIZE);
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
        g_state_entered_ms = s.millis;  // reused as "settle wait started" timer
      }
      break;
    }
    case TopupPhase::SETTLING: {
      // stability_min_wait_ms gives the pulse's mechanical bounce a floor
      // to damp out before the ADC's own stability check is trusted at
      // all -- otherwise a lucky quiet window mid-oscillation could pass
      // as "stable" far too early. stability_max_wait_ms is the fallback
      // so a sensor that never reports stable can't stall the model.
      bool minWaitElapsed = s.millis - g_state_entered_ms >= g_settings.stability_min_wait_ms;
      bool settled = s.stable || s.millis - g_state_entered_ms >= g_settings.stability_max_wait_ms;
      if (minWaitElapsed && settled) {
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
  if (millis() - g_grinder_stopped_ms >= g_settings.topup_timeout_ms * 10) {
    grinderRelay(false);
    transitionTo(DosingState::FINALIZE);
  }
}

/** Dosing task entry point: the session FSM's whole lifetime lives in this loop. */
void dosingTaskFn(void *) {
  pinMode(GRINDER_RELAY_PIN, OUTPUT);
  grinderRelay(false);

  // Wait for the full boot gate (settings loaded, scale ready, display
  // ready) before leaving BOOT.
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE,
                       portMAX_DELAY);
  pollSettings();

  // Seed the in-RAM models from Settings task's cold snapshot, read
  // exactly once here -- never read again mid-session.
  if (xQueueReceive(g_topup_model_mailbox, &g_persisted_model, portMAX_DELAY) != pdTRUE) {
    g_persisted_model = makeDefaultTopupModel();
  }
  // Restore the model mailbox so any future reader can peek it without
  // needing to check g_persisted_model directly.
  xQueueOverwrite(g_topup_model_mailbox, &g_persisted_model);

  g_main_grind_model = new MainGrindModel(g_persisted_model);
  g_topup_model = new TopupModel(g_persisted_model);
  g_coast_model = new CoastModel(g_persisted_model);
  sendModelStateTelemetry(g_persisted_model);

  transitionTo(DosingState::IDLE);
  sendDisplayCommand();

  for (;;) {
    pollSettings();

    // Drain every pending scale sample every tick, regardless of FSM
    // state -- Dosing always has a current weight for every display mode.
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
        // Someone approaching/using the machine (placing or removing a
        // cup, dosing manually) shouldn't have to press a button first
        // to wake the display. Requires a settled reading -- an
        // in-progress placement/removal reads unstable while it's
        // actually happening, and a bare threshold on an unstable
        // sample risks waking on a passing vibration rather than a
        // real, settled weight change. No bounded fallback needed here
        // (unlike a dosing decision): any button press independently
        // dismisses SCREENSAVER regardless of the scale.
        float delta = fabsf(sample.grams - g_screensaver_baseline_grams);
        if (sample.stable && delta > g_settings.screensaver_wake_weight_delta_g) {
          transitionTo(DosingState::IDLE);
        }
      }
    }

    // A manual/API dose request, only actionable while idle.
    DoseRequest dose;
    while (xQueueReceive(g_dose_request_q, &dose, 0) == pdTRUE) {
      if (g_state == DosingState::IDLE && dose.requested_grams > 0.0f &&
          dose.requested_grams <= 50.0f) {
        g_is_double = false;
        g_session_discard_training = dose.discard_training;
        computeCorrectedTarget(dose.requested_grams, g_is_double, g_target_grams,
                               g_target_grams_corrected);
        transitionTo(DosingState::TARE);
      }
    }

    // Scale task's reply to the re-tare request TARE's entry sent --
    // only meaningful while still waiting in TARE for it; a reply
    // arriving after that (state already moved on some other way) is
    // simply discarded. A failed tare is not a recoverable condition to
    // retry from here -- it already exhausted its own bounded retry
    // inside Scale task -- so this aborts the dose outright rather than
    // starting it from an unproven baseline.
    TareResult tare_result;
    while (xQueueReceive(g_tare_result_q, &tare_result, 0) == pdTRUE) {
      if (g_state == DosingState::TARE && !tare_result.ok) {
        sendLog("pre-dose tare never stabilized -- aborting dose");
        transitionTo(DosingState::IDLE);
      }
    }

    // Debounced button presses, interpreted according to Dosing's own
    // current state -- e.g. CONFIRM's same-button-confirms /
    // different-button-cancels logic lives here.
    ButtonPress press;
    while (xQueueReceive(g_button_press_q, &press, 0) == pdTRUE) {
      g_last_activity_ms = millis();
      {
        char line[96];
        snprintf(line, sizeof(line), "button %d received in state %s",
                  static_cast<int>(press.button), dosingStateName(g_state));
        sendLog(line);
      }
      switch (g_state) {
        case DosingState::IDLE:
        case DosingState::SCREENSAVER:
          if (press.button == ButtonId::LEFT || press.button == ButtonId::RIGHT) {
            g_is_double = press.button == ButtonId::RIGHT;
            g_pending_confirm_button = press.button;
            // Preview the real target now, not whatever g_target_grams
            // held before this press, so CONFIRM never shows a stale value.
            float base = g_is_double ? g_settings.target_dose_double
                                      : g_settings.target_dose_single;
            computeCorrectedTarget(base, g_is_double, g_target_grams,
                                   g_target_grams_corrected);
            transitionTo(DosingState::CONFIRM);
          } else if (g_state == DosingState::SCREENSAVER) {
            // Any other button just dismisses the screensaver.
            transitionTo(DosingState::IDLE);
          }
          break;
        case DosingState::CONFIRM:
          if (press.button == g_pending_confirm_button) {
            float base = g_is_double ? g_settings.target_dose_double
                                      : g_settings.target_dose_single;
            g_session_discard_training = false;  // a real physical dose always trains the model
            computeCorrectedTarget(base, g_is_double, g_target_grams,
                                   g_target_grams_corrected);
            transitionTo(DosingState::TARE);
          } else {
            transitionTo(DosingState::IDLE);
          }
          break;
        case DosingState::GRINDING:
        case DosingState::TOPUP:
          if (press.button == ButtonId::BACK) {
            // Stop immediately regardless of sub-phase -- a cancel must
            // never queue up behind anything.
            grinderRelay(false);
            transitionTo(DosingState::FINALIZE);
          }
          break;
        default:
          break;
      }
    }

    // Non-sample-driven state transitions.
    switch (g_state) {
      case DosingState::IDLE: {
        uint32_t timeout_ms = g_settings.screensaver_timeout_s * 1000UL;
        if (millis() - g_last_activity_ms >= timeout_ms) {
          transitionTo(DosingState::SCREENSAVER);
        }
        break;
      }
      case DosingState::CONFIRM: {
        // Lapses back to IDLE if nobody confirms or cancels in time,
        // rather than waiting forever.
        if (millis() - g_state_entered_ms >= g_settings.confirm_timeout_ms) {
          transitionTo(DosingState::IDLE);
        }
        break;
      }
      case DosingState::TARE: {
        // Wait for a settled reading, same bar as the ADC's own auto-tare --
        // a mid-transient sample here would bias every stop decision for the
        // rest of the session, since they're all deltas from this baseline.
        // Also require the sample to postdate the re-tare request itself
        // (sample_seq compared with wraparound-safe signed subtraction) --
        // otherwise a cup that was already sitting stable before TARE was
        // even entered would latch on the very next tick, using a baseline
        // from before the fresh hardware re-tare actually landed.
        bool sample_postdates_retare =
            g_have_sample &&
            static_cast<int32_t>(g_last_sample.sample_seq - g_tare_requested_after_seq) > 0;
        if (sample_postdates_retare && g_last_sample.stable) {
          g_grams_on_grind_start = g_last_sample.grams;  // software tare baseline
          {
            // AR-073 diagnostic: this baseline is held fixed for the whole
            // session, so a wrong value here explains every later reading.
            char line[96];
            snprintf(line, sizeof(line), "TARE baseline latched: %.4fg (raw_adc=%ld)",
                      g_grams_on_grind_start, static_cast<long>(g_last_sample.raw_adc));
            sendLog(line);
          }
          g_session_id = g_next_session_id++;
          g_main_grind_model->startSession();
          g_grinder_started_ms = millis();
          grinderRelay(true);
          // Delta-from-baseline is 0 by definition right at tare -- the
          // baseline itself (whatever the cup happens to weigh) must never
          // be sent as "current weight" here, same reasoning as everywhere
          // else this value is used.
          sendTelemetry(TelemetryType::TARGET, 0.0f, g_target_grams);
          // Debug-only stats: the same physical dosing cup should tare to
          // roughly the same raw ADC count every time -- logging it (never
          // persisting it as a calibration constant) lets that assumption
          // actually be checked across sessions instead of just presumed.
          // Sent after TARGET, not before: TelemetryTask's session row
          // (which this references by FK) is only created on that event.
          sendTelemetry(TelemetryType::TARE_DEBUG, g_grams_on_grind_start);
          transitionTo(DosingState::GRINDING);
        }
        break;
      }
      case DosingState::STOPPING: {
        // Waits for a settled reading (bounded, so a persistently noisy
        // sensor can't stall here forever) before recording the coast --
        // it trains the model that predicts the *main grind's* stop time,
        // so a transient reading here is a slow, compounding bias.
        bool settled = (g_have_sample && g_last_sample.stable) ||
                       millis() - g_state_entered_ms >= g_settings.stability_max_wait_ms;
        if (settled && g_have_sample) {
          double observed_coast = g_last_sample.grams - g_weight_at_relay_off;
          g_coast_model->recordCoast(observed_coast, nowEpochS());
          g_topup_phase = TopupPhase::DECIDING;
          g_topup_deciding_entered_ms = millis();
          transitionTo(DosingState::TOPUP);
        }
        break;
      }
      case DosingState::FINALIZE: {
        if (!g_finalize_done) {
          // Wait for a settled reading before latching the final weight --
          // same bar as TARE -- since the coast right after the grinder
          // cuts off can leave it mid-transient for a moment.
          bool settled = (g_have_sample && g_last_sample.stable) ||
                         millis() - g_state_entered_ms >= g_settings.stability_max_wait_ms;
          if (!settled) {
            break;
          }
          // finalize_timeout_ms below now counts from the actual latch,
          // not from FINALIZE entry, so the result stays on screen for
          // its full duration regardless of how long settling took.
          g_state_entered_ms = millis();

          if (g_session_discard_training) {
            // Rebuild all three models fresh from the untouched persisted
            // blob -- throws away every recordPulse()/recordCoast()/
            // addSample() mutation this session made to the in-RAM
            // objects, not just skips the NVS write below.
            delete g_main_grind_model;
            delete g_topup_model;
            delete g_coast_model;
            g_main_grind_model = new MainGrindModel(g_persisted_model);
            g_topup_model = new TopupModel(g_persisted_model);
            g_coast_model = new CoastModel(g_persisted_model);
            sendLog("FINALIZE: discard_training set, model left untouched");
          } else {
            // Fold the three models' latest state into one blob and hand
            // it to Settings task -- never a per-sample write.
            TopupModelV1 blob = g_persisted_model;
            blob = g_main_grind_model->dumpPersisted(blob);
            blob = g_topup_model->dumpPersisted(blob);
            blob = g_coast_model->dumpPersisted(blob);
            g_persisted_model = blob;

            PersistRequest req{PersistBlobId::TOPUP_MODEL_V1, blob, g_session_id};
            xQueueSend(g_persist_request_q, &req, 0);
          }
          sendModelStateTelemetry(g_persisted_model);

          // final_weight_g must be comparable to requested_weight_g/
          // target_weight_g (both plain dose amounts) -- the absolute
          // reading includes whatever the cup itself weighs, which is
          // exactly the "3-digit number" bug.
          g_finalize_latched_delta = g_have_sample ? g_last_sample.grams - g_grams_on_grind_start : 0;
          {
            // AR-073 diagnostic: the two raw operands behind the reported
            // final weight, not just their difference.
            char line[128];
            snprintf(line, sizeof(line), "FINALIZE latch: sample=%.4fg baseline=%.4fg delta=%.4fg",
                      g_have_sample ? g_last_sample.grams : 0.0f, g_grams_on_grind_start,
                      g_finalize_latched_delta);
            sendLog(line);
          }
          sendTelemetry(TelemetryType::COMPLETE, g_finalize_latched_delta, g_target_grams);
          g_last_coffee_ms = millis();
          g_finalize_done = true;
        }
        /*
         * Lifting the cup swings the absolute reading (and so the delta)
         * by roughly the cup's own weight -- tens of grams at least --
         * which the fixed-width display layout was never sized for (a
         * 3-digit, possibly-negative number overflows the line). Rather
         * than redesign the layout for a case nobody needs to read,
         * treat a swing this large as "the user picked up the cup,
         * they've seen the result" and dismiss straight to IDLE.
         */
        // Requires a settled reading, same discipline as every other
        // decision in this file -- lifting the cup reads unstable while
        // actually in motion, and a mid-lift transient is exactly the
        // kind of single-sample spike this check must not act on
        // directly. No bounded fallback needed: finalize_timeout_ms
        // below is the existing, independent backstop if the reading
        // never settles.
        constexpr float kCupLiftDeltaG = 3.0f;
        bool cupLifted =
            g_have_sample && g_last_sample.stable &&
            fabsf((g_last_sample.grams - g_grams_on_grind_start) - g_finalize_latched_delta) >=
                kCupLiftDeltaG;
        if (g_finalize_done &&
            (cupLifted || millis() - g_state_entered_ms >= g_settings.finalize_timeout_ms)) {
          g_finalize_done = false;
          transitionTo(DosingState::IDLE);
        }
        break;
      }
      default:
        break;
    }

    /*
     * Backstop: no non-idle state should ever persist indefinitely.
     * GRINDING/TOPUP/FINALIZE/CONFIRM already have their own faster,
     * purpose-built timeouts above; this is a much longer last-resort
     * net for whatever isn't -- e.g. TARE waiting on a scale sample
     * that never arrives because Scale task died, or any future state
     * this reasoning didn't anticipate. Without it, any such gap leaves
     * the device stuck until a physical power-cycle, and also blocks
     * OTA indefinitely, since every non-idle state counts as an active
     * session.
     */
    constexpr uint32_t kMaxStateDurationMs = 60000;
    if (g_state != DosingState::IDLE && g_state != DosingState::SCREENSAVER &&
        g_state != DosingState::BOOT &&
        millis() - g_state_entered_ms >= kMaxStateDurationMs) {
      char line[96];
      snprintf(line, sizeof(line), "backstop: forcing IDLE from %s after %lums",
                dosingStateName(g_state), static_cast<unsigned long>(kMaxStateDurationMs));
      sendLog(line);
      grinderRelay(false);
      g_finalize_done = false;
      transitionTo(DosingState::IDLE);
    }

    // Network task owns the display mailbox during an OTA flash --
    // writing here too would race it and flip the screen mode.
    bool ota_in_progress = (xEventGroupGetBits(g_sys_events) & kOtaInProgressBit) != 0;
    if (!ota_in_progress && (got_sample || g_state != DosingState::GRINDING)) {
      sendDisplayCommand();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void createDosingTask() {
  xTaskCreatePinnedToCore(dosingTaskFn, "Dosing", TaskConfig::kDosingStackBytes,
                           nullptr, TaskConfig::kDosingPriority, nullptr,
                           TaskConfig::kDosingCore);
}
