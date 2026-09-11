#pragma once

// Task priorities/cores/stack sizes, taken directly from
// .agent/design/rtos-architecture.md §1 and §6.7. Centralized here so
// src/main.cpp's xTaskCreatePinnedToCore calls are the single place that
// reads these, and so the reasoning in the design doc has one obvious home
// in code to point back to.
//
// Note: unlike vanilla FreeRTOS (where xTaskCreate's stack-depth argument is
// a word count), the ESP-IDF FreeRTOS port used by Arduino-ESP32 takes
// xTaskCreatePinnedToCore's stack-depth argument in *bytes* -- these
// constants are exactly the byte figures from the design doc's table,
// unconverted.

#include <cstdint>

#include <freertos/FreeRTOS.h>

namespace TaskConfig {

constexpr int kCoreApp = 1;  // Scale/Dosing/Input/Display -- §6.7
constexpr int kCorePro = 0;  // Network/Telemetry/Settings -- §6.7

// #1 Scale Sampling -- §6.1
constexpr UBaseType_t kScalePriority = 9;
constexpr uint32_t kScaleStackBytes = 3072;
constexpr int kScaleCore = kCoreApp;

// #2 Dosing/Session Control -- §6.2
constexpr UBaseType_t kDosingPriority = 8;
constexpr uint32_t kDosingStackBytes = 4096;
constexpr int kDosingCore = kCoreApp;

// #3 Input -- §6.3
constexpr UBaseType_t kInputPriority = 7;
constexpr uint32_t kInputStackBytes = 2048;
constexpr int kInputCore = kCoreApp;

// #4 Display -- §6.4
constexpr UBaseType_t kDisplayPriority = 4;
constexpr uint32_t kDisplayStackBytes = 4096;
constexpr int kDisplayCore = kCoreApp;

// #5 Settings/NVS -- §6.5
constexpr UBaseType_t kSettingsPriority = 3;
constexpr uint32_t kSettingsStackBytes = 4096;
constexpr int kSettingsCore = kCorePro;

// #6 Network -- §6.5
constexpr UBaseType_t kNetworkPriority = 3;
constexpr uint32_t kNetworkStackBytes = 8192;
constexpr int kNetworkCore = kCorePro;

// #7 Telemetry -- §6.6. Stack bumped from the design doc's original 6144 to
// 8192 (matching Network's) once the real body used HTTPClient/WiFiClient --
// their internal header/response buffers push noticeably past what the
// queue-drain-and-forward stub needed.
constexpr UBaseType_t kTelemetryPriority = 2;
constexpr uint32_t kTelemetryStackBytes = 8192;
constexpr int kTelemetryCore = kCorePro;

}  // namespace TaskConfig
