-- =============================================================================
-- 002_tare_debug_stats.sql
--
-- Debug-only table logging each session's tare baseline (raw ADC count +
-- converted grams), one row per session. Purpose: the owner always uses
-- the same physical dosing cup, so its tare should read the same raw ADC
-- count session to session -- this table lets that assumption actually be
-- checked against real data instead of just presumed, while investigating
-- a final-weight discrepancy that turned out NOT to be a stability/settling
-- issue (confirmed by lifting the cup, waiting, and placing it back down --
-- same result both on an independent scale and the device itself).
--
-- NOT a calibration input. Nothing on the device ever reads this table
-- back; it exists purely for the owner to query by hand while debugging,
-- and can be dropped once that's done (`DROP TABLE v2.tare_debug;`) without
-- affecting dosing behavior at all.
-- =============================================================================

BEGIN;

CREATE TABLE IF NOT EXISTS v2.tare_debug (
    id                     BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,

    -- ON DELETE CASCADE for the same reason as v2.events/v2.raw_samples --
    -- a session is the natural deletion unit for this device-authored data.
    session_id             UUID NOT NULL
                          REFERENCES v2.sessions (session_id) ON DELETE CASCADE,

    -- Raw ADS1232 count at the moment of tare (before the calibration
    -- factor is applied) -- this is the number to compare across sessions,
    -- since `grams` already has calibration_factor baked in and would mask
    -- a raw-count drift that gets scaled into something that still looks
    -- plausible in grams.
    raw_adc                BIGINT NOT NULL,

    -- The tare baseline in grams (same units as g_grams_on_grind_start) --
    -- kept alongside raw_adc so a raw-count difference can be sanity-
    -- checked against calibration_factor without a second lookup.
    grams                  REAL NOT NULL,

    created_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE v2.tare_debug IS
  'Debug-only: one row per session logging the tare baseline raw ADC count '
  'and converted grams. Not read back by the firmware, not a calibration '
  'input -- purely for checking that the same physical dosing cup tares '
  'consistently across sessions. Safe to drop once no longer needed.';

CREATE INDEX IF NOT EXISTS idx_tare_debug_session_id ON v2.tare_debug (session_id);

COMMIT;

-- Same role/grant pattern as 001_sessions_schema.sql's v2.raw_samples --
-- insert-only from the device's perspective (plus select, for
-- `Prefer: return=representation`), no column ever patched after the fact.
GRANT SELECT, INSERT ON v2.tare_debug TO postgrest_anon;
