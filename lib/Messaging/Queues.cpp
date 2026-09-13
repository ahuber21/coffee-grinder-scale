#include "Queues.h"

QueueHandle_t g_scale_sample_q = nullptr;
QueueHandle_t g_latest_sample_mailbox = nullptr;
QueueHandle_t g_display_mailbox = nullptr;
QueueHandle_t g_button_edge_q = nullptr;
QueueHandle_t g_button_press_q = nullptr;
QueueHandle_t g_telemetry_q = nullptr;
QueueHandle_t g_dose_request_q = nullptr;
QueueHandle_t g_tare_request_q = nullptr;
QueueHandle_t g_tare_result_q = nullptr;
QueueHandle_t g_settings_mailbox_scale = nullptr;
QueueHandle_t g_settings_mailbox_dosing = nullptr;
QueueHandle_t g_settings_mailbox_input = nullptr;
QueueHandle_t g_settings_mailbox_network = nullptr;
QueueHandle_t g_settings_write_q = nullptr;
QueueHandle_t g_topup_model_mailbox = nullptr;
QueueHandle_t g_persist_request_q = nullptr;
QueueHandle_t g_ws_broadcast_q = nullptr;

EventGroupHandle_t g_sys_events = nullptr;

void initQueuesAndEvents() {
  g_scale_sample_q = xQueueCreate(kScaleSampleQueueDepth, sizeof(ScaleSample));
  g_latest_sample_mailbox = xQueueCreate(1, sizeof(ScaleSample));

  g_display_mailbox = xQueueCreate(1, sizeof(DisplayCommand));

  g_button_edge_q = xQueueCreate(kButtonEdgeQueueDepth, sizeof(ButtonEdge));
  g_button_press_q = xQueueCreate(kButtonPressQueueDepth, sizeof(ButtonPress));

  g_telemetry_q = xQueueCreate(kTelemetryQueueDepth, sizeof(TelemetryEvent));

  g_dose_request_q = xQueueCreate(kDoseRequestQueueDepth, sizeof(DoseRequest));
  g_tare_request_q = xQueueCreate(kTareRequestQueueDepth, sizeof(TareRequest));
  g_tare_result_q = xQueueCreate(kTareResultQueueDepth, sizeof(TareResult));

  g_settings_mailbox_scale = xQueueCreate(1, sizeof(SettingsSnapshot));
  g_settings_mailbox_dosing = xQueueCreate(1, sizeof(SettingsSnapshot));
  g_settings_mailbox_input = xQueueCreate(1, sizeof(SettingsSnapshot));
  g_settings_mailbox_network = xQueueCreate(1, sizeof(SettingsSnapshot));

  g_settings_write_q = xQueueCreate(kSettingsWriteQueueDepth, sizeof(SettingsWriteRequest));

  g_topup_model_mailbox = xQueueCreate(1, sizeof(TopupModelV1));
  g_persist_request_q = xQueueCreate(kPersistRequestQueueDepth, sizeof(PersistRequest));

  g_ws_broadcast_q = xQueueCreate(kWsBroadcastQueueDepth, sizeof(TelemetryEvent));

  g_sys_events = xEventGroupCreate();

  configASSERT(g_scale_sample_q && g_latest_sample_mailbox && g_display_mailbox &&
               g_button_edge_q && g_button_press_q && g_telemetry_q &&
               g_dose_request_q && g_tare_request_q && g_tare_result_q &&
               g_settings_mailbox_scale && g_settings_mailbox_dosing &&
               g_settings_mailbox_input && g_settings_mailbox_network &&
               g_settings_write_q && g_topup_model_mailbox && g_persist_request_q &&
               g_ws_broadcast_q && g_sys_events);
}
