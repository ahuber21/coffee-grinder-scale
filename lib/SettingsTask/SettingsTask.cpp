#include "SettingsTask.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "DosingModel.h"
#include "Messages.h"
#include "Queues.h"
#include "TaskConfig.h"

namespace {

SettingsSnapshot g_settings;  ///< Canonical copy -- only this task ever writes it.
TopupModelV1 g_topup_model;   ///< Canonical cold copy -- ditto.

// ESP32 NVS caps namespace/key names at 15 characters -- every key
// below stays under that. One `Preferences` handle, opened and closed
// per call rather than held open, since this task only touches NVS on
// an accepted settings change.
Preferences g_prefs;

constexpr const char *kNvsNamespace = "settings";

/*
 * Bumped only when SettingsSnapshot's on-NVS shape changes in a way old
 * data can't be safely reinterpreted as. History: 1->2 forced a stale
 * NVS blob (written by an early migration pass that shipped without
 * gain/speed/read_samples) to fall back to compiled-in defaults and
 * reload correctly, once that gap was found and fixed.
 */
constexpr uint32_t kSettingsSchemaVersion = 2;

constexpr const char *kKeySchemaVer = "schema_ver";
constexpr const char *kKeyReadSamples = "read_samples";
constexpr const char *kKeySpeed = "speed";
constexpr const char *kKeyGain = "gain";
constexpr const char *kKeyCalFactor = "cal_factor";
constexpr const char *kKeyDoseSingle = "dose_single";
constexpr const char *kKeyDoseDouble = "dose_double";
constexpr const char *kKeyMarginSingle = "margin_sing";
constexpr const char *kKeyMarginDouble = "margin_dbl";
constexpr const char *kKeyMinTopupG = "min_topup_g";
constexpr const char *kKeyRateCalcPct = "rate_calc_pct";
constexpr const char *kKeyTopupToMs = "topup_to_ms";
constexpr const char *kKeyGrindToMs = "grind_to_ms";
constexpr const char *kKeyFinalToMs = "final_to_ms";
constexpr const char *kKeyConfirmToMs = "confirm_to_ms";
constexpr const char *kKeyStabMinMs = "stab_min_ms";
constexpr const char *kKeyStabMaxMs = "stab_max_ms";
constexpr const char *kKeyMinTopupRt = "min_topup_rt";
constexpr const char *kKeyTopupIntMs = "topup_int_ms";
constexpr const char *kKeySsTimeoutS = "ss_timeout_s";
constexpr const char *kKeyBtnDebounce = "btn_debounce";
constexpr const char *kKeyBtnHoldMs = "btn_hold_ms";
constexpr const char *kKeyWifiReset = "wifi_reset";
constexpr const char *kKeyWifiReboot = "wifi_reboot";
constexpr const char *kKeyTopupModel = "topup_model";

/** @see isPlausibleTopupModel */
bool isPlausibleTopupModel(const TopupModelV1 &m);

// Field validators: the single source of truth, shared by the live
// write path and the NVS loader below.

/** True unless a calibration factor of exactly 0.0f would zero the scale. */
bool validCalibrationFactor(float v) { return std::isfinite(v) && v != 0.0f; }
/** A plausible single/double target dose. */
bool validDoseGrams(float v) { return std::isfinite(v) && v > 0.0f && v <= 100.0f; }
/** A non-negative top-up margin. */
bool validTopUpMargin(float v) { return std::isfinite(v) && v >= 0.0f; }
/** A sane debounce window, in ms. */
bool validButtonDebounceMs(uint32_t v) { return v > 0 && v <= 2000; }
/** Feeds ADS1232::setRingBufferSize, capped at RING_BUFFER_MAX_SIZE (48). */
bool validReadSamples(uint8_t v) { return v >= 1 && v <= 48; }
/** Feeds ADS1232::setSpeed, which only accepts these two hardware speeds. */
bool validSpeedSps(uint8_t v) { return v == 10 || v == 80; }
/** Feeds ADS1232::setGain, which only accepts these four hardware gains. */
bool validGain(uint8_t v) { return v == 1 || v == 2 || v == 64 || v == 128; }
/** A plausible minimum top-up pulse size. */
bool validMinTopupGrams(float v) { return std::isfinite(v) && v >= 0.0f && v <= 5.0f; }
/** A fraction in (0, 1]. */
bool validRateCalcPct(float v) { return std::isfinite(v) && v > 0.0f && v <= 1.0f; }
/** Generic positive bound for the various ms-scale timeouts, capped at 10 minutes. */
bool validTimeoutMs(uint32_t v) { return v > 0 && v <= 600000UL; }
/** A sane screensaver idle window, in seconds. */
bool validScreensaverTimeoutS(uint32_t v) { return v > 0 && v <= 86400UL; }

/**
 * Loads SettingsSnapshot from NVS. Returns false (leaving `out`
 * untouched) if the namespace doesn't exist yet or its schema_ver
 * doesn't match -- the caller falls back to compiled-in defaults either
 * way. Each field is validated independently, so one corrupt value
 * doesn't take the rest of a legitimate snapshot down with it.
 */
bool loadSettingsFromNvs(SettingsSnapshot &out) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    Serial.println(
        "[Settings] NVS namespace not found (first boot / blank partition) "
        "-- using compiled-in defaults");
    return false;
  }

  const uint32_t stored_schema = g_prefs.getUInt(kKeySchemaVer, 0);
  if (stored_schema != kSettingsSchemaVersion) {
    Serial.printf(
        "[Settings] NVS schema_ver=%u (expected %u) -- using compiled-in "
        "defaults\n",
        static_cast<unsigned>(stored_schema),
        static_cast<unsigned>(kSettingsSchemaVersion));
    g_prefs.end();
    return false;
  }

  const SettingsSnapshot defaults{};  // Per-field fallback values.
  out = defaults;

  uint8_t read_samples = g_prefs.getUChar(kKeyReadSamples, defaults.read_samples);
  if (validReadSamples(read_samples)) {
    out.read_samples = read_samples;
  } else {
    Serial.println("[Settings] NVS read_samples out of range -- using default");
  }

  uint8_t speed = g_prefs.getUChar(kKeySpeed, defaults.speed);
  if (validSpeedSps(speed)) {
    out.speed = speed;
  } else {
    Serial.println("[Settings] NVS speed invalid -- using default");
  }

  uint8_t gain = g_prefs.getUChar(kKeyGain, defaults.gain);
  if (validGain(gain)) {
    out.gain = gain;
  } else {
    Serial.println("[Settings] NVS gain invalid -- using default");
  }

  float cal_factor = g_prefs.getFloat(kKeyCalFactor, defaults.calibration_factor);
  if (validCalibrationFactor(cal_factor)) {
    out.calibration_factor = cal_factor;
  } else {
    Serial.println("[Settings] NVS calibration_factor invalid -- using default");
  }

  float dose_single = g_prefs.getFloat(kKeyDoseSingle, defaults.target_dose_single);
  if (validDoseGrams(dose_single)) {
    out.target_dose_single = dose_single;
  } else {
    Serial.println("[Settings] NVS target_dose_single invalid -- using default");
  }

  float dose_double = g_prefs.getFloat(kKeyDoseDouble, defaults.target_dose_double);
  if (validDoseGrams(dose_double)) {
    out.target_dose_double = dose_double;
  } else {
    Serial.println("[Settings] NVS target_dose_double invalid -- using default");
  }

  float margin_single = g_prefs.getFloat(kKeyMarginSingle, defaults.top_up_margin_single);
  if (validTopUpMargin(margin_single)) {
    out.top_up_margin_single = margin_single;
  } else {
    Serial.println("[Settings] NVS top_up_margin_single invalid -- using default");
  }

  float margin_double = g_prefs.getFloat(kKeyMarginDouble, defaults.top_up_margin_double);
  if (validTopUpMargin(margin_double)) {
    out.top_up_margin_double = margin_double;
  } else {
    Serial.println("[Settings] NVS top_up_margin_double invalid -- using default");
  }

  float min_topup_grams = g_prefs.getFloat(kKeyMinTopupG, defaults.min_topup_grams);
  if (validMinTopupGrams(min_topup_grams)) {
    out.min_topup_grams = min_topup_grams;
  } else {
    Serial.println("[Settings] NVS min_topup_grams invalid -- using default");
  }

  float rate_calc_pct =
      g_prefs.getFloat(kKeyRateCalcPct, defaults.rate_calculation_percentage);
  if (validRateCalcPct(rate_calc_pct)) {
    out.rate_calculation_percentage = rate_calc_pct;
  } else {
    Serial.println("[Settings] NVS rate_calculation_percentage invalid -- using default");
  }

  uint32_t topup_to_ms = g_prefs.getUInt(kKeyTopupToMs, defaults.topup_timeout_ms);
  if (validTimeoutMs(topup_to_ms)) {
    out.topup_timeout_ms = topup_to_ms;
  } else {
    Serial.println("[Settings] NVS topup_timeout_ms invalid -- using default");
  }

  uint32_t grind_to_ms = g_prefs.getUInt(kKeyGrindToMs, defaults.grinding_timeout_ms);
  if (validTimeoutMs(grind_to_ms)) {
    out.grinding_timeout_ms = grind_to_ms;
  } else {
    Serial.println("[Settings] NVS grinding_timeout_ms invalid -- using default");
  }

  uint32_t final_to_ms = g_prefs.getUInt(kKeyFinalToMs, defaults.finalize_timeout_ms);
  if (validTimeoutMs(final_to_ms)) {
    out.finalize_timeout_ms = final_to_ms;
  } else {
    Serial.println("[Settings] NVS finalize_timeout_ms invalid -- using default");
  }

  uint32_t confirm_to_ms = g_prefs.getUInt(kKeyConfirmToMs, defaults.confirm_timeout_ms);
  if (validTimeoutMs(confirm_to_ms)) {
    out.confirm_timeout_ms = confirm_to_ms;
  } else {
    Serial.println("[Settings] NVS confirm_timeout_ms invalid -- using default");
  }

  uint32_t stab_min_ms = g_prefs.getUInt(kKeyStabMinMs, defaults.stability_min_wait_ms);
  if (validTimeoutMs(stab_min_ms)) {
    out.stability_min_wait_ms = stab_min_ms;
  } else {
    Serial.println("[Settings] NVS stability_min_wait_ms invalid -- using default");
  }

  uint32_t stab_max_ms = g_prefs.getUInt(kKeyStabMaxMs, defaults.stability_max_wait_ms);
  if (validTimeoutMs(stab_max_ms)) {
    out.stability_max_wait_ms = stab_max_ms;
  } else {
    Serial.println("[Settings] NVS stability_max_wait_ms invalid -- using default");
  }

  uint32_t min_topup_rt = g_prefs.getUInt(kKeyMinTopupRt, defaults.min_topup_runtime_ms);
  if (validTimeoutMs(min_topup_rt)) {
    out.min_topup_runtime_ms = min_topup_rt;
  } else {
    Serial.println("[Settings] NVS min_topup_runtime_ms invalid -- using default");
  }

  uint32_t topup_int_ms = g_prefs.getUInt(kKeyTopupIntMs, defaults.min_topup_interval_ms);
  if (validTimeoutMs(topup_int_ms)) {
    out.min_topup_interval_ms = topup_int_ms;
  } else {
    Serial.println("[Settings] NVS min_topup_interval_ms invalid -- using default");
  }

  uint32_t ss_timeout_s = g_prefs.getUInt(kKeySsTimeoutS, defaults.screensaver_timeout_s);
  if (validScreensaverTimeoutS(ss_timeout_s)) {
    out.screensaver_timeout_s = ss_timeout_s;
  } else {
    Serial.println("[Settings] NVS screensaver_timeout_s invalid -- using default");
  }

  uint32_t btn_debounce_ms = g_prefs.getUInt(kKeyBtnDebounce, defaults.button_debounce_ms);
  if (validButtonDebounceMs(btn_debounce_ms)) {
    out.button_debounce_ms = btn_debounce_ms;
  } else {
    Serial.println("[Settings] NVS button_debounce_ms invalid -- using default");
  }

  uint32_t btn_hold_ms = g_prefs.getUInt(kKeyBtnHoldMs, defaults.button_min_hold_ms);
  if (validTimeoutMs(btn_hold_ms)) {
    out.button_min_hold_ms = btn_hold_ms;
  } else {
    Serial.println("[Settings] NVS button_min_hold_ms invalid -- using default");
  }

  // Flags have no illegal state -- any stored bool is trusted as-is.
  out.wifi_reset_flag = g_prefs.getBool(kKeyWifiReset, defaults.wifi_reset_flag);
  out.wifi_reboot_flag = g_prefs.getBool(kKeyWifiReboot, defaults.wifi_reboot_flag);

  g_prefs.end();
  return true;
}

/**
 * Writes `snap` through to NVS. `snap` is always the already-validated
 * g_settings here (applyWrite() validates before this is ever called) --
 * this is a plain write-through, not a second validation gate.
 */
void saveSettingsToNvs(const SettingsSnapshot &snap) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Serial.println("[Settings] NVS open for write failed -- settings not persisted");
    return;
  }

  g_prefs.putUInt(kKeySchemaVer, kSettingsSchemaVersion);
  g_prefs.putUChar(kKeyReadSamples, snap.read_samples);
  g_prefs.putUChar(kKeySpeed, snap.speed);
  g_prefs.putUChar(kKeyGain, snap.gain);
  g_prefs.putFloat(kKeyCalFactor, snap.calibration_factor);
  g_prefs.putFloat(kKeyDoseSingle, snap.target_dose_single);
  g_prefs.putFloat(kKeyDoseDouble, snap.target_dose_double);
  g_prefs.putFloat(kKeyMarginSingle, snap.top_up_margin_single);
  g_prefs.putFloat(kKeyMarginDouble, snap.top_up_margin_double);
  g_prefs.putFloat(kKeyMinTopupG, snap.min_topup_grams);
  g_prefs.putFloat(kKeyRateCalcPct, snap.rate_calculation_percentage);
  g_prefs.putUInt(kKeyTopupToMs, snap.topup_timeout_ms);
  g_prefs.putUInt(kKeyGrindToMs, snap.grinding_timeout_ms);
  g_prefs.putUInt(kKeyFinalToMs, snap.finalize_timeout_ms);
  g_prefs.putUInt(kKeyConfirmToMs, snap.confirm_timeout_ms);
  g_prefs.putUInt(kKeyStabMinMs, snap.stability_min_wait_ms);
  g_prefs.putUInt(kKeyStabMaxMs, snap.stability_max_wait_ms);
  g_prefs.putUInt(kKeyMinTopupRt, snap.min_topup_runtime_ms);
  g_prefs.putUInt(kKeyTopupIntMs, snap.min_topup_interval_ms);
  g_prefs.putUInt(kKeySsTimeoutS, snap.screensaver_timeout_s);
  g_prefs.putUInt(kKeyBtnDebounce, snap.button_debounce_ms);
  g_prefs.putUInt(kKeyBtnHoldMs, snap.button_min_hold_ms);
  g_prefs.putBool(kKeyWifiReset, snap.wifi_reset_flag);
  g_prefs.putBool(kKeyWifiReboot, snap.wifi_reboot_flag);

  g_prefs.end();
}

/**
 * TopupModelV1 NVS round-trip: the whole POD struct as one raw-bytes
 * blob (it's trivially copyable/standard-layout by construction, see
 * DosingModel.h's static_asserts) rather than one key per field --
 * unlike SettingsSnapshot, nothing here is meant to be hand-edited
 * field-by-field, so there's no reason to decompose it.
 */
bool loadTopupModelFromNvs(TopupModelV1 &out) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    Serial.println("[Settings] NVS namespace not found -- topup model uses cold-start priors");
    return false;
  }
  TopupModelV1 loaded{};
  size_t got = g_prefs.getBytes(kKeyTopupModel, &loaded, sizeof(loaded));
  g_prefs.end();

  // A version mismatch means either nothing was ever saved (got == 0)
  // or a firmware update changed the struct's shape -- either way, the
  // safe move is the same as SettingsSnapshot's schema_ver guard: fall
  // back to cold-start priors rather than reinterpret stale bytes.
  if (got != sizeof(loaded) || loaded.version != kTopupModelVersion) {
    Serial.println("[Settings] Topup model NVS blob missing/stale -- using cold-start priors");
    return false;
  }
  if (!isPlausibleTopupModel(loaded)) {
    Serial.println("[Settings] Topup model NVS blob implausible -- using cold-start priors");
    return false;
  }
  out = loaded;
  return true;
}

/** @see loadTopupModelFromNvs */
void saveTopupModelToNvs(const TopupModelV1 &model) {
  if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Serial.println("[Settings] NVS open for write failed -- topup model not persisted");
    return;
  }
  g_prefs.putBytes(kKeyTopupModel, &model, sizeof(model));
  g_prefs.end();
}

/** Overwrites every subscriber's mailbox with the current snapshot. */
void broadcastSnapshot() {
  xQueueOverwrite(g_settings_mailbox_scale, &g_settings);
  xQueueOverwrite(g_settings_mailbox_dosing, &g_settings);
  xQueueOverwrite(g_settings_mailbox_input, &g_settings);
  xQueueOverwrite(g_settings_mailbox_network, &g_settings);
}

/**
 * Validates and applies one write request to g_settings -- this task is
 * the sole authority on whether a value is legal. Returns false,
 * leaving g_settings untouched, if the value fails its validator.
 */
bool applyWrite(const SettingsWriteRequest &req) {
  switch (req.field_id) {
    case SettingsFieldId::CALIBRATION_FACTOR:
      if (!validCalibrationFactor(req.value.f)) return false;
      g_settings.calibration_factor = req.value.f;
      return true;
    case SettingsFieldId::TARGET_DOSE_SINGLE:
      if (!validDoseGrams(req.value.f)) return false;
      g_settings.target_dose_single = req.value.f;
      return true;
    case SettingsFieldId::TARGET_DOSE_DOUBLE:
      if (!validDoseGrams(req.value.f)) return false;
      g_settings.target_dose_double = req.value.f;
      return true;
    case SettingsFieldId::TOP_UP_MARGIN_SINGLE:
      if (!validTopUpMargin(req.value.f)) return false;
      g_settings.top_up_margin_single = req.value.f;
      return true;
    case SettingsFieldId::TOP_UP_MARGIN_DOUBLE:
      if (!validTopUpMargin(req.value.f)) return false;
      g_settings.top_up_margin_double = req.value.f;
      return true;
    case SettingsFieldId::BUTTON_DEBOUNCE_MS:
      if (!validButtonDebounceMs(req.value.u)) return false;
      g_settings.button_debounce_ms = req.value.u;
      return true;
    case SettingsFieldId::WIFI_RESET_FLAG:
      g_settings.wifi_reset_flag = req.value.b;
      return true;
    case SettingsFieldId::WIFI_REBOOT_FLAG:
      g_settings.wifi_reboot_flag = req.value.b;
      return true;
    case SettingsFieldId::READ_SAMPLES:
      if (!validReadSamples(static_cast<uint8_t>(req.value.u))) return false;
      g_settings.read_samples = static_cast<uint8_t>(req.value.u);
      return true;
    case SettingsFieldId::SPEED:
      if (!validSpeedSps(static_cast<uint8_t>(req.value.u))) return false;
      g_settings.speed = static_cast<uint8_t>(req.value.u);
      return true;
    case SettingsFieldId::GAIN:
      if (!validGain(static_cast<uint8_t>(req.value.u))) return false;
      g_settings.gain = static_cast<uint8_t>(req.value.u);
      return true;
    case SettingsFieldId::MIN_TOPUP_GRAMS:
      if (!validMinTopupGrams(req.value.f)) return false;
      g_settings.min_topup_grams = req.value.f;
      return true;
    case SettingsFieldId::RATE_CALCULATION_PERCENTAGE:
      if (!validRateCalcPct(req.value.f)) return false;
      g_settings.rate_calculation_percentage = req.value.f;
      return true;
    case SettingsFieldId::TOPUP_TIMEOUT_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.topup_timeout_ms = req.value.u;
      return true;
    case SettingsFieldId::GRINDING_TIMEOUT_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.grinding_timeout_ms = req.value.u;
      return true;
    case SettingsFieldId::FINALIZE_TIMEOUT_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.finalize_timeout_ms = req.value.u;
      return true;
    case SettingsFieldId::CONFIRM_TIMEOUT_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.confirm_timeout_ms = req.value.u;
      return true;
    case SettingsFieldId::STABILITY_MIN_WAIT_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.stability_min_wait_ms = req.value.u;
      return true;
    case SettingsFieldId::STABILITY_MAX_WAIT_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.stability_max_wait_ms = req.value.u;
      return true;
    case SettingsFieldId::MIN_TOPUP_RUNTIME_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.min_topup_runtime_ms = req.value.u;
      return true;
    case SettingsFieldId::MIN_TOPUP_INTERVAL_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.min_topup_interval_ms = req.value.u;
      return true;
    case SettingsFieldId::SCREENSAVER_TIMEOUT_S:
      if (!validScreensaverTimeoutS(req.value.u)) return false;
      g_settings.screensaver_timeout_s = req.value.u;
      return true;
    case SettingsFieldId::BUTTON_MIN_HOLD_MS:
      if (!validTimeoutMs(req.value.u)) return false;
      g_settings.button_min_hold_ms = req.value.u;
      return true;
  }
  return false;
}

/** Plausibility bounds for a persisted TopupModelV1 blob, same discipline as applyWrite(). */
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

/** Settings task entry point: loads NVS, then serves the write/persist queues. */
void settingsTaskFn(void *) {
  g_settings = SettingsSnapshot{};  // Compiled-in defaults (Messages.h).
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

  // Every subscriber's first read is now guaranteed to be a valid,
  // fully loaded snapshot, never a default-constructed placeholder.
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
