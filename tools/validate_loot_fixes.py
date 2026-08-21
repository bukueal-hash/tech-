#!/usr/bin/env python3
"""
validate_loot_fixes.py - Compare pre-fix baseline vs post-fix debug log
for the three loot/container fixes:
  1. FinalizeWorldCacheMap uncached DMA -> container_open_batch hit rate
  2. ContainerList ring reset thrashing -> softmiss count, memo_lockout
  3. ItemList identityProven gate -> item_posgate_drop count + false positive check

Usage:
  python tools/validate_loot_fixes.py debug-c190fb.log [--baseline BASELINE_LOG]

If --baseline is not given, uses hardcoded baseline numbers from the pre-fix session.
"""

import argparse
import json
import sys
from collections import Counter


def parse_container_ring(path):
    ring_n = []
    ring_ms = []
    softmiss = 0
    softmiss_detail = Counter()
    open_batch = []
    open_flip = 0
    memo_lockout = 0
    admit_ring = 0

    with open(path, "r", errors="ignore") as f:
        for line in f:
            try:
                j = json.loads(line.strip())
                msg = j.get("message", "")
                loc = j.get("location", "")
                d = j.get("data", {})
            except Exception:
                continue

            if "ContainerList" not in loc:
                continue

            if msg == "container_admit_ring":
                admit_ring += 1
                ring_n.append(d.get("n", 0))
                ring_ms.append(d.get("cycleMs", 0))
            elif msg == "container_verify_softmiss":
                softmiss += 1
                reason = d.get("reason", "?")
                softmiss_detail[reason] += 1
            elif msg == "container_open_batch":
                open_batch.append(d)
            elif msg == "container_open_flip":
                open_flip += 1
            elif msg == "container_memo_lockout":
                memo_lockout += 1

    probed = [d.get("probed", 0) for d in open_batch]
    hits = [d.get("hits", 0) for d in open_batch]
    total_probed = sum(probed)
    total_hits = sum(hits)
    hit_rate = total_hits / max(1, total_probed) * 100

    return {
        "admit_ring": admit_ring,
        "softmiss": softmiss,
        "softmiss_detail": softmiss_detail,
        "open_batch_n": len(open_batch),
        "total_probed": total_probed,
        "total_hits": total_hits,
        "hit_rate": hit_rate,
        "open_flip": open_flip,
        "memo_lockout": memo_lockout,
        "ring_cycle_avg_ms": sum(ring_ms) / max(1, len(ring_ms)),
        "ring_cycle_max_ms": max(ring_ms) if ring_ms else 0,
    }


def parse_item_posgate(path):
    posgate = Counter()
    fp_keywords = [
        "filing cabinet", "kitchen bench", "bench", "table", "chair", "desk",
        "shelf", "door", "vent", "pipe", "panel", "button", "wall", "floor",
        "ceiling", "stair", "fence", "gate", "pillar", "column", "beam",
        "antenna", "scatter", "constructable", "backdrop", "decal",
        "landscape streaming", "refrigerator", "fridge",
    ]

    with open(path, "r", errors="ignore") as f:
        for line in f:
            try:
                j = json.loads(line.strip())
                if "item_posgate_drop" in j.get("message", ""):
                    d = j.get("data", {})
                    lbl = d.get("label", "?")
                    posgate[lbl] += 1
            except Exception:
                continue

    total = sum(posgate.values())
    false_positives = []
    for lbl, cnt in posgate.items():
        low = lbl.lower()
        for kw in fp_keywords:
            if kw in low:
                false_positives.append((lbl, cnt, kw))
                break

    return {
        "total": total,
        "labels": posgate,
        "false_positives": false_positives,
    }


def compare(label, baseline_val, post_val, better="lower"):
    b = baseline_val
    p = post_val
    if b == 0 and p == 0:
        return "  %-30s  baseline=%s  post=%s  (both zero)" % (label, b, p)
    if b == 0:
        return "  %-30s  baseline=%s  post=%s  (NEW)" % (label, b, p)
    pct = (p - b) / b * 100
    if better == "lower":
        status = "OK" if p <= b else "REGRESSION"
    else:
        status = "OK" if p >= b else "REGRESSION"
    return "  %-30s  baseline=%s  post=%s  (%+.1f%%)  [%s]" % (label, b, p, pct, status)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logfile", help="Post-fix debug log")
    parser.add_argument("--baseline", help="Pre-fix baseline log (optional)")
    args = parser.parse_args()

    post_container = parse_container_ring(args.logfile)
    post_item = parse_item_posgate(args.logfile)

    if args.baseline:
        base_container = parse_container_ring(args.baseline)
        base_item = parse_item_posgate(args.baseline)
    else:
        # Hardcoded from debug-c190fb.log pre-fix session
        base_container = {
            "admit_ring": 4600,
            "softmiss": 23823,
            "open_batch_n": 3808,
            "total_probed": int(3808 * 3.3),
            "total_hits": 0,
            "hit_rate": 0.0,
            "open_flip": 172,
            "memo_lockout": 25,
            "ring_cycle_avg_ms": 9721,
            "ring_cycle_max_ms": 126128,
        }
        base_item = {
            "total": 22,
            "labels": Counter({
                "Arpeggio": 4, "Il Toro": 2, "Rascal": 2, "Grenade": 2,
                "Mat Junk": 2, "Renegade": 2, "Osprey": 2,
                "Actor Armorpack": 1, "Actor Basic Sticky Grenade": 1,
                "Actor Consumable Shield Over Time Pack": 1,
                "Ferro": 1, "Hullcracker": 1, "Actor Homing Missile Grenade": 1,
            }),
            "false_positives": [],
        }

    sep = "=" * 70
    print(sep)
    print("  FIX VALIDATION: Loot / Container / Identity")
    print(sep)

    # -- Fix 1: ContainerLootLooksOpened DMA cache --
    print("\n-- Fix 1: ContainerLootLooksOpened DMA cache --")
    print(compare("open_batch_n", base_container["open_batch_n"], post_container["open_batch_n"]))
    print(compare("total_probed", base_container["total_probed"], post_container["total_probed"]))
    print(compare("total_hits", base_container["total_hits"], post_container["total_hits"], better="higher"))
    print("  hit rate:  baseline=%.1f%%  post=%.1f%%" % (
        base_container["hit_rate"], post_container["hit_rate"]))
    print(compare("open_flip", base_container["open_flip"], post_container["open_flip"]))

    # -- Fix 2: ContainerList ring reset thrashing --
    print("\n-- Fix 2: ContainerList ring reset thrashing --")
    print(compare("softmiss", base_container["softmiss"], post_container["softmiss"]))
    print(compare("memo_lockout", base_container["memo_lockout"], post_container["memo_lockout"]))
    print(compare("ring_cycle_avg_ms", base_container["ring_cycle_avg_ms"], post_container["ring_cycle_avg_ms"]))
    print("  ring_cycle_max_ms: baseline=%.0f  post=%.0f" % (
        base_container["ring_cycle_max_ms"], post_container["ring_cycle_max_ms"]))

    # -- Fix 3: ItemList identityProven + furniture deny-list --
    print("\n-- Fix 3: ItemList identityProven + furniture deny-list --")
    print(compare("total", base_item["total"], post_item["total"]))
    if post_item["total"] > 0:
        print("\n  Remaining dropped labels:")
        for lbl, cnt in post_item["labels"].most_common(10):
            print("    %4d  %s" % (cnt, lbl))
    if post_item["false_positives"]:
        print("\n  WARNING: Furniture/prop false positives still admitted:")
        for lbl, cnt, kw in post_item["false_positives"]:
            print("    %4d  %s  (matched: %s)" % (cnt, lbl, kw))
    else:
        print("  No furniture/prop false positives in dropped labels.")

    # -- Pass/Fail --
    print("\n" + sep)
    print("  PASS CRITERIA")
    print(sep)

    checks = []

    # Fix 1
    if post_container["hit_rate"] > 0:
        checks.append(("Fix 1: Cache hit rate > 0%", True))
    else:
        checks.append(("Fix 1: Cache hit rate > 0% (cache may not have warmed yet)", False))

    # Fix 2
    if post_container["softmiss"] < base_container["softmiss"] * 0.5:
        checks.append(("Fix 2: Softmiss < 50%% of baseline", True))
    else:
        checks.append(("Fix 2: Softmiss < 50%% of baseline", False))

    if post_container["memo_lockout"] <= base_container["memo_lockout"]:
        checks.append(("Fix 2: Memo lockout <= baseline", True))
    else:
        checks.append(("Fix 2: Memo lockout <= baseline", False))

    # Fix 3
    if post_item["total"] < base_item["total"] * 0.5:
        checks.append(("Fix 3: Posgate drops < 50%% of baseline", True))
    else:
        checks.append(("Fix 3: Posgate drops < 50%% of baseline", False))

    if not post_item["false_positives"]:
        checks.append(("Fix 3: No furniture false positives", True))
    else:
        checks.append(("Fix 3: No furniture false positives", False))

    all_pass = True
    for desc, ok in checks:
        status = "PASS" if ok else "FAIL"
        print("  [%s]  %s" % (status, desc))
        if not ok:
            all_pass = False

    print("\n" + "=" * 50)
    if all_pass:
        print("  ALL CHECKS PASSED")
    else:
        print("  SOME CHECKS FAILED")
    print("=" * 50)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
