#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "SettingsSchema.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

size_t storageSize(SettingType t) {
  switch (t) {
    case SettingType::U8: return sizeof(uint8_t);
    case SettingType::U32: return sizeof(uint32_t);
    case SettingType::F32: return sizeof(float);
    case SettingType::F64: return sizeof(double);
    case SettingType::BOOL: return sizeof(bool);
  }
  return 0;
}

const SettingDescriptor &row(const char *name) {
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  int index = findSettingByName(name);
  TEST_ASSERT_TRUE_MESSAGE(index >= 0, name);
  return table[index];
}

}  // namespace

void test_names_and_nvs_keys_are_unique_and_short(void) {
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  std::set<std::string> names;
  // Keys the settings namespace already uses for non-table entries.
  std::set<std::string> keys = {"schema_ver", "topup_model", "land_learner"};
  for (size_t i = 0; i < count; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(names.insert(table[i].name).second, table[i].name);
    TEST_ASSERT_TRUE_MESSAGE(keys.insert(table[i].nvs_key).second, table[i].nvs_key);
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(table[i].nvs_key) <= kMaxNvsKeyLength, table[i].nvs_key);
  }
}

void test_members_do_not_overlap_and_fit_in_the_snapshot(void) {
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  std::vector<std::pair<size_t, size_t>> spans;  // offset, end
  for (size_t i = 0; i < count; ++i) {
    size_t end = table[i].offset + storageSize(table[i].type);
    TEST_ASSERT_TRUE_MESSAGE(end <= sizeof(SettingsSnapshot), table[i].name);
    spans.push_back({table[i].offset, end});
  }
  std::sort(spans.begin(), spans.end());
  for (size_t i = 1; i < spans.size(); ++i) {
    TEST_ASSERT_TRUE(spans[i - 1].second <= spans[i].first);
  }
}

void test_every_default_passes_its_own_validator(void) {
  const SettingsSnapshot defaults{};
  size_t count = 0;
  const SettingDescriptor *table = settingsTable(count);
  for (size_t i = 0; i < count; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(settingAccepts(table[i], readSetting(defaults, table[i])),
                             table[i].name);
  }
}

void test_read_after_write_round_trips_every_type(void) {
  SettingsSnapshot s{};
  writeSetting(s, row("gain"), 64.0);
  writeSetting(s, row("button_debounce_ms"), 250.0);
  writeSetting(s, row("target_dose_single"), 17.5);
  writeSetting(s, row("landing_learner_enabled"), 0.0);
  writeSetting(s, row("calibration_factor_x1e6"), 0.000924583895);
  TEST_ASSERT_EQUAL_UINT8(64, s.gain);
  TEST_ASSERT_EQUAL_UINT32(250, s.button_debounce_ms);
  TEST_ASSERT_EQUAL_FLOAT(17.5f, s.target_dose_single);
  TEST_ASSERT_FALSE(s.landing_learner_enabled);
  TEST_ASSERT_EQUAL_DOUBLE(0.000924583895, s.calibration_factor);
  TEST_ASSERT_EQUAL_DOUBLE(0.000924583895, readSetting(s, row("calibration_factor_x1e6")));
}

void test_writing_one_setting_leaves_the_others_alone(void) {
  SettingsSnapshot s{};
  const SettingsSnapshot before = s;
  writeSetting(s, row("speed"), 80.0);
  TEST_ASSERT_EQUAL_UINT8(80, s.speed);
  s.speed = before.speed;
  TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(SettingsSnapshot));
}

void test_integer_settings_reject_fractions_negatives_and_overflow(void) {
  TEST_ASSERT_FALSE(settingAccepts(row("button_debounce_ms"), 150.5));
  TEST_ASSERT_FALSE(settingAccepts(row("button_debounce_ms"), -1.0));
  TEST_ASSERT_FALSE(settingAccepts(row("read_samples"), 300.0));
  TEST_ASSERT_FALSE(settingAccepts(row("grinding_timeout_ms"), 5e9));
}

void test_non_finite_values_are_rejected(void) {
  TEST_ASSERT_FALSE(settingAccepts(row("target_dose_single"), NAN));
  TEST_ASSERT_FALSE(settingAccepts(row("target_dose_single"), INFINITY));
  TEST_ASSERT_FALSE(settingAccepts(row("grinding_timeout_ms"), NAN));
}

void test_bool_settings_accept_only_zero_or_one(void) {
  TEST_ASSERT_TRUE(settingAccepts(row("landing_learner_enabled"), 0.0));
  TEST_ASSERT_TRUE(settingAccepts(row("landing_learner_enabled"), 1.0));
  TEST_ASSERT_FALSE(settingAccepts(row("landing_learner_enabled"), 2.0));
}

void test_validators_enforce_their_ranges(void) {
  TEST_ASSERT_FALSE(settingAccepts(row("gain"), 3.0));
  TEST_ASSERT_TRUE(settingAccepts(row("gain"), 128.0));
  TEST_ASSERT_FALSE(settingAccepts(row("speed"), 40.0));
  TEST_ASSERT_FALSE(settingAccepts(row("calibration_factor_x1e6"), 0.0));
  TEST_ASSERT_FALSE(settingAccepts(row("landing_learner_rate"), 0.0));
  TEST_ASSERT_FALSE(settingAccepts(row("landing_learner_rate"), 0.9));
  TEST_ASSERT_TRUE(settingAccepts(row("landing_learner_rate"), 0.05));
  TEST_ASSERT_FALSE(settingAccepts(row("landing_learner_clamp_ms"), 5000.0));
  TEST_ASSERT_FALSE(settingAccepts(row("top_up_margin_single"), -0.1));
}

void test_lookup_by_name(void) {
  TEST_ASSERT_TRUE(findSettingByName("target_dose_double") >= 0);
  TEST_ASSERT_EQUAL_INT(-1, findSettingByName("min_topup_interval_ms"));
  TEST_ASSERT_EQUAL_INT(-1, findSettingByName(""));
}

void test_calibration_factor_travels_scaled_and_the_wifi_flags_stay_off_the_wire(void) {
  TEST_ASSERT_EQUAL_DOUBLE(1e6, row("calibration_factor_x1e6").wire_scale);
  TEST_ASSERT_FALSE(row("wifi_reset_flag").broadcast);
  TEST_ASSERT_FALSE(row("wifi_reboot_flag").broadcast);
  TEST_ASSERT_TRUE(row("target_dose_single").broadcast);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_names_and_nvs_keys_are_unique_and_short);
  RUN_TEST(test_members_do_not_overlap_and_fit_in_the_snapshot);
  RUN_TEST(test_every_default_passes_its_own_validator);
  RUN_TEST(test_read_after_write_round_trips_every_type);
  RUN_TEST(test_writing_one_setting_leaves_the_others_alone);
  RUN_TEST(test_integer_settings_reject_fractions_negatives_and_overflow);
  RUN_TEST(test_non_finite_values_are_rejected);
  RUN_TEST(test_bool_settings_accept_only_zero_or_one);
  RUN_TEST(test_validators_enforce_their_ranges);
  RUN_TEST(test_lookup_by_name);
  RUN_TEST(test_calibration_factor_travels_scaled_and_the_wifi_flags_stay_off_the_wire);
  return UNITY_END();
}
