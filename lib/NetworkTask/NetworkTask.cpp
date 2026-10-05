#include "NetworkTask.h"

#include <Arduino.h>
#include <WiFi.h>
// WiFiManager.h must come before ESPAsyncWebServer.h: it pulls in the
// synchronous WebServer.h, whose WEBSERVER_H include guard is what makes
// ESPAsyncWebServer.h skip redefining the HTTP_GET/POST/... enum -- reversed,
// the two libraries' enums conflict at compile time.
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <cmath>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "SettingsSchema.h"
#include "TaskConfig.h"
#include "defines.h"  // SERVER_PORT

namespace {

/** True unless Dosing task is mid-grind, in which case an OTA start must be refused. */
bool otaSafeToStart() {
  EventBits_t bits = xEventGroupGetBits(g_sys_events);
  return (bits & kDosingActiveBit) == 0;
}

constexpr const char *kApName = "Eureka setup";  ///< WiFiManager AP name.

// mDNS hostname -> eureka.local. No OTA password: accepted risk, home
// LAN only.
constexpr const char *kMdnsHostname = "eureka";
constexpr uint16_t kOtaPort = 3232;

constexpr size_t kMaxWsClients = 4;  ///< One connection cap, enforced in one place.

constexpr const char *kGramsOutOfRange = "grams out of range (0, 50]";  ///< Keep in step with kMaxDoseGrams.

AsyncWebServer g_server(SERVER_PORT);
AsyncWebSocket g_ws("/ws");

uint32_t g_next_dose_request_id = 1;

// Cached so a newly-connecting client can be caught up immediately --
// MODEL_STATE only broadcasts on boot/session-complete, unlike settings,
// which every connect already gets replayed from its own mailbox.
bool g_have_model_state = false;
TelemetryEvent g_last_model_state{};

// One multiplexed WS channel: each message is {"type": <discriminator>, ...}.
// TelemetryType maps to "type" string; "settings" and "error" are reserved.

/** Maps a TelemetryType to its WS envelope "type" string. */
const char *telemetryTypeToString(TelemetryType t) {
  switch (t) {
    case TelemetryType::TARGET:
      return "target";
    case TelemetryType::PROGRESS:
      return "progress";
    case TelemetryType::RAW_SAMPLE:
      return "raw_sample";
    case TelemetryType::TOPUP_PULSE:
      return "topup_pulse";
    case TelemetryType::FINALIZE:
      return "finalize";
    case TelemetryType::COMPLETE:
      return "complete";
    case TelemetryType::LOG_LINE:
      return "log";
    case TelemetryType::MODEL_STATE:
      return "model_state";
    case TelemetryType::TARE_DEBUG:
      return "tare_debug";
    case TelemetryType::LANDING:
      return "landing";
  }
  return "unknown";
}

/** Serializes one TelemetryEvent into its WS envelope JSON. */
String buildTelemetryJson(const TelemetryEvent &ev) {
  JsonDocument doc;
  doc["type"] = telemetryTypeToString(ev.type);
  doc["session_id"] = ev.session_id;
  doc["runtime_ms"] = ev.runtime_ms;
  switch (ev.type) {
    case TelemetryType::LOG_LINE:
      doc["line"] = ev.log_line;
      break;
    case TelemetryType::RAW_SAMPLE:
      doc["raw_adc"] = ev.raw_adc;
      doc["grams"] = ev.grams;
      doc["stable"] = ev.stable;
      break;
    case TelemetryType::TARE_DEBUG:
      doc["raw_adc"] = ev.raw_adc;
      doc["grams"] = ev.grams;
      break;
    case TelemetryType::LANDING:
      doc["grams"] = ev.grams;
      doc["target_grams"] = ev.target_grams;
      doc["coast_g"] = ev.landing_coast_g;
      doc["margin_g"] = ev.landing_margin_g;
      doc["correction_g"] = ev.landing_correction_g;
      doc["clamped"] = ev.landing_clamped;
      break;
    case TelemetryType::TOPUP_PULSE:
      doc["grams"] = ev.grams;
      doc["delta_grams"] = ev.delta_grams;
      doc["target_grams"] = ev.target_grams;
      doc["topup_bucket"] = ev.topup_bucket;
      doc["topup_aim_weight_g"] = ev.topup_aim_weight_g;
      doc["topup_commanded_duration_ms"] = ev.topup_commanded_duration_ms;
      break;
    case TelemetryType::PROGRESS:
      doc["grams"] = ev.grams;
      doc["target_grams"] = ev.target_grams;
      doc["weight_estimate_g"] = ev.weight_estimate_g;
      doc["stop_reason"] = static_cast<uint8_t>(ev.stop_reason);
      break;
    case TelemetryType::TARGET:
    case TelemetryType::FINALIZE:
    case TelemetryType::COMPLETE:
      doc["grams"] = ev.grams;
      doc["target_grams"] = ev.target_grams;
      break;
    case TelemetryType::MODEL_STATE: {
      doc["rate_hat_g_s"] = ev.rate_hat_g_s;
      doc["rate_sd_g_s"] = ev.rate_sd_g_s;
      doc["rate_n_effective"] = ev.rate_n_effective;
      doc["coast_weight_g"] = ev.coast_weight_g;
      doc["coast_weight_sd_g"] = ev.coast_weight_sd_g;
      JsonArray durations = doc["topup_lut_duration_ms"].to<JsonArray>();
      JsonArray counts = doc["topup_lut_n"].to<JsonArray>();
      for (int i = 0; i < kTopupLutBuckets; ++i) {
        durations.add(ev.topup_lut_duration_ms[i]);
        counts.add(ev.topup_lut_n[i]);
      }
      break;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

/** Serializes the broadcast subset of SettingsSnapshot into WS envelope JSON. */
String buildSettingsJson(const SettingsSnapshot &snap) {
  JsonDocument doc;
  doc["type"] = "settings";
  doc["version"] = snap.version;
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  for (size_t i = 0; i < count; ++i) {
    const SettingDescriptor &d = table[i];
    if (!d.broadcast) continue;
    double value = readSetting(snap, d);
    // Stored types are kept on the wire, so a float prints as 0.3 rather than 0.300000012.
    switch (d.type) {
      case SettingType::U8:
      case SettingType::U32: doc[d.name] = static_cast<uint32_t>(value); break;
      case SettingType::F32: doc[d.name] = static_cast<float>(value); break;
      case SettingType::F64: doc[d.name] = value * d.wire_scale; break;
      case SettingType::BOOL: doc[d.name] = value != 0.0; break;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

/** Sends a per-client {"type":"error", ...} reply for a rejected request. */
void sendError(AsyncWebSocketClient *client, const char *message, uint32_t request_id) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["message"] = message;
  doc["request_id"] = request_id;
  String out;
  serializeJson(doc, out);
  client->text(out);
}

/**
 * Sends a per-client {"type":"raw_read", ...} reply -- the Advanced/
 * Calibration page's live polling loop, answered straight from
 * g_latest_sample_mailbox rather than through the Telemetry/session
 * machinery, since this is an ephemeral live probe with nothing to
 * persist and no session to attach it to.
 */
void sendRawRead(AsyncWebSocketClient *client, uint32_t request_id) {
  ScaleSample sample{};
  bool have_sample = xQueuePeek(g_latest_sample_mailbox, &sample, 0) == pdTRUE;
  JsonDocument doc;
  doc["type"] = "raw_read";
  doc["request_id"] = request_id;
  doc["raw_adc"] = have_sample ? sample.raw_adc : 0;
  doc["grams"] = have_sample ? sample.grams : 0.0f;
  doc["stable"] = have_sample ? sample.stable : false;
  String out;
  serializeJson(doc, out);
  client->text(out);
}

/**
 * Parses one inbound WS text frame and dispatches "settings_write" /
 * "dose_request". Network task only does cheap syntactic validation
 * and enqueues a request to the task that owns the state -- it never
 * reaches into another task's memory.
 */
void handleWsMessage(AsyncWebSocketClient *client, const uint8_t *data, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    sendError(client, "malformed JSON", 0);
    return;
  }

  const char *type = doc["type"] | "";
  uint32_t request_id = doc["request_id"] | 0;

  if (strcmp(type, "settings_write") == 0) {
    int index = findSettingByName(doc["field"] | "");
    if (index < 0) {
      sendError(client, "unknown settings field", request_id);
      return;
    }
    size_t count = 0;
    const SettingDescriptor &d = settingsTable(count)[index];
    SettingsWriteRequest req{};
    req.field_index = static_cast<uint16_t>(index);
    req.request_id = request_id;
    if (d.type == SettingType::BOOL) {
      req.value = (doc["value"] | false) ? 1.0 : 0.0;
    } else {
      // A double fallback makes ArduinoJson parse the number as a double; a float one would truncate it.
      req.value = (doc["value"] | static_cast<double>(NAN)) / d.wire_scale;
    }
    // Settings task alone decides whether the value is legal.
    if (xQueueSend(g_settings_write_q, &req, 0) != pdTRUE) {
      sendError(client, "settings queue full, try again", request_id);
    }
    return;
  }

  if (strcmp(type, "raw_read_request") == 0) {
    sendRawRead(client, request_id);
    return;
  }

  if (strcmp(type, "dose_request") == 0) {
    float grams = doc["grams"] | 0.0f;
    // Dosing task re-validates this range itself; this is just a
    // fast-fail check so a bad request doesn't queue up for nothing.
    if (!std::isfinite(grams) || grams <= 0.0f || grams > kMaxDoseGrams) {
      sendError(client, kGramsOutOfRange, request_id);
      return;
    }
    DoseRequest req{grams, request_id != 0 ? request_id : g_next_dose_request_id++,
                     doc["discard_training"] | false};
    if (xQueueSend(g_dose_request_q, &req, 0) != pdTRUE) {
      sendError(client, "dose queue full, try again", request_id);
    }
    return;
  }

  sendError(client, "unknown message type", request_id);
}

/** AsyncWebSocket event callback: connect/disconnect logging, the connection cap, and dispatch. */
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      Serial.printf("[Network] WS client #%u connected from %s\n", client->id(),
                    client->remoteIP().toString().c_str());
      if (server->count() > kMaxWsClients) {
        client->close();
        return;
      }
      // New clients get the current settings snapshot immediately rather
      // than waiting for the next write.
      SettingsSnapshot snap;
      if (xQueuePeek(g_settings_mailbox_network, &snap, 0) == pdTRUE) {
        client->text(buildSettingsJson(snap));
      }
      // Model state only broadcasts on boot/session-complete, so a
      // client connecting between those moments would otherwise see
      // nothing for a possibly very long time -- replay the last one.
      if (g_have_model_state) {
        client->text(buildTelemetryJson(g_last_model_state));
      }
      break;
    }
    case WS_EVT_DISCONNECT:
      Serial.printf("[Network] WS client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        handleWsMessage(client, data, len);
      }
      // Fragmented/binary frames aren't supported -- control messages
      // are small enough not to need it.
      break;
    }
    case WS_EVT_ERROR:
    case WS_EVT_PONG:
    default:
      break;
  }
}

// OTA refusal happens at the networkTaskFn loop (not calling handle() while
// grinding), not in onStart() -- onStart() fires after Update.begin() erases.

/** ArduinoOTA onStart callback: refuses (backstop only, see above) or begins. */
void handleOtaStart() {
  if (!otaSafeToStart()) {
    Serial.println("[Network] OTA refused: grind in progress");
    Update.abort();
    return;
  }
  xEventGroupSetBits(g_sys_events, kOtaInProgressBit);
  Serial.println("[Network] OTA starting");
  DisplayCommand cmd{};
  cmd.mode = DisplayMode::OTA_UPDATE;
  cmd.ota_percent = 0;
  // The one deliberate second writer to the display mailbox -- Dosing
  // task never assigns OTA_UPDATE, and this only fires during an
  // actual OTA flash.
  xQueueOverwrite(g_display_mailbox, &cmd);
}

/** ArduinoOTA onEnd callback: clears the in-progress bit before the reboot. */
void handleOtaEnd() {
  xEventGroupClearBits(g_sys_events, kOtaInProgressBit);
  Serial.println("[Network] OTA finished, rebooting");
}

/** ArduinoOTA onProgress callback: forwards percent complete to Display task. */
void handleOtaProgress(unsigned int progress, unsigned int total) {
  DisplayCommand cmd{};
  cmd.mode = DisplayMode::OTA_UPDATE;
  cmd.ota_percent = total ? static_cast<uint8_t>((progress * 100u) / total) : 0;
  xQueueOverwrite(g_display_mailbox, &cmd);
}

/** ArduinoOTA onError callback: clears the in-progress bit and logs. */
void handleOtaError(ota_error_t error) {
  xEventGroupClearBits(g_sys_events, kOtaInProgressBit);
  Serial.printf("[Network] OTA error [%u]\n", static_cast<unsigned>(error));
}

/**
 * Connects to WiFi (blocking): WiFiManager, AP name "Eureka setup", no
 * config-portal timeout, reboot after the portal saves new
 * credentials. Portal-active/saving status is logged, not shown on
 * screen -- Display task exclusively owns the display, and Network
 * task has no way to draw to it directly.
 */
void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kMdnsHostname);

  WiFiManager wm;
  bool portalWebServerStarted = false;
  wm.setWebServerCallback([&portalWebServerStarted]() { portalWebServerStarted = true; });
  wm.setAPCallback([](WiFiManager *) {
    Serial.println("[Network] WiFi config portal active: 'Eureka setup'");
  });
  wm.setSaveConfigCallback([]() { Serial.println("[Network] WiFi credentials saved"); });

  // Blocks here until WiFi is connected or the portal saves new
  // credentials. Only Network task blocks; every other task is already
  // running independently.
  wm.autoConnect(kApName);

  if (portalWebServerStarted) {
    // Start clean rather than continuing in the same runtime once new
    // credentials were saved via the portal.
    delay(100);
    ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED) {
    xEventGroupSetBits(g_sys_events, kWifiConnectedBit);
    Serial.printf("[Network] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[Network] WiFi not connected; continuing offline");
  }
}

/** Starts mDNS (eureka.local) and ArduinoOTA, and advertises both services. */
void setupMdnsAndOta() {
  if (!MDNS.begin(kMdnsHostname)) {
    Serial.println("[Network] mDNS init failed");
  } else {
    MDNS.addService("http", "tcp", SERVER_PORT);
    Serial.printf("[Network] mDNS up: %s.local\n", kMdnsHostname);
  }

  ArduinoOTA.setHostname(kMdnsHostname);
  ArduinoOTA.setMdnsEnabled(false);  // Avoid a second MDNS.begin(); we already own mDNS above.
  ArduinoOTA.setPort(kOtaPort);
  ArduinoOTA.onStart(handleOtaStart);
  ArduinoOTA.onEnd(handleOtaEnd);
  ArduinoOTA.onProgress(handleOtaProgress);
  ArduinoOTA.onError(handleOtaError);
  ArduinoOTA.begin();

  // Advertise the OTA service ourselves since ArduinoOTA's own mDNS
  // integration is disabled above.
  MDNS.addService("arduino", "tcp", kOtaPort);
}

/**
 * GET /api/getDosage?grams=N: cheap syntactic validation, then forwards
 * to Dosing task's dose-request queue.
 */
void handleGetDosage(AsyncWebServerRequest *request) {
  JsonDocument doc;
  String out;

  if (!request->hasParam("grams")) {
    doc["error"] = "missing parameter: grams";
    serializeJson(doc, out);
    request->send(400, "application/json", out);
    return;
  }

  float grams = request->getParam("grams")->value().toFloat();
  if (!std::isfinite(grams) || grams <= 0.0f || grams > kMaxDoseGrams) {
    doc["error"] = kGramsOutOfRange;
    serializeJson(doc, out);
    request->send(400, "application/json", out);
    return;
  }

  bool discard_training = request->hasParam("discard_training") &&
                           request->getParam("discard_training")->value() == "true";
  DoseRequest req{grams, g_next_dose_request_id++, discard_training};
  if (xQueueSend(g_dose_request_q, &req, 0) != pdTRUE) {
    doc["error"] = "device busy, try again";
    serializeJson(doc, out);
    request->send(503, "application/json", out);
    return;
  }

  doc["accepted_grams"] = grams;
  doc["request_id"] = req.request_id;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

/**
 * Mounts LittleFS and starts the AsyncWebServer: the WS at "/ws",
 * GET /api/getDosage, and the SPA served from "/". SPA routing there
 * is hash-based ("#/settings", not "/settings"), so there's no need
 * for a catch-all fallback to index.html on unknown paths -- every
 * real HTTP request is either "/", a concrete built asset, or an
 * API/WS endpoint, and 404 is the honest answer for anything else.
 */
void setupWebServer() {
  // formatOnFail=true: a device that's never had its filesystem image
  // uploaded has a raw/erased partition, not a missing one -- format
  // it as LittleFS on first mount rather than failing forever.
  if (!LittleFS.begin(true)) {
    Serial.println("[Network] LittleFS mount failed -- SPA assets unavailable");
  }

  g_ws.onEvent(onWsEvent);
  g_server.addHandler(&g_ws);

  g_server.on("/api/getDosage", HTTP_GET, handleGetDosage);

  g_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  g_server.onNotFound(
      [](AsyncWebServerRequest *request) { request->send(404, "text/plain", "not found"); });

  g_server.begin();
  Serial.printf("[Network] AsyncWebServer started on port %u\n", SERVER_PORT);
}

/** Network task entry point: connects WiFi, starts mDNS/OTA/the web server, then serves them. */
void networkTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE, portMAX_DELAY);

  setupWifi();
  setupMdnsAndOta();
  setupWebServer();

  SettingsSnapshot snap;
  uint32_t lastSeenVersion = 0;

  for (;;) {
    // Re-broadcast the settings snapshot to WS clients on every version
    // change, and act on the WiFi reset/reboot flags.
    if (xQueuePeek(g_settings_mailbox_network, &snap, 0) == pdTRUE &&
        snap.version != lastSeenVersion) {
      lastSeenVersion = snap.version;
      g_ws.textAll(buildSettingsJson(snap));

      if (snap.wifi_reset_flag) {
        Serial.println("[Network] wifi_reset_flag set -- clearing WiFi credentials, rebooting");
        WiFiManager wm;
        wm.resetSettings();
        delay(100);
        ESP.restart();
      } else if (snap.wifi_reboot_flag) {
        Serial.println("[Network] wifi_reboot_flag set -- rebooting");
        delay(100);
        ESP.restart();
      }
    }

    // Drain Telemetry task's WS-broadcast inbox and fan each event out
    // to every connected client.
    TelemetryEvent ev;
    while (xQueueReceive(g_ws_broadcast_q, &ev, 0) == pdTRUE) {
      if (ev.type == TelemetryType::MODEL_STATE) {
        g_last_model_state = ev;
        g_have_model_state = true;
      }
      g_ws.textAll(buildTelemetryJson(ev));
    }

    g_ws.cleanupClients();

    // Only pump ArduinoOTA's protocol handling while it's safe to --
    // see the OTA section above for why this, not onStart alone, is the
    // real "refuse before it starts" gate.
    if (otaSafeToStart()) {
      ArduinoOTA.handle();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void createNetworkTask() {
  xTaskCreatePinnedToCore(networkTaskFn, "Network", TaskConfig::kNetworkStackBytes, nullptr,
                           TaskConfig::kNetworkPriority, nullptr, TaskConfig::kNetworkCore);
}
