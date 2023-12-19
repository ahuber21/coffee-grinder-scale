#include "WebSocketHandler.h"

void WebSocketHandler::begin() {
  this->onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data,
                       size_t len) {
    this->onWebSocketEvent(server, client, type, arg, data, len);
  });
}

void WebSocketHandler::registerOnConnectHandler(
    void (*handler)(AsyncWebSocketClient *client)) {
  onConnectHandlers.push_back(handler);
}

void WebSocketHandler::registerOnDisconnectHandler(
    void (*handler)(AsyncWebSocketClient *client)) {
  onDisconnectHandlers.push_back(handler);
}

void WebSocketHandler::registerOnDataHandler(void (*handler)(
    AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len)) {
  onDataHandlers.push_back(handler);
}

void WebSocketHandler::onWebSocketEvent(AsyncWebSocket *server,
                                        AsyncWebSocketClient *client,
                                        AwsEventType type, void *arg,
                                        uint8_t *data, size_t len) {
  switch (type) {
  case WS_EVT_CONNECT:
    for (auto handler : onConnectHandlers) {
      handler(client);
    }
    break;
  case WS_EVT_DISCONNECT:
    for (auto handler : onDisconnectHandlers) {
      handler(client);
    }
    break;
  case WS_EVT_DATA:
    for (auto handler : onDataHandlers) {
      handler(client, arg, data, len);
    }
    break;
  }
}
