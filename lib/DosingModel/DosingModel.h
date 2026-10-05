#pragma once

/**
 * Online dosing models: main-grind rate (MainGrindModel), topup pulse
 * durations (TopupModel), post-relay-off coast (CoastModel) and the
 * landing learner (LandingLearner).
 *
 * Host-buildable on purpose: no Arduino/ESP32 headers, no NVS I/O. The
 * Settings task persists DosingModelState and LandingLearnerState as
 * plain bytes.
 */

#include <cmath>
#include <cstdint>
#include <type_traits>

/**
 * Weighted least squares for y = intercept + slope*x, updated one
 * observation at a time via sufficient statistics (Sw, Swx, Swy, Swxx,
 * Swxy, Swyy). decay() scales every accumulator by the same factor,
 * which is equivalent to down-weighting all past observations by it.
 */
class WeightedLinearFit {
 public:
  /** Clears all accumulated statistics. */
  void reset();

  /** Folds in one (x, y) observation at the given weight. */
  void addObservation(double x, double y, double weight = 1.0);

  /** Scales all accumulated statistics by `factor` (0 < factor <= 1) to forget old data. */
  void decay(double factor);

  /** False until there is enough spread in x to solve for slope/intercept. */
  bool hasFit() const;

  double slope() const;
  double intercept() const;

  /** Weighted residual variance of individual observations around the line; 0 when underdetermined. */
  double residualVariance() const;

  /** Variance of the slope estimator itself (residualVariance / Sxx_weighted). */
  double slopeVariance() const;

  /** Sum of weights, i.e. the effective observation count after decay. */
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

/// Number of gap buckets the topup lookup table covers.
constexpr int kTopupLutBuckets = 10;
/// Width of each bucket; together they cover gaps of 0 to 1g.
constexpr double kTopupLutBucketWidthG = 0.1;

/**
 * Layout version of DosingModelState. Bump it whenever the struct's shape
 * changes; a blob with a different version is discarded on load.
 */
constexpr uint8_t kDosingModelVersion = 3;

/**
 * Persisted state of MainGrindModel, TopupModel and CoastModel. POD with
 * no pointers so it round-trips through a raw byte copy to NVS;
 * `last_updated` is a fixed-width int64 because `time_t` width is
 * platform-defined and this struct needs a stable byte layout.
 */
struct DosingModelState {
  uint8_t version;
  float rate_hat;        ///< Main-grind plateau rate, g/s.
  float rate_precision;  ///< Inverse-variance of rate_hat itself.
  /// Per-bucket pulse duration (ms): bucket i covers gaps in (i*0.1g, (i+1)*0.1g].
  float topup_lut_duration_ms[kTopupLutBuckets];
  uint32_t topup_lut_n[kTopupLutBuckets];  ///< Observation count per bucket.
  uint32_t rate_n_effective;               ///< Decayed effective sample count.
  int64_t last_updated;                    ///< Seconds since epoch; 0 == never updated.
  float coast_weight_hat;                  ///< Main-grind post-relay-off "coast", grams.
  float coast_weight_precision;            ///< Inverse-variance of coast_weight_hat itself.
};

static_assert(std::is_trivially_copyable<DosingModelState>::value,
              "DosingModelState must be trivially copyable to be written to NVS as raw bytes");
static_assert(std::is_standard_layout<DosingModelState>::value,
              "DosingModelState must have standard layout for a stable on-flash byte representation");

/**
 * Cold-start priors, taken from measured grind/topup/coast data rather than guessed.
 * The rate precision follows from the measured session-to-session spread
 * (~0.09 g/s sd); kCoastWeightHat is the measured median coast, and
 * kCoastWeightSd comes from its IQR (0.22g / 1.349). kTopupSlope and
 * kTopupDeadtimeMs seed the topup table and convert an observed weight
 * error into a duration adjustment. kRateN0 pseudo-observations reflect
 * how much data went into the rate fit.
 */
namespace TopupPriors {
constexpr float kRateHat = 1.00f;
constexpr float kRateSd = 0.09f;
constexpr float kTopupSlope = 1.05f;
constexpr float kTopupDeadtimeMs = 310.0f;
constexpr uint32_t kRateN0 = 18;
constexpr float kCoastWeightHat = 0.49f;
constexpr float kCoastWeightSd = 0.16f;
}  // namespace TopupPriors

/** A fresh DosingModelState seeded from TopupPriors -- the cold-start state. */
DosingModelState makeDefaultDosingModelState();

/**
 * Main-grind plateau rate.
 *
 * Only the slope generalizes across sessions (a session's intercept
 * depends on its tare offset), so the persisted state is one scalar
 * (rate_hat, rate_precision). Each grind fits its own in-session line and
 * folds the slope into that scalar by precision-weighted (Bayesian
 * normal-normal) combination.
 */
class MainGrindModel {
 public:
  /** Tunable bounds/windows for the fit. */
  struct Config {
    /// Samples before this many ms into the grind are ignored: the chute isn't primed yet.
    double deadtime_ms = 900.0;
    /// <= 0 disables recency decay; the grinder's behavior doesn't drift with age.
    double half_life_days = 0.0;
    /// Minimum in-session points before the session fit is trusted at all.
    int min_session_points = 3;
    /// A folded-in rate outside this range is discarded.
    double min_plausible_rate = 0.3;
    double max_plausible_rate = 2.5;
    /// A drop bigger than this since the last accepted sample is a glitch (e.g. the cup lifted off).
    double max_plausible_drop_g = 1.0;
    /*
     * A rise bigger than this since the last accepted sample is a clump
     * impact reading heavier than its settled mass. It matters beyond the
     * fit: the last accepted weight is what the stop check compares to the
     * target, so an unrejected spike could stop the grind early.
     */
    double max_plausible_rise_g = 1.0;
    /// A reading implausible for this long is a real step change (cup swap), not a transient, and is accepted.
    double max_reject_duration_ms = 500.0;
  };

  explicit MainGrindModel(const DosingModelState &persisted);
  MainGrindModel(const DosingModelState &persisted, Config cfg);

  /** Begins a new grind: clears in-session state, keeps the persisted prior. */
  void startSession();

  /** Feeds one (runtime_ms, weight_g) sample from the current grind. */
  void addSample(double runtime_ms, double weight_g);

  /** Precision-weighted blend of the persisted prior and the in-session fit so far. */
  double currentRateEstimate() const;

  /**
   * Predicted absolute stop time (ms since grind start) for
   * `target_weight_g`, from the last accepted sample and the current rate.
   * Infinity when the rate is not positive.
   */
  double predictStopTimeMs(double target_weight_g) const;

  /**
   * Like predictStopTimeMs, but stops `coast_weight_g` early because that
   * much more lands after the relay turns off. The estimate is a required
   * argument so a forgotten one can't silently mean zero.
   */
  double predictStopTimeMsWithCoast(double target_weight_g, double coast_weight_g) const;

  /**
   * Folds this session's fit into the persisted state (recency-decayed
   * first) and resets for the next grind. Returns false, leaving persisted
   * state untouched, if the resulting rate would be implausible.
   */
  bool finalizeSession(int64_t now_epoch_s);

  /**
   * The most recent sample addSample() accepted after its drop/rise
   * rejection: safe to compare against a target while the grind runs,
   * unlike a raw ScaleSample. 0 before the first accepted sample.
   */
  double currentWeightEstimate() const { return m_last_weight_g; }

  double persistedRateHat() const { return m_rate_hat; }

  /** Returns `base` with the persisted rate fields overwritten by this model's state. */
  DosingModelState dumpPersisted(const DosingModelState &base) const;

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
  bool m_rejecting = false;
  double m_reject_started_ms = 0.0;

  double sessionPrecision() const;
};

/**
 * Topup pulse response as a self-tuning lookup table: one pulse duration
 * per remaining-gap bucket, nudged toward whatever duration has actually
 * produced a safe amount of weight for gaps that size.
 *
 * A table rather than a fitted duration-to-weight line, because short
 * pulses land in a discrete, clumpy regime (a pulse either dislodges a
 * small clump or does nothing) with noise (~0.23g) too large for one
 * formula to stay both safe and effective across the range.
 */
class TopupModel {
 public:
  /** Tunable bounds for pulse acceptance and the topup decision. */
  struct Config {
    /// An observation outside these bounds is sensor garbage and is rejected outright.
    double reject_hard_min_g = -1.0;
    double reject_hard_max_g = 10.0;

    /// A pulse aims for this fraction of its bucket's upper edge, leaving slack for a larger clump.
    double aim_fraction = 0.85;

    /// Learning rate per update; overshoot is corrected faster because it is the worse failure.
    double learn_rate_overshoot = 0.6;
    double learn_rate_undershoot = 0.3;

    /*
     * The relay is a mechanical switch driving a motor with static
     * friction: below ~300ms it stalls and produces nothing. A bucket's
     * tuned duration is always clamped to this floor.
     */
    double hygiene_min_duration_ms = 350.0;
    double hygiene_max_duration_ms = 5000.0;  ///< Beyond this, a duration isn't a real dose.
  };

  /** Outcome of recordPulse(). */
  struct PulseResult {
    bool accepted = false;       ///< Folded into the bucket's tuned duration.
    bool hard_rejected = false;  ///< Failed the absolute bounds check.
    int bucket = -1;             ///< Which bucket this observation updated.
  };

  /** Outcome of computeTopupDecision(). */
  struct Decision {
    bool should_fire = false;
    uint32_t duration_ms = 0;
    int bucket = -1;
    double aim_weight_g = 0.0;  ///< This bucket's aim point, for logging/telemetry.
  };

  explicit TopupModel(const DosingModelState &persisted);
  TopupModel(const DosingModelState &persisted, Config cfg);

  /** Maps a remaining gap to its LUT bucket index, clamped to the table's range. */
  static int bucketForGap(double gap_g);

  /** Validates, then folds in, one observed topup pulse for the bucket `gap_at_fire_g` mapped to. */
  PulseResult recordPulse(double gap_at_fire_g, double weight_increment_g);

  /** Decides whether/how long to fire the next topup pulse for the given gap. */
  Decision computeTopupDecision(double gap_g) const;

  double durationForBucket(int bucket) const { return m_duration_ms[bucket]; }

  /** Returns `base` with the persisted topup fields overwritten by this model's state. */
  DosingModelState dumpPersisted(const DosingModelState &base) const;

 private:
  Config m_cfg;
  double m_duration_ms[kTopupLutBuckets];
  uint32_t m_n[kTopupLutBuckets];
};

/**
 * Main-grind post-relay-off "coast": coffee that keeps landing on the
 * scale for ~1.4s / ~0.49g (median) after the relay turns off. It is
 * negligible for topup pulses (median -0.02g), so only the main grind uses it.
 *
 * Coast is a single number known once per session, after the reading
 * settles, so there is no in-session regression to blend against. The
 * update is a plain Bayesian scalar blend of the persisted estimate with
 * one observation of variance TopupPriors::kCoastWeightSd^2. It is a flat
 * estimate with no flow-rate covariate; LandingLearner models the
 * residual's dependence on rate.
 */
class CoastModel {
 public:
  /** Tunable bounds for coast-observation acceptance. */
  struct Config {
    /// <= 0 disables recency decay, as for MainGrindModel: coast comes from the same mechanics.
    double half_life_days = 0.0;
    /// Coast cannot be negative, and 3g is a generous margin above the measured p90 of 0.713g.
    double min_plausible_coast_g = 0.0;
    double max_plausible_coast_g = 3.0;
    /// Fixed per-observation sd in the Bayesian blend; defaults to the measured spread.
    double observation_sd = TopupPriors::kCoastWeightSd;
  };

  /** Outcome of recordCoast(). */
  struct RecordResult {
    bool accepted = false;  ///< Folded into the persisted estimate.
    bool rejected = false;  ///< Failed the plausibility bounds check, discarded.
  };

  explicit CoastModel(const DosingModelState &persisted);
  CoastModel(const DosingModelState &persisted, Config cfg);

  /**
   * Folds one session's observed coast (settled weight minus the weight at
   * relay-off) into the persisted estimate, decaying old evidence first.
   * Implausible observations are rejected and leave the state untouched.
   */
  RecordResult recordCoast(double observed_coast_g, int64_t now_epoch_s);

  double currentCoastEstimate() const { return m_coast_hat; }

  /** Returns `base` with the persisted coast fields overwritten by this model's state. */
  DosingModelState dumpPersisted(const DosingModelState &base) const;

 private:
  Config m_cfg;
  double m_coast_hat;
  double m_coast_precision;
  int64_t m_last_updated;
};

/** Version tag of LandingLearnerState; a mismatch on load discards the blob. */
constexpr uint16_t kLandingLearnerVersion = 1;

/** Persisted state of LandingLearner: forgetting-weighted regression sums per dose mode. */
struct LandingLearnerState {
  /// Weighted sums of one dose mode's (rate, uncorrected landing error) observations.
  struct Mode {
    double sw;   ///< Sum of weights.
    double sx;   ///< Sum of w*x, x = flow rate at the stop (g/s).
    double sy;   ///< Sum of w*y, y = uncorrected landing error (g, positive = landed high).
    double sxx;
    double sxy;
    double syy;
    uint32_t n;  ///< Lifetime accepted sessions.
  };
  uint16_t version;
  Mode mode[2];  ///< Index 0 = single, 1 = double.
};

static_assert(std::is_trivially_copyable<LandingLearnerState>::value,
              "LandingLearnerState is persisted as raw bytes");

/** An empty, current-version LandingLearnerState. */
LandingLearnerState makeDefaultLandingLearnerState();

/**
 * Predicts how far above its aim the main grind will settle, from the
 * flow rate at the stop, so the stop can be moved earlier (or later) by
 * that amount. Per dose mode it fits y = b + c*(x - mean_x) over the
 * settled landing errors with exponential forgetting; the intercept is
 * shrunk toward zero and the slope is ridge-regularised, so the first
 * few sessions barely move the stop. y is always the error the session
 * would have had with no correction applied, so the learner never
 * trains on its own action.
 */
class LandingLearner {
 public:
  struct Config {
    double rate = 0.05;         ///< Forgetting per session; the evidence window is ~1/rate sessions.
    double prior_weight = 3.0;  ///< Pseudo-sessions of zero error damping the cold-start intercept; fades as sessions accrue.
    double ridge = 0.02;        ///< Added to the centred Sxx; shrinks the slope toward zero.
    double max_abs_error_g = 1.0;  ///< Observations beyond this are glitches, not data.
  };

  explicit LandingLearner(const LandingLearnerState &persisted);
  LandingLearner(const LandingLearnerState &persisted, Config cfg);

  void setRate(double rate);

  /** Expected landed-high excess in grams for a grind at `rate_g_s`; 0 with no data. */
  double predict(bool is_double, double rate_g_s) const;

  /** Folds one session; returns false (state untouched) for an implausible observation. */
  bool record(bool is_double, double rate_g_s, double uncorrected_error_g);

  /** Sd of the uncorrected landing error around the fit, in grams. */
  double residualSd(bool is_double) const;
  /** Effective (forgetting-weighted) session count. */
  double effectiveSessions(bool is_double) const;
  uint32_t sessions(bool is_double) const;

  LandingLearnerState dumpPersisted() const { return m_state; }

 private:
  Config m_cfg;
  LandingLearnerState m_state;
};
