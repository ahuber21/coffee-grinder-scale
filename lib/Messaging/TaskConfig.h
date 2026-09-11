#pragma once

/**
 * Task priorities/cores/stack sizes, centralized here so every
 * xTaskCreatePinnedToCore call reads from one place.
 *
 * Note: unlike vanilla FreeRTOS (stack-depth in words), the ESP-IDF
 * port Arduino-ESP32 uses takes xTaskCreatePinnedToCore's stack-depth
 * argument in *bytes*.
 */

#include <cstdint>

#include <freertos/FreeRTOS.h>

namespace TaskConfig {

constexpr int kCoreApp = 1;  // Scale/Dosing/Input/Display.
constexpr int kCorePro = 0;  // Network/Telemetry/Settings.

// #1 Scale Sampling.
constexpr UBaseType_t kScalePriority = 9;
constexpr uint32_t kScaleStackBytes = 3072;
constexpr int kScaleCore = kCoreApp;

// #2 Dosing/Session Control.
constexpr UBaseType_t kDosingPriority = 8;
constexpr uint32_t kDosingStackBytes = 4096;
constexpr int kDosingCore = kCoreApp;

// #3 Input.
constexpr UBaseType_t kInputPriority = 7;
constexpr uint32_t kInputStackBytes = 2048;
constexpr int kInputCore = kCoreApp;

// #4 Display.
constexpr UBaseType_t kDisplayPriority = 4;
constexpr uint32_t kDisplayStackBytes = 4096;
constexpr int kDisplayCore = kCoreApp;

// #5 Settings/NVS.
constexpr UBaseType_t kSettingsPriority = 3;
constexpr uint32_t kSettingsStackBytes = 4096;
constexpr int kSettingsCore = kCorePro;

// #6 Network.
constexpr UBaseType_t kNetworkPriority = 3;
constexpr uint32_t kNetworkStackBytes = 8192;
constexpr int kNetworkCore = kCorePro;

// #7 Telemetry. Stack bumped from an original 6144 to 8192 (matching
// Network's) once the real body used HTTPClient/WiFiClient -- their
// internal buffers push past what a queue-drain-and-forward stub needed.
constexpr UBaseType_t kTelemetryPriority = 2;
constexpr uint32_t kTelemetryStackBytes = 8192;
constexpr int kTelemetryCore = kCorePro;

}  // namespace TaskConfig
