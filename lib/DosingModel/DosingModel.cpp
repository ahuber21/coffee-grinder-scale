#include "DosingModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kMinDenom = 1e-9;
constexpr double kMinWeight = 1e-9;
constexpr double kSecondsPerDay = 86400.0;
/// Floor on residual variance, so a near-perfect fit gives a huge but finite precision.
constexpr double kMinResidualVariance = 1e-6;

/** The weight a topup pulse for `bucket` aims to add. */
double aimWeightForBucket(int bucket, double aim_fraction) {
  return aim_fraction * (bucket + 1) * kTopupLutBucketWidthG;
}

/** Initial pulse duration for `bucket`, from the prior deadtime and slope. */
double seedDurationMs(int bucket, double aim_fraction) {
  return TopupPriors::kTopupDeadtimeMs +
         1000.0 * aimWeightForBucket(bucket, aim_fraction) / TopupPriors::kTopupSlope;
}
}  // namespace

// --- WeightedLinearFit ------------------------------------------------------

void WeightedLinearFit::reset() {
  m_sw = m_swx = m_swy = m_swxx = m_swxy = m_swyy = 0.0;
}

void WeightedLinearFit::addObservation(double x, double y, double weight) {
  if (weight <= 0.0) return;
  m_sw += weight;
  m_swx += weight * x;
  m_swy += weight * y;
  m_swxx += weight * x * x;
  m_swxy += weight * x * y;
  m_swyy += weight * y * y;
}

void WeightedLinearFit::decay(double factor) {
  if (factor >= 1.0) return;
  if (factor < 0.0) factor = 0.0;
  m_sw *= factor;
  m_swx *= factor;
  m_swy *= factor;
  m_swxx *= factor;
  m_swxy *= factor;
  m_swyy *= factor;
}

bool WeightedLinearFit::hasFit() const {
  return m_sw > kMinWeight && std::fabs(denom()) > kMinDenom;
}

double WeightedLinearFit::slope() const {
  if (!hasFit()) return 0.0;
  return (m_sw * m_swxy - m_swx * m_swy) / denom();
}

double WeightedLinearFit::intercept() const {
  if (!hasFit()) return 0.0;
  double b = slope();
  return (m_swy - b * m_swx) / m_sw;
}

double WeightedLinearFit::residualVariance() const {
  if (!hasFit()) return 0.0;
  double a = intercept();
  double b = slope();
  double sse = m_swyy - a * m_swy - b * m_swxy;  // Normal-equations identity.
  if (sse < 0.0) sse = 0.0;                       // Guard tiny negatives from rounding.
  // Floored, since heavy decay can push Sw to or below 2 without making the fit meaningless.
  double dof = std::max(m_sw - 2.0, 1.0);
  return std::max(sse / dof, kMinResidualVariance);
}

double WeightedLinearFit::slopeVariance() const {
  if (!hasFit()) return std::numeric_limits<double>::infinity();
  return residualVariance() * m_sw / denom();
}

// --- DosingModelState -----------------------------------------------------------

DosingModelState makeDefaultDosingModelState() {
  DosingModelState m{};
  m.version = kDosingModelVersion;
  m.rate_hat = TopupPriors::kRateHat;
  m.rate_precision = 1.0f / (TopupPriors::kRateSd * TopupPriors::kRateSd);
  m.rate_n_effective = TopupPriors::kRateN0;
  m.last_updated = 0;
  m.coast_weight_hat = TopupPriors::kCoastWeightHat;
  m.coast_weight_precision =
      1.0f / (TopupPriors::kCoastWeightSd * TopupPriors::kCoastWeightSd);

  TopupModel::Config default_cfg;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    m.topup_lut_duration_ms[i] = static_cast<float>(seedDurationMs(i, default_cfg.aim_fraction));
    m.topup_lut_n[i] = 0;
  }
  return m;
}

// --- MainGrindModel ----------------------------------------------------------

MainGrindModel::MainGrindModel(const DosingModelState &persisted)
    : MainGrindModel(persisted, Config()) {}

MainGrindModel::MainGrindModel(const DosingModelState &persisted, Config cfg)
    : m_cfg(cfg),
      m_rate_hat(persisted.rate_hat),
      m_rate_precision(persisted.rate_precision),
      m_rate_n_effective(persisted.rate_n_effective),
      m_last_updated(persisted.last_updated) {}

void MainGrindModel::startSession() {
  m_session_fit.reset();
  m_have_last_sample = false;
  m_rejecting = false;
}

void MainGrindModel::addSample(double runtime_ms, double weight_g) {
  if (m_have_last_sample) {
    double delta = weight_g - m_last_weight_g;
    bool implausible = delta < -m_cfg.max_plausible_drop_g || delta > m_cfg.max_plausible_rise_g;
    if (implausible) {
      if (!m_rejecting) {
        m_rejecting = true;
        m_reject_started_ms = runtime_ms;
      }
      if (runtime_ms - m_reject_started_ms < m_cfg.max_reject_duration_ms) {
        return;
      }
      // Implausible for too long to be a transient: accept it as the new weight.
    }
    m_rejecting = false;
  }

  m_have_last_sample = true;
  m_last_runtime_ms = runtime_ms;
  m_last_weight_g = weight_g;

  if (runtime_ms < m_cfg.deadtime_ms) return;  // Chute not primed yet.
  // x in seconds, so the slope comes out in g/s like rate_hat.
  m_session_fit.addObservation(runtime_ms / 1000.0, weight_g, 1.0);
}

double MainGrindModel::sessionPrecision() const {
  if (!m_session_fit.hasFit()) return 0.0;
  if (m_session_fit.effectiveWeight() < m_cfg.min_session_points) return 0.0;
  double var = m_session_fit.slopeVariance();
  if (!(var > 0.0)) return 0.0;
  return 1.0 / var;
}

double MainGrindModel::currentRateEstimate() const {
  double session_precision = sessionPrecision();
  if (session_precision <= 0.0) return m_rate_hat;

  // Bayesian normal-normal blend: whichever side has more precision dominates, with no cutover.
  double prior_precision = std::max(m_rate_precision, 0.0);
  double total_precision = prior_precision + session_precision;
  if (total_precision <= 0.0) return m_rate_hat;

  double session_rate = m_session_fit.slope();
  return (prior_precision * m_rate_hat + session_precision * session_rate) /
         total_precision;
}

double MainGrindModel::predictStopTimeMs(double target_weight_g) const {
  if (!m_have_last_sample) return 0.0;
  double rate = currentRateEstimate();
  // A non-positive rate is an untrustworthy estimate, not a reached target; defer to the other stop checks.
  if (rate <= 0.0) return std::numeric_limits<double>::infinity();
  double remaining_g = target_weight_g - m_last_weight_g;
  if (remaining_g <= 0.0) return m_last_runtime_ms;
  return m_last_runtime_ms + 1000.0 * remaining_g / rate;
}

double MainGrindModel::predictStopTimeMsWithCoast(double target_weight_g,
                                                    double coast_weight_g) const {
  return predictStopTimeMs(target_weight_g - coast_weight_g);
}

bool MainGrindModel::finalizeSession(int64_t now_epoch_s) {
  double decayed_precision = m_rate_precision;
  double decayed_n = static_cast<double>(m_rate_n_effective);

  if (m_cfg.half_life_days > 0.0 && m_last_updated != 0 && now_epoch_s > m_last_updated) {
    double elapsed_days = (now_epoch_s - m_last_updated) / kSecondsPerDay;
    double factor = std::exp(-elapsed_days / m_cfg.half_life_days);
    decayed_precision *= factor;
    decayed_n *= factor;
  }

  double session_precision = sessionPrecision();
  double new_rate_hat = m_rate_hat;
  double new_precision = decayed_precision;
  double new_n = decayed_n;

  if (session_precision > 0.0) {
    double total_precision = decayed_precision + session_precision;
    new_rate_hat = (decayed_precision * m_rate_hat +
                    session_precision * m_session_fit.slope()) /
                   total_precision;
    new_precision = total_precision;
    new_n = decayed_n + m_session_fit.effectiveWeight();
  }

  if (new_rate_hat < m_cfg.min_plausible_rate ||
      new_rate_hat > m_cfg.max_plausible_rate) {
    // Discard this session's contribution and keep the last good state.
    startSession();
    return false;
  }

  m_rate_hat = new_rate_hat;
  m_rate_precision = new_precision;
  m_rate_n_effective = static_cast<uint32_t>(new_n + 0.5);
  m_last_updated = now_epoch_s;
  startSession();
  return true;
}

DosingModelState MainGrindModel::dumpPersisted(const DosingModelState &base) const {
  DosingModelState out = base;
  out.rate_hat = static_cast<float>(m_rate_hat);
  out.rate_precision = static_cast<float>(m_rate_precision);
  out.rate_n_effective = m_rate_n_effective;
  out.last_updated = m_last_updated;
  return out;
}

// --- TopupModel ---------------------------------------------------------------

TopupModel::TopupModel(const DosingModelState &persisted) : TopupModel(persisted, Config()) {}

TopupModel::TopupModel(const DosingModelState &persisted, Config cfg) : m_cfg(cfg) {
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    double duration = persisted.topup_lut_duration_ms[i];
    // An implausible entry is reseeded rather than commanding a 0ms pulse.
    if (!(duration >= m_cfg.hygiene_min_duration_ms && duration <= m_cfg.hygiene_max_duration_ms)) {
      duration = seedDurationMs(i, m_cfg.aim_fraction);
    }
    m_duration_ms[i] = duration;
    m_n[i] = persisted.topup_lut_n[i];
  }
}

int TopupModel::bucketForGap(double gap_g) {
  int bucket = static_cast<int>(std::ceil(gap_g / kTopupLutBucketWidthG)) - 1;
  if (bucket < 0) bucket = 0;
  if (bucket >= kTopupLutBuckets) bucket = kTopupLutBuckets - 1;
  return bucket;
}

TopupModel::Decision TopupModel::computeTopupDecision(double gap_g) const {
  Decision d;
  d.bucket = bucketForGap(gap_g);
  d.aim_weight_g = aimWeightForBucket(d.bucket, m_cfg.aim_fraction);

  double duration_ms = m_duration_ms[d.bucket];
  // recordPulse already clamps to these bounds; this guarantees no pulse outside them fires.
  if (duration_ms < m_cfg.hygiene_min_duration_ms || duration_ms > m_cfg.hygiene_max_duration_ms) {
    return d;
  }

  d.should_fire = true;
  d.duration_ms = static_cast<uint32_t>(duration_ms + 0.5);
  return d;
}

TopupModel::PulseResult TopupModel::recordPulse(double gap_at_fire_g, double weight_increment_g) {
  PulseResult r;

  // Hard bounds first, so sensor-glitch garbage can't influence anything.
  if (weight_increment_g < m_cfg.reject_hard_min_g ||
      weight_increment_g > m_cfg.reject_hard_max_g) {
    r.hard_rejected = true;
    return r;
  }

  int bucket = bucketForGap(gap_at_fire_g);
  double aim_weight = aimWeightForBucket(bucket, m_cfg.aim_fraction);
  double error = aim_weight - weight_increment_g;  // > 0 means this pulse undershot.

  double learn_rate = error > 0.0 ? m_cfg.learn_rate_undershoot : m_cfg.learn_rate_overshoot;
  double adjustment_ms = learn_rate * error * 1000.0 / TopupPriors::kTopupSlope;

  double new_duration = m_duration_ms[bucket] + adjustment_ms;
  if (new_duration < m_cfg.hygiene_min_duration_ms) new_duration = m_cfg.hygiene_min_duration_ms;
  if (new_duration > m_cfg.hygiene_max_duration_ms) new_duration = m_cfg.hygiene_max_duration_ms;

  m_duration_ms[bucket] = new_duration;
  if (m_n[bucket] < UINT32_MAX) m_n[bucket] += 1;

  r.accepted = true;
  r.bucket = bucket;
  return r;
}

DosingModelState TopupModel::dumpPersisted(const DosingModelState &base) const {
  DosingModelState out = base;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    out.topup_lut_duration_ms[i] = static_cast<float>(m_duration_ms[i]);
    out.topup_lut_n[i] = m_n[i];
  }
  return out;
}

// --- CoastModel ----------------------------------------------------------------

CoastModel::CoastModel(const DosingModelState &persisted)
    : CoastModel(persisted, Config()) {}

CoastModel::CoastModel(const DosingModelState &persisted, Config cfg)
    : m_cfg(cfg),
      m_coast_hat(persisted.coast_weight_hat),
      m_coast_precision(persisted.coast_weight_precision),
      m_last_updated(persisted.last_updated) {}

CoastModel::RecordResult CoastModel::recordCoast(double observed_coast_g,
                                                   int64_t now_epoch_s) {
  RecordResult r;

  // Reject garbage before it can touch the persisted estimate.
  if (observed_coast_g < m_cfg.min_plausible_coast_g ||
      observed_coast_g > m_cfg.max_plausible_coast_g) {
    r.rejected = true;
    return r;
  }

  double decayed_precision = m_coast_precision;
  if (m_cfg.half_life_days > 0.0 && m_last_updated != 0 && now_epoch_s > m_last_updated) {
    double elapsed_days = (now_epoch_s - m_last_updated) / kSecondsPerDay;
    double factor = std::exp(-elapsed_days / m_cfg.half_life_days);
    decayed_precision *= factor;
  }

  // Normal-normal blend; the observation carries the fixed precision 1/observation_sd^2.
  double obs_sd = m_cfg.observation_sd > 0.0 ? m_cfg.observation_sd
                                              : TopupPriors::kCoastWeightSd;
  double obs_precision = 1.0 / (obs_sd * obs_sd);
  double total_precision = decayed_precision + obs_precision;

  m_coast_hat = (decayed_precision * m_coast_hat + obs_precision * observed_coast_g) /
                total_precision;
  m_coast_precision = total_precision;
  m_last_updated = now_epoch_s;

  r.accepted = true;
  return r;
}

DosingModelState CoastModel::dumpPersisted(const DosingModelState &base) const {
  DosingModelState out = base;
  out.coast_weight_hat = static_cast<float>(m_coast_hat);
  out.coast_weight_precision = static_cast<float>(m_coast_precision);
  out.last_updated = m_last_updated;
  return out;
}

// --- LandingLearner -------------------------------------------------------------

LandingLearnerState makeDefaultLandingLearnerState() {
  LandingLearnerState s{};
  s.version = kLandingLearnerVersion;
  return s;
}

LandingLearner::LandingLearner(const LandingLearnerState &persisted)
    : LandingLearner(persisted, Config()) {}

LandingLearner::LandingLearner(const LandingLearnerState &persisted, Config cfg)
    : m_cfg(cfg), m_state(persisted) {}

void LandingLearner::setRate(double rate) { m_cfg.rate = rate; }

double LandingLearner::predict(bool is_double, double rate_g_s) const {
  const LandingLearnerState::Mode &m = m_state.mode[is_double ? 1 : 0];
  if (!(m.sw > kMinWeight)) return 0.0;

  double mean_x = m.sx / m.sw;
  double sxx_c = m.sxx - m.sx * m.sx / m.sw;
  double sxy_c = m.sxy - m.sx * m.sy / m.sw;
  double slope = sxy_c / (sxx_c + m_cfg.ridge);
  double prior = m_cfg.prior_weight * m_cfg.prior_weight / (m_cfg.prior_weight + m.n);
  double intercept = m.sy / (m.sw + prior);
  return intercept + slope * (rate_g_s - mean_x);
}

bool LandingLearner::record(bool is_double, double rate_g_s, double uncorrected_error_g) {
  if (!std::isfinite(rate_g_s) || !std::isfinite(uncorrected_error_g)) return false;
  if (std::fabs(uncorrected_error_g) > m_cfg.max_abs_error_g) return false;

  LandingLearnerState::Mode &m = m_state.mode[is_double ? 1 : 0];
  double keep = 1.0 - m_cfg.rate;
  m.sw = keep * m.sw + 1.0;
  m.sx = keep * m.sx + rate_g_s;
  m.sy = keep * m.sy + uncorrected_error_g;
  m.sxx = keep * m.sxx + rate_g_s * rate_g_s;
  m.sxy = keep * m.sxy + rate_g_s * uncorrected_error_g;
  m.syy = keep * m.syy + uncorrected_error_g * uncorrected_error_g;
  ++m.n;
  return true;
}

double LandingLearner::residualSd(bool is_double) const {
  const LandingLearnerState::Mode &m = m_state.mode[is_double ? 1 : 0];
  if (!(m.sw > 1.5)) return 0.0;
  double syy_c = m.syy - m.sy * m.sy / m.sw;
  double sxx_c = m.sxx - m.sx * m.sx / m.sw;
  double sxy_c = m.sxy - m.sx * m.sy / m.sw;
  double slope = sxy_c / (sxx_c + m_cfg.ridge);
  double var = (syy_c - slope * sxy_c) / m.sw;
  return std::sqrt(std::max(var, 0.0));
}

double LandingLearner::effectiveSessions(bool is_double) const {
  return m_state.mode[is_double ? 1 : 0].sw;
}

uint32_t LandingLearner::sessions(bool is_double) const {
  return m_state.mode[is_double ? 1 : 0].n;
}
