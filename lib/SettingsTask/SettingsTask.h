#pragma once

// Task #5 -- Settings/NVS. Per rtos-architecture.md §4/§5: sole owner of the
// canonical settings struct and the only task that ever touches NVS. Every
// other task gets a private, read-only SettingsSnapshot mailbox -- never the
// shared struct itself (the concrete AR-009 fix).
//
// SettingsSnapshot is persisted for real via the Arduino Preferences
// library (NVS namespace "settings", one key per field -- see
// SettingsTask.cpp). Load falls back per-field to compiled-in defaults on
// first boot / a missing or out-of-range key, validated through the same
// predicates the live write path uses. TopupModelV1 (§5) NVS round-tripping
// is still stubbed -- a separate follow-up, out of scope for the
// SettingsSnapshot persistence pass.

void createSettingsTask();
