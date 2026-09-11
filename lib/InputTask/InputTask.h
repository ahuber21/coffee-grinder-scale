#pragma once

// Task #3 -- Input. Per rtos-architecture.md §3.3: three symmetric ISRs
// (left/right/back, no exceptions -- this is the concrete AR-001/AR-007 fix)
// push a raw ButtonEdge to a queue and touch nothing else. Input task itself
// owns the one debounce/hold-time state machine applied uniformly to every
// button and every FSM state (AR-008's fix), emitting a ButtonPress only for
// edges that survive it.

void createInputTask();
