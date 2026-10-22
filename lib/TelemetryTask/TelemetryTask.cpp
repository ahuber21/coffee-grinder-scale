#include "TelemetryTask.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <esp_system.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"
#include "defines.h"  // POSTGREST_BASE_URL

/*
 * Telemetry task: forwards Dosing task's events to the WS broadcast
 * queue and posts them to PostgREST (schema v2, plain HTTP, no auth
 * header: the deployment runs every request as one scoped anonymous role
 * that can SELECT and INSERT on the v2 tables and UPDATE only the
 * session's finalize and landing columns).
 *
 * Event to request mapping:
 *   TARGET       POST  /sessions     opens a session under a fresh UUIDv4
 *                                    (the event's own session_id is a local counter)
 *   PROGRESS     POST  /events       the MAIN_GRIND row, sent once the relay is off
 *   TARE_DEBUG   POST  /tare_debug   the session's tare baseline
 *   RAW_SAMPLE   POST  /raw_samples  handled, but Dosing task does not emit it yet
 *   LANDING      PATCH /sessions     settled weight, coast and learner correction
 *   MODEL_STATE  POST  /model_snapshots  the model parameters after a session (none at boot)
 *   FINALIZE     none                marks a normal finish, so COMPLETE can tell it from an abort
 *   COMPLETE     PATCH /sessions     final weight and outcome
 *
 * Only one session is in flight at a time (Dosing task owns the relay),
 * so the assembly state below is a single set of variables.
 */

namespace {

constexpr uint32_t kHttpTimeoutMs = 2000;

// Labels stored in v2.sessions; kept stable so session history stays comparable.
constexpr const char *kFirmwareVersion = "rtos-rewrite-dev";
constexpr const char *kModelVersion = "ModelV4";

// Current-session assembly state.
bool g_session_open = false;
char g_session_uuid[37] = {0};  ///< 8-4-4-4-12 hex digits plus NUL.
float g_main_grind_weight_before = 0.0f;
bool g_finalize_seen = false;

/** Fills `out` with a random UUIDv4 string, for a new session's PostgREST id. */
void makeUuidV4(char out[37]) {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    b[i] = r & 0xFF;
    b[i + 1] = (r >> 8) & 0xFF;
    b[i + 2] = (r >> 16) & 0xFF;
    b[i + 3] = (r >> 24) & 0xFF;
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10
  snprintf(out, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);
}

/**
 * Fires one POST or PATCH against PostgREST. Best-effort: any failure
 * (no WiFi, connection refused, non-2xx) is logged and dropped, and the
 * HTTP timeout bounds how long an unreachable server can delay the next
 * queue drain.
 */
bool postgrestRequest(const char *method, const String &path, const String &body) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  String url = String(POSTGREST_BASE_URL) + path;
  http.begin(url);
  http.setTimeout(kHttpTimeoutMs);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");  // Responses are never read.

  int code;
  if (strcmp(method, "POST") == 0) {
    code = http.POST(body);
  } else {
    code = http.PATCH(body);
  }

  bool ok = code >= 200 && code < 300;
  if (!ok) {
    Serial.printf("[Telemetry] PostgREST %s %s -> %d\n", method, path.c_str(), code);
  }
  http.end();
  return ok;
}

/** Current UTC time as an ISO-8601 timestamp string. */
String isoTimestampNowUtc() {
  time_t now = time(nullptr);
  struct tm tm_val;
  gmtime_r(&now, &tm_val);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
  return String(ts);
}

// One builder per PostgREST call, matching the schema's columns field for field.

/** TARGET -> POST /sessions: opens a new session with a fresh UUID. */
void postSessionStart(const TelemetryEvent &ev) {
  makeUuidV4(g_session_uuid);
  g_session_open = true;
  g_main_grind_weight_before = ev.grams;
  g_finalize_seen = false;

  char body[320];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"mode\":\"%s\",\"requested_weight_g\":%.4f,"
           "\"target_weight_g\":%.4f,\"firmware_version\":\"%s\","
           "\"model_version\":\"%s\"}",
           g_session_uuid, ev.is_double ? "double" : "single", ev.target_grams,
           ev.target_grams, kFirmwareVersion, kModelVersion);

  postgrestRequest("POST", "/sessions", String(body));
}

/** Maps a GrindStopReason to the text value v2.events.stop_reason expects. */
const char *stopReasonToString(GrindStopReason reason) {
  switch (reason) {
    case GrindStopReason::TIME_ESTIMATE: return "TIME_ESTIMATE";
    case GrindStopReason::RAW_WEIGHT_FALLBACK: return "RAW_WEIGHT_FALLBACK";
    case GrindStopReason::SAFETY_TIMEOUT: return "SAFETY_TIMEOUT";
  }
  return "UNKNOWN";
}

/**
 * PROGRESS -> POST /events: the main-grind row, complete in one call.
 * stop_reason and weight_estimate_g record which stop condition fired and
 * what the model believed at that instant, which the final weight cannot.
 */
void postMainGrindEvent(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  char body[320];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"event_type\":\"MAIN_GRIND\",\"pulse_index\":0,"
           "\"relay_on_at_ms\":0,\"relay_off_at_ms\":%u,"
           "\"weight_before_g\":%.4f,\"weight_after_g\":%.4f,"
           "\"stop_reason\":\"%s\",\"weight_estimate_g\":%.4f}",
           g_session_uuid, static_cast<unsigned>(ev.runtime_ms),
           g_main_grind_weight_before, ev.grams,
           stopReasonToString(ev.stop_reason), ev.weight_estimate_g);

  postgrestRequest("POST", "/events", String(body));
}

/**
 * TARE_DEBUG -> POST /tare_debug: this session's tare baseline (raw ADC
 * count and grams). Observational only; the firmware never reads it back.
 */
void postTareDebug(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  char body[160];
  snprintf(body, sizeof(body), "{\"session_id\":\"%s\",\"raw_adc\":%ld,\"grams\":%.4f}",
           g_session_uuid, static_cast<long>(ev.raw_adc), ev.grams);

  postgrestRequest("POST", "/tare_debug", String(body));
}

/** RAW_SAMPLE -> POST /raw_samples; Dosing task does not emit this event yet. */
void postRawSample(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  char body[256];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"timestamp_ms\":%u,\"raw_value\":%ld,"
           "\"filtered_value\":%.4f,\"stable\":%s,\"grinder_state\":\"%s\"}",
           g_session_uuid, static_cast<unsigned>(ev.runtime_ms),
           static_cast<long>(ev.raw_adc), ev.grams, ev.stable ? "true" : "false",
           "MAIN_GRIND");

  postgrestRequest("POST", "/raw_samples", String(body));
}

/** LANDING -> PATCH /sessions: the settled main-grind weight and what the landing learner did. */
void patchSessionLanding(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  char body[256];
  snprintf(body, sizeof(body),
           "{\"settled_weight_g\":%.4f,\"coast_g\":%.4f,"
           "\"learner_correction_g\":%.4f,\"learner_clamped\":%s}",
           ev.grams, ev.landing_coast_g, ev.landing_correction_g,
           ev.landing_clamped ? "true" : "false");

  String path = String("/sessions?session_id=eq.") + g_session_uuid;
  postgrestRequest("PATCH", path, String(body));
}

/**
 * MODEL_STATE -> POST /model_snapshots: the model parameters as they stand after this session,
 * so the Model page can chart how they moved. The boot-time event has no session and is skipped.
 */
void postModelSnapshot(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  char body[900];
  int n = snprintf(body, sizeof(body),
                   "{\"session_id\":\"%s\",\"rate_hat_g_s\":%.5f,\"rate_sd_g_s\":%.5f,"
                   "\"rate_n_effective\":%u,\"coast_g\":%.5f,\"coast_sd_g\":%.5f",
                   g_session_uuid, ev.rate_hat_g_s, ev.rate_sd_g_s,
                   static_cast<unsigned>(ev.rate_n_effective), ev.coast_weight_g,
                   ev.coast_weight_sd_g);
  static const char *const kPrefix[2] = {"single", "double"};
  for (int i = 0; i < 2 && n > 0 && n < static_cast<int>(sizeof(body)); ++i) {
    const LandingLearner::Params &p = ev.learner[i];
    n += snprintf(body + n, sizeof(body) - n,
                  ",\"%s_sessions\":%u,\"%s_effective_sessions\":%.4f,"
                  "\"%s_mean_rate_g_s\":%.5f,\"%s_intercept_g\":%.5f,"
                  "\"%s_slope_g_per_g_s\":%.5f,\"%s_residual_sd_g\":%.5f",
                  kPrefix[i], static_cast<unsigned>(p.sessions), kPrefix[i], p.effective_sessions,
                  kPrefix[i], p.mean_rate_g_s, kPrefix[i], p.intercept_g, kPrefix[i],
                  p.slope_g_per_g_s, kPrefix[i], p.residual_sd_g);
  }
  if (n > 0 && n < static_cast<int>(sizeof(body)) - 1) {
    body[n] = '}';
    body[n + 1] = '\0';
    postgrestRequest("POST", "/model_snapshots", String(body));
  }
}

/** COMPLETE -> PATCH /sessions: closes the session out with its outcome. */
void patchSessionFinal(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  const char *outcome = g_finalize_seen ? "completed" : "aborted";
  String completed_at = isoTimestampNowUtc();

  char body[256];
  snprintf(body, sizeof(body),
           "{\"final_weight_g\":%.4f,\"outcome\":\"%s\",\"completed_at\":\"%s\"}",
           ev.grams, outcome, completed_at.c_str());

  String path = String("/sessions?session_id=eq.") + g_session_uuid;
  postgrestRequest("PATCH", path, String(body));

  g_session_open = false;
}

/** Telemetry task entry point: drains Dosing task's events to the WS broadcast and PostgREST. */
void telemetryTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE,
                       portMAX_DELAY);

  TelemetryEvent ev;
  for (;;) {
    if (xQueueReceive(g_telemetry_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (ev.type == TelemetryType::LOG_LINE) {
        Serial.printf("[Telemetry] %s\n", ev.log_line);
      }

      // Zero timeout: a full broadcast queue drops the event rather than stalling the PostgREST path.
      xQueueSend(g_ws_broadcast_q, &ev, 0);

      switch (ev.type) {
        case TelemetryType::TARGET:
          postSessionStart(ev);
          break;
        case TelemetryType::PROGRESS:
          postMainGrindEvent(ev);
          break;
        case TelemetryType::RAW_SAMPLE:
          postRawSample(ev);
          break;
        case TelemetryType::TARE_DEBUG:
          postTareDebug(ev);
          break;
        case TelemetryType::FINALIZE:
          g_finalize_seen = true;
          break;
        case TelemetryType::LANDING:
          patchSessionLanding(ev);
          break;
        case TelemetryType::MODEL_STATE:
          postModelSnapshot(ev);
          break;
        case TelemetryType::COMPLETE:
          patchSessionFinal(ev);
          break;
        case TelemetryType::LOG_LINE:
        default:
          break;
      }
    }
  }
}

}  // namespace

void createTelemetryTask() {
  xTaskCreatePinnedToCore(telemetryTaskFn, "Telemetry",
                           TaskConfig::kTelemetryStackBytes, nullptr,
                           TaskConfig::kTelemetryPriority, nullptr,
                           TaskConfig::kTelemetryCore);
}
