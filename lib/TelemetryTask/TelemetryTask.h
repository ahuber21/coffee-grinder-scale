#pragma once

/**
 * Telemetry task. Lowest-priority, best-effort consumer of Dosing
 * task's TelemetryEvent stream. Forwards a subset to Network task for
 * the WS broadcast, and posts session/pulse data to the live PostgREST
 * deployment (see .agent/design/postgrest-deployment.md and
 * .agent/design/db-schema/001_sessions_schema.sql). See
 * TelemetryTask.cpp's top-of-file comment for the TelemetryEvent-stream
 * -> PostgREST-endpoint mapping and its known fidelity limitations.
 */

/** Creates and starts the Telemetry task. */
void createTelemetryTask();
