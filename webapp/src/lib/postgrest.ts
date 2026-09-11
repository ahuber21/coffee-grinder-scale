// D9: the browser queries PostgREST directly, bypassing the device --
// these mirror .agent/design/db-schema/001_sessions_schema.sql's v2
// tables (only the columns this page actually reads). CORS is open
// (Access-Control-Allow-Origin: *, verified live against the deployment
// on 2026-09-11) so no proxy/device involvement is needed.
import { postgrestBase } from "./config";

export interface Session {
  session_id: string;
  started_at: string;
  completed_at: string | null;
  mode: "single" | "double" | "api_custom";
  requested_weight_g: number;
  target_weight_g: number;
  final_weight_g: number | null;
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
    `/sessions?order=started_at.desc&limit=${limit}&select=session_id,started_at,completed_at,mode,requested_weight_g,target_weight_g,final_weight_g,outcome,grind_setting,firmware_version`
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
