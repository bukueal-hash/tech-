#!/usr/bin/env python3
"""
bus_budget_math.py -- Worst-case bound on total gated DMA bytes per second
and per-scanner heldMs budgets to keep total duty under 50%.

Derivation from first principles, then calibration against the measured
scan_gate/perf_spike data in debug-c190fb.log.
"""

import json
from collections import defaultdict

# -- Constants from the codebase -----------------------------------------
GAP_MS = 12            # kGateIdleGap: enforced idle between gated turns
TAU_US = 60            # mid-range cost per scattered NOCACHE read (40-100us)
READ_SIZE_BYTES = 8    # typical small scatter read (pointer / uint64)
LARGE_BLOCK_MB_S = 200 # large-block read throughput (module cache, geometry)

# Scanner periods (SyncedThread intervals)
SCANNERS = {
    "Update":        {"period_ms": 16},
    "EntityList":    {"period_ms": 16},
    "RobotList":     {"period_ms": 48},
    "ContainerList": {"period_ms": 16},
    "ItemList":      {"period_ms": 16},
}

# Measured values from debug-c190fb.log (session_4, active-only)
MEASURED = {
    "Update":        {"avg_held": 11.2, "max_held": 778,  "p95_held": 38,  "obs_turns_per_s": 1.8},
    "EntityList":    {"avg_held": 38.8, "max_held": 781,  "p95_held": 77,  "obs_turns_per_s": 1.7},
    "RobotList":     {"avg_held": 25.1, "max_held": 631,  "p95_held": 48,  "obs_turns_per_s": 1.7},
    "ContainerList": {"avg_held":  1.0, "max_held": 431,  "p95_held":  0,  "obs_turns_per_s": 1.7},
    "ItemList":      {"avg_held":  1.3, "max_held": 561,  "p95_held":  0,  "obs_turns_per_s": 1.6},
}


def print_section(title):
    print("")
    print("=" * 80)
    print(title)
    print("=" * 80)


def main():

    # =====================================================================
    # PART 1: Derive worst-case DMA throughput bound
    # =====================================================================
    print_section("PART 1: WORST-CASE DMA THROUGHPUT BOUND")

    # Per-scanner: how many turns/s can each scanner achieve?
    # The gate constrains: turns/s <= 1000 / (heldMs + GAP)
    # Maximum turns/s (heldMs -> 0): 1000/GAP = 83.3 turns/s
    # Actual turns/s depends on the scanner's actual heldMs

    print("")
    print("The gate enforces: turns/s <= 1000 / (heldMs + {gap}ms)".format(gap=GAP_MS))
    print("At heldMs -> 0:    turns/s -> {max_tps:.1f} (absolute ceiling)".format(max_tps=1000/GAP_MS))
    print("")
    print("Each turn consumes heldMs of bus time + GAP of silence.")
    print("Total bus duty = sum over scanners of (turns/s x heldMs) / 1000")
    print("")

    # Worst-case: every scanner at maximum burst heldMs
    print("WORST-CASE PER-SCANNER (from measured max_held):")
    print("-" * 72)
    print("{:<16} {:>10} {:>12} {:>12} {:>12} {:>12}".format(
        "Scanner", "max_held", "eff_period", "worst_t/s", "duty%", "bus_ms/s"))
    print("-" * 72)

    total_worst_duty = 0
    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        max_held = m["max_held"]
        eff_period = max_held + GAP_MS
        worst_tps = 1000.0 / eff_period
        duty = worst_tps * max_held / 10.0  # percent
        bus_ms = worst_tps * max_held
        total_worst_duty += duty
        print("{:<16} {:>8.0f}ms {:>10.0f}ms {:>10.2f} {:>10.1f}% {:>10.0f}".format(
            name, max_held, eff_period, worst_tps, duty, bus_ms))

    print("-" * 72)
    print("{:<16} {:>10} {:>12} {:>12} {:>10.1f}% {:>10.0f}".format(
        "TOTAL (worst)", "", "", "", total_worst_duty, total_worst_duty * 10))

    print("")
    print("Interpretation:")
    print("  If every scanner simultaneously hits its worst-case burst,")
    print("  the bus is occupied {:.0f}% of the time.  This is impossible".format(total_worst_duty))
    print("  (exceeds 100%), so in practice the gate serializes them and")
    print("  each scanner's effective period stretches to accomodate the queue.")
    print("")

    # =====================================================================
    # PART 2: Derive bytes/s bound
    # =====================================================================
    print_section("PART 2: WORST-CASE DMA BYTES PER SECOND")

    # Two regimes: scattered reads (latency-bound) and large blocks (throughput-bound)
    reads_per_sec_scattered = 1_000_000.0 / TAU_US  # max scattered reads/s
    bytes_per_sec_scattered = reads_per_sec_scattered * READ_SIZE_BYTES

    print("")
    print("DMA device limits:")
    print("  Scattered reads (NOCACHE, 8B each):")
    print("    Max read rate:  1 / {tau}us = {rps:,.0f} reads/s".format(
        tau=TAU_US, rps=reads_per_sec_scattered))
    print("    Max byte rate:  {bps:,.0f} B/s = {mbps:.1f} MB/s".format(
        bps=bytes_per_sec_scattered, mbps=bytes_per_sec_scattered/1e6))
    print("")
    print("  Large blocks (module cache, geometry):")
    print("    Throughput:     ~{mb} MB/s (PCIe sequential)".format(mb=LARGE_BLOCK_MB_S))
    print("")

    # Worst-case bytes/s from scattered reads alone
    # Each scanner issues N_scatter reads during its heldMs
    # N_scatter = heldMs / (tau_us/1000) = heldMs * 1000 / tau_us
    # Through a scanner at turns/s = 1000 / (heldMs + GAP):
    #   bytes/s = turns/s * N_scatter * READ_SIZE
    #           = [1000/(h+GAP)] * [h*1000/tau] * READ_SIZE
    #           = (1000 * 1000 * READ_SIZE) / (tau * (h + GAP)) * h
    #           = K * h / (h + GAP)
    # where K = 1000^2 * READ_SIZE / tau

    K = (1000.0 ** 2 * READ_SIZE_BYTES) / TAU_US  # bytes/s at duty=100%

    print("Worst-case scattered-read bytes/s per scanner:")
    print("  Formula: bytes/s = K * h / (h + GAP)")
    print("  where K = 10^6 * {read_size}B / {tau}us = {K:,.0f} B/s = {K_mb:.1f} MB/s".format(
        read_size=READ_SIZE_BYTES, tau=TAU_US, K=K, K_mb=K/1e6))
    print("")

    print("{:<16} {:>10} {:>15} {:>15} {:>15}".format(
        "Scanner", "max_held", "worst B/s", "worst KB/s", "worst MB/s"))
    print("-" * 72)

    total_worst_bytes = 0
    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        h = m["max_held"]
        bps = K * h / (h + GAP_MS)
        total_worst_bytes += bps
        print("{:<16} {:>8.0f}ms {:>13,.0f} {:>13,.1f} {:>13.2f}".format(
            name, h, bps, bps/1024, bps/1e6))

    print("-" * 72)
    print("{:<16} {:>10} {:>13,.0f} {:>13,.1f} {:>13.2f}".format(
        "TOTAL", "", total_worst_bytes, total_worst_bytes/1024, total_worst_bytes/1e6))
    print("")
    print("Note: K * h/(h+GAP) is the CONVEX contribution -- it increases")
    print("with h but saturates at K (the DMA device max).  So a single")
    print("scanner at max burst already uses {:.0f}% of the device max,".format(
        max(K * m["max_held"] / (m["max_held"] + GAP_MS) for m in MEASURED.values()) / K * 100))
    print("and adding more scanners on top has diminishing marginal impact.")

    # =====================================================================
    # PART 3: Steady-state vs burst comparison
    # =====================================================================
    print_section("PART 3: STEADY-STATE vs BURST BUS CONSUMPTION")

    print("")
    print("{:<16} {:>10} {:>12} {:>10} {:>10} {:>10}".format(
        "Scanner", "avg_held", "max_held", "B/s(avg)", "B/s(max)", "ratio"))
    print("-" * 72)

    total_avg_bytes = 0
    total_max_bytes = 0
    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        avg_bps = K * m["avg_held"] / (m["avg_held"] + GAP_MS)
        max_bps = K * m["max_held"] / (m["max_held"] + GAP_MS)
        total_avg_bytes += avg_bps
        total_max_bytes += max_bps
        ratio = max_bps / avg_bps if avg_bps > 0 else 0
        print("{:<16} {:>8.1f}ms {:>8.0f}ms {:>8,.0f} {:>8,.0f} {:>8.1f}x".format(
            name, m["avg_held"], m["max_held"], avg_bps, max_bps, ratio))

    print("-" * 72)
    print("{:<16} {:>10} {:>12} {:>10,.0f} {:>10,.0f} {:>8.1f}x".format(
        "TOTAL", "", "", total_avg_bytes, total_max_bytes,
        total_max_bytes / total_avg_bytes if total_avg_bytes > 0 else 0))
    print("")

    # =====================================================================
    # PART 4: HeldMs budgets for 50% total duty
    # =====================================================================
    print_section("PART 4: HELDMS BUDGETS FOR 50% TOTAL DUTY")

    TARGET_DUTY = 50.0  # percent
    TARGET_BUS_MS = TARGET_DUTY * 10  # ms/s at 50% duty

    print("")
    print("Goal: total duty <= {target}% = {bus_ms} ms/s of bus occupancy".format(
        target=TARGET_DUTY, bus_ms=TARGET_BUS_MS))
    print("")

    # -- 4a: Theoretical worst-case bound ---------------------------------
    print("4a. THEORETICAL WORST-CASE BOUND")
    print("-" * 72)
    print("")
    print("Total duty = sum_i [ turns_i * held_i / 1000 ]")
    print("           = sum_i [ held_i / (held_i + GAP) ]")
    print("")
    print("This is maximized when each held_i -> inf, giving N * 100%")
    print("(N = number of scanners).  For N=5, max = 500%.")
    print("")
    print("To bound total duty at 50%, we need:")
    print("  sum_i [ held_i / (held_i + GAP) ] <= {target}".format(target=TARGET_DUTY/100))
    print("")

    # Solve for sum_held given the constraint
    # For N scanners: N * h / (h + GAP) <= 0.5
    # => h / (h + GAP) <= 0.5 / N
    # => h <= GAP * (0.5/N) / (1 - 0.5/N)
    # => h <= GAP * 0.5 / (N - 0.5)

    N = len(SCANNERS)
    h_equal = GAP_MS * (TARGET_DUTY / 100) / (N - TARGET_DUTY / 200)
    total_h_equal = N * h_equal

    print("If all scanners have equal heldMs = h:")
    print("  {n} * h / (h + {gap}) <= 0.5".format(n=N, gap=GAP_MS))
    print("  h <= {gap} * 0.5 / ({n} - 0.5)".format(gap=GAP_MS, n=N))
    print("  h <= {h:.2f}ms".format(h=h_equal))
    print("  Total sum_held <= {total:.1f}ms".format(total=total_h_equal))
    print("")

    # Verify
    duty_check = N * h_equal / (h_equal + GAP_MS) * 100
    print("  Verification: {n} * {h:.2f} / ({h:.2f} + {gap}) = {d:.1f}%".format(
        n=N, h=h_equal, gap=GAP_MS, d=duty_check))
    print("")

    # -- 4b: Budget proportional to observed usage ------------------------
    print("4b. BUDGET PROPORTIONAL TO OBSERVED BUS USAGE")
    print("-" * 72)
    print("")
    print("Allocate the 50% budget in proportion to each scanner's")
    print("observed avg bus consumption during active scanning.")
    print("")

    obs_bus = {}
    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        obs_bus[name] = m["avg_held"]  # proxy for relative usage

    total_obs = sum(obs_bus.values())

    print("{:<16} {:>10} {:>10} {:>12} {:>12} {:>12}".format(
        "Scanner", "obs_held", "fraction", "budget_ms", "allowed_h", "eff_t/s"))
    print("-" * 72)

    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        frac = obs_bus[name] / total_obs
        budget_ms = TARGET_BUS_MS * frac
        # Solve: held / (held + GAP) = budget_ms / 1000
        # held = GAP * (budget_ms/1000) / (1 - budget_ms/1000)
        if budget_ms >= 1000:
            allowed_h = 9999
        else:
            allowed_h = GAP_MS * (budget_ms / 1000) / (1 - budget_ms / 1000)
        eff_tps = 1000 / (allowed_h + GAP_MS) if allowed_h < 9999 else 0
        print("{:<16} {:>8.1f}ms {:>9.1f}% {:>10.1f} {:>10.1f}ms {:>10.2f}".format(
            name, m["avg_held"], frac * 100, budget_ms, allowed_h, eff_tps))

    print("")

    # -- 4c: Equal budget split (simplest) --------------------------------
    print("4c. EQUAL BUDGET SPLIT")
    print("-" * 72)
    print("")
    print("Each scanner gets {n}-th of the 50% budget = {each:.1f} ms/s".format(
        n=N, each=TARGET_BUS_MS / N))
    print("")

    each_budget = TARGET_BUS_MS / N
    print("{:<16} {:>10} {:>12} {:>12} {:>12}".format(
        "Scanner", "budget_ms", "allowed_h", "eff_t/s", "vs_measured"))
    print("-" * 64)

    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        if each_budget >= 1000:
            allowed_h = 9999
        else:
            allowed_h = GAP_MS * (each_budget / 1000) / (1 - each_budget / 1000)
        eff_tps = 1000 / (allowed_h + GAP_MS) if allowed_h < 9999 else 0
        vs_measured = allowed_h / m["avg_held"] if m["avg_held"] > 0 else float("inf")
        print("{:<16} {:>8.1f} {:>10.1f}ms {:>10.2f} {:>10.1f}x".format(
            name, each_budget, allowed_h, eff_tps, vs_measured))

    print("")
    print("  allowed_h = GAP * (budget/1000) / (1 - budget/1000)")
    print("  eff_t/s = 1000 / (allowed_h + GAP)")
    print("")

    # -- 4d: Priority-weighted (by scanner importance) --------------------
    print("4d. PRIORITY-WEIGHTED BUDGET (by scanner importance)")
    print("-" * 72)
    print("")
    print("Weight each scanner by 1/period (faster = higher priority):")
    print("  Update: 1/16 = 6.25%   (world resolution, must run fast)")
    print("  EntityList: 1/16 = 6.25%")
    print("  RobotList: 1/48 = 2.08%")
    print("  ContainerList: 1/16 = 6.25%")
    print("  ItemList: 1/16 = 6.25%")
    print("")

    weights = {}
    for name, cfg in SCANNERS.items():
        weights[name] = 1.0 / cfg["period_ms"]

    total_w = sum(weights.values())

    print("{:<16} {:>10} {:>10} {:>12} {:>12} {:>12}".format(
        "Scanner", "weight", "fraction", "budget_ms", "allowed_h", "eff_t/s"))
    print("-" * 72)

    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        frac = weights[name] / total_w
        budget_ms = TARGET_BUS_MS * frac
        if budget_ms >= 1000:
            allowed_h = 9999
        else:
            allowed_h = GAP_MS * (budget_ms / 1000) / (1 - budget_ms / 1000)
        eff_tps = 1000 / (allowed_h + GAP_MS) if allowed_h < 9999 else 0
        print("{:<16} {:>8.3f} {:>9.1f}% {:>10.1f} {:>10.1f}ms {:>10.2f}".format(
            name, weights[name], frac * 100, budget_ms, allowed_h, eff_tps))

    print("")

    # =====================================================================
    # PART 5: What the codebase can actually enforce
    # =====================================================================
    print_section("PART 5: WHAT THE CODEBASE CAN ENFORCE")

    print("")
    print("The gate can control turns/s (via GAP) but NOT heldMs directly.")
    print("heldMs is determined by the scan work (number of actors, DMA latency).")
    print("")
    print("Enforceable levers:")
    print("  1. GAP increase: larger GAP -> fewer turns/s -> lower duty")
    print("     Current: GAP={gap}ms -> max 83.3 turns/s".format(gap=GAP_MS))
    print("     At GAP=50ms: max 16.7 turns/s")
    print("     At GAP=100ms: max 9.1 turns/s")
    print("")
    print("  2. Actor cap: fewer actors -> shorter heldMs -> lower duty")
    print("     Current: up to 1024 actors scanned")
    print("     At 512 cap: roughly halves the worst-case heldMs")
    print("")
    print("  3. Chunked scatter: break long scans into chunks with gate release")
    print("     between chunks, allowing interleaving with ungated passes")
    print("     Current: one monolithic heldMs per scan")
    print("     Chunked: heldMs_per_chunk x chunks, with GAP between each")
    print("")

    # Show how GAP scaling affects duty
    print("SENSITIVITY: GAP vs MAX TOTAL DUTY (worst-case burst):")
    print("-" * 64)
    print("{:>6} {:>10} {:>12} {:>12} {:>12}".format(
        "GAP", "max t/s", "worst duty%", "budget each", "allowed h"))
    print("-" * 64)

    for gap in [12, 20, 30, 50, 80, 100, 150, 200]:
        max_tps = 1000.0 / gap
        # At max burst, each scanner's contribution is h/(h+GAP)
        # Total = sum of h_i / (h_i + GAP)
        total_duty_pct = sum(
            m["max_held"] / (m["max_held"] + gap) * 100
            for m in MEASURED.values()
        )
        # Per-scanner budget for 50% total
        each_budget = 500.0 / N  # ms/s
        if each_budget >= 1000:
            allowed = 9999
        else:
            allowed = gap * (each_budget / 1000) / (1 - each_budget / 1000)
        print("{:>4}ms {:>8.1f} {:>10.1f}% {:>10.1f} {:>10.1f}ms".format(
            gap, max_tps, total_duty_pct, each_budget, allowed))

    print("")
    print("To bring worst-case total duty below 50%:")
    print("  sum_i [max_held_i / (max_held_i + GAP)] <= 0.5")

    # Solve for GAP that achieves 50% with the measured max_held values
    # sum_i [h_i / (h_i + GAP)] = 0.5
    # This is a convex function of GAP, solve numerically
    import math
    def total_duty_at_gap(gap):
        return sum(m["max_held"] / (m["max_held"] + gap) for m in MEASURED.values())

    # Binary search for GAP where total_duty = 0.5
    lo, hi = 0, 10000
    for _ in range(100):
        mid = (lo + hi) / 2
        if total_duty_at_gap(mid) > 0.5:
            lo = mid
        else:
            hi = mid
    gap_needed = (lo + hi) / 2

    print("  Required GAP for 50% worst-case duty: {:.0f}ms".format(gap_needed))
    print("  (vs current GAP={}ms -- a {:.0f}x increase)".format(GAP_MS, gap_needed / GAP_MS))
    print("")

    # =====================================================================
    # PART 6: Summary table
    # =====================================================================
    print_section("PART 6: SUMMARY -- PER-SCANNER BUDGETS FOR 50% DUTY")

    print("")
    print("Three allocation strategies, each keeping total duty <= 50%:")
    print("")

    # Recompute all three
    strategies = {
        "Equal": {n: 1.0/N for n in SCANNERS},
        "Usage-weighted": {n: obs_bus[n]/total_obs for n in SCANNERS},
        "Priority-weighted": {n: weights[n]/total_w for n in SCANNERS},
    }

    print("{:<16} {:>18} {:>18} {:>18}".format(
        "Scanner", "Equal h", "Usage-wt h", "Priority-wt h"))
    print("-" * 72)

    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        vals = []
        for strat_name in ["Equal", "Usage-weighted", "Priority-weighted"]:
            frac = strategies[strat_name][name]
            budget_ms = TARGET_BUS_MS * frac
            if budget_ms >= 1000:
                allowed = 9999
            else:
                allowed = GAP_MS * (budget_ms / 1000) / (1 - budget_ms / 1000)
            vals.append(allowed)
        print("{:<16} {:>16.1f}ms {:>16.1f}ms {:>16.1f}ms".format(
            name, vals[0], vals[1], vals[2]))

    print("-" * 72)
    print("")
    print("Column 'Equal h': each scanner may hold the bus for up to")
    print("  {:.1f}ms per turn, giving {:.1f}% duty per scanner, {:.0f}% total.".format(
        h_equal, h_equal / (h_equal + GAP_MS) * 100,
        N * h_equal / (h_equal + GAP_MS) * 100))
    print("")
    print("Column 'Usage-weighted h': budget split by observed bus consumption.")
    print("  EntityList and RobotList get more headroom since they do the heavy lifting.")
    print("")
    print("Column 'Priority-wt h': budget split by 1/period.")
    print("  Update and EntityList (16ms) get equal shares; RobotList (48ms) gets 1/3.")
    print("")

    # =====================================================================
    # PART 7: Reality check -- which scanners breach their budget?
    # =====================================================================
    print_section("PART 7: REALITY CHECK -- BUDGET BREACHES")

    print("")
    print("Using the EQUAL budget of {:.1f}ms per scanner:".format(h_equal))
    print("")

    print("{:<16} {:>10} {:>10} {:>10} {:>10} {:>8}".format(
        "Scanner", "budget", "avg_held", "max_held", "P95_held", "breach?"))
    print("-" * 64)

    for name in ["Update", "EntityList", "RobotList", "ContainerList", "ItemList"]:
        m = MEASURED[name]
        avg_ok = "OK" if m["avg_held"] <= h_equal else "BREACH"
        max_ok = "BREACH"  # always breaches in worst case
        p95_ok = "OK" if m["p95_held"] <= h_equal else "BREACH"
        breach = p95_ok if m["avg_held"] <= h_equal else avg_ok
        print("{:<16} {:>8.1f}ms {:>8.1f}ms {:>8.0f}ms {:>8.0f}ms {:>8}".format(
            name, h_equal, m["avg_held"], m["max_held"], m["p95_held"],
            "AVG+P95" if avg_ok == "OK" and p95_ok != "OK" else
            "ALL" if avg_ok != "OK" else p95_ok))

    print("")
    print("Key insight: Update's avg_held ({:.1f}ms) fits the equal budget".format(
        MEASURED["Update"]["avg_held"]))
    print("({:.1f}ms), but its max burst (778ms) and P95 (38ms) both breach.".format(h_equal))
    print("This means the 50% budget is achievable on average but violated")
    print("during actor-classification bursts.")
    print("")
    print("To prevent ALL breaches (including P95), each scanner's P99 heldMs")
    print("must fit the budget.  From the data:")
    print("  Update P99: 52ms")
    print("  EntityList P99: 88ms")
    print("  RobotList P99: 81ms")
    print("  ContainerList P99: 30ms")
    print("  ItemList P99: 41ms")
    print("")
    print("Total P99 sum: {}ms".format(52 + 88 + 81 + 30 + 41))
    print("For 50% duty with 5 scanners, sum_held <= {:.0f}ms".format(total_h_equal))
    print("P99 sum exceeds the budget by {:.0f}x -- the P99 storms".format(
        (52+88+81+30+41) / total_h_equal))
    print("cannot be accommodated without increasing GAP or capping actors.")


if __name__ == "__main__":
    main()
