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
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    original.topup_lut_duration_ms[i] = 350.0f + 10.0f * i;
    original.topup_lut_n[i] = static_cast<uint32_t>(i);
  }
  original.rate_n_effective = 18;
  original.last_updated = 1234567890LL;
  original.coast_weight_hat = 0.49f;
  original.coast_weight_precision = 39.06f;

  uint8_t buffer[sizeof(TopupModelV1)];
  std::memcpy(buffer, &original, sizeof(TopupModelV1));

  TopupModelV1 restored{};
  std::memcpy(&restored, buffer, sizeof(TopupModelV1));

  TEST_ASSERT_EQUAL_UINT8(original.version, restored.version);
  TEST_ASSERT_EQUAL_FLOAT(original.rate_hat, restored.rate_hat);
  TEST_ASSERT_EQUAL_FLOAT(original.rate_precision, restored.rate_precision);
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    TEST_ASSERT_EQUAL_FLOAT(original.topup_lut_duration_ms[i], restored.topup_lut_duration_ms[i]);
    TEST_ASSERT_EQUAL_UINT32(original.topup_lut_n[i], restored.topup_lut_n[i]);
  }
  TEST_ASSERT_EQUAL_UINT32(original.rate_n_effective, restored.rate_n_effective);
  TEST_ASSERT_EQUAL_INT64(original.last_updated, restored.last_updated);
  TEST_ASSERT_EQUAL_FLOAT(original.coast_weight_hat, restored.coast_weight_hat);
  TEST_ASSERT_EQUAL_FLOAT(original.coast_weight_precision, restored.coast_weight_precision);
}

// ---------------------------------------------------------------------------
// Cold start
// ---------------------------------------------------------------------------

void test_cold_start_topup_model_reports_prior(void) {
  // Every bucket is seeded from the historical slope/deadtime fit via
  // the same formula makeDefaultTopupModel() uses: deadtime +
  // 1000*aim_fraction*bucket_upper/slope.
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  double expected_bucket0 = 310.0 + 1000.0 * 0.85 * 0.1 / 1.05;
  double expected_bucket9 = 310.0 + 1000.0 * 0.85 * 1.0 / 1.05;
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, expected_bucket0, model.durationForBucket(0));
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, expected_bucket9, model.durationForBucket(9));
}

void test_cold_start_main_grind_model_reports_prior(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  MainGrindModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.00, model.currentRateEstimate());
}

void test_cold_start_coast_model_reports_prior(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  CoastModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());
}

// ---------------------------------------------------------------------------
// Convergence on synthetic data matching topup-model.md's real numbers
// ---------------------------------------------------------------------------

void test_topup_lut_converges_toward_bucket_aim_weight(void) {
  // Simulates a (for this test only) deterministic pulse response to
  // verify the online per-bucket update rule actually converges toward
  // producing that bucket's aim weight, not just nudges randomly.
  const double true_slope = 1.05;  // g/s -- matches TopupPriors::kTopupSlope
  const double true_deadtime_ms = 310.0;

  const int bucket = 4;  // gap in (0.4, 0.5]
  const double gap_g = 0.45;
  const double bucket_upper = (bucket + 1) * 0.1;
  const double aim_weight = 0.85 * bucket_upper;  // TopupModel::Config's default aim_fraction

  TopupModelV1 persisted = makeDefaultTopupModel();
  // Deliberately wrong starting point (well below what's needed) so
  // convergence is actually exercised, not already correct at seed time.
  persisted.topup_lut_duration_ms[bucket] = 350.0f;
  TopupModel model(persisted);

  for (int i = 0; i < 200; ++i) {
    double duration_ms = model.durationForBucket(bucket);
    double weight_added = std::max(0.0, true_slope * (duration_ms - true_deadtime_ms) / 1000.0);
    model.recordPulse(gap_g, weight_added);
  }

  double final_duration = model.durationForBucket(bucket);
  double final_weight = std::max(0.0, true_slope * (final_duration - true_deadtime_ms) / 1000.0);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, aim_weight, final_weight);
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

void test_main_grind_model_predicts_earlier_stop_time_with_coast(void) {
  // Noiseless, known rate: with coast anticipation the model should aim to
  // stop earlier than the no-coast prediction, by roughly the coast amount
  // (at ~1g/s, X grams of coast should pull the stop time back by ~1000*X ms).
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

  const double target = 18.0;
  const double coast_g = 0.49;
  double no_coast_stop_ms = model.predictStopTimeMs(target);
  double with_coast_stop_ms = model.predictStopTimeMsWithCoast(target, coast_g);

  TEST_ASSERT_TRUE(with_coast_stop_ms < no_coast_stop_ms);
  double expected_delta_ms = 1000.0 * coast_g / rate;
  TEST_ASSERT_DOUBLE_WITHIN(50.0, expected_delta_ms,
                             no_coast_stop_ms - with_coast_stop_ms);
}

void test_main_grind_model_ignores_sudden_weight_drop(void) {
  // A cup lifted off the scale mid-grind looks like a sudden large weight
  // decrease -- addSample() must hold the last known-good sample rather
  // than fold it in, so neither the fit nor the stop-time prediction
  // corrupts into an immediate (wrong) "already there" signal.
  TopupModelV1 persisted = makeDefaultTopupModel();
  MainGrindModel model(persisted);
  model.startSession();

  const double rate = 1.0;
  const double deadtime_s = 0.9;
  double t_ms = 0.0;
  for (; t_ms <= 5000.0; t_ms += 250.0) {
    double t_s = t_ms / 1000.0;
    double w = t_s < deadtime_s ? 0.0 : rate * (t_s - deadtime_s);
    model.addSample(t_ms, w);
  }
  // last good sample: t=5000ms, weight ~= 1.0*(5-0.9) = 4.1g

  double predicted_before = model.predictStopTimeMs(18.0);

  // Simulate the cup being lifted: weight appears to plunge.
  model.addSample(t_ms + 250.0, -120.0);

  double predicted_after = model.predictStopTimeMs(18.0);
  TEST_ASSERT_DOUBLE_WITHIN(1.0, predicted_before, predicted_after);

  // Real samples resume at the pre-glitch trajectory -- the rate estimate
  // should still reflect them, not the rejected outlier.
  for (t_ms += 500.0; t_ms <= 10000.0; t_ms += 250.0) {
    double t_s = t_ms / 1000.0;
    double w = t_s < deadtime_s ? 0.0 : rate * (t_s - deadtime_s);
    model.addSample(t_ms, w);
  }
  TEST_ASSERT_DOUBLE_WITHIN(0.05, rate, model.currentRateEstimate());
}

void test_main_grind_model_nonpositive_rate_never_predicts_immediate_stop(void) {
  // A model with no session data and a pathological (non-positive)
  // persisted prior must never report "stop right now" -- that must defer
  // to the raw-weight/safety-timeout checks, not fire on bad data.
  TopupModelV1 persisted = makeDefaultTopupModel();
  persisted.rate_hat = -1.0f;  // pathological prior, should never occur in
                                // practice (finalizeSession() validates this
                                // before persisting) but must fail safe.
  MainGrindModel model(persisted);
  model.startSession();
  model.addSample(1000.0, 1.0);

  double predicted = model.predictStopTimeMs(18.0);
  TEST_ASSERT_TRUE(predicted > 1000.0);
  TEST_ASSERT_FALSE(1000.0 >= predicted);  // never "already there"
}

// ---------------------------------------------------------------------------
// Outlier rejection
// ---------------------------------------------------------------------------

void test_topup_model_rejects_hard_bound_outlier(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);
  double duration_before = model.durationForBucket(TopupModel::bucketForGap(0.45));

  // Matches the real garbage value topup-model.md §1.3 found in the field.
  TopupModel::PulseResult r = model.recordPulse(0.45, 483.0);

  TEST_ASSERT_TRUE(r.hard_rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, duration_before,
                             model.durationForBucket(TopupModel::bucketForGap(0.45)));
}

// ---------------------------------------------------------------------------
// Recency decay
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CoastModel (Model C), §8
// ---------------------------------------------------------------------------

void test_coast_model_shifts_meaningfully_with_consistent_observations(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  CoastModel model(persisted);

  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());

  // A run of consistent observations well above the 0.49g prior should pull
  // the estimate up meaningfully, without instantly overriding the prior.
  int64_t now = 1000;
  for (int i = 0; i < 20; ++i) {
    CoastModel::RecordResult r = model.recordCoast(0.90, now);
    TEST_ASSERT_TRUE(r.accepted);
    now += 5;
  }

  double estimate = model.currentCoastEstimate();
  TEST_ASSERT_TRUE(estimate > 0.6);   // moved well off the 0.49g prior
  TEST_ASSERT_TRUE(estimate < 0.90);  // but hasn't fully overridden it either
}

void test_coast_model_rejects_negative_observation(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  CoastModel model(persisted);

  CoastModel::RecordResult r = model.recordCoast(-0.5, 1000);
  TEST_ASSERT_TRUE(r.rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());
}

void test_coast_model_rejects_absurdly_large_observation(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  CoastModel model(persisted);

  // Matches the kind of gross sensor-glitch value topup-model.md/coast
  // -effect.md both document elsewhere (e.g. the historical 483g topup
  // entry) -- physically impossible as a coast weight.
  CoastModel::RecordResult r = model.recordCoast(42.0, 1000);
  TEST_ASSERT_TRUE(r.rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());
}

void test_coast_model_recency_decay_reduces_old_observations_influence(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();

  CoastModel::Config decaying_cfg;
  decaying_cfg.half_life_days = 15.0;
  CoastModel decaying(persisted, decaying_cfg);

  CoastModel::Config static_cfg;
  static_cfg.half_life_days = 1.0e9;  // effectively no decay
  CoastModel steady(persisted, static_cfg);

  // Identical "old" observation, consistent with the prior, fed to both.
  // Uses a nonzero epoch: 0 is the struct's "never updated" sentinel.
  int64_t old_epoch = 1000;
  decaying.recordCoast(0.49, old_epoch);
  steady.recordCoast(0.49, old_epoch);

  // 45 days later (3 half-lives for the decaying model), a handful of
  // identical "fresh" observations implying a different coast weight.
  const double alt_coast = 0.80;
  int64_t later = old_epoch + 45 * 86400;
  for (int i = 0; i < 5; ++i) {
    decaying.recordCoast(alt_coast, later + i * 5);
    steady.recordCoast(alt_coast, later + i * 5);
  }

  double decaying_gap = std::fabs(decaying.currentCoastEstimate() - alt_coast);
  double steady_gap = std::fabs(steady.currentCoastEstimate() - alt_coast);

  // The decayed model discounted its older evidence more, so the same
  // fresh evidence pulled it closer to alt_coast than the non-decaying model.
  TEST_ASSERT_TRUE(decaying_gap < steady_gap);
}

// ---------------------------------------------------------------------------
// computeTopupDecision, §4.5
// ---------------------------------------------------------------------------

void test_bucket_for_gap_maps_correctly(void) {
  TEST_ASSERT_EQUAL_INT(0, TopupModel::bucketForGap(0.05));
  TEST_ASSERT_EQUAL_INT(0, TopupModel::bucketForGap(0.1));   // upper edge included
  TEST_ASSERT_EQUAL_INT(1, TopupModel::bucketForGap(0.11));  // just past the edge
  TEST_ASSERT_EQUAL_INT(4, TopupModel::bucketForGap(0.45));
  TEST_ASSERT_EQUAL_INT(9, TopupModel::bucketForGap(1.0));
  TEST_ASSERT_EQUAL_INT(9, TopupModel::bucketForGap(1.5));  // clamped, no bucket beyond 1.0g
}

void test_decision_fires_using_bucket_duration_and_aim_weight(void) {
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  TopupModel::Decision d = model.computeTopupDecision(0.45);  // bucket 4

  TEST_ASSERT_TRUE(d.should_fire);
  TEST_ASSERT_EQUAL_INT(4, d.bucket);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.85 * 0.5, d.aim_weight_g);
  TEST_ASSERT_UINT32_WITHIN(1, static_cast<uint32_t>(model.durationForBucket(4) + 0.5),
                             d.duration_ms);
}

void test_decision_fires_even_for_a_tiny_0_1g_gap(void) {
  // Owner request: topup should still attempt to close even a 0.1g gap,
  // not just accept it as undershoot -- no min-controllable-gap cutoff
  // in the bucket-based decision (DosingTask.cpp's own min_topup_grams
  // setting, ~0.08g by default, is the only "basically zero" gate).
  TopupModelV1 persisted = makeDefaultTopupModel();
  TopupModel model(persisted);

  TopupModel::Decision d = model.computeTopupDecision(0.1);
  TEST_ASSERT_TRUE(d.should_fire);
  TEST_ASSERT_EQUAL_INT(0, d.bucket);
}

void test_topup_model_falls_back_to_default_seed_for_invalid_persisted_bucket(void) {
  // An out-of-hygiene-bounds persisted duration (corrupt NVS, or an
  // older schema's blob reinterpreted) must not be trusted verbatim --
  // falls back to the same formula a fresh bucket is seeded with.
  TopupModelV1 persisted = makeDefaultTopupModel();
  double expected = persisted.topup_lut_duration_ms[3];
  persisted.topup_lut_duration_ms[3] = 50.0f;  // below the 350ms hygiene floor

  TopupModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, expected, model.durationForBucket(3));
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
  RUN_TEST(test_cold_start_coast_model_reports_prior);

  RUN_TEST(test_topup_lut_converges_toward_bucket_aim_weight);
  RUN_TEST(test_main_grind_model_converges_to_known_rate);
  RUN_TEST(test_main_grind_model_predicts_sane_stop_time);
  RUN_TEST(test_main_grind_model_predicts_earlier_stop_time_with_coast);
  RUN_TEST(test_main_grind_model_ignores_sudden_weight_drop);
  RUN_TEST(test_main_grind_model_nonpositive_rate_never_predicts_immediate_stop);

  RUN_TEST(test_topup_model_rejects_hard_bound_outlier);
  RUN_TEST(test_topup_model_falls_back_to_default_seed_for_invalid_persisted_bucket);

  RUN_TEST(test_coast_model_shifts_meaningfully_with_consistent_observations);
  RUN_TEST(test_coast_model_rejects_negative_observation);
  RUN_TEST(test_coast_model_rejects_absurdly_large_observation);
  RUN_TEST(test_coast_model_recency_decay_reduces_old_observations_influence);

  RUN_TEST(test_bucket_for_gap_maps_correctly);
  RUN_TEST(test_decision_fires_using_bucket_duration_and_aim_weight);
  RUN_TEST(test_decision_fires_even_for_a_tiny_0_1g_gap);

  return UNITY_END();
}
