-- =============================================================================
-- 006_model_snapshots.sql
--
-- The model parameters (main-grind rate, coast, and the landing learner's fit
-- per dose mode) after each session, so the Model page can chart how they
-- evolved. One row per session; the firmware posts it once the session's
-- model update is final.
-- =============================================================================

BEGIN;

CREATE TABLE IF NOT EXISTS v2.model_snapshots (
    snapshot_id            BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    session_id             UUID UNIQUE REFERENCES v2.sessions(session_id) ON DELETE CASCADE,
    taken_at               TIMESTAMPTZ NOT NULL DEFAULT now(),
    note                   TEXT,
    rate_hat_g_s           REAL NOT NULL,
    rate_sd_g_s            REAL NOT NULL,
    rate_n_effective       INTEGER NOT NULL,
    coast_g                REAL NOT NULL,
    coast_sd_g             REAL NOT NULL,
    single_sessions        INTEGER NOT NULL,
    single_effective_sessions REAL NOT NULL,
    single_mean_rate_g_s   REAL NOT NULL,
    single_intercept_g     REAL NOT NULL,
    single_slope_g_per_g_s REAL NOT NULL,
    single_residual_sd_g   REAL NOT NULL,
    double_sessions        INTEGER NOT NULL,
    double_effective_sessions REAL NOT NULL,
    double_mean_rate_g_s   REAL NOT NULL,
    double_intercept_g     REAL NOT NULL,
    double_slope_g_per_g_s REAL NOT NULL,
    double_residual_sd_g   REAL NOT NULL
);

COMMENT ON TABLE v2.model_snapshots IS
  'Model parameters after a session. session_id is NULL for rows that were not produced by a session (see note).';
COMMENT ON COLUMN v2.model_snapshots.single_intercept_g IS
  'Learner: predicted landed-high excess (g) at the mean flow rate; the stop is moved earlier by this much.';
COMMENT ON COLUMN v2.model_snapshots.single_slope_g_per_g_s IS
  'Learner: change in that excess per g/s of flow rate.';

CREATE INDEX IF NOT EXISTS idx_model_snapshots_taken_at ON v2.model_snapshots (taken_at);

-- Baseline: the device state decoded from .agent/backups/device-state-2026-10-22-pre-margin0.json,
-- with the rate precision capped the way the firmware caps it on load.
INSERT INTO v2.model_snapshots (taken_at, note, rate_hat_g_s, rate_sd_g_s, rate_n_effective, coast_g, coast_sd_g, single_sessions, single_effective_sessions, single_mean_rate_g_s, single_intercept_g, single_slope_g_per_g_s, single_residual_sd_g, double_sessions, double_effective_sessions, double_mean_rate_g_s, double_intercept_g, double_slope_g_per_g_s, double_residual_sd_g)
SELECT '2026-10-22 11:13:00+00', 'baseline from the device backup taken before margin 0', 0.961510, 0.016432, 30, 0.325867, 0.010527, 34, 16.503508, 0.962527, 0.028966, 0.043083, 0.099774, 53, 18.680586, 0.962285, 0.052150, -0.039161, 0.109564
WHERE NOT EXISTS (SELECT 1 FROM v2.model_snapshots WHERE session_id IS NULL);

COMMIT;

GRANT SELECT, INSERT ON v2.model_snapshots TO postgrest_anon;

-- PostgREST caches the schema; without this the new table is invisible to it.
NOTIFY pgrst, 'reload schema';
