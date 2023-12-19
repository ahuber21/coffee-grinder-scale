#ifndef WEBSOCKET_HANDLER_H
#define WEBSOCKET_HANDLER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <vector>

class WebSocketHandler : public AsyncWebSocket {
public:
  WebSocketHandler(const String& url) : AsyncWebSocket(url) {}
  void begin();
  void registerOnConnectHandler(void (*handler)(AsyncWebSocketClient *client));
  void registerOnDisconnectHandler(void (*handler)(AsyncWebSocketClient *client));
  void registerOnDataHandler(void (*handler)(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len));

private:
  std::vector<void (*)(AsyncWebSocketClient *client)> onConnectHandlers;
  std::vector<void (*)(AsyncWebSocketClient *client)> onDisconnectHandlers;
  std::vector<void (*)(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len)> onDataHandlers;

  void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
};

#endif
