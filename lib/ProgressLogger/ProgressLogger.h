/**
 * \file ProgressLogger.h
 * @author Andreas Huber (ahuber1121@gmail.com)
 * \brief Sends out log events to a web server
 * @version 0.1
 * \date 2025-04-24
 *
 * @copyright Copyright (c) 2025
 *
 */

#pragma once

#include <Arduino.h>

class WebSocketLogger;

class ProgressLogger {
 public:
  ProgressLogger();
  void begin(const char *endpoint, const WebSocketLogger *logger);
  void logData(unsigned long runTimeMillis, float weight);

 private:
  unsigned long _last_log_millis;
  const char *_endpoint;
  const WebSocketLogger *_logger;
};
