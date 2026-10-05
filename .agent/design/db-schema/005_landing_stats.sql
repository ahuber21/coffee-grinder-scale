-- =============================================================================
-- 005_landing_stats.sql
--
-- Per-session main-grind landing measurements, taken from the SETTLED
-- weight after the relay-off coast (v2.events MAIN_GRIND weight_after_g is
-- the pre-coast reading at relay-off and is not a landing weight).
-- =============================================================================

BEGIN;

ALTER TABLE v2.sessions
    ADD COLUMN IF NOT EXISTS settled_weight_g REAL,
    ADD COLUMN IF NOT EXISTS coast_g REAL,
    ADD COLUMN IF NOT EXISTS training_margin_g REAL,
    ADD COLUMN IF NOT EXISTS learner_correction_g REAL,
    ADD COLUMN IF NOT EXISTS learner_clamped BOOLEAN;

COMMENT ON COLUMN v2.sessions.settled_weight_g IS
  'Dose weight once the post-main-grind settle completed, before any topup (grams above the session baseline).';
COMMENT ON COLUMN v2.sessions.coast_g IS
  'settled_weight_g minus the weight at the instant the main-grind relay turned off.';
COMMENT ON COLUMN v2.sessions.training_margin_g IS
  'Offset subtracted from the requested weight to form the main-grind stop target, as configured for this session.';
COMMENT ON COLUMN v2.sessions.learner_correction_g IS
  'Landing correction the landing learner actually applied to this session''s stop point (grams, positive = stopped earlier).';
COMMENT ON COLUMN v2.sessions.learner_clamped IS
  'True if the learner''s correction was cut short by the stop-time clamp.';

COMMIT;

GRANT UPDATE (settled_weight_g, coast_g, training_margin_g, learner_correction_g, learner_clamped)
    ON v2.sessions TO postgrest_anon;
