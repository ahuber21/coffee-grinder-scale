#pragma once
#include <Arduino.h>
#include <AsyncWebSocket.h>
#include <ESPAsyncWebServer.h>

class WebSocketLogger;

// Lightweight metrics streaming over a dedicated websocket.
// Messages are small JSON objects with a 'type' discriminator.
// Replay of last N events is sent to new clients.
class WebSocketMetrics {
public:
  WebSocketMetrics();
  void begin(AsyncWebServer *server, const WebSocketLogger *logger);

  void sendTarget(float targetWeight);
  void sendProgress(float seconds, float weight); // throttled internally
  void sendTopUp(unsigned long runtimeMillis, float deltaGrams);
  void sendFinalize(float seconds, float finalWeight);

private:
  void handleEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len);
  void broadcastAndStore(const char *json);
  void replayTo(AsyncWebSocketClient *client);

  AsyncWebSocket _ws;
  AsyncWebServer *_server;
  const WebSocketLogger *_logger;

  static constexpr uint8_t REPLAY_SIZE = 48;
  String _replay[REPLAY_SIZE];
  uint8_t _replayIndex;

  uint32_t _lastProgressMillis;
};
