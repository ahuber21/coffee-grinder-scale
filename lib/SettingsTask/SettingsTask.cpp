#include "SettingsTask.h"

#include <Arduino.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "DosingModel.h"
#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"

namespace {

SettingsSnapshot g_settings;  // canonical copy -- only this task ever writes it
TopupModelV1 g_topup_model;   // canonical cold copy -- ditto

// --- NVS stand-ins (§4/§5) ---------------------------------------------
//
// Per the task brief: real NVS read/write is a follow-up. These stubs give
// the rest of the task the exact same call shape a real Preferences-backed
// implementation would have, so swapping them out later doesn't touch
// anything else in this file.

bool loadSettingsFromNvs(SettingsSnapshot &out) {
  (void)out;
  Serial.println("[Settings] NVS load stubbed -- using compiled-in defaults");
  return false;  // "no valid data" -> caller falls back to defaults, like AR-010's fix
}

void saveSettingsToNvs(const SettingsSnapshot &snap) {
  (void)snap;
  Serial.println("[Settings] NVS save stubbed (not persisted)");
}

bool loadTopupModelFromNvs(TopupModelV1 &out) {
  (void)out;
  Serial.println("[Settings] Topup model NVS load stubbed -- using cold-start priors");
  return false;
}

void saveTopupModelToNvs(const TopupModelV1 &model) {
  (void)model;
  Serial.println("[Settings] Topup model NVS save stubbed (not persisted)");
}

// --- Distribution (§4) ---------------------------------------------------

void broadcastSnapshot() {
  xQueueOverwrite(g_settings_mailbox_scale, &g_settings);
  xQueueOverwrite(g_settings_mailbox_dosing, &g_settings);
  xQueueOverwrite(g_settings_mailbox_input, &g_settings);
  xQueueOverwrite(g_settings_mailbox_network, &g_settings);
}

// --- Write-path validation (§4, the AR-016 fix: settings task is the sole
// authority on "is this legal", not the door it came in through) ---------

bool applyWrite(const SettingsWriteRequest &req) {
  switch (req.field_id) {
    case SettingsFieldId::CALIBRATION_FACTOR:
      if (!std::isfinite(req.value.f) || req.value.f == 0.0f) return false;
      g_settings.calibration_factor = req.value.f;
      return true;
    case SettingsFieldId::TARGET_DOSE_SINGLE:
      if (!std::isfinite(req.value.f) || req.value.f <= 0.0f || req.value.f > 100.0f)
        return false;
      g_settings.target_dose_single = req.value.f;
      return true;
    case SettingsFieldId::TARGET_DOSE_DOUBLE:
      if (!std::isfinite(req.value.f) || req.value.f <= 0.0f || req.value.f > 100.0f)
        return false;
      g_settings.target_dose_double = req.value.f;
      return true;
    case SettingsFieldId::TOP_UP_MARGIN_SINGLE:
      if (!std::isfinite(req.value.f) || req.value.f < 0.0f) return false;
      g_settings.top_up_margin_single = req.value.f;
      return true;
    case SettingsFieldId::TOP_UP_MARGIN_DOUBLE:
      if (!std::isfinite(req.value.f) || req.value.f < 0.0f) return false;
      g_settings.top_up_margin_double = req.value.f;
      return true;
    case SettingsFieldId::BUTTON_DEBOUNCE_MS:
      if (req.value.u == 0 || req.value.u > 2000) return false;
      g_settings.button_debounce_ms = req.value.u;
      return true;
    case SettingsFieldId::WIFI_RESET_FLAG:
      g_settings.wifi_reset_flag = req.value.b;
      return true;
    case SettingsFieldId::WIFI_REBOOT_FLAG:
      g_settings.wifi_reboot_flag = req.value.b;
      return true;
  }
  return false;
}

// Plausibility bounds mirroring topup-model.md §4.4's fallback checks --
// same discipline as applyWrite() above, applied to the persisted model
// blob instead of a scalar setting.
bool isPlausibleTopupModel(const TopupModelV1 &m) {
  if (!std::isfinite(m.rate_hat) || m.rate_hat < 0.3f || m.rate_hat > 2.5f) return false;
  if (!std::isfinite(m.topup_slope) || m.topup_slope < 0.05f || m.topup_slope > 5.0f)
    return false;
  if (!std::isfinite(m.topup_deadtime_ms) || m.topup_deadtime_ms < 0.0f ||
      m.topup_deadtime_ms > 1000.0f)
    return false;
  if (!std::isfinite(m.coast_weight_hat) || m.coast_weight_hat < 0.0f ||
      m.coast_weight_hat > 3.0f)
    return false;
  return true;
}

void settingsTaskFn(void *) {
  g_settings = SettingsSnapshot{};  // compiled-in defaults (field initializers in Messages.h)
  if (!loadSettingsFromNvs(g_settings)) {
    g_settings = SettingsSnapshot{};
  }
  g_settings.version = 1;

  g_topup_model = makeDefaultTopupModel();
  if (!loadTopupModelFromNvs(g_topup_model)) {
    g_topup_model = makeDefaultTopupModel();
  }

  broadcastSnapshot();
  xQueueOverwrite(g_topup_model_mailbox, &g_topup_model);

  // Every subscriber's first read is now guaranteed to be a valid, fully
  // loaded snapshot -- never a default-constructed placeholder racing
  // against setup() (§4/§8).
  xEventGroupSetBits(g_sys_events, kSettingsLoadedBit);

  for (;;) {
    SettingsWriteRequest write;
    bool changed = false;
    while (xQueueReceive(g_settings_write_q, &write, 0) == pdTRUE) {
      if (applyWrite(write)) {
        changed = true;
      } else {
        Serial.printf("[Settings] rejected write, field_id=%u request_id=%u\n",
                      static_cast<unsigned>(write.field_id), write.request_id);
      }
    }
    if (changed) {
      g_settings.version++;
      saveSettingsToNvs(g_settings);
      broadcastSnapshot();
    }

    PersistRequest persist;
    while (xQueueReceive(g_persist_request_q, &persist, 0) == pdTRUE) {
      if (persist.blob_id == PersistBlobId::TOPUP_MODEL_V1 &&
          isPlausibleTopupModel(persist.payload)) {
        g_topup_model = persist.payload;
        saveTopupModelToNvs(g_topup_model);
      } else {
        Serial.printf("[Settings] rejected topup-model persist, request_id=%u\n",
                      persist.request_id);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void createSettingsTask() {
  xTaskCreatePinnedToCore(settingsTaskFn, "Settings",
                           TaskConfig::kSettingsStackBytes, nullptr,
                           TaskConfig::kSettingsPriority, nullptr,
                           TaskConfig::kSettingsCore);
}
