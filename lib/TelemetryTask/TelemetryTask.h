#pragma once

// Task #7 -- Telemetry. Per rtos-architecture.md §3.4/§7: lowest-priority,
// best-effort consumer of Dosing task's TelemetryEvent stream. Forwards a
// subset to Network task's ws_broadcast_q. The eventual PostgREST POST
// (D8/D9) is out of scope for this skeleton (it needs the Network SPA/HTTP
// stack this branch hasn't built yet) -- what's real here is the queue
// drain and forwarding.

void createTelemetryTask();
