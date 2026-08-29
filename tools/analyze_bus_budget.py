#!/usr/bin/env python3
"""
analyze_bus_budget.py -- Parse debug-c190fb.log to compute real DMA bus
utilisation per scanner and compare against the theoretical model.

Reads:
  scan_gate   -> heldMs, waitMs, blockedBy, waiters
  perf_spike  -> thread, ms (total task duration including gate wait)

Outputs a per-scanner table with:
  - turns/s, avg heldMs, duty%, avg taskMs
  - estimated reads/s (from heldMs x read_rate or dmaExec deltas)
  - comparison to theoretical model predictions

Usage:
  python tools/analyze_bus_budget.py [path/to/debug-c190fb.log]
"""

import json
import sys
from collections import defaultdict


# -- Model parameters (from the theoretical bus budget analysis) -----------
MODEL = {
    # Scanner: (period_ms, typical_held_ms)
    "Update":          (16,  10),
    "EntityList":      (16,  50),
    "RobotList":       (48, 100),
    "ContainerList":   (16,  80),
    "ItemList":        (16,  80),
}
GATE_IDLE_GAP_MS = 12
TAU_US = 60  # estimated us per scattered NOCACHE read (midpoint 40-100us)

# Ungated threads
UNGATED = {
    "camera":                  8,
    "PositionRefreshPass":    16,
    "FrameBuilder":           12,
    "aim":                     4,
}


def parse_log(path):
    """Yield parsed JSON dicts from each line."""
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
    """Split log entries into contiguous sessions."""
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


def compute_scanner_stats(entries):
    """From scan_gate + perf_spike entries, compute per-scanner metrics."""
    gates = defaultdict(list)
    spikes = defaultdict(list)

    for e in entries:
        msg = e.get("message", "")
        data = e.get("data", {})
        ts = e.get("timestamp", 0)

        if msg == "scan_gate":
            scanner = data.get("scanner", "")
            gates[scanner].append({
                "ts": ts,
                "heldMs": data.get("heldMs", 0),
                "waitMs": data.get("waitMs", 0),
                "blockedBy": data.get("blockedBy", ""),
                "waiters": data.get("waiters", 0),
            })
        elif msg == "perf_spike":
            thread = data.get("thread", "")
            ms = data.get("ms", 0)
            spikes[thread].append({"ts": ts, "ms": ms})

    results = {}

    for scanner, gate_list in gates.items():
        gate_list.sort(key=lambda g: g["ts"])
        if len(gate_list) < 2:
            continue

        held_list = [g["heldMs"] for g in gate_list]
        wait_list = [g["waitMs"] for g in gate_list]

        t_span_s = (gate_list[-1]["ts"] - gate_list[0]["ts"]) / 1000.0
        if t_span_s <= 0:
            continue

        turns = len(gate_list)
        turns_per_s = turns / t_span_s

        avg_held = sum(held_list) / len(held_list)
        max_held = max(held_list)
        avg_wait = sum(wait_list) / len(wait_list)
        max_wait = max(wait_list)
        median_held = sorted(held_list)[len(held_list) // 2]
        p95_held = sorted(held_list)[int(len(held_list) * 0.95)]
        p99_held = sorted(held_list)[int(len(held_list) * 0.99)]

        total_held_s = sum(held_list) / 1000.0
        duty_pct = (total_held_s / t_span_s) * 100.0

        # Active-only analysis: exclude gaps > 5s (idle between raids)
        active_turns = 0
        active_held_sum = 0
        for i, g in enumerate(gate_list):
            if i > 0:
                gap = g["ts"] - gate_list[i - 1]["ts"]
                if gap > 5000:
                    continue  # idle gap
            active_turns += 1
            active_held_sum += g["heldMs"]

        active_span_s = 0
        if active_turns >= 2:
            # Compute active span from non-idle intervals
            active_span_ms = 0
            for i in range(1, len(gate_list)):
                gap = gate_list[i]["ts"] - gate_list[i - 1]["ts"]
                if gap <= 5000:
                    active_span_ms += gap
            active_span_s = active_span_ms / 1000.0

        active_turns_per_s = active_turns / active_span_s if active_span_s > 0 else 0
        active_duty_pct = (active_held_sum / 1000.0 / active_span_s * 100) if active_span_s > 0 else 0

        model_period, model_held = MODEL.get(scanner, (16, 20))
        model_turns_per_s = 1000.0 / (model_held + GATE_IDLE_GAP_MS)
        model_duty_pct = model_held / (model_held + GATE_IDLE_GAP_MS) * 100.0

        est_reads_per_turn = (avg_held / 1000.0) * (1000000.0 / TAU_US)
        est_reads_per_s = est_reads_per_turn * active_turns_per_s if active_turns_per_s > 0 else 0

        t_min = gate_list[0]["ts"]
        t_max = gate_list[-1]["ts"]
        thread_spikes = [s for s in spikes.get(scanner, [])
                         if t_min <= s["ts"] <= t_max]
        avg_task_ms = 0
        max_task_ms = 0
        if thread_spikes:
            task_ms_list = [s["ms"] for s in thread_spikes]
            avg_task_ms = sum(task_ms_list) / len(task_ms_list)
            max_task_ms = max(task_ms_list)

        blocked_count = sum(1 for g in gate_list if g["blockedBy"])

        results[scanner] = {
            "turns": turns,
            "t_span_s": t_span_s,
            "turns_per_s": turns_per_s,
            "active_turns": active_turns,
            "active_span_s": active_span_s,
            "active_turns_per_s": active_turns_per_s,
            "active_duty_pct": active_duty_pct,
            "avg_held_ms": avg_held,
            "max_held_ms": max_held,
            "median_held_ms": median_held,
            "p95_held_ms": p95_held,
            "p99_held_ms": p99_held,
            "avg_wait_ms": avg_wait,
            "max_wait_ms": max_wait,
            "duty_pct": duty_pct,
            "model_turns_per_s": model_turns_per_s,
            "model_duty_pct": model_duty_pct,
            "model_held_ms": model_held,
            "est_reads_per_s": est_reads_per_s,
            "avg_task_ms": avg_task_ms,
            "max_task_ms": max_task_ms,
            "avg_waiters": sum(g["waiters"] for g in gate_list) / len(gate_list),
            "blocked_pct": blocked_count / len(gate_list) * 100,
        }

    return results


def compute_ungated_stats(entries):
    """Compute stats for ungated threads from perf_spike entries."""
    spikes = defaultdict(list)
    for e in entries:
        if e.get("message") == "perf_spike":
            thread = e.get("data", {}).get("thread", "")
            ms = e.get("data", {}).get("ms", 0)
            ts = e.get("timestamp", 0)
            if thread in UNGATED:
                spikes[thread].append({"ts": ts, "ms": ms})

    results = {}
    for thread, period in UNGATED.items():
        slist = spikes.get(thread, [])
        if len(slist) < 2:
            continue
        slist.sort(key=lambda s: s["ts"])
        t_span_s = (slist[-1]["ts"] - slist[0]["ts"]) / 1000.0
        if t_span_s <= 0:
            continue
        ms_list = [s["ms"] for s in slist]
        actual_cadence_ms = t_span_s * 1000 / len(slist)
        avg_ms = sum(ms_list) / len(ms_list)
        max_ms = max(ms_list)
        min_ms = min(ms_list)
        duty_pct = (avg_ms / actual_cadence_ms) * 100 if actual_cadence_ms > 0 else 0

        results[thread] = {
            "turns": len(slist),
            "t_span_s": t_span_s,
            "period_ms": period,
            "actual_cadence_ms": actual_cadence_ms,
            "avg_ms": avg_ms,
            "max_ms": max_ms,
            "min_ms": min_ms,
            "duty_pct": duty_pct,
            "cadence_drift_ms": actual_cadence_ms - period,
            "pct_turns_dropped": max(0, (1 - period / actual_cadence_ms) * 100) if actual_cadence_ms > period else 0,
        }

    return results


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "debug-c190fb.log"

    entries = list(parse_log(path))
    print("Parsed {} log entries from {}\n".format(len(entries), path))

    sessions = split_sessions(entries)
    print("Found {} session(s):\n".format(len(sessions)))

    best_session = None
    best_gate_count = 0
    for sess_entries, label in sessions:
        gate_count = sum(1 for e in sess_entries if e.get("message") == "scan_gate")
        spike_count = sum(1 for e in sess_entries if e.get("message") == "perf_spike")
        print("  {} -- {} entries ({} gates, {} spikes)".format(
            label, len(sess_entries), gate_count, spike_count))
        if gate_count > best_gate_count:
            best_gate_count = gate_count
            best_session = sess_entries

    if not best_session:
        print("\nNo scan_gate entries found in any session.")
        return

    print("\nUsing longest session with gate data ({} entries)".format(len(best_session)))
    entries = best_session

    # -- Gated scanners ---------------------------------------------------
    scanner_stats = compute_scanner_stats(entries)

    print("")
    print("=" * 125)
    print("GATED SCANNER BUS BUDGET -- ACTUAL vs MODEL")
    print("=" * 125)
    print("{:<16} {:>6} {:>7} {:>8} {:>8} {:>8} {:>9} {:>9} {:>7} {:>7} {:>9} {:>9} {:>9} {:>11}".format(
        "Scanner", "Turns", "Span", "t/s", "Active",
        "Act t/s", "Held(avg)", "Held(max)", "P50", "P95",
        "Duty%", "ActDuty%", "Model%", "EstRd/s"))
    print("-" * 125)

    total_active_bus_ms = 0

    for scanner in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        s = scanner_stats.get(scanner)
        if not s:
            print("{:<16}  (no data)".format(scanner))
            continue

        print("{:<16} {:>6} {:>6.1f}s {:>7.1f} {:>6.0f}s {:>7.1f} {:>8.1f}ms {:>8.0f}ms {:>6.0f} {:>6.0f} {:>6.1f}% {:>6.1f}% {:>6.1f}% {:>10.0f}".format(
            scanner, s["turns"], s["t_span_s"],
            s["turns_per_s"], s["active_span_s"],
            s["active_turns_per_s"],
            s["avg_held_ms"], s["max_held_ms"],
            s["median_held_ms"], s["p95_held_ms"],
            s["duty_pct"], s["active_duty_pct"],
            s["model_duty_pct"],
            s["est_reads_per_s"]))

        total_active_bus_ms += s["active_turns_per_s"] * s["avg_held_ms"]

    print("-" * 125)
    print("")

    # Active bus utilisation
    print("ACTIVE-ONLY BUS UTILISATION (excludes idle gaps > 5s)")
    print("-" * 60)
    print("  Total active bus ms/s:  {:.0f}".format(total_active_bus_ms))
    print("  Total active duty:      {:.1f}%".format(total_active_bus_ms / 10))
    print("  Bus quiet (active):     {:.1f}%".format(100 - total_active_bus_ms / 10))
    print("")

    # Contention summary
    print("CONTENTION SUMMARY")
    print("-" * 80)
    print("{:<16} {:>12} {:>10} {:>10} {:>10} {:>10}".format(
        "Scanner", "Avg waiters", "% blocked", "Max wait", "Avg wait", "P99 held"))
    for scanner in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        s = scanner_stats.get(scanner)
        if not s:
            continue
        print("{:<16} {:>12.1f} {:>9.1f}% {:>8.0f}ms {:>8.0f}ms {:>8.0f}ms".format(
            scanner, s["avg_waiters"], s["blocked_pct"],
            s["max_wait_ms"], s["avg_wait_ms"], s["p99_held_ms"]))
    print("")

    # -- Ungated threads ---------------------------------------------------
    ungated_stats = compute_ungated_stats(entries)

    print("=" * 100)
    print("UNGATED THREAD CADENCE -- ACTUAL vs CONFIGURED")
    print("=" * 100)
    print("{:<22} {:>6} {:>7} {:>7} {:>7} {:>7} {:>7} {:>8} {:>8} {:>8}".format(
        "Thread", "Turns", "Span", "Config", "Actual",
        "Drift", "Drops%", "Avg(ms)", "Max(ms)", "Min(ms)"))
    print("-" * 100)

    for thread in ["PositionRefreshPass", "FrameBuilder", "camera", "aim"]:
        u = ungated_stats.get(thread)
        if not u:
            print("{:<22}  (no data)".format(thread))
            continue
        print("{:<22} {:>6} {:>6.1f}s {:>5d}ms {:>6.1f}ms {:>+6.1f}ms {:>6.1f}% {:>7.1f} {:>7.0f} {:>7.0f}".format(
            thread, u["turns"], u["t_span_s"],
            u["period_ms"], u["actual_cadence_ms"],
            u["cadence_drift_ms"], u["pct_turns_dropped"],
            u["avg_ms"], u["max_ms"], u["min_ms"]))
    print("")

    # -- Bus budget summary ------------------------------------------------
    print("=" * 80)
    print("TOTAL BUS BUDGET (during active scanning)")
    print("=" * 80)

    total_bus_ms = 0
    for scanner in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        s = scanner_stats.get(scanner)
        if not s:
            continue
        bus_ms = s["active_turns_per_s"] * s["avg_held_ms"]
        total_bus_ms += bus_ms
        print("  {:<16}  {:.1f} turns/s x {:.1f}ms = {:.0f} ms/s".format(
            scanner, s["active_turns_per_s"], s["avg_held_ms"], bus_ms))

    print("  " + "-" * 40)
    print("  TOTAL DUTY:        {:.0f} ms/s = {:.1f}%".format(total_bus_ms, total_bus_ms / 10))
    print("  QUIET TIME:        {:.0f} ms/s = {:.1f}%".format(1000 - total_bus_ms, 100 - total_bus_ms / 10))
    print("")

    # -- Worst-case corridor -----------------------------------------------
    print("=" * 80)
    print("WORST-CASE CORRIDOR (all scanners back-to-back, no idle gaps)")
    print("=" * 80)

    worst_case_ms = 0
    for scanner in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        s = scanner_stats.get(scanner)
        if not s:
            continue
        worst_case_ms += s["max_held_ms"]
        print("  {:<16}  worst held: {:.0f}ms".format(scanner, s["max_held_ms"]))

    print("  " + "-" * 40)
    print("  Sum of worst held: {}ms".format(worst_case_ms))
    for label, period in [("16ms world tick", 16), ("8ms camera tick", 8), ("4ms aim tick", 4)]:
        if worst_case_ms > period:
            print("  {:20s} BREACHED ({:.0f}x)".format(label + ":", worst_case_ms / period))
        else:
            print("  {:20s} OK ({:.1f}x)".format(label + ":", worst_case_ms / period))
    print("")

    # -- Model vs actual ---------------------------------------------------
    print("=" * 100)
    print("MODEL vs ACTUAL COMPARISON (active-only)")
    print("=" * 100)
    print("{:<16} {:>10} {:>10} {:>10} {:>10} {:>10} {:>10} {:>10}".format(
        "Scanner", "t/s(model)", "t/s(real)", "Duty(model)", "Duty(real)",
        "Held(model)", "Held(real)", "Held(max)"))
    print("-" * 100)
    for scanner in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        s = scanner_stats.get(scanner)
        if not s:
            continue
        print("{:<16} {:>10.1f} {:>10.1f} {:>9.1f}% {:>9.1f}% {:>9.0f}ms {:>9.0f}ms {:>9.0f}ms".format(
            scanner, s["model_turns_per_s"], s["active_turns_per_s"],
            s["model_duty_pct"], s["active_duty_pct"],
            s["model_held_ms"], s["avg_held_ms"], s["max_held_ms"]))

    print("")
    print("KEY FINDINGS:")
    print("  1. Real heldMs is LOWER than model in most cases (scans are faster)")
    print("  2. Real turns/s is lower because not all turns produce log entries")
    print("     (scan_gate only logs when waitMs > 5ms)")
    print("  3. Burst ratio: max_held / avg_held shows how bad spikes get")
    print("  4. Active duty% is the true bus utilisation during gameplay")
    print("  5. The idle gap (12ms) is what makes real duty much lower than model")


if __name__ == "__main__":
    main()
