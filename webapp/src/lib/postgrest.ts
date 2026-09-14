// The browser queries PostgREST directly, bypassing the device -- these
// mirror the v2 schema's tables (only the columns this page actually
// reads). CORS is open (Access-Control-Allow-Origin: *) so no
// proxy/device involvement is needed.
import { postgrestBase } from "./config";

export interface Session {
  session_id: string;
  started_at: string;
  completed_at: string | null;
  mode: "single" | "double" | "api_custom";
  requested_weight_g: number;
  target_weight_g: number;
  final_weight_g: number | null;
  // Owner-entered independent reference-scale reading, separate from the
  // grinder's own load cell -- see AR-073 in .agent/ARS.md. Null until
  // entered from the History tab.
  reference_weight_g: number | null;
  outcome: "in_progress" | "completed" | "aborted" | "timed_out";
  grind_setting: string | null;
  firmware_version: string;
}

export interface SessionEvent {
  event_id: number;
  session_id: string;
  event_type: "MAIN_GRIND" | "TOPUP";
  pulse_index: number;
  relay_on_at_ms: number;
  relay_off_at_ms: number;
  runtime_ms: number;
  stable_at_ms: number | null;
  weight_before_g: number;
  weight_after_g: number | null;
  weight_increment_g: number | null;
}

export interface RawSample {
  sample_id: number;
  session_id: string;
  event_id: number | null;
  timestamp_ms: number;
  filtered_value: number;
  stable: boolean;
  grinder_state: string;
}

async function get<T>(path: string): Promise<T> {
  const res = await fetch(`${postgrestBase}${path}`);
  if (!res.ok) {
    throw new Error(`PostgREST ${path} -> ${res.status} ${res.statusText}`);
  }
  return res.json() as Promise<T>;
}

export function fetchRecentSessions(limit = 50): Promise<Session[]> {
  return get<Session[]>(
    `/sessions?order=started_at.desc&limit=${limit}&select=session_id,started_at,completed_at,mode,requested_weight_g,target_weight_g,final_weight_g,reference_weight_g,outcome,grind_setting,firmware_version`
  );
}

// PATCH-only column (see 003_reference_weight.sql) -- postgrest_anon has
// no INSERT grant on it, matching every other "entered after the row
// already exists" field on this table.
export async function patchReferenceWeight(
  sessionId: string,
  referenceWeightG: number | null
): Promise<void> {
  const res = await fetch(`${postgrestBase}/sessions?session_id=eq.${sessionId}`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ reference_weight_g: referenceWeightG }),
  });
  if (!res.ok) {
    throw new Error(`PostgREST PATCH reference_weight_g -> ${res.status} ${res.statusText}`);
  }
}

export interface DoseOutcome {
  requested_weight_g: number;
  final_weight_g: number;
}

// Only completed sessions with a recorded final weight -- everything this
// histogram needs, for a given fixed target-dose mode (single/double), and
// a much larger sample than the "recent sessions" table needs to show.
// Deltas here must use requested_weight_g, not target_weight_g -- the
// latter is the margin-corrected internal MAIN_GRIND stop threshold
// (requested minus top_up_margin_single/double), not what was actually
// asked for; TOPUP's whole job is closing that margin back up.
export function fetchCompletedDosesForMode(
  mode: "single" | "double",
  limit = 500
): Promise<DoseOutcome[]> {
  return get<DoseOutcome[]>(
    `/sessions?mode=eq.${mode}&outcome=eq.completed&final_weight_g=not.is.null` +
      `&order=started_at.desc&limit=${limit}&select=requested_weight_g,final_weight_g`
  );
}

export function fetchSessionEvents(sessionId: string): Promise<SessionEvent[]> {
  return get<SessionEvent[]>(
    `/events?session_id=eq.${sessionId}&order=pulse_index.asc`
  );
}

export function fetchSessionRawSamples(sessionId: string): Promise<RawSample[]> {
  return get<RawSample[]>(
    `/raw_samples?session_id=eq.${sessionId}&order=timestamp_ms.asc&select=sample_id,session_id,event_id,timestamp_ms,filtered_value,stable,grinder_state`
  );
}

export interface TareDebugSample {
  raw_adc: number;
  grams: number;
  created_at: string;
}

// One row per dose start (button press or API custom-dose) -- the same
// physical cup is tared every time, so raw_adc should read consistently
// session to session if the tare/ADC path is behaved.
export function fetchTareDebugSamples(limit = 2000): Promise<TareDebugSample[]> {
  return get<TareDebugSample[]>(
    `/tare_debug?order=created_at.asc&limit=${limit}&select=raw_adc,grams,created_at`
  );
}
