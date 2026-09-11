#include <unity.h>

#include <cmath>
#include <cstring>
#include <random>

#include "DosingModel.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

// Fixed seed everywhere -> deterministic, reproducible test data.
std::mt19937 makeRng() { return std::mt19937(12345); }

}  // namespace

// ---------------------------------------------------------------------------
// WeightedLinearFit
// ---------------------------------------------------------------------------

void test_weighted_linear_fit_recovers_exact_line(void) {
  WeightedLinearFit fit;
  // y = 2 + 3x, noiseless -> WLS should recover it exactly.
  for (double x = 0.0; x <= 5.0; x += 1.0) {
    fit.addObservation(x, 2.0 + 3.0 * x, 1.0);
  }
  TEST_ASSERT_TRUE(fit.hasFit());
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3.0, fit.slope());
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.0, fit.intercept());
}

void test_weighted_linear_fit_decay_shrinks_effective_weight(void) {
  WeightedLinearFit fit;
  fit.addObservation(0.0, 0.0, 10.0);
  fit.addObservation(1.0, 1.0, 10.0);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 20.0, fit.effectiveWeight());
  fit.decay(0.5);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 10.0, fit.effectiveWeight());
}

// ---------------------------------------------------------------------------
// TopupModelV1 -- POD / NVS round-trip
// ---------------------------------------------------------------------------

void test_topup_model_v1_roundtrips_through_byte_copy(void) {
  TopupModelV1 original{};
  original.version = kTopupModelVersion;
  original.rate_hat = 1.0123f;
  original.rate_precision = 123.45f;
  original.topup_slope = 1.05f;
  original.topup_deadtime_ms = 310.0f;
  original.topup_precision = 44.4f;
  original.rate_n_effective = 18;
  original.topup_n_effective = 18;
  original.last_updated = 1234567890LL;

  uint8_t buffer[sizeof(TopupModelV1)];
  std::memcpy(buffer, &original, sizeof(TopupModelV1));

  TopupModelV1 restored{};
  std::memcpy(&restored, buffer, sizeof(TopupModelV1));

  TEST_ASSERT_EQUAL_UINT8(original.version, restored.version);
  TEST_ASSERT_EQUAL_FLOAT(original.rate_hat, restored.rate_hat);
  TEST_ASSERT_EQUAL_FLOAT(original.rate_precision, restored.rate_precision);
  TEST_ASSERT_EQUAL_FLOAT(original.topup_slope, restored.topup_slope);
  TEST_ASSERT_EQUAL_FLOAT(original.topup_deadtime_ms, restored.topup_deadtime_ms);
  TEST_ASSERT_EQUAL_FLOAT(original.topup_precision, restored.topup_precision);
  TEST_ASSERT_EQUAL_UINT32(original.rate_n_effective, restored.rate_n_effective);
  TEST_ASSERT_EQUAL_UINT32(original.topup_n_effective, restored.topup_n_effective);
  TEST_ASSERT_EQUAL_INT64(original.last_updated, restored.last_updated);
}

// ---------------------------------------------------------------------------
// Cold start
// ---------------------------------------------------------------------------

void test_cold_start_topup_model_reports_prior(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.05, model.slope());
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 310.0, model.deadtimeMs());
}

void test_cold_start_main_grind_model_reports_prior(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  MainGrindModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.00, model.currentRateEstimate());
}

// ---------------------------------------------------------------------------
// Convergence on synthetic data matching topup-model.md's real numbers
// ---------------------------------------------------------------------------

void test_topup_model_converges_to_known_slope_and_deadtime(void) {
  const double true_slope = 1.05;
  const double true_deadtime_s = 0.310;
  const double noise_sd = 0.15;

  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  std::mt19937 rng = makeRng();
  std::uniform_real_distribution<double> t_dist(350.0, 1300.0);
  std::normal_distribution<double> noise(0.0, noise_sd);

  int64_t now = 1000;
  for (int i = 0; i < 300; ++i) {
    double t_ms = t_dist(rng);
    double truth = std::max(0.0, true_slope * (t_ms / 1000.0 - true_deadtime_s));
    double observed = truth + noise(rng);
    model.recordPulse(t_ms, observed, now);
    now += 5;
  }

  TEST_ASSERT_DOUBLE_WITHIN(0.05, true_slope, model.slope());
  TEST_ASSERT_DOUBLE_WITHIN(60.0, true_deadtime_s * 1000.0, model.deadtimeMs());
}

void test_main_grind_model_converges_to_known_rate(void) {
  const double true_rate = 1.0;
  const double deadtime_s = 0.9;
  const double noise_sd = 0.03;

  TopupModelV1 persisted = makeDefaultTopupModel();
  MainGrindModel model(persisted);
  model.startSession();

  std::mt19937 rng = makeRng();
  std::normal_distribution<double> noise(0.0, noise_sd);

  for (double t_ms = 0.0; t_ms <= 15000.0; t_ms += 250.0) {
    double t_s = t_ms / 1000.0;
    double truth = t_s < deadtime_s ? 0.0 : true_rate * (t_s - deadtime_s);
    model.addSample(t_ms, truth + noise(rng));
  }

  TEST_ASSERT_DOUBLE_WITHIN(0.05, true_rate, model.currentRateEstimate());

  bool ok = model.finalizeSession(2000000000LL);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_DOUBLE_WITHIN(0.05, true_rate, model.persistedRateHat());
}

void test_main_grind_model_predicts_sane_stop_time(void) {
  // Noiseless, known rate: stop-time prediction should match closed form.
  TopupModelV1 persisted = makeDefaultTopupModel();
  MainGrindModel model(persisted);
  model.startSession();

  const double rate = 1.0;
  const double deadtime_s = 0.9;
  for (double t_ms = 0.0; t_ms <= 10000.0; t_ms += 250.0) {
    double t_s = t_ms / 1000.0;
    double w = t_s < deadtime_s ? 0.0 : rate * (t_s - deadtime_s);
    model.addSample(t_ms, w);
  }

  // last sample: t=10000ms, weight = 1.0*(10-0.9) = 9.1g
  // remaining 8.9g at ~1g/s
  TEST_ASSERT_DOUBLE_WITHIN(200.0, 18900.0, model.predictStopTimeMs(18.0));
}

// ---------------------------------------------------------------------------
// Cold-start blending: gradual, not a hard cutover
// ---------------------------------------------------------------------------

void test_topup_model_blend_is_gradual_not_hard_cutover(void) {
  const double alt_slope = 1.4;
  const double deadtime_s = 0.310;

  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.05, model.slope());

  std::mt19937 rng = makeRng();
  std::normal_distribution<double> noise(0.0, 0.05);
  std::uniform_real_distribution<double> t_dist(400.0, 1200.0);

  int64_t now = 1000;
  for (int i = 0; i < 5; ++i) {
    double t_ms = t_dist(rng);
    double truth = alt_slope * (t_ms / 1000.0 - deadtime_s);
    model.recordPulse(t_ms, truth + noise(rng), now);
    now += 5;
  }
  double slope_after_5 = model.slope();
  // Moved meaningfully off the prior, but 5 points can't outweigh the ~18
  // pseudo-observation prior -- should still land closer to 1.05 than 1.4.
  TEST_ASSERT_TRUE(slope_after_5 > 1.05 + 1e-4);
  TEST_ASSERT_TRUE(slope_after_5 < 1.05 + (alt_slope - 1.05) * 0.6);

  for (int i = 0; i < 200; ++i) {
    double t_ms = t_dist(rng);
    double truth = alt_slope * (t_ms / 1000.0 - deadtime_s);
    model.recordPulse(t_ms, truth + noise(rng), now);
    now += 5;
  }
  double slope_after_205 = model.slope();
  // With real data now dominating, should have converged much closer to truth.
  TEST_ASSERT_TRUE(slope_after_205 > slope_after_5);
  TEST_ASSERT_DOUBLE_WITHIN(0.1, alt_slope, slope_after_205);
}

// ---------------------------------------------------------------------------
// Outlier rejection
// ---------------------------------------------------------------------------

void test_topup_model_rejects_hard_bound_outlier(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  // Matches the real garbage value topup-model.md §1.3 found in the field.
  TopupModel::PulseResult r = model.recordPulse(700.0, 483.0, 1000);

  TEST_ASSERT_TRUE(r.hard_rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.05, model.slope());
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 310.0, model.deadtimeMs());
}

void test_topup_model_rejects_statistically_inconsistent_outlier(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  // In-bounds ([-1,10]) but wildly inconsistent with the current fit at
  // this runtime (predicted ~= 0.41g).
  TopupModel::PulseResult r = model.recordPulse(700.0, 8.0, 1000);

  TEST_ASSERT_TRUE(r.stat_rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.05, model.slope());
}

// ---------------------------------------------------------------------------
// Recency decay
// ---------------------------------------------------------------------------

void test_recency_decay_reduces_old_observations_influence(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();

  TopupModel::Config decaying_cfg;
  decaying_cfg.half_life_days = 15.0;
  TopupModel decaying(persisted, decaying_cfg);

  TopupModel::Config static_cfg;
  static_cfg.half_life_days = 0.0;  // decay disabled
  TopupModel steady(persisted, static_cfg);

  // Identical "old" pulse, consistent with the prior, fed to both. Uses a
  // nonzero epoch: 0 is the struct's "never updated" sentinel, and using it
  // here would make the *next* call look like a first-ever update too,
  // masking the decay this test is trying to observe.
  int64_t old_epoch = 1000;
  decaying.recordPulse(700.0, 1.05 * (0.7 - 0.310), old_epoch);
  steady.recordPulse(700.0, 1.05 * (0.7 - 0.310), old_epoch);

  // 45 days later (3 half-lives for the decaying model), a handful of
  // identical "fresh" pulses implying a different slope.
  const double alt_slope = 1.3;
  int64_t later = old_epoch + 45 * 86400;
  const double xs_ms[] = {400.0, 600.0, 800.0, 1000.0, 1200.0};

  for (int i = 0; i < 5; ++i) {
    double fresh_value = alt_slope * (xs_ms[i] / 1000.0 - 0.310);
    decaying.recordPulse(xs_ms[i], fresh_value, later + i * 5);
    steady.recordPulse(xs_ms[i], fresh_value, later + i * 5);
  }

  double decaying_gap = std::fabs(decaying.slope() - alt_slope);
  double steady_gap = std::fabs(steady.slope() - alt_slope);

  // The decayed model discounted its older evidence more, so the same
  // fresh evidence pulled it much closer to alt_slope than the
  // non-decaying model, which is still anchored by ~19 units of old weight.
  TEST_ASSERT_TRUE(decaying_gap < steady_gap);
}

// ---------------------------------------------------------------------------
// computeTopupDecision, §4.5
// ---------------------------------------------------------------------------

void test_decision_refuses_below_min_controllable_gap(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  TopupModel::Decision d = model.computeTopupDecision(0.1, 0.3);
  TEST_ASSERT_FALSE(d.should_fire);
}

void test_decision_aims_at_ninety_percent_of_gap(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  // Large overshoot budget so the upper-bound shrink never engages.
  TopupModel::Decision d = model.computeTopupDecision(1.0, 5.0);

  TEST_ASSERT_TRUE(d.should_fire);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.9, d.predicted_weight_g);
  // duration = deadtime_ms + 1000*0.9/1.05
  double expected_ms = 310.0 + 1000.0 * 0.9 / 1.05;
  TEST_ASSERT_UINT32_WITHIN(1, static_cast<uint32_t>(expected_ms + 0.5), d.duration_ms);
}

void test_decision_shrinks_duration_for_overshoot_budget(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  TopupModel::Decision unshrunk = model.computeTopupDecision(1.0, 5.0);
  TopupModel::Decision shrunk = model.computeTopupDecision(1.0, 0.5);

  TEST_ASSERT_TRUE(shrunk.should_fire);
  TEST_ASSERT_TRUE(shrunk.duration_ms < unshrunk.duration_ms);
  TEST_ASSERT_TRUE(shrunk.predicted_weight_g < unshrunk.predicted_weight_g);

  // safe_weight = overshoot_budget - k_sigma*residual_sd; derive the
  // expected sd from the model itself (the seeded fit's dof-corrected
  // residual sd is close to, but not bit-identical to, the nominal prior).
  double safe_weight = 0.5 - 2.0 * model.residualStdDev();
  double expected_ms = model.deadtimeMs() + 1000.0 * safe_weight / model.slope();
  TEST_ASSERT_UINT32_WITHIN(1, static_cast<uint32_t>(expected_ms + 0.5), shrunk.duration_ms);
}

void test_decision_respects_hygiene_clamp(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  // A huge gap pushes the naive duration well past the 5s hygiene clamp
  // (mirrors the legacy `top_up_seconds > 5.0f` check).
  TopupModel::Decision d = model.computeTopupDecision(10.0, 20.0);
  TEST_ASSERT_FALSE(d.should_fire);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();

  RUN_TEST(test_weighted_linear_fit_recovers_exact_line);
  RUN_TEST(test_weighted_linear_fit_decay_shrinks_effective_weight);

  RUN_TEST(test_topup_model_v1_roundtrips_through_byte_copy);

  RUN_TEST(test_cold_start_topup_model_reports_prior);
  RUN_TEST(test_cold_start_main_grind_model_reports_prior);

  RUN_TEST(test_topup_model_converges_to_known_slope_and_deadtime);
  RUN_TEST(test_main_grind_model_converges_to_known_rate);
  RUN_TEST(test_main_grind_model_predicts_sane_stop_time);

  RUN_TEST(test_topup_model_blend_is_gradual_not_hard_cutover);

  RUN_TEST(test_topup_model_rejects_hard_bound_outlier);
  RUN_TEST(test_topup_model_rejects_statistically_inconsistent_outlier);

  RUN_TEST(test_recency_decay_reduces_old_observations_influence);

  RUN_TEST(test_decision_refuses_below_min_controllable_gap);
  RUN_TEST(test_decision_aims_at_ninety_percent_of_gap);
  RUN_TEST(test_decision_shrinks_duration_for_overshoot_budget);
  RUN_TEST(test_decision_respects_hygiene_clamp);

  return UNITY_END();
}
