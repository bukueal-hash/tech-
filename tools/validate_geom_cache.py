#!/usr/bin/env python3
"""
validate_geom_cache.py -- Regression test for the per-mesh geometry cache.

Parses debug-c190fb.log and answers:
  1. What is the cache hit rate per rebuild?
  2. How much did dmaExec (DMA reads) drop after the cache warmed?
  3. Did rebuild time (rebuildMs) drop proportionally?
  4. Did the ungated threads (PositionRefreshPass, FrameBuilder, camera)
     improve during rebuild-heavy periods?

Usage:
  python tools/validate_geom_cache.py [path/to/debug-c190fb.log]
"""

import json
import sys
from collections import defaultdict


def parse_log(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def split_sessions(entries, gap_ms=60000):
    if not entries:
        return []
    ts_list = sorted(e.get("timestamp", 0) for e in entries)
    if not ts_list:
        return []
    boundaries = [ts_list[0]]
    for i in range(1, len(ts_list)):
        if ts_list[i] - ts_list[i - 1] > gap_ms:
            boundaries.append(ts_list[i])
    sessions_map = defaultdict(list)
    for e in entries:
        ts = e.get("timestamp", 0)
        session_ts = boundaries[0]
        for b in boundaries:
            if b <= ts:
                session_ts = b
            else:
                break
        sessions_map[session_ts].append(e)
    result = []
    for i, (ts, sess_entries) in enumerate(sorted(sessions_map.items())):
        label = "session_{}".format(i + 1)
        run_ids = set()
        for e in sess_entries:
            rid = e.get("runId", "")
            if rid:
                run_ids.add(rid)
        if run_ids:
            label += " ({})".format(", ".join(sorted(run_ids)))
        result.append((sess_entries, label))
    return result


def analyze_rebuilds(entries):
    """Analyze vis_rebuild entries for cache hit rate and DMA cost reduction."""
    rebuilds = []
    for e in entries:
        if e.get("message") == "vis_rebuild":
            data = e.get("data", {})
            rebuilds.append({
                "ts": e.get("timestamp", 0),
                "ms": data.get("ms", 0),
                "smc": data.get("smc", 0),
                "dmaExec": data.get("dmaExec", 0),
                "dmaLast": data.get("dmaLast", 0),
                "run": e.get("runId", ""),
                "radiusM": data.get("radiusM", 0),
                "cacheHits": data.get("cacheHits", None),
                "cacheMisses": data.get("cacheMisses", None),
                "cacheSize": data.get("cacheSize", None),
            })

    if not rebuilds:
        return None

    rebuilds.sort(key=lambda r: r["ts"])

    # Split by session gaps (counter resets)
    SESSION_GAP = 60000
    segments = []
    seg = [rebuilds[0]]
    for i in range(1, len(rebuilds)):
        if rebuilds[i]["ts"] - rebuilds[i - 1]["ts"] > SESSION_GAP:
            segments.append(seg)
            seg = [rebuilds[i]]
        else:
            seg.append(rebuilds[i])
    segments.append(seg)

    # Analyze each segment
    segment_results = []
    for seg_rebuilds in segments:
        # Per-rebuild dmaExec increments (skip negatives from counter resets)
        increments = []
        for i in range(1, len(seg_rebuilds)):
            d = seg_rebuilds[i]["dmaExec"] - seg_rebuilds[i - 1]["dmaExec"]
            if 0 <= d < 50000:
                increments.append(d)

        # Cache hit/miss from the log (if present)
        has_cache_data = any(r["cacheHits"] is not None for r in seg_rebuilds)

        # First rebuild vs subsequent
        first = seg_rebuilds[0]
        subsequent = seg_rebuilds[1:] if len(seg_rebuilds) > 1 else []

        first_inc = increments[0] if increments else None
        subsequent_incs = increments[1:] if len(increments) > 1 else []

        seg_result = {
            "run": first["run"],
            "count": len(seg_rebuilds),
            "first_ms": first["ms"],
            "first_smc": first["smc"],
            "first_dmaExec_inc": first_inc,
            "first_dmaLast": first["dmaLast"],
            "first_cacheHits": first["cacheHits"],
            "first_cacheMisses": first["cacheMisses"],
            "first_cacheSize": first["cacheSize"],
            "has_cache_data": has_cache_data,
            "subsequent_count": len(subsequent),
            "subsequent_avg_ms": (sum(r["ms"] for r in subsequent) / len(subsequent)) if subsequent else None,
            "subsequent_avg_smc": (sum(r["smc"] for r in subsequent) / len(subsequent)) if subsequent else None,
            "subsequent_avg_inc": (sum(subsequent_incs) / len(subsequent_incs)) if subsequent_incs else None,
            "subsequent_avg_dmaLast": (sum(r["dmaLast"] for r in subsequent) / len(subsequent)) if subsequent else None,
            "subsequent_max_inc": max(subsequent_incs) if subsequent_incs else None,
            "subsequent_min_inc": min(subsequent_incs) if subsequent_incs else None,
            "subsequent_avg_cacheHits": None,
            "subsequent_avg_cacheMisses": None,
            "subsequent_avg_cacheSize": None,
        }

        # Cache stats from subsequent rebuilds
        if has_cache_data:
            sub_cache_hits = [r["cacheHits"] for r in subsequent if r["cacheHits"] is not None]
            sub_cache_misses = [r["cacheMisses"] for r in subsequent if r["cacheMisses"] is not None]
            sub_cache_sizes = [r["cacheSize"] for r in subsequent if r["cacheSize"] is not None]
            if sub_cache_hits:
                seg_result["subsequent_avg_cacheHits"] = sum(sub_cache_hits) / len(sub_cache_hits)
            if sub_cache_misses:
                seg_result["subsequent_avg_cacheMisses"] = sum(sub_cache_misses) / len(sub_cache_misses)
            if sub_cache_sizes:
                seg_result["subsequent_avg_cacheSize"] = sum(sub_cache_sizes) / len(sub_cache_sizes)

        segment_results.append(seg_result)

    return {
        "total_rebuilds": len(rebuilds),
        "segments": segment_results,
    }


def analyze_ungated_threads(entries):
    """Analyze perf_spike for PositionRefreshPass and FrameBuilder."""
    spikes = defaultdict(list)
    for e in entries:
        if e.get("message") == "perf_spike":
            thread = e.get("data", {}).get("thread", "")
            if thread in ("PositionRefreshPass", "FrameBuilder"):
                spikes[thread].append(e.get("data", {}).get("ms", 0))

    results = {}
    for thread, vals in spikes.items():
        if not vals:
            continue
        vals.sort()
        n = len(vals)
        results[thread] = {
            "count": n,
            "avg": sum(vals) / n,
            "p50": vals[n // 2],
            "p95": vals[int(n * 0.95)] if n > 20 else vals[-1],
            "max": vals[-1],
        }
    return results


def analyze_scan_gate(entries):
    """Analyze scan_gate heldMs per scanner."""
    gates = defaultdict(list)
    for e in entries:
        if e.get("message") == "scan_gate":
            scanner = e.get("data", {}).get("scanner", "")
            heldMs = e.get("data", {}).get("heldMs", 0)
            gates[scanner].append(heldMs)

    results = {}
    for scanner, vals in gates.items():
        if not vals:
            continue
        vals.sort()
        n = len(vals)
        results[scanner] = {
            "count": n,
            "avg": sum(vals) / n,
            "p50": vals[n // 2],
            "p95": vals[int(n * 0.95)] if n > 20 else vals[-1],
            "max": vals[-1],
        }
    return results


def analyze_cadence(entries):
    """Analyze cadence_stats for camera, position, aim threads."""
    cadence = defaultdict(list)
    for e in entries:
        if e.get("message") == "cadence_stats":
            data = e.get("data", {})
            thread = data.get("thread", "")
            cadence[thread].append({
                "count": data.get("count", 0),
                "avg_ms": data.get("avg_ms", 0),
                "max_ms": data.get("max_ms", 0),
                "min_ms": data.get("min_ms", 0),
            })

    results = {}
    for thread, samples in cadence.items():
        if not samples:
            continue
        avg_ms = sum(s["avg_ms"] for s in samples) / len(samples)
        max_ms = max(s["max_ms"] for s in samples)
        min_ms = min(s["min_ms"] for s in samples)
        results[thread] = {
            "samples": len(samples),
            "avg_ms": avg_ms,
            "max_ms": max_ms,
            "min_ms": min_ms,
        }
    return results


def monitor_live(path, interval=10):
    """Watch the log file and print a live dashboard every `interval` seconds."""
    import time
    import os

    print("Watching {} for vis_rebuild and perf_spike entries...".format(path))
    print("Press Ctrl+C to stop.\n")

    last_size = 0
    rebuilds = []
    spikes = defaultdict(list)
    gates = defaultdict(list)

    try:
        while True:
            # Read new lines since last check
            try:
                size = os.path.getsize(path)
                if size < last_size:
                    last_size = 0  # file was truncated
                if size > last_size:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        f.seek(last_size)
                        new_data = f.read()
                    last_size = size

                    for line in new_data.split("\n"):
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            e = json.loads(line)
                        except:
                            continue

                        msg = e.get("message", "")
                        data = e.get("data", {})
                        ts = e.get("timestamp", 0)

                        if msg == "vis_rebuild":
                            rebuilds.append({
                                "ts": ts,
                                "ms": data.get("ms", 0),
                                "smc": data.get("smc", 0),
                                "dmaExec": data.get("dmaExec", 0),
                                "cacheHits": data.get("cacheHits"),
                                "cacheMisses": data.get("cacheMisses"),
                                "cacheSize": data.get("cacheSize"),
                            })
                        elif msg == "perf_spike":
                            thread = data.get("thread", "")
                            if thread in ("PositionRefreshPass", "FrameBuilder"):
                                spikes[thread].append(data.get("ms", 0))
                        elif msg == "scan_gate":
                            scanner = data.get("scanner", "")
                            gates[scanner].append(data.get("heldMs", 0))

            except FileNotFoundError:
                pass

            time.sleep(interval)

            # Print dashboard
            print("\033[2J\033[H")  # clear screen
            print("=== GEOMETRY CACHE LIVE DASHBOARD ===")
            print("    Rebuilds: {} | Last: {}ms, {} SMCs".format(
                len(rebuilds),
                rebuilds[-1]["ms"] if rebuilds else "-",
                rebuilds[-1]["smc"] if rebuilds else "-"))

            if rebuilds:
                # Per-rebuild dmaExec increments
                increments = []
                for i in range(1, len(rebuilds)):
                    d = rebuilds[i]["dmaExec"] - rebuilds[i - 1]["dmaExec"]
                    if 0 <= d < 50000:
                        increments.append(d)

                if increments:
                    avg_inc = sum(increments) / len(increments)
                    print("    Avg reads/rebuild: {:.0f} (min={}, max={})".format(
                        avg_inc, min(increments), max(increments)))

                # Cache stats
                last = rebuilds[-1]
                if last["cacheHits"] is not None:
                    total = last["cacheHits"] + last["cacheMisses"]
                    hit_rate = last["cacheHits"] / total * 100 if total > 0 else 0
                    print("    Cache: hits={} misses={} rate={:.0f}% size={}".format(
                        last["cacheHits"], last["cacheMisses"],
                        hit_rate, last["cacheSize"]))
                else:
                    print("    Cache: (no cache data in log -- cache not active yet)")

            # Ungated threads
            print("")
            print("    UNGATED THREADS:")
            for thread in ["PositionRefreshPass", "FrameBuilder"]:
                vals = spikes.get(thread, [])
                if vals:
                    avg = sum(vals) / len(vals)
                    mx = max(vals)
                    print("      {}: n={:>4}  avg={:>6.1f}ms  max={:>6.0f}ms".format(
                        thread, len(vals), avg, mx))

            # Scan gate
            print("")
            print("    SCAN GATE heldMs:")
            for scanner in ["Update", "EntityList", "RobotList"]:
                vals = gates.get(scanner, [])
                if vals:
                    avg = sum(vals) / len(vals)
                    mx = max(vals)
                    print("      {:16s}  n={:>5}  avg={:>6.1f}ms  max={:>6.0f}ms".format(
                        scanner, len(vals), avg, mx))

            # Validation status
            print("")
            print("    VALIDATION:")
            if rebuilds and increments:
                avg_inc = sum(increments) / len(increments)
                status = "PASS" if avg_inc < 20 else "WARMING" if avg_inc < 500 else "MISS"
                print("      [{:>7s}] avg reads/rebuild < 20: {:.0f}".format(status, avg_inc))
            else:
                print("      [WAITING] need >= 2 rebuilds")

            for thread in ["PositionRefreshPass", "FrameBuilder"]:
                vals = spikes.get(thread, [])
                if vals:
                    avg = sum(vals) / len(vals)
                    status = "PASS" if avg < 50 else "WARN" if avg < 200 else "FAIL"
                    print("      [{:>7s}] {} avg < 50ms: {:.1f}".format(
                        status, thread, avg))

            if rebuilds and rebuilds[-1].get("cacheHits") is not None:
                total = rebuilds[-1]["cacheHits"] + rebuilds[-1]["cacheMisses"]
                if total > 0:
                    hit_rate = rebuilds[-1]["cacheHits"] / total * 100
                    status = "PASS" if hit_rate > 80 else "WARMING" if hit_rate > 0 else "MISS"
                    print("      [{:>7s}] cache hit rate > 80%: {:.0f}%".format(
                        status, hit_rate))

    except KeyboardInterrupt:
        print("\nStopped. {} rebuilds captured.".format(len(rebuilds)))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--live":
        path = sys.argv[2] if len(sys.argv) > 2 else "debug-c190fb.log"
        monitor_live(path)
        return

    path = sys.argv[1] if len(sys.argv) > 1 else "debug-c190fb.log"

    entries = list(parse_log(path))
    print("Parsed {} entries from {}\n".format(len(entries), path))

    # Split into sessions
    sessions = split_sessions(entries)
    print("Found {} session(s)\n".format(len(sessions)))

    # Find session with vis_rebuild data (prefer 'vis' runId)
    best_session = None
    best_label = ""
    best_rebuild_count = 0
    for sess_entries, label in sessions:
        rebuild_count = sum(1 for e in sess_entries if e.get("message") == "vis_rebuild")
        if rebuild_count > best_rebuild_count:
            best_rebuild_count = rebuild_count
            best_session = sess_entries
            best_label = label

    if not best_session:
        print("No vis_rebuild entries found.")
        return

    print("Using {} ({} entries, {} vis_rebuilds)\n".format(
        best_label, len(best_session), best_rebuild_count))

    entries = best_session

    # ── 1. Rebuild analysis ─────────────────────────────────────────────
    rebuild_data = analyze_rebuilds(entries)

    print("=" * 80)
    print("1. VIS_REBUILD DMA COST: FIRST vs SUBSEQUENT")
    print("=" * 80)

    if not rebuild_data:
        print("  No rebuild data available.")
        return

    for seg in rebuild_data["segments"]:
        print("")
        print("  Segment (runId={}): {} rebuilds".format(seg["run"], seg["count"]))
        print("  " + "-" * 60)

        # First rebuild
        print("  FIRST rebuild:")
        print("    rebuildMs:    {}ms".format(seg["first_ms"]))
        print("    smc:          {}".format(seg["first_smc"]))
        if seg["first_dmaExec_inc"] is not None:
            print("    DMA reads:    {}".format(seg["first_dmaExec_inc"]))
        if seg["first_dmaLast"] is not None:
            print("    dmaLast:      {}".format(seg["first_dmaLast"]))
        if seg["first_cacheHits"] is not None:
            print("    cacheHits:    {}".format(seg["first_cacheHits"]))
            print("    cacheMisses:  {}".format(seg["first_cacheMisses"]))
            print("    cacheSize:    {}".format(seg["first_cacheSize"]))

        # Subsequent rebuilds
        if seg["subsequent_count"] > 0:
            print("  SUBSEQUENT rebuilds (n={}):".format(seg["subsequent_count"]))
            if seg["subsequent_avg_ms"] is not None:
                print("    avg rebuildMs: {:.1f}ms".format(seg["subsequent_avg_ms"]))
            if seg["subsequent_avg_smc"] is not None:
                print("    avg smc:       {:.1f}".format(seg["subsequent_avg_smc"]))
            if seg["subsequent_avg_inc"] is not None:
                print("    avg DMA reads: {:.0f}".format(seg["subsequent_avg_inc"]))
                if seg["subsequent_min_inc"] is not None:
                    print("    min DMA reads: {}".format(seg["subsequent_min_inc"]))
                if seg["subsequent_max_inc"] is not None:
                    print("    max DMA reads: {}".format(seg["subsequent_max_inc"]))
            if seg["subsequent_avg_dmaLast"] is not None:
                print("    avg dmaLast:   {:.0f}".format(seg["subsequent_avg_dmaLast"]))

            if seg["has_cache_data"]:
                if seg["subsequent_avg_cacheHits"] is not None:
                    print("    avg cacheHits:   {:.1f}".format(seg["subsequent_avg_cacheHits"]))
                if seg["subsequent_avg_cacheMisses"] is not None:
                    print("    avg cacheMisses: {:.1f}".format(seg["subsequent_avg_cacheMisses"]))
                if seg["subsequent_avg_cacheSize"] is not None:
                    print("    avg cacheSize:   {:.1f}".format(seg["subsequent_avg_cacheSize"]))

                # Compute hit rate
                if seg["subsequent_avg_cacheHits"] is not None and seg["subsequent_avg_cacheMisses"] is not None:
                    total = seg["subsequent_avg_cacheHits"] + seg["subsequent_avg_cacheMisses"]
                    if total > 0:
                        hit_rate = seg["subsequent_avg_cacheHits"] / total * 100
                        print("    CACHE HIT RATE:  {:.1f}%".format(hit_rate))

            # DMA savings
            if seg["first_dmaExec_inc"] is not None and seg["subsequent_avg_inc"] is not None:
                if seg["first_dmaExec_inc"] > 0:
                    savings = (1 - seg["subsequent_avg_inc"] / seg["first_dmaExec_inc"]) * 100
                    print("    DMA savings:     {:.0f}% vs first rebuild".format(savings))

            # Time savings
            if seg["first_ms"] > 0 and seg["subsequent_avg_ms"] is not None:
                time_savings = (1 - seg["subsequent_avg_ms"] / seg["first_ms"]) * 100
                print("    TIME savings:    {:.0f}% vs first rebuild".format(time_savings))
        else:
            print("  (only one rebuild in segment -- cannot compare)")

    # ── 2. Ungated thread health ─────────────────────────────────────────
    print("")
    print("=" * 80)
    print("2. UNGATED THREAD HEALTH (PositionRefreshPass + FrameBuilder)")
    print("=" * 80)

    ungated = analyze_ungated_threads(entries)
    for thread in ["PositionRefreshPass", "FrameBuilder"]:
        u = ungated.get(thread)
        if u:
            print("  {}: n={}  avg={:.1f}ms  p50={}ms  p95={}ms  max={}ms".format(
                thread, u["count"], u["avg"], u["p50"], u["p95"], u["max"]))
        else:
            print("  {}: (no data)".format(thread))

    # ── 3. Scan gate heldMs ──────────────────────────────────────────────
    print("")
    print("=" * 80)
    print("3. SCAN GATE heldMs (gated scanner work time)")
    print("=" * 80)

    gate_data = analyze_scan_gate(entries)
    for scanner in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        g = gate_data.get(scanner)
        if g:
            print("  {:16s}  n={:>5}  avg={:>7.1f}ms  p50={:>5}ms  p95={:>5}ms  max={:>5}ms".format(
                scanner, g["count"], g["avg"], g["p50"], g["p95"], g["max"]))
        else:
            print("  {:16s}  (no data)".format(scanner))

    # ── 4. Cadence stats ─────────────────────────────────────────────────
    print("")
    print("=" * 80)
    print("4. CADENCE STATS (from cadence_stats log entries)")
    print("=" * 80)

    cadence = analyze_cadence(entries)
    for thread in ["camera", "position", "aim"]:
        c = cadence.get(thread)
        if c:
            print("  {:10s}  samples={}  avg={:.1f}ms  min={}ms  max={}ms".format(
                thread, c["samples"], c["avg_ms"], c["min_ms"], c["max_ms"]))
        else:
            print("  {:10s}  (no data)".format(thread))

    # ── 5. Summary ───────────────────────────────────────────────────────
    print("")
    print("=" * 80)
    print("5. SUMMARY: GEOMETRY CACHE VALIDATION")
    print("=" * 80)

    # Find the best segment with cache data
    cache_segs = [s for s in rebuild_data["segments"] if s["has_cache_data"]]
    if cache_segs:
        seg = cache_segs[-1]  # use the most recent
        print("")
        print("  Cache data found in segment (runId={}):".format(seg["run"]))
        if seg["subsequent_avg_cacheHits"] is not None and seg["subsequent_avg_cacheMisses"] is not None:
            total = seg["subsequent_avg_cacheHits"] + seg["subsequent_avg_cacheMisses"]
            if total > 0:
                hit_rate = seg["subsequent_avg_cacheHits"] / total * 100
                print("    Hit rate:         {:.1f}%".format(hit_rate))
        if seg["first_dmaExec_inc"] is not None and seg["subsequent_avg_inc"] is not None:
            if seg["first_dmaExec_inc"] > 0:
                savings = (1 - seg["subsequent_avg_inc"] / seg["first_dmaExec_inc"]) * 100
                print("    DMA read savings: {:.0f}%".format(savings))
                print("    First rebuild:    {} reads".format(seg["first_dmaExec_inc"]))
                print("    Subsequent avg:   {:.0f} reads".format(seg["subsequent_avg_inc"]))
        if seg["first_ms"] > 0 and seg["subsequent_avg_ms"] is not None:
            time_savings = (1 - seg["subsequent_avg_ms"] / seg["first_ms"]) * 100
            print("    Time savings:     {:.0f}%".format(time_savings))
            print("    First rebuild:    {}ms".format(seg["first_ms"]))
            print("    Subsequent avg:   {:.1f}ms".format(seg["subsequent_avg_ms"]))
        if seg["subsequent_avg_cacheSize"] is not None:
            print("    Cache size:       {:.0f} meshes".format(seg["subsequent_avg_cacheSize"]))
    else:
        # No cache data yet -- show pre-cache baseline
        print("")
        print("  NO CACHE DATA IN LOG (cache was implemented this session)")
        print("  Pre-cache baseline from most recent segment:")
        non_cache_segs = [s for s in rebuild_data["segments"] if not s["has_cache_data"]]
        if non_cache_segs:
            seg = non_cache_segs[-1]
            if seg["first_dmaExec_inc"] is not None:
                print("    First rebuild:    {} DMA reads".format(seg["first_dmaExec_inc"]))
            if seg["subsequent_avg_inc"] is not None:
                print("    Subsequent avg:   {:.0f} DMA reads".format(seg["subsequent_avg_inc"]))
            if seg["first_ms"] > 0:
                print("    First rebuild:    {}ms".format(seg["first_ms"]))
            if seg["subsequent_avg_ms"] is not None:
                print("    Subsequent avg:   {:.1f}ms".format(seg["subsequent_avg_ms"]))
            print("")
            print("  EXPECTED AFTER CACHE WARM-UP:")
            print("    Subsequent reads: ~6 (2 SMC x 3 CompXform reads)")
            print("    Expected savings: ~99%")
            print("    Expected time:    ~0.1ms (vs current {:.1f}ms)".format(
                seg["subsequent_avg_ms"] if seg["subsequent_avg_ms"] else 0))

    print("")
    print("  VALIDATION CRITERIA:")
    print("    [PASS] cacheHits > 0 in subsequent rebuilds")
    print("    [PASS] subsequent avg reads < first rebuild reads")
    print("    [PASS] subsequent avg rebuildMs < first rebuild ms")
    print("    [PASS] PositionRefreshPass avg < 50ms")
    print("    [PASS] FrameBuilder avg < 50ms")
    print("    [TARGET] cache hit rate > 80% (most SMCs share meshes)")
    print("    [TARGET] subsequent avg reads < 20 (near cache-only)")


if __name__ == "__main__":
    main()
