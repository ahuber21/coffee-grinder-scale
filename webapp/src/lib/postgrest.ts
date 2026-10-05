// The browser queries PostgREST directly, not through the device. These types mirror the v2
// schema's tables, limited to the columns the pages read.
import { postgrestBase } from "./config";

export interface Session {
  session_id: string;
  started_at: string;
  completed_at: string | null;
  mode: "single" | "double" | "api_custom";
  requested_weight_g: number;
  target_weight_g: number;
  final_weight_g: number | null;
  /** An independent reference-scale reading entered by hand from the History tab; null until then. */
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

/** Sets the reference weight. The column is PATCH-only: the anonymous role has no INSERT grant on it. */
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

/**
 * Completed sessions with a final weight, for one dose mode. Errors must be taken against
 * requested_weight_g: target_weight_g is the margin-corrected stop target, which topup closes.
 */
export function fetchCompletedDosesForMode(
  mode: "single" | "double",
  limit = 500
): Promise<DoseOutcome[]> {
  return get<DoseOutcome[]>(
    `/sessions?mode=eq.${mode}&outcome=eq.completed&final_weight_g=not.is.null` +
      `&order=started_at.desc&limit=${limit}&select=requested_weight_g,final_weight_g`
  );
}

export interface LandingOutcome {
  settled_weight_g: number;
  target_weight_g: number;
}

/**
 * The settled weight right after the main grind, before any topup, against the stop target
 * (target_weight_g). Only sessions that recorded a settled weight appear.
 */
export function fetchLandingForMode(
  mode: "single" | "double",
  limit = 500
): Promise<LandingOutcome[]> {
  return get<LandingOutcome[]>(
    `/sessions?mode=eq.${mode}&settled_weight_g=not.is.null` +
      `&order=started_at.desc&limit=${limit}&select=settled_weight_g,target_weight_g`
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

/** One row per dose start; the same cup is tared every time, so raw_adc should repeat across sessions. */
export function fetchTareDebugSamples(limit = 2000): Promise<TareDebugSample[]> {
  return get<TareDebugSample[]>(
    `/tare_debug?order=created_at.asc&limit=${limit}&select=raw_adc,grams,created_at`
  );
}
