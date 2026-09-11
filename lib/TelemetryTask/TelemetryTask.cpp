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

// -----------------------------------------------------------------------------
// Real PostgREST posting, replacing the earlier logging-only stub. Endpoint
// shape and auth model come straight from .agent/design/postgrest-deployment.md
// and .agent/design/db-schema/001_sessions_schema.sql (D8/D9):
//
//   - Base URL http://192.168.0.111:3000, schema v2, plain HTTP (no TLS --
//     the deployment doc is explicit that HTTPS is a future hardening step,
//     not implemented server-side yet).
//   - No auth header/bearer token: this v1 deployment has no JWT config
//     (`db-anon-role` only), so every request already runs as the scoped
//     `postgrest_anon` role purely from which port/schema it hits. There is
//     no "INSERT-only role credential" for firmware to present -- the
//     column-level GRANT/REVOKE scoping happens entirely server-side.
//   - `postgrest_anon` holds SELECT+INSERT on all three v2 tables, plus
//     UPDATE scoped to just the "finalize" columns -- sessions can only be
//     PATCHed for final_weight_g/outcome/completed_at/relay_off_at_ms/
//     stable_at_ms, matching exactly what patchSessionFinal() below sends.
//
// TelemetryEvent -> PostgREST mapping
// ------------------------------------
// The design doc's §3.4 sketch imagined a richer/higher-cadence
// TelemetryEvent stream (per-tick PROGRESS, continuous RAW_SAMPLE) than what
// Dosing task actually emits today (lib/DosingTask/DosingTask.cpp's
// sendTelemetry() call sites): one TARGET at grind start, one PROGRESS when
// the main grind's relay turns off, one TOPUP_PULSE per completed
// (pulse+settle) topup cycle, one FINALIZE when the topup loop decides it's
// done, one COMPLETE at session end. This task's body is written against
// that *actual* stream (not the design doc's aspirational cadence) --
// per the task brief, the messaging pattern/struct set is not being
// redesigned here, so the mapping below is deliberately built to make
// correct PostgREST calls from exactly the events Dosing task really sends:
//
//   TARGET       -> POST /sessions (session_id is a fresh UUIDv4 generated
//                    here, since TelemetryEvent.session_id is only a local
//                    uint32 counter -- PostgREST's session_id column is a
//                    real UUID per the schema's "ID STRATEGY" note).
//   PROGRESS     -> POST /events (event_type=MAIN_GRIND, pulse_index=0).
//                    Dosing only sends this once, right after the main-grind
//                    relay is already off, so relay_on/off_at_ms and
//                    weight_before/after_g are all known in the one call --
//                    no follow-up PATCH is needed for this row.
//   TOPUP_PULSE  -> POST /events (event_type=TOPUP, pulse_index=1,2,...).
//                    Same reasoning: Dosing only sends this after the pulse's
//                    settle has already completed, so before/after weight is
//                    known in one shot. Known fidelity gap: TelemetryEvent
//                    doesn't carry the pulse's own relay-on/relay-off
//                    timestamps (only the settle-complete runtime_ms), so
//                    relay_on_at_ms/relay_off_at_ms are both approximated as
//                    that one timestamp rather than a real pulse duration --
//                    flagged in the task report as an AR candidate (Dosing
//                    task already has g_topup_pulse_start_ms locally; a
//                    follow-up could forward it).
//   RAW_SAMPLE   -> POST /raw_samples. Not actually emitted by Dosing task
//                    yet (no per-sample streaming wired up), but handled
//                    here for forward-compatibility with the enum/schema.
//   FINALIZE     -> no HTTP call; just marks "the topup loop finished
//                    normally" so COMPLETE's outcome can distinguish that
//                    from a BACK-button abort or the topup-loop safety
//                    timeout (neither of which sends a FINALIZE-typed event
//                    before COMPLETE -- see DosingTask.cpp's BACK-press and
//                    "overall safety cutoff" branches). This can't
//                    distinguish "aborted" from "timed_out" specifically;
//                    both collapse to 'aborted' here -- another AR
//                    candidate (Dosing task knows the real cause; nothing in
//                    the current TelemetryEvent stream carries it).
//   COMPLETE     -> PATCH /sessions?session_id=eq.<uuid>, closing the
//                    session out.
//
// Only one grind session is ever in flight at a time (Dosing task is the
// sole owner of the grinder relay, §2), so the assembly state below is a
// single set of "current session" variables, not a session table.
// -----------------------------------------------------------------------------

namespace {

constexpr const char *kPostgrestBase = "http://192.168.0.111:3000";
constexpr uint32_t kHttpTimeoutMs = 2000;

// No repo-wide firmware version scheme exists yet (git describe / build-time
// define) -- an AR candidate. This is the closest available stand-in so the
// NOT NULL firmware_version column isn't sent empty.
constexpr const char *kFirmwareVersion = "rtos-rewrite-dev";
constexpr const char *kModelVersion = "TopupModelV1";

uint32_t g_dropped_events = 0;
uint32_t g_http_failures = 0;

// --- Current-session assembly state (single in-flight session, see above) --
bool g_session_open = false;
char g_session_uuid[37] = {0};  // 8-4-4-4-12 hex + NUL
float g_main_grind_weight_before = 0.0f;
int g_pulse_index = 0;
bool g_finalize_seen = false;

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

// Fires one POST or PATCH against PostgREST. Best-effort: on any failure
// (no WiFi, connection refused, non-2xx) it logs and drops -- Telemetry task
// must never block Dosing (§3.4, principle #4), and this task's own queue
// receive already has a short blocking timeout, not an infinite one, so a
// slow/unreachable PostgREST just delays the next drain by
// kHttpTimeoutMs at worst, never stalls the rest of the system.
bool postgrestRequest(const char *method, const String &path, const String &body) {
  if (WiFi.status() != WL_CONNECTED) {
    // Network task's real WiFi bring-up is separate, concurrent work (per
    // the task brief) -- this skeleton path may run before WiFi ever
    // associates. Fail soft rather than block/retry.
    return false;
  }

  HTTPClient http;
  String url = String(kPostgrestBase) + path;
  http.begin(url);
  http.setTimeout(kHttpTimeoutMs);
  http.addHeader("Content-Type", "application/json");
  // Sessions/events need the row back (event_id in particular, for a real
  // pulse-level PATCH follow-up) in the general case; harmless/ignored for
  // the all-in-one-call rows this task actually sends today.
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

String isoTimestampNowUtc() {
  time_t now = time(nullptr);
  struct tm tm_val;
  gmtime_r(&now, &tm_val);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
  return String(ts);
}

// --- One builder per PostgREST call, matching postgrest-deployment.md's
//     "Endpoint shape" section field-for-field. ------------------------------

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

void postRawSample(const TelemetryEvent &ev) {
  if (!g_session_open) return;  // not emitted by Dosing task yet -- see above

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

void telemetryTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE,
                       portMAX_DELAY);

  TelemetryEvent ev;
  for (;;) {
    // Best-effort, lowest priority in the app band (§6.6) -- block briefly
    // rather than busy-poll; Dosing task never waits on us either way
    // (its own sends are zero-timeout, §3.4).
    if (xQueueReceive(g_telemetry_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (ev.type == TelemetryType::LOG_LINE) {
        Serial.printf("[Telemetry] %s\n", ev.log_line);
      }

      // Forward a subset to Network's WS-broadcast inbox (§7). Zero-timeout
      // send with a drop-and-count policy, matching the drop discipline
      // used everywhere else non-critical data crosses a queue boundary.
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
