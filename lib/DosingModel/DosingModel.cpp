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
  m.topup_slope = TopupPriors::kTopupSlope;
  m.topup_deadtime_ms = TopupPriors::kTopupDeadtimeMs;
  m.topup_precision =
      1.0f / (TopupPriors::kTopupResidualSd * TopupPriors::kTopupResidualSd);
  m.rate_n_effective = TopupPriors::kRateN0;
  m.topup_n_effective = TopupPriors::kTopupN0;
  m.last_updated = 0;
  m.coast_weight_hat = TopupPriors::kCoastWeightHat;
  m.coast_weight_precision =
      1.0f / (TopupPriors::kCoastWeightSd * TopupPriors::kCoastWeightSd);
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
}

void MainGrindModel::addSample(double runtime_ms, double weight_g) {
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
  if (rate <= 0.0) return m_last_runtime_ms;
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

  if (m_last_updated != 0 && now_epoch_s > m_last_updated) {
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

TopupModel::TopupModel(const TopupModelV1 &persisted)
    : TopupModel(persisted, Config()) {}

TopupModel::TopupModel(const TopupModelV1 &persisted, Config cfg)
    : m_cfg(cfg), m_last_updated(persisted.last_updated) {
  seedFromPersisted(persisted);
}

void TopupModel::seedFromPersisted(const TopupModelV1 &persisted) {
  m_fit.reset();

  // The fit's x-axis is seconds (see predictedWeight()/recordPulse()), so
  // slope comes out directly in g/s, matching the persisted field's units.
  double slope = persisted.topup_slope;
  double deadtime_s = persisted.topup_deadtime_ms / 1000.0;
  double n0 = std::max<double>(persisted.topup_n_effective, 1.0);
  double residual_sd = persisted.topup_precision > 0.0f
                            ? 1.0 / std::sqrt(static_cast<double>(persisted.topup_precision))
                            : TopupPriors::kTopupResidualSd;

  /*
   * Reconstruct an equivalent WeightedLinearFit from the compact
   * persisted summary via four symmetric pseudo-observations at two
   * x-locations (spanning the ~400-1300ms linear region the fit covers).
   * Cluster means sit exactly on the persisted line, so the weighted
   * regression through them reproduces `slope`/`deadtime` exactly; the
   * +-residual_sd spread within each cluster reproduces the persisted
   * residual variance. Total weight equals n0, so this carries forward
   * the model's confidence, not just its point estimate.
   */
  double x1 = deadtime_s + 0.2;
  double x2 = deadtime_s + 0.9;
  double y1 = slope * (x1 - deadtime_s);
  double y2 = slope * (x2 - deadtime_s);
  double w = n0 / 4.0;

  m_fit.addObservation(x1, y1 + residual_sd, w);
  m_fit.addObservation(x1, y1 - residual_sd, w);
  m_fit.addObservation(x2, y2 + residual_sd, w);
  m_fit.addObservation(x2, y2 - residual_sd, w);
}

double TopupModel::predictedWeight(double runtime_ms) const {
  double s = m_fit.slope();
  double deadtime_s = deadtimeMs() / 1000.0;
  return std::max(0.0, s * (runtime_ms / 1000.0 - deadtime_s));
}

double TopupModel::deadtimeMs() const {
  double s = m_fit.slope();
  if (std::fabs(s) < kMinDenom) return TopupPriors::kTopupDeadtimeMs;
  return -m_fit.intercept() / s * 1000.0;
}

double TopupModel::residualStdDev() const {
  double sd = m_fit.residualStdDev();
  return sd > 0.0 ? sd : TopupPriors::kTopupResidualSd;
}

uint32_t TopupModel::effectiveN() const {
  return static_cast<uint32_t>(m_fit.effectiveWeight() + 0.5);
}

void TopupModel::applyDecay(int64_t now_epoch_s) {
  if (m_cfg.half_life_days <= 0.0) return;  // Decay can be disabled entirely.
  if (m_last_updated == 0 || now_epoch_s <= m_last_updated) return;

  double elapsed_days = (now_epoch_s - m_last_updated) / kSecondsPerDay;
  double factor = std::exp(-elapsed_days / m_cfg.half_life_days);
  m_fit.decay(factor);
}

bool TopupModel::isPlausible() const {
  if (!m_fit.hasFit()) return false;
  double s = m_fit.slope();
  if (s < m_cfg.min_plausible_slope || s > m_cfg.max_plausible_slope) return false;
  double dt = deadtimeMs();
  if (dt < 0.0 || dt > m_cfg.max_plausible_deadtime_ms) return false;
  double sd = m_fit.residualStdDev();
  if (sd > m_cfg.max_plausible_residual_sd) return false;
  return true;
}

TopupModel::PulseResult TopupModel::recordPulse(double runtime_ms,
                                                 double weight_increment_g,
                                                 int64_t now_epoch_s) {
  PulseResult r;

  /*
   * Hard bounds first: catches sensor-glitch garbage (e.g. the
   * historical 483g / -129g entries) before it can influence anything,
   * regardless of what the current model happens to predict.
   */
  if (weight_increment_g < m_cfg.reject_hard_min_g ||
      weight_increment_g > m_cfg.reject_hard_max_g) {
    r.hard_rejected = true;
    return r;
  }

  r.predicted_g = predictedWeight(runtime_ms);
  r.residual_g = weight_increment_g - r.predicted_g;

  double sd = residualStdDev();
  double reject_threshold = std::max(m_cfg.reject_min_abs_g, m_cfg.reject_k_sigma * sd);
  if (std::fabs(r.residual_g) > reject_threshold) {
    r.stat_rejected = true;
    return r;
  }

  applyDecay(now_epoch_s);

  WeightedLinearFit backup = m_fit;
  m_fit.addObservation(runtime_ms / 1000.0, weight_increment_g, 1.0);  // fit x is seconds

  if (!isPlausible()) {
    // Revert to the last known-good fit rather than let one update push
    // the model somewhere physically implausible.
    m_fit = backup;
    r.fallback = true;
    return r;
  }

  r.accepted = true;
  m_last_updated = now_epoch_s;
  return r;
}

TopupModel::Decision TopupModel::computeTopupDecision(double gap_g,
                                                        double overshoot_budget_g) const {
  Decision d;

  if (gap_g < m_cfg.min_controllable_gap_g) {
    return d;  // Accept the undershoot, don't gamble a pulse
  }

  double s = m_fit.slope();
  if (!(s > 0.0)) return d;  // defensive; fallback logic should keep this positive

  double target_weight = m_cfg.aim_fraction * gap_g;  // Aim ~90% of the gap.

  double sd = residualStdDev();
  double predicted_upper = target_weight + m_cfg.overshoot_k_sigma * sd;
  if (predicted_upper > overshoot_budget_g) {
    // Shrink toward whatever leaves the upper bound inside the remaining
    // overshoot budget.
    double safe_weight = overshoot_budget_g - m_cfg.overshoot_k_sigma * sd;
    target_weight = std::min(target_weight, std::max(0.0, safe_weight));
  }

  if (target_weight <= 0.0) return d;  // no positive duration is safe

  double duration_ms = deadtimeMs() + 1000.0 * target_weight / s;

  // Hygiene clamp, mirrors the old firmware's
  // `top_up_seconds <= 0 || top_up_seconds > 5.0f` check.
  if (duration_ms <= m_cfg.hygiene_min_duration_ms ||
      duration_ms > m_cfg.hygiene_max_duration_ms) {
    return d;
  }

  d.should_fire = true;
  d.duration_ms = static_cast<uint32_t>(duration_ms + 0.5);
  d.predicted_weight_g = target_weight;
  return d;
}

TopupModelV1 TopupModel::dumpPersisted(const TopupModelV1 &base) const {
  TopupModelV1 out = base;
  out.topup_slope = static_cast<float>(m_fit.slope());
  out.topup_deadtime_ms = static_cast<float>(deadtimeMs());
  double var = m_fit.residualVariance();
  out.topup_precision = var > 0.0 ? static_cast<float>(1.0 / var) : base.topup_precision;
  out.topup_n_effective = effectiveN();
  out.last_updated = m_last_updated;
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
  if (m_last_updated != 0 && now_epoch_s > m_last_updated) {
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
