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

/**
 * Real PostgREST posting for Telemetry task. Endpoint shape and auth
 * model come from the live deployment: base URL
 * http://192.168.0.111:3000, schema v2, plain HTTP (no TLS -- that is a
 * future hardening step, not implemented server-side yet), and no auth
 * header -- this deployment has no JWT config, so every request already
 * runs as a single scoped anonymous role purely from which port/schema
 * it hits. That role can SELECT+INSERT on all three v2 tables, plus
 * UPDATE scoped to just the "finalize" columns, matching exactly what
 * patchSessionFinal() below sends.
 *
 * TelemetryEvent -> PostgREST mapping. Dosing task's actual telemetry
 * stream (see its sendTelemetry() call sites) is simpler than a
 * per-tick stream would be: one TARGET at grind start, one PROGRESS
 * when the main grind's relay turns off, one TOPUP_PULSE per completed
 * topup cycle, one FINALIZE when the topup loop decides it's done, one
 * COMPLETE at session end. This task's body is written against that
 * actual stream:
 *
 *   TARGET       -> POST /sessions (session_id is a fresh UUIDv4
 *                    generated here, since TelemetryEvent.session_id is
 *                    only a local counter -- the sessions table's
 *                    session_id column is a real UUID).
 *   PROGRESS     -> POST /events (MAIN_GRIND, pulse_index=0). Dosing
 *                    only sends this once the main-grind relay is
 *                    already off, so before/after weight and both relay
 *                    timestamps are all known in the one call.
 *   TOPUP_PULSE  -> POST /events (TOPUP, pulse_index=1,2,...). Same
 *                    reasoning -- Dosing only sends this after the
 *                    pulse's settle has already completed. Known gap:
 *                    TelemetryEvent doesn't carry the pulse's own
 *                    relay-on/relay-off timestamps (only the
 *                    settle-complete runtime_ms), so both are
 *                    approximated as that one timestamp rather than a
 *                    real pulse duration; Dosing task already has the
 *                    real start time locally and could forward it.
 *   RAW_SAMPLE   -> POST /raw_samples. Not actually emitted by Dosing
 *                    task yet, but handled here for forward-compatibility.
 *   FINALIZE     -> no HTTP call; just marks that the topup loop
 *                    finished normally, so COMPLETE's outcome can tell
 *                    that apart from a manual abort or the topup loop's
 *                    own safety timeout -- neither of which sends a
 *                    FINALIZE event first. Those two causes aren't
 *                    distinguishable from each other yet; both collapse
 *                    to "aborted" here, since nothing in the current
 *                    stream carries which one actually happened.
 *   COMPLETE     -> PATCH /sessions?session_id=eq.<uuid>, closing the
 *                    session out.
 *
 * Only one grind session is ever in flight at a time (Dosing task is
 * the sole owner of the grinder relay), so the assembly state below is
 * a single set of "current session" variables, not a session table.
 */

namespace {

constexpr const char *kPostgrestBase = "http://192.168.0.111:3000";
constexpr uint32_t kHttpTimeoutMs = 2000;

// No repo-wide firmware version scheme exists yet (e.g. a build-time
// git-describe define) -- this is a stand-in so the schema's required
// firmware_version column isn't sent empty.
constexpr const char *kFirmwareVersion = "rtos-rewrite-dev";
constexpr const char *kModelVersion = "TopupModelV1";

uint32_t g_dropped_events = 0;
uint32_t g_http_failures = 0;

// Current-session assembly state (single in-flight session, see above).
bool g_session_open = false;
char g_session_uuid[37] = {0};  // 8-4-4-4-12 hex + NUL
float g_main_grind_weight_before = 0.0f;
int g_pulse_index = 0;
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
 * Fires one POST or PATCH against PostgREST. Best-effort: on any
 * failure (no WiFi, connection refused, non-2xx) it logs and drops --
 * Telemetry task must never block Dosing task, and its own queue
 * receive already has a short timeout rather than an infinite one, so
 * a slow/unreachable PostgREST only delays the next drain, never stalls
 * the rest of the system.
 */
bool postgrestRequest(const char *method, const String &path, const String &body) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  String url = String(kPostgrestBase) + path;
  http.begin(url);
  http.setTimeout(kHttpTimeoutMs);
  http.addHeader("Content-Type", "application/json");
  // Harmless for the all-in-one-call rows this task actually sends
  // today; would matter if a future pulse-level PATCH needed event_id
  // back from the insert.
  http.addHeader("Prefer", "return=representation");

  int code;
  if (strcmp(method, "POST") == 0) {
    code = http.POST(body);
  } else {
    code = http.PATCH(body);
  }

  bool ok = code >= 200 && code < 300;
  if (!ok) {
    g_http_failures++;
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

// One builder per PostgREST call below, matching the live schema's
// columns field-for-field.

/** TARGET -> POST /sessions: opens a new session with a fresh UUID. */
void postSessionStart(const TelemetryEvent &ev) {
  makeUuidV4(g_session_uuid);
  g_session_open = true;
  g_main_grind_weight_before = ev.grams;
  g_pulse_index = 0;
  g_finalize_seen = false;

  float target_weight_g =
      ev.target_grams_corrected > 0.0f ? ev.target_grams_corrected : ev.target_grams;

  char body[320];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"mode\":\"%s\",\"requested_weight_g\":%.4f,"
           "\"target_weight_g\":%.4f,\"firmware_version\":\"%s\","
           "\"model_version\":\"%s\"}",
           g_session_uuid, ev.is_double ? "double" : "single", ev.target_grams,
           target_weight_g, kFirmwareVersion, kModelVersion);

  postgrestRequest("POST", "/sessions", String(body));
}

/** PROGRESS -> POST /events: the main-grind row, complete in one call. */
void postMainGrindEvent(const TelemetryEvent &ev) {
  if (!g_session_open) return;  // stray PROGRESS with no open session

  char body[256];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"event_type\":\"MAIN_GRIND\",\"pulse_index\":0,"
           "\"relay_on_at_ms\":0,\"relay_off_at_ms\":%u,"
           "\"weight_before_g\":%.4f,\"weight_after_g\":%.4f}",
           g_session_uuid, static_cast<unsigned>(ev.runtime_ms),
           g_main_grind_weight_before, ev.grams);

  postgrestRequest("POST", "/events", String(body));
}

/** TOPUP_PULSE -> POST /events: one topup pulse's row, complete in one call. */
void postTopupPulseEvent(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  g_pulse_index++;
  float weight_before = ev.grams - ev.delta_grams;

  char body[256];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"event_type\":\"TOPUP\",\"pulse_index\":%d,"
           "\"relay_on_at_ms\":%u,\"relay_off_at_ms\":%u,"
           "\"weight_before_g\":%.4f,\"weight_after_g\":%.4f}",
           g_session_uuid, g_pulse_index, static_cast<unsigned>(ev.runtime_ms),
           static_cast<unsigned>(ev.runtime_ms), weight_before, ev.grams);

  postgrestRequest("POST", "/events", String(body));
}

/** RAW_SAMPLE -> POST /raw_samples: forward-compatible, not emitted yet. */
void postRawSample(const TelemetryEvent &ev) {
  if (!g_session_open) return;

  const char *grinder_state = g_pulse_index == 0 ? "MAIN_GRIND" : "TOPUP_SETTLE";

  char body[256];
  snprintf(body, sizeof(body),
           "{\"session_id\":\"%s\",\"timestamp_ms\":%u,\"raw_value\":%ld,"
           "\"filtered_value\":%.4f,\"stable\":%s,\"grinder_state\":\"%s\"}",
           g_session_uuid, static_cast<unsigned>(ev.runtime_ms),
           static_cast<long>(ev.raw_adc), ev.grams, ev.stable ? "true" : "false",
           grinder_state);

  postgrestRequest("POST", "/raw_samples", String(body));
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
    // Best-effort, lowest task priority -- block briefly rather than
    // busy-poll; Dosing task never waits on us either way.
    if (xQueueReceive(g_telemetry_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (ev.type == TelemetryType::LOG_LINE) {
        Serial.printf("[Telemetry] %s\n", ev.log_line);
      }

      // Forward a subset to Network task's WS-broadcast inbox.
      // Zero-timeout send with a drop-and-count policy.
      if (xQueueSend(g_ws_broadcast_q, &ev, 0) != pdTRUE) {
        g_dropped_events++;
      }

      // PostgREST posting -- see the file-level comment for the mapping.
      switch (ev.type) {
        case TelemetryType::TARGET:
          postSessionStart(ev);
          break;
        case TelemetryType::PROGRESS:
          postMainGrindEvent(ev);
          break;
        case TelemetryType::TOPUP_PULSE:
          postTopupPulseEvent(ev);
          break;
        case TelemetryType::RAW_SAMPLE:
          postRawSample(ev);
          break;
        case TelemetryType::FINALIZE:
          g_finalize_seen = true;
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
