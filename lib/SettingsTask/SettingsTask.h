#pragma once

/**
 * Settings/NVS task. Sole owner of the canonical settings struct and
 * the only task that ever touches NVS. Every other task gets a
 * private, read-only SettingsSnapshot mailbox -- never the shared
 * struct itself.
 *
 * SettingsSnapshot persists via the Arduino Preferences library (NVS
 * namespace "settings", one key per field -- see SettingsTask.cpp).
 * Load falls back per-field to compiled-in defaults on first boot or a
 * missing/out-of-range key, validated through the same predicates the
 * live write path uses. TopupModelV1 NVS round-tripping is still
 * stubbed -- a separate, still-open follow-up.
 */

/** Creates and starts the Settings task. */
void createSettingsTask();
