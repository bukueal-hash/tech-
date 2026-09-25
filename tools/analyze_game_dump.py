"""Offline offset verification against a Windows minidump of the game.

Reads an MDMP (PioneerGame.DMP), builds a VA -> file-offset map, then:

  info    module base / image size, and whether the SDK global offsets are in range
  sdk     raw values at base + GWORLD / GNAMES / KEYTABLE (+ old values for comparison)
  fname   runs the Core/SDK.hpp FName pipeline (decode_fname_pool_address +
          decode_fname) against the dump and prints resolved names
  gworld  scans the image's writable sections for the UWorld slot (strong chain:
          *(W+0x110) is a valid pointer and *(PL+0x130) == W)

Usage:
  python tools/analyze_game_dump.py info   [dump]
  python tools/analyze_game_dump.py sdk    [dump]
  python tools/analyze_game_dump.py fname  [dump]
  python tools/analyze_game_dump.py gworld [dump]
"""

import bisect
import mmap
import struct
import sys

DEFAULT_DUMP = r"G:\PioneerGame.DMP"

MDMP_SIG = b"MDMP"
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_SYSTEM_INFO = 7
STREAM_MEMORY64_LIST = 9

MODULE_EXE_SUFFIX = b"-Win64-Shipping.exe"

# ── SDK drop values (Project/Core/SDK.hpp game::offsets) ─────────────────────
SDK_GWORLD = 0x10839A98
# live-verified against the running game: the static 0x10839A98 reads as the
# constant non-pointer GWorldRaw in the field logs, this one is the real slot.
LIVE_GWORLD = 0x10971338
SDK_GNAMES = 0x10987D40
SDK_KEYTABLE = 0x1082B26C
SDK_FSTRING_VERIF = 0xD3C29A0

# previous build values, for comparison
OLD_UWORLD = 0xE782D78
OLD_GNAMEPOOL = 0xE35AB00
OLD_KEYSTREAM = 0xE2997F4

PTR_LO = 0x10000
PTR_HI = 0x7FFFFFFFFFFF


def rol32(v, n):
    v &= 0xFFFFFFFF
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def rol64(v, n):
    v &= 0xFFFFFFFFFFFFFFFF
    return ((v << n) | (v >> (64 - n))) & 0xFFFFFFFFFFFFFFFF


def clmul64_low(a, b):
    out = 0
    while b:
        if b & 1:
            out ^= a
        b >>= 1
        a = (a << 1) & 0xFFFFFFFFFFFFFFFF
    return out


def shuffle64(value):
    indices = (6, 5, 1, 2, 7, 3, 4, 0)
    src = value.to_bytes(8, "little")
    return int.from_bytes(bytes(src[i] for i in indices), "little")


def uobject_selector(addr):
    a = (addr + 0x10) & 0xFFFFFFFFFFFFFFFF
    v = (rol32(a & 0xFFFFFFFF, 28) * 0x01000193 + 0x2306CC41) & 0xFFFFFFFF
    v = (rol32(v, 21) * 0x01000193 + (a >> 32) + 0x2306CC41) & 0xFFFFFFFF
    v = (rol32(v, 28) * 0x01000193 + 0x2306CC41) & 0xFFFFFFFF
    v = ((v >> 11) * 0x01000193 + 0x2306CC41) & 0xFFFFFFFF
    return ((v >> 16) ^ v) & 3


def fname_pool_selector(addr):
    v = (rol32(addr & 0xFFFFFFFF, 17) * 0x01000193 + 0xD69AD929) & 0xFFFFFFFF
    v = (rol32(v, 27) * 0x01000193 + (addr >> 32) + 0xD69AD929) & 0xFFFFFFFF
    v = (rol32(v, 17) * 0x01000193 + 0xD69AD929) & 0xFFFFFFFF
    v = ((v >> 5) * 0x01000193 + 0xD69AD929) & 0xFFFFFFFF
    return (v ^ (v >> 16)) & 0xFFFFFFFF


def decode_fname_pool_first(slot):
    an = int.from_bytes(bytes([0x0A, 0x9B, 0x74, 0x95, 0x58, 0xDD, 0x72, 0x8D]), "little")
    aa = int.from_bytes(bytes([0xF5, 0x64, 0x8B, 0x6A, 0xA7, 0x22, 0x8D, 0x72]), "little")
    xx = int.from_bytes(bytes([0x90, 0xA6, 0x69, 0x03, 0xE6, 0x1D, 0x73, 0x79]), "little")
    v = slot
    selected = ((~v & an) | (v & aa)) ^ xx
    return shuffle64(rol64(selected, 19))


def decode_fname_pool_second(slot):
    xx = int.from_bytes(bytes([0x9A, 0x3D, 0x1D, 0x96, 0xBE, 0xC0, 0x01, 0xF4]), "little")
    return shuffle64(rol64(slot ^ xx, 19))


def decode_fname_pool_address(gnames_address, chunk_window, chunk_offset=0):
    selector = fname_pool_selector(gnames_address + chunk_offset + 0x40)
    base = chunk_offset + 0x50
    first = decode_fname_pool_first(
        int.from_bytes(chunk_window[base + (selector & 7) * 0x20:
                                    base + (selector & 7) * 0x20 + 8], "little"))
    second = decode_fname_pool_second(
        int.from_bytes(chunk_window[base + ((selector + 1) & 7) * 0x20:
                                    base + ((selector + 1) & 7) * 0x20 + 8], "little"))
    mixed = (rol64(first, 40) * 0x00000100000001B3 + 0x6C5FD4827126D389) & 0xFFFFFFFFFFFFFFFF
    mixed = (rol64(mixed, 50) * 0x00000100000001B3 + 0x6C5FD4827126D389) & 0xFFFFFFFFFFFFFFFF
    return ((mixed ^ second) + first) & 0xFFFFFFFFFFFFFFFF


def decode_fname_header_len(hdr):
    return ((hdr >> 3) & 0x3F8) + (hdr >> 13)


def decode_fname(hdr, buf, key_table_base):
    length = decode_fname_header_len(hdr)
    out = bytearray(buf[:length])
    state = (length + 0xADF) & 0xFFFFFFFF
    index = 0
    while index + 1 < length:
        def tbl(i):
            return int.from_bytes(key_table_base[i * 2:i * 2 + 2], "little") & 0xFFFF
        out[index] ^= (tbl(state & 63) >> 3) & 0xFF
        out[index + 1] ^= (tbl((state * 33) & 63) >> 3) & 0xFF
        state = (state * 0x00762E41 + 0xFE5AE580) & 0xFFFFFFFF
        index += 2
    if index < length:
        out[index] ^= (int.from_bytes(key_table_base[(state & 63) * 2:(state & 63) * 2 + 2],
                                      "little") >> 3) & 0xFF
    return bytes(out), length


class Dump:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.mm = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        self.regions = []      # sorted list of (start, size, file_off)
        self.starts = []
        self.modules = []      # (base, size, name)
        self._parse()

    def _parse(self):
        mm = self.mm
        sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", mm, 0)
        if mm[:4] != MDMP_SIG:
            raise SystemExit("not a minidump (MDMP) file")
        for i in range(nstreams):
            stype, dsize, srva = struct.unpack_from("<III", mm, dir_rva + i * 12)
            if stype == STREAM_MEMORY64_LIST:
                count, base_rva = struct.unpack_from("<QQ", mm, srva)
                off = base_rva
                for j in range(count):
                    start, size = struct.unpack_from("<QQ", mm, srva + 16 + j * 16)
                    self.regions.append((start, size, off))
                    off += size
            elif stype == STREAM_MEMORY_LIST:
                count = struct.unpack_from("<I", mm, srva)[0]
                for j in range(count):
                    start, size, rva = struct.unpack_from("<QII", mm, srva + 4 + j * 16)
                    self.regions.append((start, size, rva))
            elif stype == STREAM_MODULE_LIST:
                count = struct.unpack_from("<I", mm, srva)[0]
                for j in range(count):
                    mbase, msize = struct.unpack_from("<QI", mm, srva + 4 + j * 108)
                    name_rva = struct.unpack_from("<I", mm, srva + 4 + j * 108 + 20)[0]
                    nlen = struct.unpack_from("<I", mm, name_rva)[0]
                    name = mm[name_rva + 4:name_rva + 4 + nlen].decode("utf-16-le", "replace")
                    self.modules.append((mbase, msize, name))
        self.regions.sort()
        self.starts = [r[0] for r in self.regions]

    def read(self, va, size):
        """Read `size` bytes at VA; None if any part is not in the dump."""
        idx = bisect.bisect_right(self.starts, va) - 1
        if idx < 0:
            return None
        start, rsize, foff = self.regions[idx]
        if va < start or va + size > start + rsize:
            return None
        return self.mm[foff + (va - start): foff + (va - start) + size]

    def read_u16(self, va):
        b = self.read(va, 2)
        return None if b is None else struct.unpack("<H", b)[0]

    def read_u32(self, va):
        b = self.read(va, 4)
        return None if b is None else struct.unpack("<I", b)[0]

    def read_u64(self, va):
        b = self.read(va, 8)
        return None if b is None else struct.unpack("<Q", b)[0]

    def mapped(self, va):
        return self.read(va, 1) is not None

    def main_module(self):
        for base, size, name in self.modules:
            if name.lower().endswith(MODULE_EXE_SUFFIX.decode().lower()):
                return base, size, name
        for base, size, name in self.modules:
            if name.lower().endswith(".exe") and "system32" not in name.lower():
                return base, size, name
        return None

    def sections(self, base):
        """PE sections of the image at `base` -> list of dicts."""
        e_lfanew = self.read_u32(base + 0x3C)
        if e_lfanew is None:
            return []
        coff = base + e_lfanew + 4
        nsec, = struct.unpack("<H", self.read(coff + 2, 2))
        opt_size, = struct.unpack("<H", self.read(coff + 16, 2))
        opt = coff + 20
        secs = []
        soff = opt + opt_size
        for i in range(nsec):
            raw = self.read(soff + i * 40, 40)
            name = raw[:8].rstrip(b"\x00").decode("ascii", "replace")
            vsize, va, rawsize = struct.unpack_from("<III", raw, 8)
            chars, = struct.unpack_from("<I", raw, 36)
            secs.append({"name": name, "va": va, "vsize": vsize,
                         "rawsize": rawsize, "chars": chars})
        return secs


def cmd_info(d):
    base, size, name = d.main_module()
    print(f"dump regions: {len(d.regions)}")
    print(f"module: {name}  base=0x{base:X}  size=0x{size:X} ({size / 1024 / 1024:.1f} MB)")
    print(f"covers [0x{base:X}, 0x{base + size:X})")
    print()
    for label, rva in (("SDK GWORLD", SDK_GWORLD), ("live GWORLD", LIVE_GWORLD),
                       ("SDK GNAMES", SDK_GNAMES),
                       ("SDK KEYTABLE", SDK_KEYTABLE),
                       ("SDK FString_Verification", SDK_FSTRING_VERIF),
                       ("old UWorld", OLD_UWORLD), ("old GNamePool", OLD_GNAMEPOOL),
                       ("old Keystream", OLD_KEYSTREAM)):
        va = base + rva
        inside = "in image" if rva < size else "OUTSIDE image"
        val = d.read_u64(va)
        mapped = "mapped" if d.mapped(va) else "UNMAPPED"
        shown = "n/a" if val is None else f"0x{val:X}"
        print(f"  {label:<24} rva=0x{rva:<10X} va=0x{va:<12X} {inside:<14} "
              f"{mapped:<9} value={shown}")
    print()
    for s in d.sections(base):
        flags = []
        if s["chars"] & 0x80000000:
            flags.append("W")
        if s["chars"] & 0x40000000:
            flags.append("R")
        if s["chars"] & 0x20000000:
            flags.append("X")
        print(f"  {s['name']:<10} va=0x{s['va']:<9X} vsize=0x{s['vsize']:<9X} "
              f"rawsize=0x{s['rawsize']:<9X} {'|'.join(flags)}")


def cmd_sdk(d):
    base, size, _ = d.main_module()
    print(f"module base=0x{base:X} size=0x{size:X}")
    for label, rva in (("GWORLD (sdk)", SDK_GWORLD), ("GWORLD (live)", LIVE_GWORLD),
                       ("GNAMES", SDK_GNAMES),
                       ("KEYTABLE", SDK_KEYTABLE)):
        print(f"\n{label} @ base+0x{rva:X} = 0x{base + rva:X}")
        for off in range(0, 0x20, 8):
            v = d.read_u64(base + rva + off)
            print(f"  +0x{off:02X}: {'unmapped' if v is None else f'0x{v:016X}'}")

    print(f"\nKEYTABLE raw dump (base+0x{SDK_KEYTABLE:X}, 0x60 bytes):")
    for row in range(0, 0x60, 16):
        b = d.read(base + SDK_KEYTABLE + row, 16)
        if b is None:
            print(f"  +0x{row:02X}: unmapped")
        else:
            print(f"  +0x{row:02X}: {b.hex(' ')}")


def cmd_fname(d):
    base, size, _ = d.main_module()
    gnames = base + SDK_GNAMES
    keytable = base + SDK_KEYTABLE
    key_window = d.read(keytable, 0x18 + 64 * 2)
    if key_window is None:
        print("KEYTABLE not mapped")
        return
    print("key table word check (u16 at +0x18):")
    for i in range(8):
        w, = struct.unpack_from("<H", key_window, 0x18 + i * 2)
        print(f"  [{i}] 0x{w:04X}")

    for chunk_index in (0, 1, 2):
        chunk_off = chunk_index * 0x100
        chunk_addr = gnames + chunk_off
        window = d.read(chunk_addr, 0x50 + 8 * 0x20)
        if window is None:
            print(f"\nchunk {chunk_index}: chunk window unmapped")
            continue
        pool = decode_fname_pool_address(chunk_addr, window, 0)
        print(f"\nchunk {chunk_index} (offset 0x{chunk_off:X}): pool=0x{pool:X} "
              f"mapped={d.mapped(pool)}")
        names = []
        for name_off in range(0, 64):
            entry = pool + 2 * name_off
            hdr = d.read_u16(entry)
            if hdr is None:
                continue
            length = decode_fname_header_len(hdr)
            if length in (0, 0x3FF) or length > 200:
                continue
            wide = hdr & 1
            if wide:
                raw = d.read(entry + 2, length * 2)
                if raw is None:
                    continue
                text = "".join(chr(int.from_bytes(raw[i * 2:i * 2 + 2], "little") & 0x7F)
                               for i in range(length))
            else:
                raw = d.read(entry + 2, length)
                if raw is None:
                    continue
                dec, _ = decode_fname(hdr, raw, key_window)
                text = "".join(chr(c) if 32 <= c <= 126 else "." for c in dec)
            if text.strip("."):
                names.append((name_off, hdr, text))
        for name_off, hdr, text in names[:40]:
            print(f"    [{name_off:3d}] hdr=0x{hdr:04X} {text}")
        if not names:
            print("    (no plausible names decoded)")


def cmd_gworld(d):
    base, size, _ = d.main_module()
    candidates = []
    for s in d.sections(base):
        if not (s["chars"] & 0x80000000):   # need writable
            continue
        span = min(s["vsize"], s["rawsize"]) if s["rawsize"] else s["vsize"]
        if span == 0:
            continue
        va = base + s["va"]
        print(f"scanning {s['name']} va=0x{va:X} size=0x{span:X} ...", flush=True)
        step = 1 << 20
        for off in range(0, span, step):
            blk = d.read(va + off, min(step, span - off))
            if blk is None:
                continue
            count = len(blk) // 8
            qwords = struct.unpack_from(f"<{count}Q", blk)
            for i, v in enumerate(qwords):
                if v < PTR_LO or v > PTR_HI or not d.mapped(v):
                    continue
                pl = d.read_u64(v + 0x110)
                if pl is None or pl < PTR_LO or pl > PTR_HI or not d.mapped(pl):
                    continue
                owner = d.read_u64(pl + 0x130)
                if owner != v:
                    continue
                slot = va + off + i * 8
                candidates.append((slot, v, pl))
    print(f"\ncandidates: {len(candidates)}")
    for slot, world, pl in candidates:
        rva = slot - base
        print(f"  slot=0x{slot:X} (rva 0x{rva:X})  UWorld=0x{world:X}  "
              f"PersistentLevel=0x{pl:X}")
        for label, off in (("OwningGameInstance", 0x478), ("LevelCollections", 0x240),
                           ("LocalPlayers", 0x138), ("TimeSeconds", 0x8B0)):
            v = d.read_u64(world + off)
            print(f"      +0x{off:X} {label:<20} "
                  f"{'unmapped' if v is None else f'0x{v:016X}'}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("info", "sdk", "fname", "gworld"):
        print(__doc__)
        return 2
    path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DUMP
    mode = sys.argv[1]
    print(f"# dump: {path}  mode: {mode}")
    d = Dump(path)
    {"info": cmd_info, "sdk": cmd_sdk, "fname": cmd_fname, "gworld": cmd_gworld}[mode](d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
