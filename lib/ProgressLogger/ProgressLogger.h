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
#include <vector>

#define SEND_INTERVAL 10000 // 10 seconds interval for sending data
#define SEND_TIMEOUT 1000 // 1 second timeout for sending data

class WebSocketLogger;

struct LogEntry {
  unsigned long runTimeMillis;
  float weight;
};

class ProgressLogger {
 public:
  ProgressLogger();
  void begin(const char *endpoint, const WebSocketLogger *logger,
             unsigned long minLogInterval = 200);
  void logData(unsigned long runTimeMillis, float weight);
  void update();

  private:
  void sendBufferedData();
  void handleSendingTimeout();

  unsigned long _min_log_interval = 200; // 5 Hz default (ms)
  String _endpoint;
  const WebSocketLogger *_logger;
  std::vector<LogEntry> _buffer;
  bool _sending = false;
  unsigned long _sending_start_millis = 0;
  size_t _max_buffer_size = 100; // Maximum buffer size before sending

};
