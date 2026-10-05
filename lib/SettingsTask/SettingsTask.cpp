#include "SettingsTask.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "DosingModel.h"
#include "Messages.h"
#include "Queues.h"
#include "SettingsSchema.h"
#include "TaskConfig.h"

namespace {

SettingsSnapshot g_settings;            ///< Canonical copy; only this task ever writes it.
DosingModelState g_dosing_model;        ///< Canonical cold copy of the dosing models' state.
LandingLearnerState g_landing_learner;  ///< Canonical cold copy of the landing learner's state.

// One handle, opened and closed per call, since NVS is only touched on an accepted change.
Preferences g_prefs;

constexpr const char *kNvsNamespace = "settings";

/// Bumped when the stored settings layout changes so that old data reloads from defaults.
constexpr uint32_t kSettingsSchemaVersion = 2;

constexpr const char *kKeySchemaVer = "schema_ver";
/// Keeps the name from before the state struct was renamed, so stored models still load.
constexpr const char *kKeyDosingModel = "topup_model";
constexpr const char *kKeyLandingLearner = "land_learner";

/** Reads one setting from NVS (open for reading), falling back to `fallback`. */
double loadSettingValue(const SettingDescriptor &d, double fallback) {
  switch (d.type) {
    case SettingType::U8: return g_prefs.getUChar(d.nvs_key, static_cast<uint8_t>(fallback));
    case SettingType::U32: return g_prefs.getUInt(d.nvs_key, static_cast<uint32_t>(fallback));
    case SettingType::F32: return g_prefs.getFloat(d.nvs_key, static_cast<float>(fallback));
    case SettingType::F64: return g_prefs.getDouble(d.nvs_key, fallback);
    case SettingType::BOOL: return g_prefs.getBool(d.nvs_key, fallback != 0.0) ? 1.0 : 0.0;
  }
  return fallback;
}

/** Writes one setting to NVS (open for writing). */
void saveSettingValue(const SettingDescriptor &d, double value) {
  switch (d.type) {
    case SettingType::U8: g_prefs.putUChar(d.nvs_key, static_cast<uint8_t>(value)); break;
    case SettingType::U32: g_prefs.putUInt(d.nvs_key, static_cast<uint32_t>(value)); break;
    case SettingType::F32: g_prefs.putFloat(d.nvs_key, static_cast<float>(value)); break;
    case SettingType::F64: g_prefs.putDouble(d.nvs_key, value); break;
    case SettingType::BOOL: g_prefs.putBool(d.nvs_key, value != 0.0); break;
  }
}

/**
 * Loads SettingsSnapshot from NVS. Returns false (leaving `out`
 * untouched) if the namespace doesn't exist yet or its schema_ver
 * doesn't match, and the caller then uses the compiled-in defaults.
 * Each field is validated on its own, so one corrupt value doesn't
 * discard the rest of a valid snapshot.
 */
bool loadSettingsFromNvs(SettingsSnapshot &out) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    Serial.println("[Settings] NVS namespace not found -- using compiled-in defaults");
    return false;
  }

  const uint32_t stored_schema = g_prefs.getUInt(kKeySchemaVer, 0);
  if (stored_schema != kSettingsSchemaVersion) {
    Serial.printf("[Settings] NVS schema_ver=%u (expected %u) -- using compiled-in defaults\n",
                  static_cast<unsigned>(stored_schema),
                  static_cast<unsigned>(kSettingsSchemaVersion));
    g_prefs.end();
    return false;
  }

  const SettingsSnapshot defaults{};
  out = defaults;

  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  for (size_t i = 0; i < count; ++i) {
    double value = loadSettingValue(table[i], readSetting(defaults, table[i]));
    if (settingAccepts(table[i], value)) {
      writeSetting(out, table[i], value);
    } else {
      Serial.printf("[Settings] NVS %s invalid -- using default\n", table[i].name);
    }
  }

  g_prefs.end();
  return true;
}

/**
 * Writes `snap` through to NVS. `snap` is always the already-validated
 * g_settings, so this is a plain write-through rather than a second gate.
 */
void saveSettingsToNvs(const SettingsSnapshot &snap) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Serial.println("[Settings] NVS open for write failed -- settings not persisted");
    return;
  }

  g_prefs.putUInt(kKeySchemaVer, kSettingsSchemaVersion);
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  for (size_t i = 0; i < count; ++i) {
    saveSettingValue(table[i], readSetting(snap, table[i]));
  }

  g_prefs.end();
}

/** Plausibility bounds for a persisted DosingModelState blob. */
bool isPlausibleDosingModel(const DosingModelState &m) {
  if (!std::isfinite(m.rate_hat) || m.rate_hat < 0.3f || m.rate_hat > 2.5f) return false;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    if (!std::isfinite(m.topup_lut_duration_ms[i]) || m.topup_lut_duration_ms[i] < 0.0f ||
        m.topup_lut_duration_ms[i] > 5000.0f)
      return false;
  }
  return std::isfinite(m.coast_weight_hat) && m.coast_weight_hat >= 0.0f &&
         m.coast_weight_hat <= 3.0f;
}

/** Plausibility bounds for a persisted LandingLearnerState: finite sums, non-negative weights. */
bool isPlausibleLandingLearner(const LandingLearnerState &s) {
  for (const LandingLearnerState::Mode &m : s.mode) {
    if (!std::isfinite(m.sw) || !std::isfinite(m.sx) || !std::isfinite(m.sy) ||
        !std::isfinite(m.sxx) || !std::isfinite(m.sxy) || !std::isfinite(m.syy))
      return false;
    if (m.sw < 0.0 || m.sw > 1000.0 || m.syy < 0.0) return false;
  }
  return true;
}

/**
 * Loads a model blob stored as raw bytes (the structs are trivially
 * copyable), under `key`. A missing blob, a stale layout version or an
 * implausible value all fall back to the caller's defaults.
 */
template <typename State>
bool loadBlobFromNvs(const char *key, const char *what, uint32_t expected_version,
                     bool (*plausible)(const State &), State &out) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    Serial.printf("[Settings] NVS namespace not found -- %s starts from defaults\n", what);
    return false;
  }
  State loaded{};
  size_t got = g_prefs.getBytes(key, &loaded, sizeof(loaded));
  g_prefs.end();

  if (got != sizeof(loaded) || loaded.version != expected_version || !plausible(loaded)) {
    Serial.printf("[Settings] %s NVS blob missing/stale/implausible -- starting from defaults\n",
                  what);
    return false;
  }
  out = loaded;
  return true;
}

/** Stores a model blob as raw bytes under `key`. */
template <typename State>
void saveBlobToNvs(const char *key, const char *what, const State &state) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Serial.printf("[Settings] NVS open for write failed -- %s not persisted\n", what);
    return;
  }
  g_prefs.putBytes(key, &state, sizeof(state));
  g_prefs.end();
}

/** Overwrites every subscriber's mailbox with the current snapshot. */
void broadcastSnapshot() {
  xQueueOverwrite(g_settings_mailbox_scale, &g_settings);
  xQueueOverwrite(g_settings_mailbox_dosing, &g_settings);
  xQueueOverwrite(g_settings_mailbox_input, &g_settings);
  xQueueOverwrite(g_settings_mailbox_network, &g_settings);
  xQueueOverwrite(g_settings_mailbox_display, &g_settings);
}

/** Validates and applies one write request to g_settings; false if it is rejected. */
bool applyWrite(const SettingsWriteRequest &req) {
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  if (req.field_index >= count || !settingAccepts(table[req.field_index], req.value)) {
    return false;
  }
  writeSetting(g_settings, table[req.field_index], req.value);
  return true;
}

/** Applies every queued write; true if any was accepted. */
bool drainWrites() {
  bool changed = false;
  SettingsWriteRequest write;
  while (xQueueReceive(g_settings_write_q, &write, 0) == pdTRUE) {
    if (applyWrite(write)) {
      changed = true;
    } else {
      Serial.printf("[Settings] rejected write, field_index=%u request_id=%u\n",
                    static_cast<unsigned>(write.field_index), write.request_id);
    }
  }
  return changed;
}

/** Stores every queued model blob that passes its plausibility check. */
void drainPersistRequests() {
  PersistRequest persist;
  while (xQueueReceive(g_persist_request_q, &persist, 0) == pdTRUE) {
    if (persist.blob_id == PersistBlobId::DOSING_MODEL &&
        isPlausibleDosingModel(persist.payload)) {
      g_dosing_model = persist.payload;
      saveBlobToNvs(kKeyDosingModel, "dosing model", g_dosing_model);
    } else if (persist.blob_id == PersistBlobId::LANDING_LEARNER &&
               persist.learner_payload.version == kLandingLearnerVersion &&
               isPlausibleLandingLearner(persist.learner_payload)) {
      g_landing_learner = persist.learner_payload;
      saveBlobToNvs(kKeyLandingLearner, "landing learner", g_landing_learner);
    } else {
      Serial.printf("[Settings] rejected model persist, request_id=%u\n", persist.request_id);
    }
  }
}

/** Settings task entry point: loads NVS, then serves the write/persist queues. */
void settingsTaskFn(void *) {
  g_settings = SettingsSnapshot{};
  if (!loadSettingsFromNvs(g_settings)) {
    g_settings = SettingsSnapshot{};
  }
  g_settings.version = 1;

  g_dosing_model = makeDefaultDosingModelState();
  if (!loadBlobFromNvs(kKeyDosingModel, "dosing model", kDosingModelVersion,
                       isPlausibleDosingModel, g_dosing_model)) {
    g_dosing_model = makeDefaultDosingModelState();
  }
  g_landing_learner = makeDefaultLandingLearnerState();
  if (!loadBlobFromNvs(kKeyLandingLearner, "landing learner", kLandingLearnerVersion,
                       isPlausibleLandingLearner, g_landing_learner)) {
    g_landing_learner = makeDefaultLandingLearnerState();
  }

  broadcastSnapshot();
  xQueueOverwrite(g_dosing_model_mailbox, &g_dosing_model);
  xQueueOverwrite(g_landing_learner_mailbox, &g_landing_learner);

  // Every subscriber's first read is now a valid, fully loaded snapshot.
  xEventGroupSetBits(g_sys_events, kSettingsLoadedBit);

  for (;;) {
    if (drainWrites()) {
      g_settings.version++;
      saveSettingsToNvs(g_settings);
      broadcastSnapshot();
    }
    drainPersistRequests();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void createSettingsTask() {
  xTaskCreatePinnedToCore(settingsTaskFn, "Settings", TaskConfig::kSettingsStackBytes, nullptr,
                          TaskConfig::kSettingsPriority, nullptr, TaskConfig::kSettingsCore);
}
