#pragma once

// Task #2 -- Dosing/Session Control. Per rtos-architecture.md §2/§3/§5/§6.2:
// the session FSM, sole owner of the grinder relay, and home of the hot
// per-sample lib/DosingModel update path. Never blocks on anything
// downstream (display/network/settings/telemetry) -- every send out of this
// task is either a mailbox overwrite or a zero-timeout queue send.

void createDosingTask();
