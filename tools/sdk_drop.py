#!/usr/bin/env python3
"""Parse sdk/sdk.txt - the CL drop - into a flat namespace::NAME -> value map.

The drop is one of the offline sources of truth, so it is read in exactly one
place and both consumers (tools/gen_offsets.py, tools/reconcile_offsets.py)
import this module instead of carrying their own regex.

sdk/sdk.txt is three headers pasted together:

  * arc_decrypt.h   - TebDecrypt / GameInstanceTlsDecrypt / PlayerDecrypt /
                      OuterDecrypt / GameInstanceStaticDecrypt + the crypto keys
  * <FName header>  - namespace FName, the CL-1389382 name pipeline
  * arc_offsets.h   - namespace ArcOffsets, the UObject/UWorld/Actor layout

Every `constexpr NAME = 0x...;` that sits *directly* inside a namespace is
recorded, whatever the namespace is called, with its full path as the key.
Entries nested deeper (a marker-scan loop, a helper body) are skipped: a local
`constexpr` is not a declaration of a game offset and must never be able to
shadow one.

Each entry carries what the reconciliation needs to score it:

  value     the constant
  comment   the drop's own trailing comment
  fresh     the drop re-checked this entry for 2026-09-22
  path      namespace::NAME
  scope     the outermost namespace (the header it came from)
  line      1-based line in sdk/sdk.txt
"""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DROP_PATH = os.path.join(ROOT, "sdk", "sdk.txt")

# A declaration, not a loop counter: `constexpr <type> NAME = <number>;`
_DECL = re.compile(
    r"^\s*(?:static\s+|inline\s+)*constexpr\s+[A-Za-z0-9_:<>,\s]+?\s+"
    r"(?P<name>\w+)\s*=\s*(?P<value>0[xX][0-9A-Fa-f]+|\d+)"
    r"(?:[uUlL]{0,3}|\s*ULL)?\s*;\s*(?://\s*(?P<comment>.*))?$")
_NS_OPEN = re.compile(r"^\s*namespace\s+(\w+)\s*\{")
_STRIP_COMMENT = re.compile(r"//.*$")


def parse(path=DROP_PATH):
    """Return {path::NAME: {...}} for every namespace-level constant."""
    out = {}
    stack = []          # [(namespace, brace depth before its '{')]
    depth = 0
    covered = {}        # innermost namespace -> every commented word it holds
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            comment = line.split("//", 1)[1].strip() if "//" in line else ""
            code = _STRIP_COMMENT.sub("", line)
            m = _NS_OPEN.match(code)
            if m:
                stack.append((m.group(1), depth))
            elif stack and depth == stack[-1][1] + 1:
                d = _DECL.match(line)
                if d:
                    own = (d.group("comment") or "").strip()
                    path_key = "::".join(n for n, _ in stack) + "::" + d.group("name")
                    out[path_key] = {
                        "value": int(d.group("value"), 0),
                        "comment": own,
                        "fresh": "2026-09-22" in own or "v20260922" in own,
                        "path": path_key,
                        "scope": stack[0][0],
                        "line": lineno,
                        # everything the drop writes in this namespace, so a
                        # constant can be read in the light of the prose around
                        # it ("retired 2026-09-09, kept for history only")
                        "block_comment": covered.get(stack[-1][0], "")[-2000:],
                    }
            if comment:
                for name, _depth in stack:
                    covered[name] = covered.get(name, "") + comment + " "
            depth += code.count("{") - code.count("}")
            while stack and depth <= stack[-1][1]:
                stack.pop()
    return out


def by_name(drop):
    """NAME (upper-cased, non-alphanumerics dropped) -> [path, ...]."""
    out = {}
    for path in drop:
        out.setdefault(_norm(path.rsplit("::", 1)[-1]), []).append(path)
    return out


def by_value(drop):
    """value -> [path, ...] - lets a shipped literal find the drop entry that
    describes it even when the two names diverged."""
    out = {}
    for path, entry in drop.items():
        out.setdefault(entry["value"], []).append(path)
    return out


def _norm(text):
    return re.sub(r"[^A-Z0-9]", "", (text or "").upper())


if __name__ == "__main__":
    import sys
    drop = parse()
    scopes = {}
    for entry in drop.values():
        scopes[entry["scope"]] = scopes.get(entry["scope"], 0) + 1
    print("sdk/sdk.txt: %d namespace-level constants" % len(drop))
    for scope, count in sorted(scopes.items(), key=lambda kv: -kv[1]):
        print("  %-28s %d" % (scope, count))
    if len(sys.argv) > 1:
        needle = sys.argv[1].lower()
        for path in sorted(drop):
            if needle in path.lower():
                e = drop[path]
                print("  %-52s 0x%-8X %s" % (path, e["value"], e["comment"][:48]))
