#include "WebSocketMetrics.h"
#include <ArduinoJson.h>
#include "WebSocketLogger.h"

WebSocketMetrics::WebSocketMetrics()
    : _ws("/MetricsWebSocket"), _server(nullptr), _logger(nullptr),
      _replayIndex(0), _lastProgressMillis(0) {}

void WebSocketMetrics::begin(AsyncWebServer *server, const WebSocketLogger *logger) {
  _server = server;
  _logger = logger;
  _server->addHandler(&_ws);
  _server->on(
      "/metrics", HTTP_GET,
      [this](AsyncWebServerRequest *request) { request->send(200, "text/plain", "Metrics WS at /MetricsWebSocket"); });
  _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    this->handleEvent(server, client, type, arg, data, len);
  });
}

void WebSocketMetrics::handleEvent(AsyncWebSocket *server,
                                   AsyncWebSocketClient *client,
                                   AwsEventType type, void *arg, uint8_t *data,
                                   size_t len) {
  if (type == WS_EVT_CONNECT) {
    if (_logger) _logger->println("[metrics] client connected");
    replayTo(client);
  } else if (type == WS_EVT_DISCONNECT) {
    if (_logger) _logger->println("[metrics] client disconnected");
  }
}

void WebSocketMetrics::broadcastAndStore(const char *json) {
  _ws.textAll(json);
  _replay[_replayIndex] = json;
  _replayIndex = (++_replayIndex) % REPLAY_SIZE;
}

void WebSocketMetrics::replayTo(AsyncWebSocketClient *client) {
  // Send stored events in order of oldest to newest
  uint8_t start = _replayIndex; // first to send
  for (uint8_t i = 0; i < REPLAY_SIZE; ++i) {
    uint8_t idx = (start + i) % REPLAY_SIZE;
    if (_replay[idx].length()) {
      client->text(_replay[idx]);
    }
  }
}

void WebSocketMetrics::sendTarget(float targetWeight) {
  StaticJsonDocument<48> doc;
  doc["type"] = "target";
  doc["target_weight"] = targetWeight;
  char buf[64];
  serializeJson(doc, buf, sizeof(buf));
  broadcastAndStore(buf);
}

void WebSocketMetrics::sendProgress(float seconds, float weight) {
  uint32_t now = millis();
  if (now - _lastProgressMillis < 150) return; // throttle ~6-7 Hz separate from graph
  _lastProgressMillis = now;
  StaticJsonDocument<64> doc;
  doc["type"] = "progress";
  doc["seconds"] = seconds;
  doc["weight"] = weight;
  char buf[80];
  serializeJson(doc, buf, sizeof(buf));
  broadcastAndStore(buf);
}

void WebSocketMetrics::sendTopUp(unsigned long runtimeMillis, float deltaGrams) {
  StaticJsonDocument<72> doc;
  doc["type"] = "topup";
  doc["runtime_ms"] = runtimeMillis;
  doc["delta_g"] = deltaGrams;
  char buf[96];
  serializeJson(doc, buf, sizeof(buf));
  broadcastAndStore(buf);
}

void WebSocketMetrics::sendFinalize(float seconds, float finalWeight) {
  StaticJsonDocument<64> doc;
  doc["type"] = "finalize";
  doc["seconds"] = seconds;
  doc["weight"] = finalWeight;
  char buf[80];
  serializeJson(doc, buf, sizeof(buf));
  broadcastAndStore(buf);
}
