-- =============================================================================
-- 003_reference_weight.sql
--
-- Adds v2.sessions.reference_weight_g: the owner's own independent
-- reference-scale reading for a finished dose, entered by hand from the
-- History tab after weighing the cup separately from the grinder's own
-- load cell. AR-073 (see .agent/ARS.md) exists because every prior
-- accuracy check compared the firmware's own final_weight_g against
-- itself -- there was no column for the one number that actually matters,
-- ground truth independent of the device being investigated. Nullable and
-- set well after the session row exists (PATCH, not part of the initial
-- POST), so most historical rows will simply never have one.
-- =============================================================================

BEGIN;

ALTER TABLE v2.sessions
    ADD COLUMN IF NOT EXISTS reference_weight_g REAL;

ALTER TABLE v2.sessions
    ADD CONSTRAINT sessions_reference_weight_g_check
    CHECK (reference_weight_g IS NULL OR reference_weight_g > 0);

COMMENT ON COLUMN v2.sessions.reference_weight_g IS
  'Owner-entered independent reference-scale reading for this session''s '
  'finished dose, separate from the grinder''s own load cell -- NULL until '
  'entered. See AR-073 in .agent/ARS.md.';

COMMIT;

-- Same pattern as 001_sessions_schema.sql's other "finalize" columns --
-- entered after the row already exists, so UPDATE only, no INSERT.
GRANT UPDATE (reference_weight_g) ON v2.sessions TO postgrest_anon;
