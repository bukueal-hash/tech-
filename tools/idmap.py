#!/usr/bin/env python3
"""Parse the IDA map that ships with the dump: sdk/IDAMappings/*.idmap.

Format (reverse-engineered; the file is binary and undocumented):

  header    0x00..0x32   u24 serial, then (count:u32, fileOffset:u32)[] section
                         descriptors, running up to where the name blob starts
  names     0x33..       [u16 len][ascii chars]*  -- one blob; a record's name
                         field is a 0-based offset into it:
                             file_off = NAME_BASE + ref
  sections  count x 12   (u32 value, u32 refA, u32 refB)
                         value is module-relative (preferred base 0x140000000)
                         refA/refB are name refs, 0 or 0xFFFFFFFF for "none"

Sections found on this drop:

  4     records   data globals  -- (RVA, type name, symbol name):
                  GObjects, GNames, GWorld, UObject::ProcessEvent
  5969  records   class vtables -- (vtable RVA, none, "<Class>_VFT")
  9460  records   functions     -- (stub RVA, mangled/related, demangled)
  17/8  records   FProperty flag-bit and class-bit tables (in the name gap)

Usage:
  python tools/idmap.py globals             # the global data symbols
  python tools/idmap.py sections            # every section
  python tools/idmap.py lookup GWorld       # by symbol name (wildcards ok)
  python tools/idmap.py lookup AActor::*
  python tools/idmap.py lookup UObject_VFT
  python tools/idmap.py check               # reconcile with Core/Offsets.h
"""
import argparse
import glob
import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NAME_BASE = 0x33           # file offset where the name blob starts
HDR_BASE = 0x1B            # file offset of the (count, fileOff) descriptor list
IMG_SIZE = 0x13da8000      # SizeOfImage from the dump header
IMAGE_BASE = 0x140000000   # preferred base of PioneerGame.exe
REC_SIZE = 12
NULL_REFS = (0, 0xFFFFFFFF)

# constant name in Offsets.h -> symbol name in the idmap
OFFSETS_SYMBOLS = {
    "UWorld": "GWorld",
    "GNamePoolRva": "GNames",
    "GObjectsRva": "GObjects",
    "ProcessEventRva": "UObject::ProcessEvent",
}


def find_file(explicit=None):
    if explicit:
        return explicit
    hits = sorted(glob.glob(os.path.join(ROOT, "sdk", "IDAMappings", "*.idmap")))
    if not hits:
        # the Dumper-7 dump keeps its map one level deeper (sdk/bigger sdk/)
        hits = sorted(glob.glob(
            os.path.join(ROOT, "sdk", "*", "IDAMappings", "*.idmap")))
    if not hits:
        raise SystemExit("no sdk/IDAMappings/*.idmap found")
    return hits[0]


class Idmap:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.d = fh.read()
        self.path = path
        self.names = {}          # name -> [ref, ...]
        self.sections = []       # (fileOff, count, kind)
        self.records = []        # (value, refA name, refB name, fileOff, section)
        self.header_sections = self._header_sections()
        self._find_sections()
        self._read_records()

    # ── names ────────────────────────────────────────────────────────────
    def name(self, ref):
        """name at a 0-based ref into the name blob"""
        off = NAME_BASE + ref
        d = self.d
        if off < 0 or off + 2 > len(d):
            return None
        ln = struct.unpack_from("<H", d, off)[0]
        if ln == 0 or ln > 200:
            return None
        s = d[off + 2:off + 2 + ln]
        if len(s) < ln or any(c < 32 or c > 126 for c in s):
            return None
        return s.decode("ascii")

    def _walk_names(self):
        """contiguous [u16 len][chars] runs: the name blob, and the regions
        that therefore cannot hold records"""
        self.name_runs = []
        p, end = NAME_BASE, len(self.d)
        while p < end - 4:
            if self.name(p - NAME_BASE) is None:
                p += 1
                continue
            st = p
            while True:
                n = self.name(p - NAME_BASE)
                if n is None:
                    break
                self.names.setdefault(n, []).append(p - NAME_BASE)
                p += 2 + len(n)
            self.name_runs.append((st, p))

    def _in_names(self, p):
        return any(st <= p < en for st, en in self.name_runs)

    # ── sections ─────────────────────────────────────────────────────────
    def _header_sections(self):
        """(count, fileOff) descriptors stored in the file header"""
        out, p = [], HDR_BASE
        while p + 8 <= NAME_BASE:
            cnt, off = struct.unpack_from("<II", self.d, p)
            out.append((off, cnt))
            p += 8
        return out

    def _ok(self, p):
        d = self.d
        if p + REC_SIZE > len(d):
            return False
        v, a, b = struct.unpack_from("<III", d, p)
        if not (0x1000 <= v < IMG_SIZE):
            return False
        na = None if a in NULL_REFS else self.name(a)
        nb = None if b in NULL_REFS else self.name(b)
        if a not in NULL_REFS and na is None:
            return False
        if b not in NULL_REFS and nb is None:
            return False
        return na is not None or nb is not None

    def _kind(self, off, count):
        vals = [struct.unpack_from("<III", self.d, off + i * REC_SIZE)[0]
                for i in range(min(count, 8))]
        if all(v and (v & (v - 1)) == 0 for v in vals):
            return "flag/class bits"          # powers of two = FProperty bits
        v, a, b = struct.unpack_from("<III", self.d, off)
        nmv = None if b in NULL_REFS else self.name(b)
        if v >= 0x1000000 and (nmv or "").endswith("_VFT"):
            return "class vtables"
        if count <= 8:
            return "data globals"
        return "functions"

    def globals_section(self):
        """index of the section holding the data globals: the header lists it
        first, and it is the only one carrying (type, symbol) name pairs"""
        if self.header_sections:
            want = self.header_sections[0][0]
            for i, (off, _, _) in enumerate(self.sections):
                if off == want:
                    return i
        for r in self.records:
            if "GWorld" in (r[1], r[2]):
                return r[4]
        return None

    def _find_sections(self):
        """the header's descriptors, plus any dense run the header omits"""
        self._walk_names()
        claimed = []
        for off, cnt in self.header_sections:
            self.sections.append((off, cnt, self._kind(off, cnt)))
            claimed.append((off, off + cnt * REC_SIZE))
        p, end = NAME_BASE, len(self.d)
        while p < end - 4 * REC_SIZE:
            if (self._in_names(p) or not self._ok(p)
                    or any(lo <= p < hi for lo, hi in claimed)):
                p += 4
                continue
            if self._ok(p + 12) and self._ok(p + 24) and self._ok(p + 36):
                st, n = p, 0
                while self._ok(p):
                    p += REC_SIZE
                    n += 1
                if n >= 4:
                    self.sections.append((st, n, self._kind(st, n)))
            else:
                p += 4
        self.sections.sort()

    def _read_records(self):
        for si, (off, cnt, _) in enumerate(self.sections):
            for i in range(cnt):
                p = off + i * REC_SIZE
                if p + REC_SIZE > len(self.d):
                    break
                v, a, b = struct.unpack_from("<III", self.d, p)
                self.records.append((v,
                                     None if a in NULL_REFS else self.name(a),
                                     None if b in NULL_REFS else self.name(b),
                                     p, si))

    # ── queries ──────────────────────────────────────────────────────────
    def by_symbol(self, sym):
        return [r for r in self.records if r[1] == sym or r[2] == sym]

    def globals(self):
        """(symbol, rva, type/related) of the data-globals section, which must
        contain GWorld / GNames / GObjects / UObject::ProcessEvent"""
        si = self.globals_section()
        if si is None:
            return []
        out = [(b, v, a) for v, a, b, _p, s in self.records if s == si and b]
        if "GWorld" not in [o[0] for o in out]:
            return []
        return out

    def search(self, pattern):
        rx = re.compile(re.escape(pattern).replace(r"\*", ".*") + "$")
        return sorted(n for n in self.names if rx.match(n))


def report_globals(m):
    print("idmap: %s" % os.path.relpath(m.path, ROOT))
    print("       %d bytes, %d names, %d sections, %d address records" % (
        len(m.d), len(m.names), len(m.sections), len(m.records)))
    print()
    print("%-22s %-12s %-14s %s" % ("symbol", "RVA", "absolute", "type / related"))
    for sym, rva, typ in m.globals():
        print("%-22s 0x%08X   0x%X   %s" % (sym, rva, IMAGE_BASE + rva, typ))


def report_sections(m):
    for off, cnt, kind in m.sections:
        recs = [r for r in m.records if r[4] == m.sections.index((off, cnt, kind))]
        ex = recs[0][2] or recs[0][1] if recs else ""
        print("  @%#010x  %5d records x%d  %-18s  e.g. %r" % (off, cnt, REC_SIZE, kind, ex))
    print()
    print("  header descriptors: %s" % ", ".join(
        "%d@%#x" % (c, o) for o, c in m.header_sections))


def lookup(m, pat):
    syms = m.search(pat)
    if not syms:
        print("no name matches %r" % pat)
        return 1
    for s in syms:
        recs = m.by_symbol(s)
        if recs:
            for v, a, b, p, si in recs:
                other = a if a != s else b
                print("%-44s 0x%08X  (%s)" % (s, v, m.sections[si][2]))
                if other:
                    print("%-44s            related: %s" % ("", other))
        else:
            print("%-44s -- no address record (name only)" % s)
    return 0


# Constants the live target has pinned to a value this map does not carry. The map
# (and the dump header, and OffsetsInfo.json) agree on the older GWorld RVA, so a
# disagreement here is expected: report it as an override, not a mismatch.
LIVE_OVERRIDES = {
    "UWorld": (0x10971338, "live-verified GWorld RVA; idmap carries 0x10839A98"),
}


def check(m, out_path=None, sdk_path=None):
    """reconcile the idmap globals with the generated Offsets.h

    Offsets.h constants are either hex literals or re-exports of the SDK drop
    (game::offsets::NAME -> Project/Core/SDK.hpp), so both forms resolve here.
    """
    out_path = out_path or os.path.join(ROOT, "Project", "Core", "Offsets.h")
    sdk_path = sdk_path or os.path.join(ROOT, "Project", "Core", "SDK.hpp")
    if not os.path.exists(out_path):
        print("no Offsets.h to check")
        return 1
    text = open(out_path, encoding="utf-8").read()
    sdk = open(sdk_path, encoding="utf-8").read() if os.path.exists(sdk_path) else ""

    def sdk_value(name):
        mm = re.search(r"\b%s\s*=\s*(0x[0-9A-Fa-f]+)" % re.escape(name), sdk)
        return int(mm.group(1), 16) if mm else None

    def resolve(const):
        for line in text.splitlines():
            mm = re.match(r"\s*constexpr\s+\S+\s+%s\b" % re.escape(const), line)
            if not mm:
                continue
            lit = re.search(r"=\s*(0x[0-9A-Fa-f]+)", line)
            if lit:
                return int(lit.group(1), 16), "literal"
            ref = re.search(r"game::offsets::(\w+)", line)
            if ref:
                return sdk_value(ref.group(1)), "game::offsets::%s" % ref.group(1)
            return None, "unresolved"
        return None, "absent"

    idmap = {sym: rva for sym, rva, _ in m.globals()}
    if not idmap:
        print("idmap has no data-globals section -- nothing to reconcile")
        return 1
    bad = 0
    print("%-22s %-14s %-14s %s" % ("constant", "Offsets.h", "idmap", "status"))
    for const, sym in sorted(OFFSETS_SYMBOLS.items()):
        want = idmap.get(sym)
        got, src = resolve(const)
        if want is None:
            print("%-22s %-14s %-14s no idmap entry" % (
                const, hex(got) if got else "-", "-"))
            continue
        if got is None:
            bad += 1
            print("%-22s %-14s %#-13x MISSING (%s)" % (const, "-", want, src))
            continue
        ov = LIVE_OVERRIDES.get(const)
        if ov and got == ov[0]:
            print("%-22s %#-13x %#-13x overridden  [%s; %s]" % (
                const, got, want, src, ov[1]))
            continue
        ok = got == want
        if not ok:
            bad += 1
        print("%-22s %#-13x %#-13x %s  [%s]" % (
            const, got, want, "ok" if ok else "MISMATCH", src))
    print()
    print("reconciled %d idmap global(s) against %s: %d mismatch(es)" % (
        len(idmap), os.path.relpath(out_path, ROOT), bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["globals", "sections", "lookup", "check"])
    ap.add_argument("pattern", nargs="?")
    ap.add_argument("--file")
    a = ap.parse_args()
    m = Idmap(find_file(a.file))
    if a.command == "globals":
        report_globals(m)
        return 0
    if a.command == "sections":
        report_sections(m)
        return 0
    if a.command == "lookup":
        if not a.pattern:
            raise SystemExit("lookup needs a pattern")
        return lookup(m, a.pattern)
    return check(m)


if __name__ == "__main__":
    sys.exit(main())
