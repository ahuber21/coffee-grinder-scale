#include "DosingTask.h"

#include <Arduino.h>
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
ButtonId g_pending_confirm_button = ButtonId::LEFT;

float g_target_grams = 0.0f;  ///< Full requested target.
float g_target_grams_corrected = 0.0f;  ///< Target minus topup margin -- the actual stop threshold.
float g_grams_on_grind_start = 0.0f;    ///< Software tare baseline for this session.
uint32_t g_grinder_started_ms = 0;
uint32_t g_grinder_stopped_ms = 0;
float g_weight_at_relay_off = 0.0f;

uint32_t g_state_entered_ms = 0;

/** Sub-state within DosingState::TOPUP -- one pulse's decide/fire/settle cycle. */
enum class TopupPhase { DECIDING, PULSING, SETTLING };
TopupPhase g_topup_phase = TopupPhase::DECIDING;
uint32_t g_topup_pulse_start_ms = 0;
uint32_t g_topup_pulse_duration_ms = 0;
float g_topup_weight_before_pulse = 0.0f;

bool g_finalize_done = false;

/*
 * lib/DosingModel integration: hot, running model state lives here as
 * plain task-local state, updated on every ScaleSample this task
 * already receives while GRINDING/TOPUP -- no queue hop on the
 * per-sample path. The persisted cold copy is loaded once from
 * Settings task before this task leaves BOOT, and written back only
 * when a session finishes.
 */

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
  if (next != g_state) {
    char line[96];
    snprintf(line, sizeof(line), "state %s -> %s", dosingStateName(g_state),
              dosingStateName(next));
    sendLog(line);
  }
  g_state = next;
  g_state_entered_ms = millis();
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
  cmd.current_grams = g_have_sample ? g_last_sample.grams : 0.0f;
  cmd.target_grams = g_target_grams;
  cmd.elapsed_s = g_grinder_started_ms
                      ? (millis() - g_grinder_started_ms) / 1000.0f
                      : 0.0f;
  cmd.debug_raw_adc = g_have_sample ? g_last_sample.raw_adc : 0;
  cmd.debug_stable = g_have_sample ? g_last_sample.stable : false;
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

/** Energizes/de-energizes the grinder relay -- the sole writer of this pin. */
void grinderRelay(bool on) { digitalWrite(GRINDER_RELAY_PIN, on ? HIGH : LOW); }

/** Per-sample GRINDING update: feeds the rate/coast models and checks every stop condition. */
void handleGrindingSample(const ScaleSample &s) {
  uint32_t runtime_ms = s.millis - g_grinder_started_ms;
  double delta_weight = s.grams - g_grams_on_grind_start;
  g_main_grind_model->addSample(runtime_ms, delta_weight);

  double target_delta = g_target_grams_corrected - g_grams_on_grind_start;
  double coast_estimate = g_coast_model->currentCoastEstimate();
  double predicted_stop_ms =
      g_main_grind_model->predictStopTimeMsWithCoast(target_delta, coast_estimate);

  // Primary (time-estimate) and fallback (raw-weight) stop checks both
  // compare against the same canonical value, target_grams_corrected --
  // there is no second variable either could diverge from.
  bool time_estimate_fired = runtime_ms >= predicted_stop_ms;
  bool raw_weight_fallback_fired = s.grams >= g_target_grams_corrected;
  bool safety_timeout_fired = runtime_ms >= g_settings.grinding_timeout_ms;

  if (time_estimate_fired || raw_weight_fallback_fired || safety_timeout_fired) {
    grinderRelay(false);
    g_grinder_stopped_ms = s.millis;
    g_weight_at_relay_off = s.grams;
    g_main_grind_model->finalizeSession(nowEpochS());
    sendTelemetry(TelemetryType::PROGRESS, s.grams, g_target_grams);
    transitionTo(DosingState::STOPPING);
  }
}

/** Per-sample TOPUP update: drives the DECIDING/PULSING/SETTLING pulse cycle. */
void handleTopupSample(const ScaleSample &s) {
  float gap = g_target_grams - s.grams;

  switch (g_topup_phase) {
    case TopupPhase::DECIDING: {
      if (gap <= g_settings.min_topup_grams) {
        sendTelemetry(TelemetryType::FINALIZE, s.grams, g_target_grams);
        transitionTo(DosingState::FINALIZE);
        return;
      }
      double overshoot_budget = 0.3;  // Hard overshoot cap.
      TopupModel::Decision decision =
          g_topup_model->computeTopupDecision(gap, overshoot_budget);
      if (!decision.should_fire) {
        // Accept the undershoot rather than gamble a pulse below the
        // controllable floor.
        sendTelemetry(TelemetryType::FINALIZE, s.grams, g_target_grams);
        transitionTo(DosingState::FINALIZE);
        return;
      }
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
      if (s.millis - g_state_entered_ms >= g_settings.stability_min_wait_ms) {
        double actual_duration_ms = s.millis - g_topup_pulse_start_ms;
        double weight_added = s.grams - g_topup_weight_before_pulse;
        g_topup_model->recordPulse(actual_duration_ms, weight_added, nowEpochS());
        sendTelemetry(TelemetryType::TOPUP_PULSE, s.grams, g_target_grams,
                      static_cast<float>(weight_added));
        g_topup_phase = TopupPhase::DECIDING;
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
  // Put it back for anyone else that might peek it later (there is no
  // other reader today, but this keeps the mailbox non-empty).
  xQueueOverwrite(g_topup_model_mailbox, &g_persisted_model);

  g_main_grind_model = new MainGrindModel(g_persisted_model);
  g_topup_model = new TopupModel(g_persisted_model);
  g_coast_model = new CoastModel(g_persisted_model);

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
      }
    }

    // A manual/API dose request, only actionable while idle.
    DoseRequest dose;
    while (xQueueReceive(g_dose_request_q, &dose, 0) == pdTRUE) {
      if (g_state == DosingState::IDLE && dose.requested_grams > 0.0f &&
          dose.requested_grams <= 40.0f) {
        g_is_double = false;
        computeCorrectedTarget(dose.requested_grams, g_is_double, g_target_grams,
                               g_target_grams_corrected);
        transitionTo(DosingState::TARE);
      }
    }

    // Debounced button presses, interpreted according to Dosing's own
    // current state -- e.g. CONFIRM's same-button-confirms /
    // different-button-cancels logic lives here.
    ButtonPress press;
    while (xQueueReceive(g_button_press_q, &press, 0) == pdTRUE) {
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
            // Preview the real target immediately, not whatever
            // g_target_grams happened to hold before this press --
            // otherwise the CONFIRM screen shows a stale or default value.
            float base = g_is_double ? g_settings.target_dose_double
                                      : g_settings.target_dose_single;
            computeCorrectedTarget(base, g_is_double, g_target_grams,
                                   g_target_grams_corrected);
            transitionTo(DosingState::CONFIRM);
          }
          break;
        case DosingState::CONFIRM:
          if (press.button == g_pending_confirm_button) {
            float base = g_is_double ? g_settings.target_dose_double
                                      : g_settings.target_dose_single;
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
      case DosingState::CONFIRM: {
        // Lapses back to IDLE if nobody confirms or cancels in time --
        // otherwise a missed button press (or nobody ever pressing one)
        // leaves this state waiting forever, which also permanently
        // blocks OTA since CONFIRM counts as an active session.
        if (millis() - g_state_entered_ms >= g_settings.confirm_timeout_ms) {
          transitionTo(DosingState::IDLE);
        }
        break;
      }
      case DosingState::TARE: {
        if (g_have_sample) {
          g_grams_on_grind_start = g_last_sample.grams;  // software tare baseline
          g_session_id = g_next_session_id++;
          g_main_grind_model->startSession();
          g_grinder_started_ms = millis();
          grinderRelay(true);
          sendTelemetry(TelemetryType::TARGET, g_grams_on_grind_start, g_target_grams);
          transitionTo(DosingState::GRINDING);
        }
        break;
      }
      case DosingState::STOPPING: {
        if (millis() - g_state_entered_ms >= g_settings.stability_min_wait_ms &&
            g_have_sample) {
          double observed_coast = g_last_sample.grams - g_weight_at_relay_off;
          g_coast_model->recordCoast(observed_coast, nowEpochS());
          g_topup_phase = TopupPhase::DECIDING;
          transitionTo(DosingState::TOPUP);
        }
        break;
      }
      case DosingState::FINALIZE: {
        if (!g_finalize_done) {
          // Fold the three models' latest state into one blob and hand
          // it to Settings task -- never a per-sample write.
          TopupModelV1 blob = g_persisted_model;
          blob = g_main_grind_model->dumpPersisted(blob);
          blob = g_topup_model->dumpPersisted(blob);
          blob = g_coast_model->dumpPersisted(blob);
          g_persisted_model = blob;

          PersistRequest req{PersistBlobId::TOPUP_MODEL_V1, blob, g_session_id};
          xQueueSend(g_persist_request_q, &req, 0);

          sendTelemetry(TelemetryType::COMPLETE, g_have_sample ? g_last_sample.grams : 0,
                        g_target_grams);
          g_finalize_done = true;
        }
        if (millis() - g_state_entered_ms >= g_settings.finalize_timeout_ms) {
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

    if (got_sample || g_state != DosingState::GRINDING) {
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
