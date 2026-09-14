#include "DosingModel.h"

#include <algorithm>
#include <limits>

namespace {
constexpr double kMinDenom = 1e-9;
constexpr double kMinWeight = 1e-9;
constexpr double kSecondsPerDay = 86400.0;
/*
 * Floor for residual variance so a (near-)perfect fit yields a very high
 * but finite precision instead of literal zero/infinity -- avoids the
 * fused estimate blowing up or collapsing to "no information" on
 * unusually clean data (e.g. synthetic test fixtures).
 */
constexpr double kMinResidualVariance = 1e-6;
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
  // Standard weighted-least-squares identity via the normal equations:
  // SSE = Swyy - a*Swy - b*Swxy.
  double sse = m_swyy - a * m_swy - b * m_swxy;
  if (sse < 0.0) sse = 0.0;  // guard tiny negative from float error
  // Floor the degrees of freedom too: heavy decay can push Sw arbitrarily
  // close to (or below) 2 without the fit becoming meaningless.
  double dof = std::max(m_sw - 2.0, 1.0);
  return std::max(sse / dof, kMinResidualVariance);
}

double WeightedLinearFit::residualStdDev() const {
  return std::sqrt(residualVariance());
}

double WeightedLinearFit::slopeVariance() const {
  if (!hasFit()) return std::numeric_limits<double>::infinity();
  return residualVariance() * m_sw / denom();
}

// --- TopupModelV1 -----------------------------------------------------------

TopupModelV1 makeDefaultTopupModel() {
  TopupModelV1 m{};
  m.version = kTopupModelVersion;
  m.rate_hat = TopupPriors::kRateHat;
  m.rate_precision = 1.0f / (TopupPriors::kRateSd * TopupPriors::kRateSd);
  m.rate_n_effective = TopupPriors::kRateN0;
  m.last_updated = 0;
  m.coast_weight_hat = TopupPriors::kCoastWeightHat;
  m.coast_weight_precision =
      1.0f / (TopupPriors::kCoastWeightSd * TopupPriors::kCoastWeightSd);

  // Seeds every bucket's duration from the historical slope/deadtime
  // fit (duration = deadtime + aim_weight / slope) as a starting point
  // for TopupModel's per-bucket online tuning (see
  // TopupModel::recordPulse) to correct from real pulses.
  TopupModel::Config default_cfg;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    double bucket_upper = (i + 1) * kTopupLutBucketWidthG;
    double aim = default_cfg.aim_fraction * bucket_upper;
    double duration = TopupPriors::kTopupDeadtimeMs + 1000.0 * aim / TopupPriors::kTopupSlope;
    m.topup_lut_duration_ms[i] = static_cast<float>(duration);
    m.topup_lut_n[i] = 0;
  }
  return m;
}

// --- MainGrindModel ----------------------------------------------------------

MainGrindModel::MainGrindModel(const TopupModelV1 &persisted)
    : MainGrindModel(persisted, Config()) {}

MainGrindModel::MainGrindModel(const TopupModelV1 &persisted, Config cfg)
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
      // Implausible for too long to be a transient spike -- fall through
      // and accept it as the new real weight.
    }
    m_rejecting = false;
  }

  m_have_last_sample = true;
  m_last_runtime_ms = runtime_ms;
  m_last_weight_g = weight_g;

  if (runtime_ms < m_cfg.deadtime_ms) return;  // chute not primed yet
  // Fit x in seconds so the resulting slope comes out directly in g/s,
  // matching rate_hat's persisted units.
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

  // Bayesian normal-normal precision-weighted combination: dominated by
  // whichever side currently has more precision, with no hard cutover.
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
  // A non-positive rate means the estimate itself is untrustworthy, not
  // that the target has already been reached -- returning "no prediction"
  // here defers the stop decision to the raw-weight and safety-timeout
  // checks instead of firing early on bad data.
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
    // Discard this session's contribution outright and keep the last
    // known-good persisted state untouched.
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

TopupModelV1 MainGrindModel::dumpPersisted(const TopupModelV1 &base) const {
  TopupModelV1 out = base;
  out.rate_hat = static_cast<float>(m_rate_hat);
  out.rate_precision = static_cast<float>(m_rate_precision);
  out.rate_n_effective = m_rate_n_effective;
  out.last_updated = m_last_updated;
  return out;
}

// --- TopupModel ---------------------------------------------------------------

TopupModel::TopupModel(const TopupModelV1 &persisted) : TopupModel(persisted, Config()) {}

TopupModel::TopupModel(const TopupModelV1 &persisted, Config cfg) : m_cfg(cfg) {
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    double duration = persisted.topup_lut_duration_ms[i];
    // A zero/implausible persisted entry (e.g. an older-schema blob that
    // never had this bucket) falls back to the same formula
    // makeDefaultTopupModel() seeds fresh buckets with, rather than
    // silently commanding a 0ms pulse.
    if (!(duration >= m_cfg.hygiene_min_duration_ms && duration <= m_cfg.hygiene_max_duration_ms)) {
      double bucket_upper = (i + 1) * kTopupLutBucketWidthG;
      double aim = m_cfg.aim_fraction * bucket_upper;
      duration = TopupPriors::kTopupDeadtimeMs + 1000.0 * aim / TopupPriors::kTopupSlope;
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
  double bucket_upper = (d.bucket + 1) * kTopupLutBucketWidthG;
  d.aim_weight_g = m_cfg.aim_fraction * bucket_upper;

  double duration_ms = m_duration_ms[d.bucket];
  // Safety net: a bucket's tuned duration should already respect these
  // bounds (recordPulse clamps every update to them), but never fire a
  // pulse outside them regardless of how it got there.
  if (duration_ms < m_cfg.hygiene_min_duration_ms || duration_ms > m_cfg.hygiene_max_duration_ms) {
    return d;
  }

  d.should_fire = true;
  d.duration_ms = static_cast<uint32_t>(duration_ms + 0.5);
  return d;
}

TopupModel::PulseResult TopupModel::recordPulse(double gap_at_fire_g, double weight_increment_g) {
  PulseResult r;

  /*
   * Hard bounds first: catches sensor-glitch garbage (e.g. the
   * historical 483g / -129g entries) before it can influence anything.
   */
  if (weight_increment_g < m_cfg.reject_hard_min_g ||
      weight_increment_g > m_cfg.reject_hard_max_g) {
    r.hard_rejected = true;
    return r;
  }

  int bucket = bucketForGap(gap_at_fire_g);
  double bucket_upper = (bucket + 1) * kTopupLutBucketWidthG;
  double aim_weight = m_cfg.aim_fraction * bucket_upper;
  double error = aim_weight - weight_increment_g;  // > 0 == undershot this pulse

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

TopupModelV1 TopupModel::dumpPersisted(const TopupModelV1 &base) const {
  TopupModelV1 out = base;
  for (int i = 0; i < kTopupLutBuckets; ++i) {
    out.topup_lut_duration_ms[i] = static_cast<float>(m_duration_ms[i]);
    out.topup_lut_n[i] = m_n[i];
  }
  return out;
}

// --- CoastModel ----------------------------------------------------------------

CoastModel::CoastModel(const TopupModelV1 &persisted)
    : CoastModel(persisted, Config()) {}

CoastModel::CoastModel(const TopupModelV1 &persisted, Config cfg)
    : m_cfg(cfg),
      m_coast_hat(persisted.coast_weight_hat),
      m_coast_precision(persisted.coast_weight_precision),
      m_last_updated(persisted.last_updated) {}

CoastModel::RecordResult CoastModel::recordCoast(double observed_coast_g,
                                                   int64_t now_epoch_s) {
  RecordResult r;

  /*
   * Plausibility bounds first, mirroring TopupModel's hard-bounds check:
   * reject garbage (negative coast, or an absurdly large
   * value) outright, before it can touch the persisted estimate at all.
   */
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

  /*
   * Bayesian normal-normal scalar blend: the new observation carries
   * fixed precision 1/observation_sd^2 (there's no in-session fit to
   * derive a per-observation precision from, unlike Model A's
   * session_precision).
   */
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

TopupModelV1 CoastModel::dumpPersisted(const TopupModelV1 &base) const {
  TopupModelV1 out = base;
  out.coast_weight_hat = static_cast<float>(m_coast_hat);
  out.coast_weight_precision = static_cast<float>(m_coast_precision);
  out.last_updated = m_last_updated;
  return out;
}
