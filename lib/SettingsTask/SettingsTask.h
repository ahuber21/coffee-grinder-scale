#pragma once

// Task #5 -- Settings/NVS. Per rtos-architecture.md §4/§5: sole owner of the
// canonical settings struct and the only task that ever touches NVS. Every
// other task gets a private, read-only SettingsSnapshot mailbox -- never the
// shared struct itself (the concrete AR-009 fix).
//
// Per the task brief, NVS read/write is stubbed to compiled-in defaults for
// now (logged, not actually persisted) -- the task/mailbox structure and the
// validated single-writer write path are real.

void createSettingsTask();
