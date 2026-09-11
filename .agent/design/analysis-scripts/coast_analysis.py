import csv, statistics, datetime
from collections import defaultdict

SCRATCH = "/private/tmp/claude-501/-Users-ahuber-Nextcloud-basteln-2026-09-11-coffee-grinder-scale/73fbb13f-0caa-4a2e-9bb7-22943def5e0d/scratchpad"

# ---------- load topup, reconstruct clusters (sessions) with rn, same heuristic as topup-model.md ----------
topup_rows = []
with open(f"{SCRATCH}/topup.csv") as f:
    r = csv.DictReader(f)
    for row in r:
        ts = datetime.datetime.fromisoformat(row["timestamp"])
        topup_rows.append({
            "id": int(row["id"]),
            "ts": ts,
            "runtimemillis": float(row["runtimemillis"]),
            "weightincrement": float(row["weightincrement"]),
        })
topup_rows.sort(key=lambda r: r["ts"])

clusters = []  # each: list of rows, in order, with rn
cur = []
prev_ts = None
for row in topup_rows:
    if prev_ts is not None and (row["ts"] - prev_ts).total_seconds() > 20:
        clusters.append(cur)
        cur = []
    cur.append(row)
    prev_ts = row["ts"]
if cur:
    clusters.append(cur)

for c in clusters:
    for i, row in enumerate(c):
        row["rn"] = i + 1

print(f"topup rows: {len(topup_rows)}, clusters(sessions): {len(clusters)}")

# index clusters by approximate start time for matching
clusters_by_start = [(c[0]["ts"], c[-1]["ts"], c) for c in clusters]

# ---------- load raw_data, group by event_id ----------
events = defaultdict(list)
with open(f"{SCRATCH}/raw_data.csv") as f:
    r = csv.DictReader(f)
    for row in r:
        events[int(row["event_id"])].append({
            "t": int(row["timestamp_ms"]),
            "v": float(row["filtered_value"]),
            "stable": row["stable"] == "t",
            "recv": datetime.datetime.fromisoformat(row["received_at"]),
        })

print(f"raw_data events: {len(events)}")

for ev in events.values():
    ev.sort(key=lambda r: r["t"])


def detect_stable_segments(rows, min_run=2):
    """Return list of (start_idx, end_idx) inclusive, for runs of stable=True
    with length >= min_run."""
    segs = []
    i = 0
    n = len(rows)
    while i < n:
        if rows[i]["stable"]:
            j = i
            while j + 1 < n and rows[j + 1]["stable"]:
                j += 1
            if (j - i + 1) >= min_run:
                segs.append((i, j))
            i = j + 1
        else:
            i += 1
    return segs


def interp_value(rows, t_target):
    """Linear interpolation of filtered_value at session-time t_target (ms).
    rows sorted by t. Returns None if t_target outside range."""
    n = len(rows)
    if t_target <= rows[0]["t"]:
        return rows[0]["v"] if t_target >= rows[0]["t"] - 200 else None
    if t_target >= rows[-1]["t"]:
        return None
    # binary search
    lo, hi = 0, n - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if rows[mid]["t"] < t_target:
            lo = mid + 1
        else:
            hi = mid
    # rows[lo]["t"] >= t_target
    if lo == 0:
        return rows[0]["v"]
    a, b = rows[lo - 1], rows[lo]
    if b["t"] == a["t"]:
        return a["v"]
    frac = (t_target - a["t"]) / (b["t"] - a["t"])
    return a["v"] + frac * (b["v"] - a["v"])


# ---------- match events to clusters by wall-clock proximity ----------
results = []  # dicts: event_id, rn, kind(main/topup), coast_time_ms, coast_weight_g, runtime_ms
match_log = []

for event_id, rows in events.items():
    if len(rows) < 10:
        continue
    # estimate session start wall-clock = received_at - timestamp_ms (median over first few rows for robustness)
    ests = []
    for row in rows[:5]:
        ests.append(row["recv"] - datetime.timedelta(milliseconds=row["t"]))
    # median of datetimes: sort and take middle
    ests.sort()
    session_start_wall = ests[len(ests) // 2]

    # baseline segments: all stable segments
    segs = detect_stable_segments(rows, min_run=2)
    if len(segs) < 2:
        continue  # need at least baseline + 1 post-grind stabilization

    # first segment = baseline (near-zero weight) -- verify
    first_seg_val = rows[segs[0][0]]["v"]
    if abs(first_seg_val) > 1.0:
        # doesn't look like a baseline-starting event (mid-session websocket connect); skip
        continue

    post_grind_segs = segs[1:]  # these correspond to rn=1 (main grind), rn=2 (topup1), ...

    # find best-matching cluster: min(timestamp) close to session_start_wall
    best = None
    best_dt = None
    for start_ts, end_ts, c in clusters_by_start:
        dt = abs((start_ts - session_start_wall).total_seconds())
        if dt > 15:
            continue
        if best_dt is None or dt < best_dt:
            best_dt = dt
            best = c
    if best is None:
        continue

    match_log.append((event_id, best_dt, len(post_grind_segs), len(best)))

    # require segment count to line up reasonably (allow raw_data to have fewer
    # trailing segments than the cluster, e.g. if websocket client disconnected
    # before the very last pulse's settle was captured -- but not more)
    K = min(len(post_grind_segs), len(best))

    T_on = 0  # main grind starts at session-relative t=0
    for k in range(K):
        rn = k + 1
        topup_row = best[k]
        if topup_row["rn"] != rn:
            break
        runtime = topup_row["runtimemillis"]
        seg = post_grind_segs[k]
        t_stable_start = rows[seg[0]]["t"]
        v_stable_start = rows[seg[0]]["v"]

        T_off = T_on + runtime

        v_off = interp_value(rows, T_off)
        if v_off is None:
            # advance T_on for next iter anyway
            T_on = rows[seg[1]]["t"]
            continue

        coast_time = t_stable_start - T_off
        coast_weight = v_stable_start - v_off

        results.append({
            "event_id": event_id,
            "rn": rn,
            "kind": "main_grind" if rn == 1 else "topup",
            "runtime_ms": runtime,
            "coast_time_ms": coast_time,
            "coast_weight_g": coast_weight,
            "topup_weightincrement": topup_row["weightincrement"],
        })

        # next pulse's T_on = end of this stable segment
        T_on = rows[seg[1]]["t"]

print(f"\nmatched events (within 15s of a cluster start): {len(match_log)}")
print(f"total pulse-coast observations: {len(results)}")

# ---------- summarize ----------

def summarize(label, vals):
    vals = sorted(vals)
    n = len(vals)
    if n == 0:
        print(f"{label}: n=0")
        return
    def pct(p):
        idx = min(n - 1, max(0, int(round(p * (n - 1)))))
        return vals[idx]
    print(f"{label}: n={n}  median={pct(0.5):.3f}  p25={pct(0.25):.3f}  p75={pct(0.75):.3f}  "
          f"p10={pct(0.10):.3f}  p90={pct(0.90):.3f}  min={vals[0]:.3f}  max={vals[-1]:.3f}")


main_ct = [r["coast_time_ms"] for r in results if r["kind"] == "main_grind"]
main_cw = [r["coast_weight_g"] for r in results if r["kind"] == "main_grind"]
topup_ct = [r["coast_time_ms"] for r in results if r["kind"] == "topup"]
topup_cw = [r["coast_weight_g"] for r in results if r["kind"] == "topup"]

print("\n--- RAW (unfiltered) ---")
summarize("main_grind coast_time_ms", main_ct)
summarize("main_grind coast_weight_g", main_cw)
summarize("topup coast_time_ms", topup_ct)
summarize("topup coast_weight_g", topup_cw)

# sanity: print a handful of raw examples
print("\n--- sample rows (first 20) ---")
for r in results[:20]:
    print(r)

# ---------- filtered / sanity-bounded version ----------
# drop physically implausible: negative coast_time beyond small jitter, huge coast_weight
def plausible(r):
    if r["coast_time_ms"] < -300 or r["coast_time_ms"] > 8000:
        return False
    if abs(r["coast_weight_g"]) > 5:
        return False
    return True

filt = [r for r in results if plausible(r)]
main_f = [r for r in filt if r["kind"] == "main_grind"]
topup_f = [r for r in filt if r["kind"] == "topup"]

print(f"\n--- FILTERED (coast_time in [-300,8000]ms, |coast_weight|<5g): n={len(filt)}/{len(results)} kept ---")
summarize("main_grind coast_time_ms", [r["coast_time_ms"] for r in main_f])
summarize("main_grind coast_weight_g", [r["coast_weight_g"] for r in main_f])
summarize("topup coast_time_ms", [r["coast_time_ms"] for r in topup_f])
summarize("topup coast_weight_g", [r["coast_weight_g"] for r in topup_f])

# distinct events count
print(f"\ndistinct events contributing filtered main-grind obs: {len(set(r['event_id'] for r in main_f))}")
print(f"distinct events contributing filtered topup obs: {len(set(r['event_id'] for r in topup_f))}")

# check correlation: coast_weight vs runtime_ms (proxy for whether longer pulses -> bigger coast)
import math
def corr(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs)/n; my = sum(ys)/n
    sxy = sum((x-mx)*(y-my) for x,y in zip(xs,ys))
    sxx = sum((x-mx)**2 for x in xs)
    syy = sum((y-my)**2 for y in ys)
    if sxx == 0 or syy == 0:
        return None
    return sxy / math.sqrt(sxx*syy)

print("\ncorr(runtime_ms, coast_weight_g) topup:", corr([r["runtime_ms"] for r in topup_f], [r["coast_weight_g"] for r in topup_f]))
print("corr(runtime_ms, coast_time_ms) topup:", corr([r["runtime_ms"] for r in topup_f], [r["coast_time_ms"] for r in topup_f]))
print("corr(runtime_ms, coast_weight_g) main:", corr([r["runtime_ms"] for r in main_f], [r["coast_weight_g"] for r in main_f]))
print("corr(runtime_ms, coast_time_ms) main:", corr([r["runtime_ms"] for r in main_f], [r["coast_time_ms"] for r in main_f]))
