#pragma once

// Online-fitted dosing model for main-grind rate and topup-pulse response.
// Replaces the static topup_lookup_table[6] with two small recursive
// least-squares (RLS) models, per .agent/design/topup-model.md §4.
//
// Host-buildable on purpose: no Arduino/ESP32 headers, no NVS I/O. NVS
// read/write of TopupModelV1 is a future Settings-task concern; this module
// only produces/consumes the struct as plain bytes.

#include <cmath>
#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// WeightedLinearFit — weighted least squares for y = intercept + slope*x,
// updated one observation at a time via sufficient statistics (Sw, Swx, Swy,
// Swxx, Swxy, Swyy). This is the "RLS-equivalent" the design doc allows
// (§4.2: "literally Sx, Sy, Sxy, Sx2 ... or their precision-weighted RLS
// equivalents"). decay() implements the forgetting factor: scaling every
// accumulator by the same factor before folding in a new point is
// mathematically equivalent to down-weighting all past observations by that
// factor, which is the standard trick for calendar-time forgetting in RLS.
// ---------------------------------------------------------------------------
class WeightedLinearFit {
public:
  void reset();

  void addObservation(double x, double y, double weight = 1.0);

  // Scale all accumulated statistics by factor (0 < factor <= 1) to forget
  // old data by elapsed wall-clock time.
  void decay(double factor);

  // False until there's enough spread in x to solve for slope/intercept
  // (guards the Sw*Swxx - Swx^2 denominator).
  bool hasFit() const;

  double slope() const;
  double intercept() const;

  // Weighted residual variance / stddev of individual observations around
  // the fitted line. Falls back to 0 when underdetermined.
  double residualVariance() const;
  double residualStdDev() const;

  // Variance of the slope estimator itself (residualVariance / Sxx_weighted).
  double slopeVariance() const;

  // Sum of weights == the model's "effective n" (decays with old data).
  double effectiveWeight() const { return m_sw; }

private:
  double m_sw = 0.0;
  double m_swx = 0.0;
  double m_swy = 0.0;
  double m_swxx = 0.0;
  double m_swxy = 0.0;
  double m_swyy = 0.0;

  double denom() const { return m_sw * m_swxx - m_swx * m_swx; }
};

// ---------------------------------------------------------------------------
// TopupModelV1 — persistable model state (§4.3, extended by §8 for Model C).
// Deliberately POD: no pointers, no non-trivial members, so it round-trips
// through a raw byte copy to/from NVS. `last_updated` is a fixed-width
// int64 (seconds since epoch) rather than `time_t` as the design doc
// sketches it, because `time_t` width is platform-defined (32-bit on some
// Arduino cores) and this struct's whole point is to be a stable on-flash
// byte layout.
//
// Version history:
//   v1: rate_hat/rate_precision (Model A) + topup_slope/topup_deadtime_ms/
//       topup_precision (Model B).
//   v2: + coast_weight_hat/coast_weight_precision (Model C, §8). Nothing has
//       shipped to a device yet (no NVS read/write path exists), so there is
//       no real migration to perform here -- the fields are simply added.
//       The version is still bumped rather than left at 1, on the same
//       discipline the project's own postmortem on skipped version bumps
//       (AR-005/AR-010: EEPROM structs that grew fields without a version
//       bump, leaving no reliable way to detect stale on-flash layouts)
//       argues for: get in the habit now, while it's free, rather than
//       relearning the lesson once something has actually shipped.
// ---------------------------------------------------------------------------
constexpr uint8_t kTopupModelVersion = 2;

struct TopupModelV1 {
  uint8_t version;
  float rate_hat;            // main-grind plateau rate, g/s
  float rate_precision;      // inverse-variance of rate_hat itself
  float topup_slope;         // g/s
  float topup_deadtime_ms;
  float topup_precision;     // inverse-variance of a single topup observation's residual
  uint32_t rate_n_effective;   // decayed effective sample count
  uint32_t topup_n_effective;
  int64_t last_updated;      // seconds since epoch; 0 == never updated
  float coast_weight_hat;       // main-grind post-relay-off "coast", grams (§8, Model C)
  float coast_weight_precision; // inverse-variance of coast_weight_hat itself
};

static_assert(std::is_trivially_copyable<TopupModelV1>::value,
              "TopupModelV1 must be trivially copyable to be written to NVS as raw bytes");
static_assert(std::is_standard_layout<TopupModelV1>::value,
              "TopupModelV1 must have standard layout for a stable on-flash byte representation");

// Cold-start priors, §4.4 (Models A/B) and §8 (Model C). Numbers are the
// fleet-wide fit from topup-model.md / coast-effect.md, not guesses:
// rate_hat/topup_slope/topup_deadtime_ms match the historical OLS fit,
// rate_precision derives from the measured p10-p90 session-to-session
// spread (~0.09 g/s sd), topup_precision from the measured ~0.14-0.21g
// per-pulse core noise (using ~0.15g as a representative sd). N0 ~= 18
// pseudo-observations per §4.4's "N0 ~= 15-20" guidance.
//
// kCoastWeightHat is the flat fleet median coast weight for the main grind,
// coast-effect.md §2 ("main-grind coast... median 0.49g"). kCoastWeightSd is
// derived the same way kRateSd was (from a measured spread, not a guess):
// coast-effect.md §2 gives IQR 0.38-0.60g, i.e. IQR = 0.22g; for a
// (roughly, per the reported percentiles) normal distribution,
// sd ~= IQR / 1.349 ~= 0.163g, rounded to 0.16g at the same 2-significant
// -figure precision as kRateSd/kTopupResidualSd.
namespace TopupPriors {
constexpr float kRateHat = 1.00f;
constexpr float kRateSd = 0.09f;
constexpr float kTopupSlope = 1.05f;
constexpr float kTopupDeadtimeMs = 310.0f;
constexpr float kTopupResidualSd = 0.15f;
constexpr uint32_t kRateN0 = 18;
constexpr uint32_t kTopupN0 = 18;
constexpr float kCoastWeightHat = 0.49f;
constexpr float kCoastWeightSd = 0.16f;
}  // namespace TopupPriors

TopupModelV1 makeDefaultTopupModel();

// ---------------------------------------------------------------------------
// MainGrindModel (Model A) — main-grind plateau rate.
//
// Persisted state is a single scalar (rate_hat, rate_precision): unlike the
// topup model, only the *slope* generalizes across sessions (the intercept
// of a session's weight-vs-time line depends on that session's tare/start
// offset and isn't meaningful cross-session). Each grind fits its own
// in-session line via WeightedLinearFit and, at the end, folds the
// resulting slope estimate into the persisted scalar via precision-weighted
// (Bayesian normal-normal) combination.
// ---------------------------------------------------------------------------
class MainGrindModel {
public:
  struct Config {
    // Ignore samples before this many ms into the grind: the chute isn't
    // primed yet (§2.1, ~0.8-1.0s dead time), so early samples aren't on
    // the plateau line and would bias the slope.
    double deadtime_ms = 900.0;
    // Recency half-life for the persisted cross-session rate, §4.3 (30-60d
    // range -- this is where the measured burr-wear drift lives, §2.3).
    double half_life_days = 45.0;
    // Minimum in-session points before the session fit is trusted at all.
    int min_session_points = 3;
    // Fallback bounds (§4.4 / matches firmware's existing rate_min_valid /
    // rate_max_valid): a folded-in rate outside this range is discarded.
    double min_plausible_rate = 0.3;
    double max_plausible_rate = 2.5;
  };

  // Two overloads (rather than a `Config cfg = Config()` default argument)
  // to sidestep a default-member-initializer-visibility quirk some
  // compilers hit when a nested class's defaulted members are referenced
  // from the enclosing class's own default argument.
  explicit MainGrindModel(const TopupModelV1 &persisted);
  MainGrindModel(const TopupModelV1 &persisted, Config cfg);

  // Begin a new grind: clears in-session accumulation only, keeps the
  // persisted cross-session prior.
  void startSession();

  // Feed one (runtime_ms, weight_g) sample from the current grind.
  void addSample(double runtime_ms, double weight_g);

  // Precision-weighted blend of the persisted prior and the in-session fit
  // so far -- dominated by the prior early in a grind, shifting toward this
  // grind's own data as it accumulates (§4.2).
  double currentRateEstimate() const;

  // Predicted absolute stop time (ms since grind start) for target_weight_g,
  // using the last sample fed via addSample() and the current blended rate.
  double predictStopTimeMs(double target_weight_g) const;

  // Same as predictStopTimeMs, but aims for an *effective* target that
  // anticipates post-relay-off "coast" (§8 / Model C, see CoastModel below):
  // stop when the grind reaches target_weight_g - coast_weight_g, not
  // target_weight_g itself, since coast_weight_g more will still land after
  // the relay turns off. Kept as a thin wrapper (rather than a second
  // parameter with a default) so callers who want coast anticipation must
  // pass an explicit estimate -- typically CoastModel::currentCoastEstimate()
  // -- instead of silently getting 0 from a forgotten argument.
  double predictStopTimeMsWithCoast(double target_weight_g, double coast_weight_g) const;

  // Fold this session's fit into the persisted state (recency-decayed by
  // elapsed wall-clock time first) and reset for the next grind. Returns
  // false (leaving persisted state untouched) if the resulting rate would
  // be physically implausible.
  bool finalizeSession(int64_t now_epoch_s);

  double persistedRateHat() const { return m_rate_hat; }
  double persistedRatePrecision() const { return m_rate_precision; }
  uint32_t persistedRateNEffective() const { return m_rate_n_effective; }

  TopupModelV1 dumpPersisted(const TopupModelV1 &base) const;

private:
  Config m_cfg;

  double m_rate_hat;
  double m_rate_precision;
  uint32_t m_rate_n_effective;
  int64_t m_last_updated;

  WeightedLinearFit m_session_fit;
  bool m_have_last_sample = false;
  double m_last_runtime_ms = 0.0;
  double m_last_weight_g = 0.0;

  double sessionPrecision() const;
};

// ---------------------------------------------------------------------------
// TopupModel (Model B) — topup pulse response
// weight_added(t) = max(0, topup_slope * (t - topup_deadtime_ms)).
//
// Unlike Model A, both parameters generalize cross-session, so the fitted
// line itself is the persisted state. On construction the persisted
// (slope, deadtime, precision, n_effective) is expanded back into an
// equivalent WeightedLinearFit via four symmetric pseudo-observations (two
// x-locations, +-1 residual-sd each) -- this reproduces the fitted line and
// residual variance exactly while carrying the right total weight, and is
// how both the true cold-start prior (§4.4) and a warm-started persisted
// fit are seeded through the exact same code path.
// ---------------------------------------------------------------------------
class TopupModel {
public:
  struct Config {
    // Recency half-life for the topup fit. <= 0 disables decay: §4.3 found
    // no measurable drift in the topup slope over 17 months, so v1 can omit
    // it (unlike Model A, where the half-life is load-bearing).
    double half_life_days = 0.0;

    // Hard bounds a raw observation must satisfy before it's even
    // considered, §1.3 (catches sensor-glitch garbage like the historical
    // 483g / -129g entries outright).
    double reject_hard_min_g = -1.0;
    double reject_hard_max_g = 10.0;
    // Statistical consistency check against the *current* model, §1.3:
    // reject if |actual - predicted| > max(reject_min_abs_g, k * residual_sd).
    double reject_k_sigma = 3.0;
    double reject_min_abs_g = 0.5;

    // Plausibility bounds for the fallback check, §4.4.
    double min_plausible_slope = 0.05;
    double max_plausible_slope = 5.0;
    double max_plausible_deadtime_ms = 1000.0;
    double max_plausible_residual_sd = 2.0;

    // Topup decision logic, §4.5.
    double min_controllable_gap_g = 0.18;
    double aim_fraction = 0.9;
    double overshoot_k_sigma = 2.0;
    double hygiene_min_duration_ms = 0.0;
    double hygiene_max_duration_ms = 5000.0;  // mirrors legacy `top_up_seconds > 5.0f`
  };

  struct PulseResult {
    bool accepted = false;      // folded into the fit
    bool hard_rejected = false; // failed the absolute bounds check
    bool stat_rejected = false; // failed the |actual-predicted| consistency check
    bool fallback = false;      // folded in, but produced an implausible fit and was reverted
    double predicted_g = 0.0;
    double residual_g = 0.0;
  };

  struct Decision {
    bool should_fire = false;
    uint32_t duration_ms = 0;
    double predicted_weight_g = 0.0;  // expected topup output if fired
  };

  explicit TopupModel(const TopupModelV1 &persisted);
  TopupModel(const TopupModelV1 &persisted, Config cfg);

  PulseResult recordPulse(double runtime_ms, double weight_increment_g, int64_t now_epoch_s);

  Decision computeTopupDecision(double gap_g, double overshoot_budget_g) const;

  double slope() const { return m_fit.slope(); }
  double deadtimeMs() const;
  double residualStdDev() const;
  // Derived from the fit's decayed weight sum rather than tracked
  // separately, so it can never drift out of sync with the fit itself.
  uint32_t effectiveN() const;
  int64_t lastUpdated() const { return m_last_updated; }

  TopupModelV1 dumpPersisted(const TopupModelV1 &base) const;

private:
  Config m_cfg;
  WeightedLinearFit m_fit;
  int64_t m_last_updated;

  double predictedWeight(double runtime_ms) const;
  void seedFromPersisted(const TopupModelV1 &persisted);
  void applyDecay(int64_t now_epoch_s);
  bool isPlausible() const;
};

// ---------------------------------------------------------------------------
// CoastModel (Model C) — main-grind post-relay-off "coast": coffee that
// keeps landing on the scale for ~1.4s / ~0.49g (fleet median) after
// grinderOff(), per coast-effect.md and topup-model.md §8. Confirmed
// negligible for topup pulses (coast-effect.md: median -0.02g), so this
// exists only for the main grind -- Model B is left untouched.
//
// Kept as its own class, mirroring the existing A/B split, rather than
// folded into MainGrindModel: it has no in-session regression to blend
// against (coast is a single number known only once per session, after the
// grind has already stopped and the reading has settled), so its update
// rule is a plain Bayesian scalar blend -- precision-weighted combination
// of the persisted (coast_weight_hat, coast_weight_precision) with one new
// observation of assumed variance TopupPriors::kCoastWeightSd^2 -- rather
// than MainGrindModel's "blend against this session's own WeightedLinearFit"
// pattern. Folding it into MainGrindModel would mean bolting an unrelated
// update rule onto a class whose whole shape is built around the in-session
// fit; a separate class keeps each model's persistence/decay/fallback logic
// independently readable and testable, same as TopupModel today.
//
// Deliberately the flat-median version only (topup-model.md §8: "starting
// with just the flat median ... already captures most of the benefit; the
// rate-scaled version is a natural v2") -- no flow-rate covariate, despite
// the measured r=0.50 correlation with flow-rate-at-cutoff.
// ---------------------------------------------------------------------------
class CoastModel {
public:
  struct Config {
    // Recency half-life for the persisted coast estimate. Reuses Model A's
    // half-life rather than inventing a new number: coast-effect.md doesn't
    // separately measure coast drift over time, but coast is tied to the
    // same main-grind/burr-wear physics Model A's rate is (§2.3's drift
    // finding), so Model A's measured-and-justified half-life is the most
    // defensible existing number to reuse here.
    double half_life_days = 45.0;
    // Plausibility bounds on a raw observation, checked before it's folded
    // in at all (mirrors TopupModel's hard-bounds check, §1.3/§4.4): coast
    // physically cannot be negative (chute inventory doesn't un-fall), and
    // coast-effect.md's own outlier filter used |coast_weight_g| < 5 to drop
    // sensor-glitch garbage -- 3g here is a tighter, still-generous margin
    // above the measured p90 of 0.713g.
    double min_plausible_coast_g = 0.0;
    double max_plausible_coast_g = 3.0;
    // Assumed per-session population sd of true coast weight, used as the
    // fixed observation variance in the Bayesian scalar blend. Defaults to
    // the measured spread (TopupPriors::kCoastWeightSd), not a guess.
    double observation_sd = TopupPriors::kCoastWeightSd;
  };

  struct RecordResult {
    bool accepted = false;  // folded into the persisted estimate
    bool rejected = false;  // failed the plausibility bounds check, discarded
  };

  explicit CoastModel(const TopupModelV1 &persisted);
  CoastModel(const TopupModelV1 &persisted, Config cfg);

  // Fold one session's observed coast weight (settled weight minus weight
  // at the instant of grinderOff()) into the persisted estimate, decaying
  // old evidence by elapsed wall-clock time first (same mechanism as
  // MainGrindModel::finalizeSession). Implausible observations are rejected
  // outright and never touch the persisted state.
  RecordResult recordCoast(double observed_coast_g, int64_t now_epoch_s);

  double currentCoastEstimate() const { return m_coast_hat; }
  double persistedCoastPrecision() const { return m_coast_precision; }
  int64_t lastUpdated() const { return m_last_updated; }

  TopupModelV1 dumpPersisted(const TopupModelV1 &base) const;

private:
  Config m_cfg;
  double m_coast_hat;
  double m_coast_precision;
  int64_t m_last_updated;
};
