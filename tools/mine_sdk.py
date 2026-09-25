#!/usr/bin/env python3
"""Mine the FrostDumper SDK text dump (sdk/SDK.txt).

Usage:
  python tools/mine_sdk.py meta
  python tools/mine_sdk.py globals
  python tools/mine_sdk.py show <ClassName> [--members a,b,c]
  python tools/mine_sdk.py grep <regex> [--limit N]
  python tools/mine_sdk.py offsets            # every "name = 0x..." in ARC::Globals
"""
import re
import sys

SDK = "sdk/SDK.txt"


def load():
    with open(SDK, "r", errors="replace") as fh:
        return fh.read().split("\n")


def meta(lines):
    for l in lines[:40]:
        print(l.rstrip())


def section_span(lines, kind):
    """Return (start, end) line indices for `namespace <kind> {`."""
    start = end = None
    for i, l in enumerate(lines):
        if l.startswith("namespace %s " % kind) or l.startswith("namespace %s{" % kind):
            start = i
        elif start is not None and l.startswith("} // namespace %s" % kind):
            end = i
            break
    return start, end


def globals_(lines):
    s, e = section_span(lines, "Globals")
    if s is None:
        print("no ARC::Globals section")
        return
    print("ARC::Globals spans lines %d..%d" % (s, e))
    for l in lines[s:min(e + 1, s + 400) if e else s + 400]:
        print(l.rstrip())


CLASS_RE = re.compile(
    r"^(?:class|struct)\s+(?:ARC::(?:Classes|Structs)::)?(\w+)\s*(?::|$|\s*\{)")


def find_def(lines, name):
    """Find a class/struct definition named `name`; return (start, end)."""
    pat = re.compile(r"^(?:class|struct)\s+\w*\b%s\b\s*(?::|$|\s*\{)" % re.escape(name))
    for i, l in enumerate(lines):
        if pat.match(l):
            depth = 0
            started = False
            for j in range(i, min(i + 4000, len(lines))):
                depth += lines[j].count("{") - lines[j].count("}")
                if "{" in lines[j]:
                    started = True
                if started and depth <= 0:
                    return i, j
            return i, min(i + 200, len(lines))
    return None


def show(lines, name, members=None):
    span = find_def(lines, name)
    if not span:
        print("NOT FOUND: %s" % name)
        return
    s, e = span
    print("=== %s (lines %d..%d) ===" % (name, s, e))
    if members:
        want = [m.lower() for m in members]
        for l in lines[s:e + 1]:
            low = l.lower()
            if any(w in low for w in want) or l.strip().startswith("// 0x"):
                print(l.rstrip()[:200])
    else:
        for l in lines[s:e + 1]:
            print(l.rstrip()[:200])


def grep(lines, pattern, limit=60):
    rx = re.compile(pattern)
    n = 0
    for i, l in enumerate(lines):
        if rx.search(l):
            print("%7d: %s" % (i, l.rstrip()[:200]))
            n += 1
            if n >= limit:
                return


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    lines = load()
    cmd = args[0]
    if cmd == "meta":
        meta(lines)
    elif cmd == "globals":
        globals_(lines)
    elif cmd == "show":
        members = None
        if "--members" in args:
            members = args[args.index("--members") + 1].split(",")
        show(lines, args[1], members)
    elif cmd == "grep":
        limit = 60
        if "--limit" in args:
            limit = int(args[args.index("--limit") + 1])
        grep(lines, args[1], limit)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
