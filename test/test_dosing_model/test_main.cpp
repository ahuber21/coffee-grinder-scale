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

// --- WeightedLinearFit ---------------------------------------------------------

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

// --- DosingModelState -- POD / NVS round-trip ----------------------------------

void test_dosing_model_state_roundtrips_through_byte_copy(void) {
  DosingModelState original{};
  original.version = kDosingModelVersion;
  original.rate_hat = 1.0123f;
  original.rate_precision = 123.45f;
  original.rate_n_effective = 18;
  original.last_updated = 1234567890LL;
  original.coast_weight_hat = 0.49f;
  original.coast_weight_precision = 39.06f;

  uint8_t buffer[sizeof(DosingModelState)];
  std::memcpy(buffer, &original, sizeof(DosingModelState));

  DosingModelState restored{};
  std::memcpy(&restored, buffer, sizeof(DosingModelState));

  TEST_ASSERT_EQUAL_UINT8(original.version, restored.version);
  TEST_ASSERT_EQUAL_FLOAT(original.rate_hat, restored.rate_hat);
  TEST_ASSERT_EQUAL_FLOAT(original.rate_precision, restored.rate_precision);
  TEST_ASSERT_EQUAL_UINT32(original.rate_n_effective, restored.rate_n_effective);
  TEST_ASSERT_EQUAL_INT64(original.last_updated, restored.last_updated);
  TEST_ASSERT_EQUAL_FLOAT(original.coast_weight_hat, restored.coast_weight_hat);
  TEST_ASSERT_EQUAL_FLOAT(original.coast_weight_precision, restored.coast_weight_precision);
}

// --- Cold start ----------------------------------------------------------------

void test_cold_start_main_grind_model_reports_prior(void) {
  DosingModelState persisted = makeDefaultDosingModelState();
  MainGrindModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.00, model.currentRateEstimate());
}

void test_cold_start_coast_model_reports_prior(void) {
  DosingModelState persisted = makeDefaultDosingModelState();
  CoastModel model(persisted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());
}

// --- Convergence on synthetic data ---------------------------------------------

void test_main_grind_model_converges_to_known_rate(void) {
  const double true_rate = 1.0;
  const double deadtime_s = 0.9;
  const double noise_sd = 0.03;

  DosingModelState persisted = makeDefaultDosingModelState();
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

/** Runs one noisy 500Hz grind at `rate` and folds it into `state`. */
static DosingModelState runGrindSession(const DosingModelState &state, double rate, std::mt19937 &rng,
                                        int64_t now_s) {
  MainGrindModel model(state);
  model.startSession();
  std::normal_distribution<double> noise(0.0, 0.05);
  for (double t_ms = 0.0; t_ms <= 10000.0; t_ms += 2.0) {
    double t_s = t_ms / 1000.0;
    double truth = t_s < 0.9 ? 0.0 : rate * (t_s - 0.9);
    model.addSample(t_ms, truth + noise(rng));
  }
  TEST_ASSERT_TRUE(model.finalizeSession(now_s));
  return model.dumpPersisted(state);
}

void test_main_grind_model_rate_uncertainty_stays_realistic(void) {
  std::mt19937 rng = makeRng();
  DosingModelState state = makeDefaultDosingModelState();
  for (int i = 0; i < 300; ++i) {
    state = runGrindSession(state, 0.96, rng, 2000000000LL + i * 60);
  }
  double sd = 1.0 / std::sqrt(state.rate_precision);
  TEST_ASSERT_TRUE(sd > 0.01);
  TEST_ASSERT_TRUE(state.rate_n_effective <= 30);
  TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.96, state.rate_hat);
}

void test_main_grind_model_keeps_adapting_after_many_sessions(void) {
  std::mt19937 rng = makeRng();
  DosingModelState state = makeDefaultDosingModelState();
  for (int i = 0; i < 100; ++i) {
    state = runGrindSession(state, 1.0, rng, 2000000000LL + i * 60);
  }
  for (int i = 0; i < 60; ++i) {
    state = runGrindSession(state, 0.8, rng, 2000100000LL + i * 60);
  }
  TEST_ASSERT_DOUBLE_WITHIN(0.05, 0.8, state.rate_hat);
}

void test_main_grind_model_clamps_a_saturated_persisted_precision(void) {
  DosingModelState saturated = makeDefaultDosingModelState();
  saturated.rate_hat = 0.9645f;
  saturated.rate_precision = 2.0e9f;
  saturated.rate_n_effective = 1253574;
  MainGrindModel model(saturated);
  DosingModelState out = model.dumpPersisted(saturated);
  TEST_ASSERT_EQUAL_FLOAT(0.9645f, out.rate_hat);
  TEST_ASSERT_TRUE(out.rate_precision <= 30.0f / (0.09f * 0.09f) + 1.0f);
  TEST_ASSERT_TRUE(out.rate_n_effective <= 30);
}

void test_main_grind_model_predicts_sane_stop_time(void) {
  // Noiseless, known rate: stop-time prediction should match closed form.
  DosingModelState persisted = makeDefaultDosingModelState();
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
  // Noiseless, known rate: X grams of coast should pull the stop ~1000*X ms earlier at ~1g/s.
  DosingModelState persisted = makeDefaultDosingModelState();
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
  // A cup lifted mid-grind reads as a sudden drop; addSample() must hold the last good sample.
  DosingModelState persisted = makeDefaultDosingModelState();
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

void test_main_grind_model_ignores_sudden_weight_spike(void) {
  // A clump impact reads heavier than its settled mass; folding the spike in would
  // make predictStopTimeMs() report "at target" early.
  DosingModelState persisted = makeDefaultDosingModelState();
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

  // A spike straight to the target, the case that would otherwise report "stop now".
  model.addSample(t_ms + 250.0, 18.0);

  double predicted_after = model.predictStopTimeMs(18.0);
  TEST_ASSERT_DOUBLE_WITHIN(1.0, predicted_before, predicted_after);
  TEST_ASSERT_FALSE(t_ms + 250.0 >= predicted_after);  // must not say "stop now"

  // Real samples resume at the pre-glitch trajectory -- the rate estimate
  // should still reflect them, not the rejected outlier.
  for (t_ms += 500.0; t_ms <= 10000.0; t_ms += 250.0) {
    double t_s = t_ms / 1000.0;
    double w = t_s < deadtime_s ? 0.0 : rate * (t_s - deadtime_s);
    model.addSample(t_ms, w);
  }
  TEST_ASSERT_DOUBLE_WITHIN(0.05, rate, model.currentRateEstimate());
}

void test_main_grind_model_accepts_persistent_step_change_after_reject_window(void) {
  // A reading that stays far away past max_reject_duration_ms is a real step change
  // (a cup swap), which the model must track rather than stay blind to.
  DosingModelState persisted = makeDefaultDosingModelState();
  MainGrindModel::Config cfg;
  MainGrindModel model(persisted, cfg);
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

  // A step change lands and holds -- every sample here is still far from
  // the last accepted 4.1g, so each is implausible on its own.
  for (int i = 0; i < 3; ++i) {
    t_ms += 250.0;
    model.addSample(t_ms, 18.0);
  }
  // 750ms of persistent rejection is past max_reject_duration_ms (500ms
  // by default) -- the model must have accepted the new value by now.
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 18.0, model.currentWeightEstimate());
  TEST_ASSERT_TRUE(t_ms >= model.predictStopTimeMs(18.0));
}

void test_main_grind_model_nonpositive_rate_never_predicts_immediate_stop(void) {
  // A non-positive prior must never report "stop now"; that defers to the other stop checks.
  DosingModelState persisted = makeDefaultDosingModelState();
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

// --- Recency decay -------------------------------------------------------------

// --- CoastModel ------------------------------------------------------------

void test_coast_model_shifts_meaningfully_with_consistent_observations(void) {
  DosingModelState persisted = makeDefaultDosingModelState();
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
  DosingModelState persisted = makeDefaultDosingModelState();
  CoastModel model(persisted);

  CoastModel::RecordResult r = model.recordCoast(-0.5, 1000);
  TEST_ASSERT_TRUE(r.rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());
}

void test_coast_model_rejects_absurdly_large_observation(void) {
  DosingModelState persisted = makeDefaultDosingModelState();
  CoastModel model(persisted);

  // A gross sensor glitch (hundreds of grams), physically impossible as a coast weight.
  CoastModel::RecordResult r = model.recordCoast(42.0, 1000);
  TEST_ASSERT_TRUE(r.rejected);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.49, model.currentCoastEstimate());
}

void test_coast_model_recency_decay_reduces_old_observations_influence(void) {
  DosingModelState persisted = makeDefaultDosingModelState();

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

// --- Version-3 blob migration --------------------------------------------------

// The device's persisted blob as it was read back on 2026-10-22.
const uint8_t kDeviceV3Blob[kDosingModelV3Bytes] = {
    0x03, 0x00, 0x00, 0x00, 0x8b, 0x25, 0x76, 0x3f, 0x71, 0xc7, 0x20, 0x4f, 0x25, 0xf3, 0xd8, 0x43, 0x48, 0x70, 0xdd, 0x43, 0xf5, 0x98, 0xd7, 0x43, 0xe5, 0x99, 0x25, 0x44, 0x7c, 0xc2, 0x3d, 0x44, 0xa0, 0x2e, 0x3b, 0x44, 0x93, 0x39, 0x59, 0x44, 0x31, 0x43, 0x54, 0x44, 0xf5, 0xc7, 0x84, 0x44, 0x7c, 0x0b, 0x9e, 0x44, 0x11, 0x00, 0x00, 0x00, 0x6f, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x41, 0xd5, 0x17, 0x00, 0xd1, 0x6b, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0xd8, 0xa6, 0x3e, 0xc0, 0xfd, 0x0c, 0x46};

void test_migration_carries_rate_and_coast_from_a_real_v3_blob(void) {
  DosingModelState migrated{};
  TEST_ASSERT_TRUE(migrateDosingModelV3(kDeviceV3Blob, sizeof(kDeviceV3Blob), migrated));
  TEST_ASSERT_EQUAL_UINT8(kDosingModelVersion, migrated.version);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.9615f, migrated.rate_hat);
  TEST_ASSERT_EQUAL_UINT32(1561921, migrated.rate_n_effective);
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 0.326f, migrated.coast_weight_hat);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 9023.4f, migrated.coast_weight_precision);
  TEST_ASSERT_EQUAL_INT64(3501009, migrated.last_updated);
}

void test_migration_rejects_other_sizes_and_versions(void) {
  DosingModelState out = makeDefaultDosingModelState();
  uint8_t wrong_version[kDosingModelV3Bytes];
  std::memcpy(wrong_version, kDeviceV3Blob, sizeof(wrong_version));
  wrong_version[0] = 2;
  TEST_ASSERT_FALSE(migrateDosingModelV3(wrong_version, sizeof(wrong_version), out));
  TEST_ASSERT_FALSE(migrateDosingModelV3(kDeviceV3Blob, sizeof(kDeviceV3Blob) - 1, out));
  TEST_ASSERT_FLOAT_WITHIN(1e-6, ModelPriors::kRateHat, out.rate_hat);
}

// --- LandingLearner ------------------------------------------------------------

void test_landing_learner_predicts_zero_with_no_data(void) {
  LandingLearner l(makeDefaultLandingLearnerState());
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, l.predict(false, 1.0));
}

void test_landing_learner_converges_on_a_constant_bias(void) {
  LandingLearner l(makeDefaultLandingLearnerState());
  for (int i = 0; i < 200; ++i) l.record(false, 1.0, 0.10);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.10, l.predict(false, 1.0));
}

void test_landing_learner_first_session_barely_moves_the_stop(void) {
  LandingLearner l(makeDefaultLandingLearnerState());
  l.record(false, 1.0, 0.30);
  TEST_ASSERT_TRUE(l.predict(false, 1.0) < 0.1);
}

void test_landing_learner_learns_rate_dependence_and_cuts_spread(void) {
  auto rng = makeRng();
  std::normal_distribution<double> rate_noise(1.0, 0.1);
  std::normal_distribution<double> noise(0.0, 0.04);
  LandingLearner l(makeDefaultLandingLearnerState());
  // Truth: landing error = 0.05 + 1.2*(rate - 1.0) + noise.
  for (int i = 0; i < 300; ++i) {
    double x = rate_noise(rng);
    l.record(false, x, 0.05 + 1.2 * (x - 1.0) + noise(rng));
  }
  double sum_raw = 0.0, sum_fixed = 0.0;
  const int n = 400;
  for (int i = 0; i < n; ++i) {
    double x = rate_noise(rng);
    double y = 0.05 + 1.2 * (x - 1.0) + noise(rng);
    sum_raw += y * y;
    double resid = y - l.predict(false, x);
    sum_fixed += resid * resid;
  }
  TEST_ASSERT_TRUE(std::sqrt(sum_fixed / n) < 0.6 * std::sqrt(sum_raw / n));
}

void test_landing_learner_modes_are_independent(void) {
  LandingLearner l(makeDefaultLandingLearnerState());
  for (int i = 0; i < 100; ++i) l.record(true, 1.0, 0.2);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, l.predict(false, 1.0));
  TEST_ASSERT_TRUE(l.predict(true, 1.0) > 0.15);
}

void test_landing_learner_rejects_glitch_observations(void) {
  LandingLearner l(makeDefaultLandingLearnerState());
  TEST_ASSERT_FALSE(l.record(false, 1.0, 2.5));
  TEST_ASSERT_FALSE(l.record(false, std::nan(""), 0.1));
  TEST_ASSERT_EQUAL_UINT32(0, l.sessions(false));
}

void test_landing_learner_state_roundtrips_through_byte_copy(void) {
  LandingLearner a(makeDefaultLandingLearnerState());
  for (int i = 0; i < 30; ++i) a.record(false, 1.0 + 0.01 * i, 0.05 * (i % 3));
  LandingLearnerState raw = a.dumpPersisted();
  LandingLearnerState copy{};
  std::memcpy(&copy, &raw, sizeof(raw));
  LandingLearner b(copy);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, a.predict(false, 1.1), b.predict(false, 1.1));
}

void test_landing_learner_forgets_old_evidence(void) {
  LandingLearner::Config cfg;
  cfg.rate = 0.2;
  LandingLearner l(makeDefaultLandingLearnerState(), cfg);
  for (int i = 0; i < 100; ++i) l.record(false, 1.0, 0.3);
  for (int i = 0; i < 100; ++i) l.record(false, 1.0, 0.0);
  TEST_ASSERT_TRUE(std::fabs(l.predict(false, 1.0)) < 0.02);
}


void test_landing_learner_params_describe_the_fit_that_predict_uses(void) {
  LandingLearner l(makeDefaultLandingLearnerState());
  TEST_ASSERT_EQUAL_UINT32(0, l.params(false).sessions);
  for (int i = 0; i < 60; ++i) {
    double rate = 0.9 + 0.002 * (i % 20);
    l.record(true, rate, 0.1 + 0.5 * (rate - 0.92));
  }
  LandingLearner::Params p = l.params(true);
  TEST_ASSERT_EQUAL_UINT32(60, p.sessions);
  TEST_ASSERT_TRUE(p.effective_sessions > 10.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.919f, p.mean_rate_g_s);
  TEST_ASSERT_DOUBLE_WITHIN(1e-5, l.predict(true, p.mean_rate_g_s), p.intercept_g);
  TEST_ASSERT_DOUBLE_WITHIN(1e-4, l.predict(true, p.mean_rate_g_s + 1.0) - p.intercept_g,
                            p.slope_g_per_g_s);
  TEST_ASSERT_EQUAL_UINT32(0, l.params(false).sessions);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();

  RUN_TEST(test_weighted_linear_fit_recovers_exact_line);
  RUN_TEST(test_weighted_linear_fit_decay_shrinks_effective_weight);

  RUN_TEST(test_dosing_model_state_roundtrips_through_byte_copy);

  RUN_TEST(test_cold_start_main_grind_model_reports_prior);
  RUN_TEST(test_cold_start_coast_model_reports_prior);

  RUN_TEST(test_main_grind_model_converges_to_known_rate);
  RUN_TEST(test_main_grind_model_predicts_sane_stop_time);
  RUN_TEST(test_main_grind_model_predicts_earlier_stop_time_with_coast);
  RUN_TEST(test_main_grind_model_ignores_sudden_weight_drop);
  RUN_TEST(test_main_grind_model_ignores_sudden_weight_spike);
  RUN_TEST(test_main_grind_model_accepts_persistent_step_change_after_reject_window);
  RUN_TEST(test_main_grind_model_nonpositive_rate_never_predicts_immediate_stop);
  RUN_TEST(test_main_grind_model_rate_uncertainty_stays_realistic);
  RUN_TEST(test_main_grind_model_keeps_adapting_after_many_sessions);
  RUN_TEST(test_main_grind_model_clamps_a_saturated_persisted_precision);


  RUN_TEST(test_migration_carries_rate_and_coast_from_a_real_v3_blob);
  RUN_TEST(test_migration_rejects_other_sizes_and_versions);

  RUN_TEST(test_coast_model_shifts_meaningfully_with_consistent_observations);
  RUN_TEST(test_coast_model_rejects_negative_observation);
  RUN_TEST(test_coast_model_rejects_absurdly_large_observation);
  RUN_TEST(test_coast_model_recency_decay_reduces_old_observations_influence);

  RUN_TEST(test_landing_learner_predicts_zero_with_no_data);
  RUN_TEST(test_landing_learner_converges_on_a_constant_bias);
  RUN_TEST(test_landing_learner_first_session_barely_moves_the_stop);
  RUN_TEST(test_landing_learner_learns_rate_dependence_and_cuts_spread);
  RUN_TEST(test_landing_learner_modes_are_independent);
  RUN_TEST(test_landing_learner_rejects_glitch_observations);
  RUN_TEST(test_landing_learner_state_roundtrips_through_byte_copy);
  RUN_TEST(test_landing_learner_forgets_old_evidence);
  RUN_TEST(test_landing_learner_params_describe_the_fit_that_predict_uses);

  return UNITY_END();
}
