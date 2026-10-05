#pragma once

/**
 * Telemetry task. Lowest-priority, best-effort consumer of Dosing task's
 * TelemetryEvent stream: it forwards each event to Network task for the
 * WS broadcast and posts session data to PostgREST (schema in
 * .agent/design/db-schema/). TelemetryTask.cpp maps events to requests.
 */

/** Creates and starts the Telemetry task. */
void createTelemetryTask();
