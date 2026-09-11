#pragma once

/**
 * Input task. Three symmetric ISRs (left/right/back, no exceptions)
 * push a raw ButtonEdge to a queue and touch nothing else. Input task
 * itself owns the one debounce/hold-time state machine applied
 * uniformly to every button and every FSM state, emitting a
 * ButtonPress only for edges that survive it.
 */

/** Creates and starts the Input task, and attaches its button ISRs. */
void createInputTask();
