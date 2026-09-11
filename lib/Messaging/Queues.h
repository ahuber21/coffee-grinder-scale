#pragma once

/**
 * Global queue/mailbox handles and the startup readiness event group.
 * Declared here so every task library can wire itself up without
 * owning another task's queue creation -- initQueuesAndEvents() creates
 * all of them once, from src/main.cpp, before any task starts.
 *
 * Two message shapes:
 *   - queue (depth > 1): every item matters, never overwritten.
 *   - mailbox (depth 1, written via xQueueOverwrite): only the latest
 *     value matters. A mailbox with more than one subscriber gets one
 *     handle per subscriber so each reader can xQueuePeek() without
 *     stealing the value from anyone else.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include "Messages.h"

// Scale -> Dosing.
constexpr UBaseType_t kScaleSampleQueueDepth = 8;
extern QueueHandle_t g_scale_sample_q;

// Dosing (and, narrowly, Network for OTA_UPDATE) -> Display.
extern QueueHandle_t g_display_mailbox;  // depth 1, xQueueOverwrite

// Button ISR -> Input -> Dosing.
constexpr UBaseType_t kButtonEdgeQueueDepth = 8;
constexpr UBaseType_t kButtonPressQueueDepth = 4;
extern QueueHandle_t g_button_edge_q;
extern QueueHandle_t g_button_press_q;

// Dosing -> Telemetry.
constexpr UBaseType_t kTelemetryQueueDepth = 32;
extern QueueHandle_t g_telemetry_q;

// Network -> Dosing.
constexpr UBaseType_t kDoseRequestQueueDepth = 2;
extern QueueHandle_t g_dose_request_q;

// Settings snapshot distribution: one mailbox per subscriber.
extern QueueHandle_t g_settings_mailbox_scale;
extern QueueHandle_t g_settings_mailbox_dosing;
extern QueueHandle_t g_settings_mailbox_input;
extern QueueHandle_t g_settings_mailbox_network;

// Settings write path: Network -> Settings.
constexpr UBaseType_t kSettingsWriteQueueDepth = 4;
extern QueueHandle_t g_settings_write_q;

// Topup-model persistence round trip.
extern QueueHandle_t g_topup_model_mailbox;  // Settings -> Dosing, depth 1, read once at boot
constexpr UBaseType_t kPersistRequestQueueDepth = 2;
extern QueueHandle_t g_persist_request_q;    // Dosing -> Settings, session-boundary only

// Telemetry -> Network WS broadcast.
constexpr UBaseType_t kWsBroadcastQueueDepth = 16;
extern QueueHandle_t g_ws_broadcast_q;

// Startup readiness / status event group.
extern EventGroupHandle_t g_sys_events;

constexpr EventBits_t kSettingsLoadedBit = BIT0;
constexpr EventBits_t kScaleReadyBit = BIT1;
constexpr EventBits_t kDisplayReadyBit = BIT2;
constexpr EventBits_t kWifiConnectedBit = BIT3;  // not part of the startup boot gate
constexpr EventBits_t kOtaInProgressBit = BIT4;
constexpr EventBits_t kDosingActiveBit = BIT5;   // set/cleared outside IDLE/SCREENSAVER

constexpr EventBits_t kBootGateBits =
    kSettingsLoadedBit | kScaleReadyBit | kDisplayReadyBit;

/**
 * Creates every queue/mailbox/event group above. Must run once, before
 * any task that touches them is created -- called from
 * src/main.cpp::setup().
 */
void initQueuesAndEvents();
