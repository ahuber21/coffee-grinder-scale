#pragma once

/**
 * Dosing/Session Control task: the session FSM, sole owner of the
 * grinder relay, and home of the hot per-sample lib/DosingModel update
 * path. Never blocks on anything downstream (display/network/settings/
 * telemetry) -- every send out of this task is either a mailbox
 * overwrite or a zero-timeout queue send.
 */

/** Creates and starts the Dosing task. */
void createDosingTask();
