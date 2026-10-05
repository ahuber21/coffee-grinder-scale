#pragma once

/**
 * The single description of every tunable setting: its WebSocket name,
 * NVS key, storage type, validator and wire scale. Settings task's NVS
 * load/save/write path and Network task's JSON in/out are all driven by
 * this table, so adding a setting means adding one SettingsSnapshot
 * member and one row here.
 *
 * Header-only and free of Arduino includes, so it is unit-testable on
 * the host.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "Messages.h"

/** Storage type of a SettingsSnapshot member. */
enum class SettingType : uint8_t { U8, U32, F32, F64, BOOL };

/** One row of the settings table. */
struct SettingDescriptor {
  const char *name;     ///< Field name on the WebSocket wire.
  const char *nvs_key;  ///< NVS key, at most 15 characters.
  SettingType type;
  size_t offset;               ///< offsetof the member in SettingsSnapshot.
  bool (*valid)(double);       ///< Range check applied after the type check; null accepts any value.
  bool broadcast;              ///< Included in the settings JSON sent to WebSocket clients.
  double wire_scale;           ///< Wire value = stored value * wire_scale.
};

/// ESP32 NVS caps key names at this many characters.
constexpr size_t kMaxNvsKeyLength = 15;

namespace settings_validators {

/** A calibration factor of exactly 0 would zero the scale. */
inline bool calibrationFactor(double v) { return std::isfinite(v) && v != 0.0; }
/** A plausible single/double target dose. */
inline bool doseGrams(double v) { return std::isfinite(v) && v > 0.0 && v <= 100.0; }
inline bool topUpMargin(double v) { return std::isfinite(v) && v >= 0.0; }
inline bool minTopupGrams(double v) { return std::isfinite(v) && v >= 0.0 && v <= 5.0; }
inline bool buttonDebounceMs(double v) { return v > 0.0 && v <= 2000.0; }
/** Feeds ADS1232::setRingBufferSize, capped at its RING_BUFFER_MAX_SIZE. */
inline bool readSamples(double v) { return v >= 1.0 && v <= 96.0; }
/** ADS1232::setSpeed accepts only these two hardware speeds. */
inline bool speedSps(double v) { return v == 10.0 || v == 80.0; }
/** ADS1232::setGain accepts only these four hardware gains. */
inline bool gain(double v) { return v == 1.0 || v == 2.0 || v == 64.0 || v == 128.0; }
/** The ms-scale timeouts, capped at 10 minutes. */
inline bool timeoutMs(double v) { return v > 0.0 && v <= 600000.0; }
inline bool screensaverTimeoutS(double v) { return v > 0.0 && v <= 86400.0; }
inline bool screensaverWakeWeightDeltaG(double v) { return std::isfinite(v) && v > 0.0 && v <= 500.0; }
/** 0 turns the animation off; 3x the default is the most the clump pool can hold. */
inline bool clumpDensity(double v) { return std::isfinite(v) && v >= 0.0 && v <= 3.0; }
/** Must stay positive, or clumps freeze in place. */
inline bool clumpGravity(double v) { return std::isfinite(v) && v > 0.0 && v <= 5.0; }
/** Per-session forgetting: never 0 (frozen) and not past 0.5 (noise-chasing). */
inline bool landingRate(double v) { return std::isfinite(v) && v >= 0.01 && v <= 0.5; }
/** Bounded so a bad value can't move the stop by seconds. */
inline bool landingClampMs(double v) { return v >= 0.0 && v <= 2000.0; }

}  // namespace settings_validators

static_assert(std::is_standard_layout<SettingsSnapshot>::value,
              "offsetof on SettingsSnapshot requires standard layout");

#define SETTING_OFFSET(member) offsetof(SettingsSnapshot, member)

/** The settings table; a field's index in it identifies the field in a SettingsWriteRequest. */
inline const SettingDescriptor *settingsTable(size_t &count) {
  namespace v = settings_validators;
  static const SettingDescriptor kTable[] = {
      {"calibration_factor_x1e6", "cal_factor", SettingType::F64, SETTING_OFFSET(calibration_factor),
       v::calibrationFactor, true, 1e6},
      {"target_dose_single", "dose_single", SettingType::F32, SETTING_OFFSET(target_dose_single),
       v::doseGrams, true, 1.0},
      {"target_dose_double", "dose_double", SettingType::F32, SETTING_OFFSET(target_dose_double),
       v::doseGrams, true, 1.0},
      {"top_up_margin_single", "margin_sing", SettingType::F32, SETTING_OFFSET(top_up_margin_single),
       v::topUpMargin, true, 1.0},
      {"top_up_margin_double", "margin_dbl", SettingType::F32, SETTING_OFFSET(top_up_margin_double),
       v::topUpMargin, true, 1.0},
      {"min_topup_grams", "min_topup_g", SettingType::F32, SETTING_OFFSET(min_topup_grams),
       v::minTopupGrams, true, 1.0},
      {"button_debounce_ms", "btn_debounce", SettingType::U32, SETTING_OFFSET(button_debounce_ms),
       v::buttonDebounceMs, true, 1.0},
      {"button_min_hold_ms", "btn_hold_ms", SettingType::U32, SETTING_OFFSET(button_min_hold_ms),
       v::timeoutMs, true, 1.0},
      {"screensaver_timeout_s", "ss_timeout_s", SettingType::U32, SETTING_OFFSET(screensaver_timeout_s),
       v::screensaverTimeoutS, true, 1.0},
      {"screensaver_wake_weight_delta_g", "ss_wake_delta", SettingType::F32,
       SETTING_OFFSET(screensaver_wake_weight_delta_g), v::screensaverWakeWeightDeltaG, true, 1.0},
      {"read_samples", "read_samples", SettingType::U8, SETTING_OFFSET(read_samples), v::readSamples,
       true, 1.0},
      {"speed", "speed", SettingType::U8, SETTING_OFFSET(speed), v::speedSps, true, 1.0},
      {"gain", "gain", SettingType::U8, SETTING_OFFSET(gain), v::gain, true, 1.0},
      {"topup_timeout_ms", "topup_to_ms", SettingType::U32, SETTING_OFFSET(topup_timeout_ms),
       v::timeoutMs, true, 1.0},
      {"grinding_timeout_ms", "grind_to_ms", SettingType::U32, SETTING_OFFSET(grinding_timeout_ms),
       v::timeoutMs, true, 1.0},
      {"finalize_timeout_ms", "final_to_ms", SettingType::U32, SETTING_OFFSET(finalize_timeout_ms),
       v::timeoutMs, true, 1.0},
      {"confirm_timeout_ms", "confirm_to_ms", SettingType::U32, SETTING_OFFSET(confirm_timeout_ms),
       v::timeoutMs, true, 1.0},
      {"stability_min_wait_ms", "stab_min_ms", SettingType::U32, SETTING_OFFSET(stability_min_wait_ms),
       v::timeoutMs, true, 1.0},
      {"stability_max_wait_ms", "stab_max_ms", SettingType::U32, SETTING_OFFSET(stability_max_wait_ms),
       v::timeoutMs, true, 1.0},
      {"display_clump_density", "clump_density", SettingType::F32, SETTING_OFFSET(display_clump_density),
       v::clumpDensity, true, 1.0},
      {"display_clump_gravity", "clump_gravity", SettingType::F32, SETTING_OFFSET(display_clump_gravity),
       v::clumpGravity, true, 1.0},
      {"landing_learner_enabled", "land_enabled", SettingType::BOOL,
       SETTING_OFFSET(landing_learner_enabled), nullptr, true, 1.0},
      {"landing_learner_rate", "land_rate", SettingType::F32, SETTING_OFFSET(landing_learner_rate),
       v::landingRate, true, 1.0},
      {"landing_learner_clamp_ms", "land_clamp_ms", SettingType::U32,
       SETTING_OFFSET(landing_learner_clamp_ms), v::landingClampMs, true, 1.0},
      {"wifi_reset_flag", "wifi_reset", SettingType::BOOL, SETTING_OFFSET(wifi_reset_flag), nullptr,
       false, 1.0},
      {"wifi_reboot_flag", "wifi_reboot", SettingType::BOOL, SETTING_OFFSET(wifi_reboot_flag), nullptr,
       false, 1.0},
  };
  count = sizeof(kTable) / sizeof(kTable[0]);
  return kTable;
}

#undef SETTING_OFFSET

/** Finds a setting by its wire name; returns its table index, or -1 if there is none. */
inline int findSettingByName(const char *name) {
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(table[i].name, name) == 0) return static_cast<int>(i);
  }
  return -1;
}

/** Reads a setting's stored value, widened to double. */
inline double readSetting(const SettingsSnapshot &s, const SettingDescriptor &d) {
  const unsigned char *p = reinterpret_cast<const unsigned char *>(&s) + d.offset;
  switch (d.type) {
    case SettingType::U8: return *p;
    case SettingType::U32: {
      uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case SettingType::F32: {
      float v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case SettingType::F64: {
      double v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case SettingType::BOOL: {
      bool v;
      std::memcpy(&v, p, sizeof(v));
      return v ? 1.0 : 0.0;
    }
  }
  return 0.0;
}

/** True if `v` is representable in the setting's storage type and passes its validator. */
inline bool settingAccepts(const SettingDescriptor &d, double v) {
  if (!std::isfinite(v)) return false;
  switch (d.type) {
    case SettingType::U8:
      if (v < 0.0 || v > 255.0 || v != std::floor(v)) return false;
      break;
    case SettingType::U32:
      if (v < 0.0 || v > 4294967295.0 || v != std::floor(v)) return false;
      break;
    case SettingType::F32:
      if (std::fabs(v) > 3.4e38) return false;
      break;
    case SettingType::F64:
      break;
    case SettingType::BOOL:
      if (v != 0.0 && v != 1.0) return false;
      break;
  }
  return d.valid == nullptr || d.valid(v);
}

/** Stores `v` into the setting's member; the caller has already checked settingAccepts(). */
inline void writeSetting(SettingsSnapshot &s, const SettingDescriptor &d, double v) {
  unsigned char *p = reinterpret_cast<unsigned char *>(&s) + d.offset;
  switch (d.type) {
    case SettingType::U8:
      *p = static_cast<uint8_t>(v);
      break;
    case SettingType::U32: {
      uint32_t x = static_cast<uint32_t>(v);
      std::memcpy(p, &x, sizeof(x));
      break;
    }
    case SettingType::F32: {
      float x = static_cast<float>(v);
      std::memcpy(p, &x, sizeof(x));
      break;
    }
    case SettingType::F64:
      std::memcpy(p, &v, sizeof(v));
      break;
    case SettingType::BOOL: {
      bool x = v != 0.0;
      std::memcpy(p, &x, sizeof(x));
      break;
    }
  }
}
