/**
 * \file TopupLogger.h
 * @author Andreas Huber (ahuber1121@gmail.com)
 * \brief Sends out log events to a web server
 * @version 0.1
 * \date 2024-10-14
 *
 * @copyright Copyright (c) 2024
 *
 */

#pragma once

#include <Arduino.h>

class WebSocketLogger;

class TopupLogger {
public:
  TopupLogger();
  void begin(const char *endpoint, const WebSocketLogger *logger);
  void logTopupData(unsigned long runTimeMillis, float weightIncrement);

private:
  const char *_endpoint;
  const WebSocketLogger *_logger;
};
