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

/** Creates and starts the Settings task. */
void createSettingsTask();
