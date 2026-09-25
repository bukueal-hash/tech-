#!/usr/bin/env python3
"""Cross-check the shipped offsets against the Dumper-7 dump in `sdk/bigger sdk/`.

The big folder is a second, independent dump of the same 2026-09-22 build, made
with Dumper-7 where tools/sdk_index.py indexes a FrostDumper text dump. Two
dumpers agreeing is real evidence; two dumpers disagreeing is a bug in one of
them. Everything is read by content and never modified:

  Dumpspace/OffsetsInfo.json   data globals (OFFSET_GOBJECTS, OFFSET_GWORLD, ...)
  Dumpspace/ClassesInfo.json   class sizes + full inheritance chains
  Dumpspace/StructsInfo.json   struct sizes + chains
  CppSDK/SDK/Basic.hpp         Dumper-7's own Offsets namespace (the globals again)
  CppSDK/SDK/*_classes.hpp     real (non-Pad) member offsets of native classes

Every Angelscript class is `uint8 Pad_D0[0x180]` in CppSDK, so game-class
members stay the FrostDumper index's job. What this dump adds is the native
reflection layout (UStruct/UClass/UField members), a size for every one of the
27k classes and structs, and a second opinion on the data globals. Enum values
are NOT in the dump (EnumsInfo is names only) and are never guessed here.

Usage:
  python tools/sdk7_index.py check           # verify Offsets.h; exit 1 on drift
  python tools/sdk7_index.py globals         # the data globals, both sides
  python tools/sdk7_index.py natives         # every real CppSDK member
  python tools/sdk7_index.py type UItemBase  # size + inheritance chain
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
D7 = os.path.join(ROOT, "sdk", "bigger sdk")
DUMPSPACE = os.path.join(D7, "Dumpspace")
CPPSDK = os.path.join(D7, "CppSDK", "SDK")
OFFSETS = os.path.join(ROOT, "Project", "Core", "Offsets.h")
SDK_HPP = os.path.join(ROOT, "Project", "Core", "SDK.hpp")
INDEX = os.path.join(ROOT, "tools", "sdk_index.json")

# Offsets.h constant -> the Dumper-7 name for the same quantity. The dumper and
# the drop rename each other's symbols freely, so every pairing below was read
# and agreed on by hand; nothing is matched by a fuzzy name algorithm. A row
# the dump has no view of is skipped and reported as such, never judged.
CHECKS = [
    ("GObjectsRva", "global", "OFFSET_GOBJECTS"),
    ("GNamePoolRva", "global", "OFFSET_GNAMES"),
    ("UWorld", "global", "OFFSET_GWORLD"),
    ("ProcessEventRva", "global", "OFFSET_PROCESSEVENT"),
    ("ProcessEventIndex", "global", "INDEX_PROCESSEVENT"),
    ("UStruct_SuperStruct", "native", "UStruct.SuperStruct"),
    ("UStruct_PropertyLink", "native", "UStruct.Children"),
    ("UClass_DefaultObjectSlot", "native", "UClass.ClassDefaultObject"),
    # no row for FProperty_PropertyLinkNext: Dumper-7 cannot see the FField
    # chain and writes UField::Next as a 0x0000 placeholder next to its own
    # Pad_8[0x98]. The reflect dump's 0x98 is what ships and what the walk
    # tests pin - a placeholder is not a disagreement.
]

# Basic.hpp keeps its own copy of the globals - same numbers must hold there.
BASIC_GLOBALS = [
    ("GObjects", "OFFSET_GOBJECTS"),
    ("GNames", "OFFSET_GNAMES"),
    ("GWorld", "OFFSET_GWORLD"),
    ("ProcessEvent", "OFFSET_PROCESSEVENT"),
    ("ProcessEventIdx", "INDEX_PROCESSEVENT"),
]

MEMBER = re.compile(
    r"^\s+[\w:<>,\s\*&]+?\s+(\w+)\s*;\s*//\s*(0x[0-9A-Fa-f]+)\((0x[0-9A-Fa-f]+)\)"
    r"(.*)$")
# `struct alignas(0x08) FVector` and `ABP_X::FGenerated final : public FBase`
# both come through Dumper-7 - the type name is the identifier after the
# optional alignas() and before any `::` path.
TYPE = re.compile(r"^(?:class|struct)\s+(?:alignas\([^)]*\)\s+)?([\w:]+)")
BASIC = re.compile(r"constexpr\s+\w+\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
# Dumper-7 hand-writes a few engine-internal fields it cannot reflect
# (`UField::Next`, `Basic.hpp`'s GNames) and tags them like this. The number
# next to the tag is its own struct guess, not a reflected offset - evidence
# only ever comes from properties the game itself reflects.
HAND_PLACED = "NOT AUTO-GENERATED PROPERTY"


def load_globals():
    """{name: value} from Dumpspace/OffsetsInfo.json (ints only)."""
    path = os.path.join(DUMPSPACE, "OffsetsInfo.json")
    if not os.path.exists(path):
        raise SystemExit("sdk/bigger sdk/Dumpspace not present - sdk7 check skipped")
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)["data"]
    return {name: value for name, value in data if isinstance(value, int)}


def load_types():
    """{Type: {kind, size, inherits}} from ClassesInfo + StructsInfo."""
    out = {}
    for fname, kind in (("ClassesInfo.json", "class"), ("StructsInfo.json", "struct")):
        with open(os.path.join(DUMPSPACE, fname), encoding="utf-8") as fh:
            for entry in json.load(fh)["data"]:
                for name, info in entry.items():
                    out[name] = {
                        "kind": kind,
                        "size": info[1].get("__MDKClassSize"),
                        "inherits": info[0].get("__InheritInfo", []),
                    }
    return out


def load_basic():
    """{name: value} for the constexprs in CppSDK/SDK/Basic.hpp."""
    out = {}
    path = os.path.join(CPPSDK, "Basic.hpp")
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for name, value in BASIC.findall(fh.read()):
            out[name] = int(value, 0)
    return out


def load_natives():
    """Real (non-Pad) CppSDK members: {Type: {member: (offset, size)}}.

    Dumper-7 pads every Angelscript class out, so the members it actually names
    are the native engine internals - precisely the reflection layout that no
    reflected property dump can see twice. This is where UStruct::SuperStruct
    and UClass::ClassDefaultObject get their independent confirmation.
    """
    out = {}
    paths = sorted(glob.glob(os.path.join(CPPSDK, "*_classes.hpp"))
                   + glob.glob(os.path.join(CPPSDK, "*_structs.hpp")))
    for path in paths:
        cur = None
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "// 0x" not in line and not line.startswith(("class", "struct", "};")):
                    continue
                m = TYPE.match(line)
                if m:
                    cur = m.group(1).split("::")[-1]
                    continue
                if line.startswith("};"):
                    cur = None
                    continue
                m = MEMBER.match(line)
                if m and cur and not m.group(1).startswith("Pad_"):
                    out.setdefault(cur, {})[m.group(1)] = (
                        int(m.group(2), 16), int(m.group(3), 16),
                        HAND_PLACED in m.group(4))
    return out


def load():
    return {"globals": load_globals(), "types": load_types(),
            "basic": load_basic(), "natives": load_natives()}


def short(name):
    """Dumper-7 spells `UStruct`/`FVector2D`/`APickup`; the reflect dump drops
    the marker letter (`CoreUObject.Struct`). Strip it for comparison."""
    if len(name) > 1 and name[0] in "UAF" and name[1].isupper():
        return name[1:]
    return name


def resolve_offsets(path=OFFSETS):
    """Offsets.h constant -> (value, source). Literals resolve directly, the
    `game::offsets::X` re-exports through Project/Core/SDK.hpp."""
    text = open(path, encoding="utf-8").read()
    sdk = open(SDK_HPP, encoding="utf-8").read() if os.path.exists(SDK_HPP) else ""

    def sdk_value(name):
        mm = re.search(r"\b%s\s*=\s*(0x[0-9A-Fa-f]+)" % re.escape(name), sdk)
        return int(mm.group(1), 16) if mm else None

    out = {}
    for line in text.splitlines():
        mm = re.match(r"\s*constexpr\s+\S+\s+(\w+)\s*=", line)
        if not mm:
            continue
        lit = re.search(r"=\s*(0x[0-9A-Fa-f]+)", line)
        if lit:
            out[mm.group(1)] = (int(lit.group(1), 16), "literal")
            continue
        ref = re.search(r"game::offsets::(\w+)", line)
        if ref:
            out[mm.group(1)] = (sdk_value(ref.group(1)),
                                "game::offsets::%s" % ref.group(1))
        else:
            out[mm.group(1)] = (None, "expr")
    return out


def shipped_classes():
    """Classes named in a gen_offsets spec - the only ones a shipped constant
    reads from, so the only ones where a size disagreement is actionable."""
    import gen_offsets as go
    out = set()
    for _title, entries in go.SECTIONS:
        for _name, _ctype, spec, _note in entries:
            kind, _, arg = spec.partition(":")
            if kind in ("prop", "maskOf", "sizeof"):
                cls = arg.rpartition(".")[0] if kind != "sizeof" else arg
                out.add(cls)
    return out


def sweep_sizes(db):
    """Every FrostDumper class/struct size against Dumper-7's __MDKClassSize."""
    with open(INDEX, encoding="utf-8") as fh:
        idx = json.load(fh)
    by_short = {}
    for name, t in db["types"].items():
        by_short.setdefault(short(name), []).append((name, t))
    agree, missing, rows = 0, 0, []
    for bucket in ("classes", "structs"):
        for key, entry in sorted(idx.get(bucket, {}).items()):
            want = entry.get("size")
            if not want:
                continue
            cands = by_short.get(short(key.rsplit(".", 1)[-1]), [])
            if not cands:
                missing += 1
            elif any(t["size"] == want for _n, t in cands):
                agree += 1
            else:
                rows.append((key, want, [(n, t["size"]) for n, t in cands]))
    return agree, missing, rows


def sweep_inherits(db):
    """The reflect dump names one parent, Dumper-7 the whole chain. Compare the
    nearest one; report-only because the two name families differ on purpose."""
    with open(INDEX, encoding="utf-8") as fh:
        idx = json.load(fh)
    by_short = {}
    for name, t in db["types"].items():
        by_short.setdefault(short(name), []).append((name, t))
    rows = []
    for bucket in ("classes", "structs"):
        for key, entry in sorted(idx.get(bucket, {}).items()):
            want = entry.get("inherits")
            if not want or want == "None":
                continue
            cands = by_short.get(short(key.rsplit(".", 1)[-1]), [])
            chains = [t["inherits"] for _n, t in cands if t["inherits"]]
            # reflect names already lose the U/A/F marker ("AIController" is
            # Dumper-7's "AAIController"), so only strip the Dumper-7 side -
            # stripping both double-chops names like AIController.
            if chains and not any(chain and short(chain[0]) == want
                                  for chain in chains):
                rows.append((key, want,
                             [chain[0] for chain in chains if chain]))
    return rows


def check(db, quiet=False):
    """Compare Offsets.h (and the reflect index) against the dump.

    Returns the number of mismatches - anything non-zero fails the
    gen_offsets --check gate exactly like the idmap reconciliation does.
    """
    values = resolve_offsets()
    bad = 0
    n_native = sum(len(v) for v in db["natives"].values())
    print("sdk7: %d globals, %d types, %d native members (Dumper-7, 2026-09-22)"
          % (len(db["globals"]), len(db["types"]), n_native))
    print("%-28s %-14s %-14s %s" % ("constant", "Offsets.h", "dumper-7", "status"))
    for const, kind, key in CHECKS:
        if kind == "global":
            want, where = db["globals"].get(key), "OffsetsInfo " + key
        else:
            cls, _, member = key.partition(".")
            hit = db["natives"].get(cls, {}).get(member)
            want, where = (hit[0] if hit else None), "CppSDK %s::%s" % (cls, member)
            if hit and hit[2]:
                where += " (hand-placed)"
        if want is None:
            print("%-28s %-14s %-14s no dump view (skipped)" % (const, "-", "-"))
            continue
        got, src = values.get(const, (None, "absent"))
        ok = got == want
        if not ok:
            bad += 1
        print("%-28s %-14s %-14s %-8s %s" % (
            const, "0x%X" % got if got is not None else "-",
            "0x%X" % want, "ok" if ok else "MISMATCH", where))

    # the dump must not disagree with itself: Basic.hpp vs OffsetsInfo
    for name, key in BASIC_GLOBALS:
        want, got = db["globals"].get(key), db["basic"].get(name)
        if want is not None and got is not None and got != want:
            bad += 1
            print("Basic.hpp %-18s 0x%-12X 0x%-12X MISMATCH (dump self-conflict)"
                  % (name, got, want))

    needed = shipped_classes()
    agree, missing, rows = sweep_sizes(db)
    for key, want, cands in rows:
        # a size disagreement only fails the gate for a class an Offsets.h
        # constant actually depends on; elsewhere the two dumpers are known
        # to round trailing padding differently (Transient.PropertyBag_*).
        used = key in needed or key.rsplit(".", 1)[-1] in needed
        if used:
            bad += 1
        print("size %-23s reflect=0x%X dumper-7=%s %s"
              % (key, want, ", ".join("%s=0x%X" % c for c in cands)[:48],
                 "MISMATCH" if used else "(report-only: no shipped constant)"))
    print("sizes: %d agree, %d mismatch, %d not in the dumper-7 dump"
          % (agree, len(rows), missing))

    for key, want, chains in sweep_inherits(db):
        print("inherits %-20s reflect=%s dumper-7=%s (report-only)"
              % (key, want, ", ".join(chains)[:48]))

    print("sdk7: %d mismatch(es)" % bad)
    return bad


def main():
    db = load()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        return 1 if check(db) else 0
    if cmd == "globals":
        print("%-22s %-14s %s" % ("symbol", "RVA", "Basic.hpp"))
        for name, key in BASIC_GLOBALS:
            print("%-22s 0x%-12X 0x%X" % (key, db["globals"].get(key, 0),
                                          db["basic"].get(name, 0)))
        return 0
    if cmd == "natives":
        for cls in sorted(db["natives"]):
            for member, (off, size, hand) in sorted(db["natives"][cls].items()):
                print("%-12s %-28s 0x%04X (0x%X)%s"
                      % (cls, member, off, size,
                         "  [hand-placed, not reflected]" if hand else ""))
        return 0
    if cmd == "type" and len(sys.argv) > 2:
        for name, t in sorted(db["types"].items()):
            if sys.argv[2].lower() in name.lower():
                print("%-40s %-7s size=%-6s inherits=%s"
                      % (name, t["kind"], t["size"], " <- ".join(t["inherits"])))
        return 0
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
