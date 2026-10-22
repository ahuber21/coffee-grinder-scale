#pragma once

/**
 * Settings/NVS task. Sole owner of the canonical settings struct and
 * the only task that touches NVS. Every other task gets a private,
 * read-only SettingsSnapshot mailbox, never the shared struct.
 *
 * Settings persist through the Arduino Preferences library (NVS
 * namespace "settings", one key per field, as listed in SettingsSchema.h).
 * Load falls back per field to the compiled-in default on a missing or
 * out-of-range key, using the same validators as the live write path.
 * The dosing models and landing learner are stored as raw-byte blobs.
 */

#include <WString.h>

/** Creates and starts the Settings task. */
void createSettingsTask();

/**
 * Copies out the JSON backup of what NVS held when the Settings task booted: every
 * setting plus the raw hex of both model blobs, whatever their version or plausibility.
 * Returns false until the Settings task has finished loading.
 */
bool settingsBootBackupJson(String &out);
