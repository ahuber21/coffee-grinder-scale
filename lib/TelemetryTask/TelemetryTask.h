#pragma once

// Task #7 -- Telemetry. Per rtos-architecture.md §3.4/§7: lowest-priority,
// best-effort consumer of Dosing task's TelemetryEvent stream. Forwards a
// subset to Network task's ws_broadcast_q, and posts session/pulse data to
// the live PostgREST deployment (D8/D9, .agent/design/postgrest-deployment.md,
// .agent/design/db-schema/001_sessions_schema.sql) -- the real HTTP body
// behind what used to be a stub. See TelemetryTask.cpp's top-of-file comment
// for the TelemetryEvent-stream -> PostgREST-endpoint mapping and its known
// fidelity limitations.

void createTelemetryTask();
