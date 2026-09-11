#include "NetworkTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"

bool otaSafeToStart() {
  // D12: refuse, don't abort. DOSING_ACTIVE is set/cleared by Dosing task on
  // entering/leaving anything other than IDLE/SCREENSAVER (see DosingTask.cpp).
  EventBits_t bits = xEventGroupGetBits(g_sys_events);
  return (bits & kDosingActiveBit) == 0;
}

namespace {

void networkTaskFn(void *) {
  xEventGroupWaitBits(g_sys_events, kBootGateBits, pdFALSE, pdTRUE,
                       portMAX_DELAY);

  // WiFiManager/AsyncWebServer/ArduinoOTA bring-up is out of scope for this
  // skeleton (per the task brief) -- this is where it would live. The
  // device must remain usable offline either way (§8: WIFI_CONNECTED is
  // deliberately not part of the boot gate above).

  SettingsSnapshot snap;
  uint32_t lastSeenVersion = 0;

  for (;;) {
    if (xQueuePeek(g_settings_mailbox_network, &snap, 0) == pdTRUE &&
        snap.version != lastSeenVersion) {
      lastSeenVersion = snap.version;
      // A real implementation re-broadcasts the snapshot to WS clients
      // driving the live settings UI here.
    }

    // §7: drain the WS-broadcast inbox Telemetry task feeds; a real
    // implementation calls ws.textAll() per entry and ws.cleanupClients()
    // once per tick (the direct AR-013 fix). Stubbed to a log line here.
    TelemetryEvent ev;
    while (xQueueReceive(g_ws_broadcast_q, &ev, 0) == pdTRUE) {
      Serial.printf("[Network] would broadcast telemetry type=%u session=%u\n",
                    static_cast<unsigned>(ev.type), ev.session_id);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void createNetworkTask() {
  xTaskCreatePinnedToCore(networkTaskFn, "Network",
                           TaskConfig::kNetworkStackBytes, nullptr,
                           TaskConfig::kNetworkPriority, nullptr,
                           TaskConfig::kNetworkCore);
}
