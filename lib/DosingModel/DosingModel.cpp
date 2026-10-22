#include "DosingModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr double kMinDenom = 1e-9;
constexpr double kMinWeight = 1e-9;
constexpr double kSecondsPerDay = 86400.0;
/// Floor on residual variance, so a near-perfect fit gives a huge but finite precision.
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
  m.rate_hat = ModelPriors::kRateHat;
  m.rate_precision = 1.0f / (ModelPriors::kRateSd * ModelPriors::kRateSd);
  m.rate_n_effective = ModelPriors::kRateN0;
  m.last_updated = 0;
  m.coast_weight_hat = ModelPriors::kCoastWeightHat;
  m.coast_weight_precision =
      1.0f / (ModelPriors::kCoastWeightSd * ModelPriors::kCoastWeightSd);
  return m;
}

bool migrateDosingModelV3(const void *bytes, size_t len, DosingModelState &out) {
  // Field offsets of the version-3 layout, which no struct describes any more.
  constexpr size_t kVersion = 0, kRateHat = 4, kRatePrecision = 8, kRateNEff = 92,
                   kLastUpdated = 96, kCoastHat = 104, kCoastPrecision = 108;
  if (len != kDosingModelV3Bytes) return false;
  const uint8_t *raw = static_cast<const uint8_t *>(bytes);
  if (raw[kVersion] != 3) return false;

  DosingModelState migrated = makeDefaultDosingModelState();
  std::memcpy(&migrated.rate_hat, raw + kRateHat, sizeof(float));
  std::memcpy(&migrated.rate_precision, raw + kRatePrecision, sizeof(float));
  std::memcpy(&migrated.rate_n_effective, raw + kRateNEff, sizeof(uint32_t));
  std::memcpy(&migrated.last_updated, raw + kLastUpdated, sizeof(int64_t));
  std::memcpy(&migrated.coast_weight_hat, raw + kCoastHat, sizeof(float));
  std::memcpy(&migrated.coast_weight_precision, raw + kCoastPrecision, sizeof(float));
  out = migrated;
  return true;
}

// --- MainGrindModel ----------------------------------------------------------

MainGrindModel::MainGrindModel(const DosingModelState &persisted)
    : MainGrindModel(persisted, Config()) {}

MainGrindModel::MainGrindModel(const DosingModelState &persisted, Config cfg)
    : m_cfg(cfg),
      m_rate_hat(persisted.rate_hat),
      m_rate_precision(persisted.rate_precision),
      m_rate_n_effective(persisted.rate_n_effective),
      m_last_updated(persisted.last_updated) {
  m_rate_precision = std::min(m_rate_precision, maxRatePrecision());
  m_rate_n_effective = std::min(m_rate_n_effective, static_cast<uint32_t>(m_cfg.session_window));
}

double MainGrindModel::maxRatePrecision() const {
  return m_cfg.session_window / (m_cfg.session_rate_sd * m_cfg.session_rate_sd);
}

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
  double var = std::max(m_session_fit.slopeVariance(), m_cfg.min_live_slope_sd * m_cfg.min_live_slope_sd);
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
  double keep = 1.0 - 1.0 / m_cfg.session_window;
  double decayed_precision = m_rate_precision * keep;
  double decayed_n = static_cast<double>(m_rate_n_effective) * keep;

  if (m_cfg.half_life_days > 0.0 && m_last_updated != 0 && now_epoch_s > m_last_updated) {
    double elapsed_days = (now_epoch_s - m_last_updated) / kSecondsPerDay;
    double factor = std::exp(-elapsed_days / m_cfg.half_life_days);
    decayed_precision *= factor;
    decayed_n *= factor;
  }

  double new_rate_hat = m_rate_hat;
  double new_precision = decayed_precision;
  double new_n = decayed_n;

  if (sessionPrecision() > 0.0) {
    double obs_precision = 1.0 / (m_cfg.session_rate_sd * m_cfg.session_rate_sd);
    double total_precision = decayed_precision + obs_precision;
    new_rate_hat = (decayed_precision * m_rate_hat + obs_precision * m_session_fit.slope()) /
                   total_precision;
    new_precision = std::min(total_precision, maxRatePrecision());
    new_n = decayed_n + 1.0;
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

double MainGrindModel::hardTimeoutMs(double target_weight_g, double overrun_ms) const {
  // The stored rate is kept inside the plausible range, so this floor only guards a corrupt value.
  double rate = std::max(m_rate_hat, m_cfg.min_plausible_rate);
  return 1000.0 * target_weight_g / rate + overrun_ms;
}

DosingModelState MainGrindModel::dumpPersisted(const DosingModelState &base) const {
  DosingModelState out = base;
  out.rate_hat = static_cast<float>(m_rate_hat);
  out.rate_precision = static_cast<float>(m_rate_precision);
  out.rate_n_effective = m_rate_n_effective;
  out.last_updated = m_last_updated;
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
                                              : ModelPriors::kCoastWeightSd;
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

  Params p = params(is_double);
  return p.intercept_g + p.slope_g_per_g_s * (rate_g_s - p.mean_rate_g_s);
}

LandingLearner::Params LandingLearner::params(bool is_double) const {
  const LandingLearnerState::Mode &m = m_state.mode[is_double ? 1 : 0];
  Params p;
  p.sessions = m.n;
  p.effective_sessions = static_cast<float>(m.sw);
  if (!(m.sw > kMinWeight)) return p;

  double sxx_c = m.sxx - m.sx * m.sx / m.sw;
  double sxy_c = m.sxy - m.sx * m.sy / m.sw;
  double prior = m_cfg.prior_weight * m_cfg.prior_weight / (m_cfg.prior_weight + m.n);
  p.mean_rate_g_s = static_cast<float>(m.sx / m.sw);
  p.intercept_g = static_cast<float>(m.sy / (m.sw + prior));
  p.slope_g_per_g_s = static_cast<float>(sxy_c / (sxx_c + m_cfg.ridge));
  p.residual_sd_g = static_cast<float>(residualSd(is_double));
  return p;
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
