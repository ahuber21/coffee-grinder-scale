-- =============================================================================
-- 004_stop_diagnostics.sql
--
-- Adds the per-event decision inputs a live sendLog()/WS "log" line used
-- to be the only place they were ever visible -- AR-074 (see
-- .agent/ARS.md): a 9.5g dose finished at 10.4g and the owner asked for a
-- post-mortem, but nobody was watching the WS log at the exact moment it
-- happened, so that diagnostic was already gone. weight_before_g/
-- weight_after_g alone can explain WHAT happened (a topup pulse added far
-- more than expected); these columns explain WHY, permanently, without
-- needing a live listener:
--   - MAIN_GRIND: stop_reason (which of GRINDING's three stop conditions
--     fired) and weight_estimate_g (MainGrindModel's own plausibility-
--     protected estimate at that instant).
--   - TOPUP: topup_bucket/topup_aim_weight_g/topup_commanded_duration_ms
--     (this specific pulse's inputs at fire time -- topup_commanded_
--     duration_ms especially can't be reconstructed later, since
--     TopupModel keeps retuning each bucket's duration from every pulse
--     that lands in it).
-- All nullable and event-type-specific (MAIN_GRIND-only columns are NULL
-- on TOPUP rows and vice versa) -- no backfill for existing rows.
-- =============================================================================

BEGIN;

ALTER TABLE v2.events
    ADD COLUMN IF NOT EXISTS stop_reason TEXT,
    ADD COLUMN IF NOT EXISTS weight_estimate_g REAL,
    ADD COLUMN IF NOT EXISTS topup_bucket SMALLINT,
    ADD COLUMN IF NOT EXISTS topup_aim_weight_g REAL,
    ADD COLUMN IF NOT EXISTS topup_commanded_duration_ms INTEGER;

ALTER TABLE v2.events
    ADD CONSTRAINT events_stop_reason_check
    CHECK (stop_reason IS NULL OR
           stop_reason = ANY (ARRAY['TIME_ESTIMATE', 'RAW_WEIGHT_FALLBACK', 'SAFETY_TIMEOUT']));

ALTER TABLE v2.events
    ADD CONSTRAINT events_topup_bucket_check
    CHECK (topup_bucket IS NULL OR topup_bucket >= 0);

COMMENT ON COLUMN v2.events.stop_reason IS
  'MAIN_GRIND only: which of GRINDING''s three stop conditions fired. See AR-074.';
COMMENT ON COLUMN v2.events.weight_estimate_g IS
  'MAIN_GRIND only: MainGrindModel::currentWeightEstimate() at the stop instant. See AR-074.';
COMMENT ON COLUMN v2.events.topup_bucket IS
  'TOPUP only: the gap bucket this pulse fired from. See AR-074.';
COMMENT ON COLUMN v2.events.topup_aim_weight_g IS
  'TOPUP only: TopupModel''s aim weight for this pulse''s bucket, at fire time. See AR-074.';
COMMENT ON COLUMN v2.events.topup_commanded_duration_ms IS
  'TOPUP only: the pulse duration TopupModel actually commanded, at fire time -- not reconstructable '
  'later since online learning keeps retuning it. See AR-074.';

COMMIT;

-- No new GRANT needed: 001_sessions_schema.sql's table-level
-- "GRANT SELECT, INSERT ON v2.events TO postgrest_anon" already covers
-- any column added later.
