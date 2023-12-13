/**
 * \file wslogger.h
 * @author Andreas Huber (ahuber1121@gmail.com)
 * \brief Creates a websocket that can be attached to a server for logging
 * @version 0.1
 * \date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 */

#pragma once

#include <Arduino.h>
#include <AsyncWebSocket.h>

#include "defines.h"

namespace wslogger {
void onConnect(AsyncWebServerRequest *request);

void handleWebSocketData(AsyncWebSocketClient *client, uint8_t *data,
                         size_t len);

void sendWelcomeMessage(AsyncWebSocketClient *client);

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len);
} // namespace wslogger

class WSLogger {
public:
  WSLogger() : _ws("/ws") {}

  void begin(AsyncWebServer *srv);

  void print(const String &message);

  void println(const String &message) { print(message + "\n"); }

  template <typename T> void print(T message) { print(String(message)); }

  template <typename T> void println(T message) { println(String(message)); }

private:
  AsyncWebServer *_server;
  AsyncWebSocket _ws;
};