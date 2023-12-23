#pragma once

#include <Arduino.h>
#include <AsyncWebSocket.h>
#include <ESPAsyncWebServer.h>

#include <ArduinoJson.h>

class WebSocketGraph {
public:
  WebSocketGraph();
  void begin(AsyncWebServer *server);
  void resetGraph(float target_weight);
  void updateGraphData(float seconds, float weight);

private:
  void handleWebSocketEvent(AsyncWebSocket *server,
                            AsyncWebSocketClient *client, AwsEventType type,
                            void *arg, uint8_t *data, size_t len);

  // Constants
  static const char PROGMEM graph_html[];

  // Member variables
  AsyncWebSocket _ws;
  AsyncWebServer *_server;
};