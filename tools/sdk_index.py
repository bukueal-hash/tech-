#!/usr/bin/env python3
"""Index everything in sdk/ into queryable JSON (tools/sdk_index.json).

Two kinds of source live in sdk/, and they are identified by *content*, never by
filename - on a case-insensitive filesystem `sdk.txt` and `SDK.txt` are the same
file, and a file can be either kind:

  reflect  a FrostDumper text dump: `// Class Engine.World` + `// Size: 0x..` +
           `constexpr uint32_t Member = 0x..; // type` lines. The only source
           that sees reflected UPROPERTYs with offset, type and packed mask.
  drop     the CL drop (`sdk/sdk.txt`): C++ headers with `namespace ArcOffsets
           { constexpr ... }`, key tables and the crypto/decrypt constants.

Output:
  {
    "meta":    {"Game build": ..., "sources": [{path, kind, sha256, bytes, counts}]},
    "classes": {"Engine.World": {"size":.., "inherits":.., "props": {..}}},
    "structs": {...},
    "drop":    {"constants": {"ArcOffsets::UWorld::PERSISTENT_LEVEL":
                              {"value":.., "comment":.., "fresh":.., "scope":..,
                               "line":.., "namespace":..}},
                "namespaces": {"UWorld": ["PERSISTENT_LEVEL", ...]}}
  }

`build` merges: a section whose source is no longer in sdk/ is carried over from
the previous index (and marked as such in meta) rather than dropped, because
every `prop:` spec in tools/gen_offsets.py depends on the classes bucket.

Usage:
  python tools/sdk_index.py build                  # refresh from sdk/
  python tools/sdk_index.py sources                # what was indexed, and from where
  python tools/sdk_index.py prop <Class> <Property>
  python tools/sdk_index.py class <Class> [members...]
  python tools/sdk_index.py search <classsubstr> <propsubstr>
  python tools/sdk_index.py drop <substr>          # search the CL drop section
"""
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK_DIR = os.path.join(HERE, "..", "sdk")
OUT = os.path.join(HERE, "sdk_index.json")

sys.path.insert(0, HERE)
import sdk_drop

HEADER = re.compile(r"^// (Class|Struct|Enum|Function|Package) (/\w+/)?([\w.]+)\s*$")
SIZE = re.compile(r"^// Size: (0x[0-9a-fA-F]+)")
INHERITS = re.compile(r"^// Inherits: (\w+)")
PROP = re.compile(r"^constexpr uint32_t (\w+)\s*=\s*(0x[0-9a-fA-F]+);\s*//\s*(.*)$")
META_KEYS = ("Dumper build", "Game build", "Game updated", "Image size",
             "FName pipeline", "PID")


def source_files():
    """Every distinct file in sdk/, realpath-deduped (case-insensitive filesystems
    report sdk.txt and SDK.txt as two names for one file)."""
    seen, out = set(), []
    for name in sorted(os.listdir(SDK_DIR)):
        path = os.path.join(SDK_DIR, name)
        if not os.path.isfile(path):
            continue
        real = os.path.realpath(path).lower()
        if real in seen:
            continue
        seen.add(real)
        out.append(path)
    return out


def classify(path):
    """('reflect'|'drop'|'unknown') from the file's content."""
    head = open(path, encoding="utf-8", errors="replace").read()
    reflect = bool(re.search(r"^// (Class|Struct) [\w.]+", head, re.M))
    drop = "namespace ArcOffsets" in head or "namespace FName" in head
    if reflect and drop:
        return "both"
    if reflect:
        return "reflect"
    if drop:
        return "drop"
    return "unknown"


def parse_reflect(path):
    """FrostDumper text dump -> (meta, classes, structs)."""
    meta, classes, structs = {}, {}, {}
    cur = kind = None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("// ") and cur is None:
            key, _, val = line[3:].partition(": ")
            if key in META_KEYS:
                meta[key] = val
        m = HEADER.match(line)
        if m:
            kind, full = m.group(1), m.group(3)
            pkg, _, name = full.rpartition(".")
            cur = {"pkg": pkg, "name": name, "full": full,
                   "props": {}, "size": None, "inherits": None}
            continue
        if cur is None:
            continue
        ms = SIZE.match(line)
        if ms:
            cur["size"] = int(ms.group(1), 16)
            continue
        mi = INHERITS.match(line)
        if mi:
            cur["inherits"] = mi.group(1)
            continue
        mp = PROP.match(line)
        if mp:
            name, off, rest = mp.group(1), int(mp.group(2), 16), mp.group(3)
            fields = [f.strip() for f in rest.split("//")]
            mask = size = None
            for f in fields[1:]:
                if f.startswith("mask="):
                    mask = int(f[5:], 16)
                elif f.startswith("size="):
                    size = int(f[5:], 16)
            cur["props"][name] = {"off": off, "type": fields[0] if fields else "",
                                  "size": size, "mask": mask}
            continue
        if line.startswith("} // namespace"):
            if kind == "Class":
                classes[cur["full"]] = cur
            elif kind == "Struct":
                structs[cur["full"]] = cur
            cur = kind = None
    return meta, classes, structs


def parse_drop_section():
    """CL drop -> {"constants": {...}, "namespaces": {ns: [name, ...]}}."""
    entries = sdk_drop.parse()
    constants, namespaces = {}, {}
    for path, e in entries.items():
        constants[path] = {
            "value": e["value"], "comment": e["comment"], "fresh": e["fresh"],
            "scope": e["scope"], "line": e["line"],
            "namespace": path.split("::")[-2],
        }
        namespaces.setdefault(path.split("::")[-2], []).append(
            path.rsplit("::", 1)[-1])
    for names in namespaces.values():
        names.sort()
    return {"constants": constants,
            "namespaces": {k: v for k, v in sorted(namespaces.items())}}


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def build():
    old = {}
    if os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as fh:
            old = json.load(fh)

    meta = dict(old.get("meta", {}))
    classes = old.get("classes", {})
    structs = old.get("structs", {})
    drop = old.get("drop")
    sources, carried = [], []

    for path in source_files():
        kind = classify(path)
        rel = os.path.relpath(path, os.path.join(HERE, ".."))
        info = {"path": rel.replace("\\", "/"), "kind": kind,
                "sha256": sha256(path)[:16], "bytes": os.path.getsize(path)}
        if kind in ("reflect", "both"):
            file_meta, new_classes, new_structs = parse_reflect(path)
            meta.update({k: v for k, v in file_meta.items() if k in META_KEYS})
            classes.update(new_classes)
            structs.update(new_structs)
            info["counts"] = {"classes": len(new_classes), "structs": len(new_structs)}
        elif kind == "drop":
            drop = parse_drop_section()
            info["counts"] = {"constants": len(drop["constants"]),
                              "namespaces": len(drop["namespaces"])}
        else:
            info["counts"] = {}
        sources.append(info)

    if kind_missing(sources, "reflect") and classes:
        carried.append("classes/structs")
    if kind_missing(sources, "drop") and drop:
        carried.append("drop")
    if not drop:
        drop = {"constants": {}, "namespaces": {}}

    meta["sources"] = sources
    meta["carried_over"] = carried
    meta["generated"] = __import__("datetime").datetime.now().isoformat(
        timespec="seconds")

    idx = {"meta": meta, "classes": classes, "structs": structs, "drop": drop}
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(idx, fh)
    print("wrote %s" % OUT)
    for info in sources:
        print("  %-8s %-22s %s  %s" % (
            info["kind"], info["path"], info["sha256"],
            json.dumps(info["counts"]) if info["counts"] else ""))
    print("  classes=%d structs=%d drop.constants=%d drop.namespaces=%d"
          % (len(classes), len(structs), len(drop["constants"]),
             len(drop["namespaces"])))
    for what in carried:
        print("  [!] %s carried over from the previous index - no %s source in sdk/"
              % (what, "reflect" if what.startswith("classes") else "drop"))


def kind_missing(sources, want):
    return not any(s["kind"] in (want, "both") for s in sources)


def load():
    with open(OUT, encoding="utf-8") as fh:
        return json.load(fh)


def resolve(idx, name):
    for bucket in ("classes", "structs"):
        if name in idx[bucket]:
            return bucket, name, idx[bucket][name]
    for bucket in ("classes", "structs"):
        hits = [k for k in idx[bucket] if k.rpartition(".")[2] == name]
        if hits:
            return bucket, hits[0], idx[bucket][hits[0]]
    return None, None, None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "build":
        build()
        return
    idx = load()
    if cmd == "sources":
        print(json.dumps(idx.get("meta", {}).get("sources", []), indent=1))
        print("carried over:", idx.get("meta", {}).get("carried_over"))
        return
    if cmd == "prop":
        cls, prop = sys.argv[2], sys.argv[3]
        _, key, c = resolve(idx, cls)
        if not c:
            print("class not found:", cls)
            return
        print("%s size=0x%x inherits=%s" % (key, c["size"] or 0, c["inherits"]))
        for p, info in c["props"].items():
            if prop.lower() in p.lower():
                print("  %-40s 0x%-5x %s%s" % (
                    p, info["off"], info["type"],
                    (" mask=0x%x" % info["mask"]) if info["mask"] else ""))
    elif cmd == "dump-prop":
        sub = sys.argv[2].lower()
        for bucket in ("classes", "structs"):
            for key, c in idx[bucket].items():
                for p, info in c["props"].items():
                    if sub in p.lower():
                        print("%-46s %-34s 0x%-6x %s%s" % (
                            key, p, info["off"], info["type"],
                            (" mask=0x%x" % info["mask"]) if info["mask"] else ""))
    elif cmd == "class":
        _, key, c = resolve(idx, sys.argv[2])
        if not c:
            print("class not found:", sys.argv[2])
            return
        wants = [w.lower() for w in sys.argv[3:]]
        print("%s size=0x%x inherits=%s props=%d" % (
            key, c["size"] or 0, c["inherits"], len(c["props"])))
        for p, info in c["props"].items():
            if wants and not any(w in p.lower() for w in wants):
                continue
            print("  %-42s 0x%-6x %s%s" % (
                p, info["off"], info["type"],
                (" mask=0x%x" % info["mask"]) if info["mask"] else ""))
    elif cmd == "search":
        cs, ps = sys.argv[2].lower(), sys.argv[3].lower()
        for key, c in list(idx["classes"].items()) + list(idx["structs"].items()):
            if cs not in key.lower():
                continue
            for p, info in c["props"].items():
                if ps in p.lower():
                    print("%-40s %-40s 0x%-6x %s" % (key, p, info["off"], info["type"]))
    elif cmd == "drop":
        sub = sys.argv[2].lower()
        for path, e in sorted(idx.get("drop", {}).get("constants", {}).items()):
            if sub in path.lower():
                print("%-58s 0x%-10X %-13s L%-5d %s" % (
                    path, e["value"],
                    "fresh" if e["fresh"] else "cl-value", e["line"],
                    e["comment"][:44]))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
