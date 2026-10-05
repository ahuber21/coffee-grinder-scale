/** Formats an ISO timestamp in the viewer's locale. */
export function formatTimestamp(iso: string): string {
  return new Date(iso).toLocaleString();
}
