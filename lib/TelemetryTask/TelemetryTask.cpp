#include "TelemetryTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"

namespace {

uint32_t g_dropped_events = 0;

void telemetryTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE,
                       portMAX_DELAY);

  TelemetryEvent ev;
  for (;;) {
    // Best-effort, lowest priority in the app band (§6.6) -- block briefly
    // rather than busy-poll; Dosing task never waits on us either way
    // (its own sends are zero-timeout, §3.4).
    if (xQueueReceive(g_telemetry_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (ev.type == TelemetryType::LOG_LINE) {
        Serial.printf("[Telemetry] %s\n", ev.log_line);
      }

      // Forward a subset to Network's WS-broadcast inbox (§7). Zero-timeout
      // send with a drop-and-count policy, matching the drop discipline
      // used everywhere else non-critical data crosses a queue boundary.
      if (xQueueSend(g_ws_broadcast_q, &ev, 0) != pdTRUE) {
        g_dropped_events++;
      }

      // The eventual per-session buffer for the PostgREST POST (fired on
      // COMPLETE) accumulates here once Network's HTTP stack exists.
    }
  }
}

}  // namespace

void createTelemetryTask() {
  xTaskCreatePinnedToCore(telemetryTaskFn, "Telemetry",
                           TaskConfig::kTelemetryStackBytes, nullptr,
                           TaskConfig::kTelemetryPriority, nullptr,
                           TaskConfig::kTelemetryCore);
}
