#!/usr/bin/env python3
"""Score every constant in Project/Core/Offsets.h against every offline source.

Nothing here asks a human to pick a winner. Each constant is scored by the
provenance of its shipped value, each *disagreeing* source is scored by what it
can actually see, and the two scores decide the row:

  * a source that outranks the shipped value wins      -> SHOULD-CHANGE
  * a source that ties a live-pinned value loses        -> OVERRULED
  * a source that cannot see the slot is not evidence   -> INAPPLICABLE
  * a tie inside the same file family is broken once, deterministically
    (the raw drop beats the hand-copied header)         -> TIE-RESOLVED
  * only a genuine tie between two *different* sources that both see the slot
    is reported as CONFLICT, and there are none on this build.

Authority ladder (highest first) - the numbers below are the confidence score:

  6  live    pinned against the running target (gen_offsets.LIVE_OVERRIDES
             or a value the SDK drop itself marks user-verified)
  5  dump    tools/sdk_index.json - the FrostDumper property index of the
             2026-09-22 build. The only source that sees reflected UPROPERTYs
             with their real offset, mask and owning class size.
  4  drop    sdk/sdk.txt entry the drop re-checked for 2026-09-22
  4  header  SDK.hpp game::offsets, the drop value the project hand-copied
  3  drop    sdk/sdk.txt entry sitting on its older CL value
  2  derived arithmetic/alias over a higher-ranked constant
  1  probed  nothing offline can see this slot

A shipped constant's provenance comes from its generator spec, never from a
hand-kept table: `prop:` counts as dump, `drop:`/`sdk:` as drop/header, `expr:`
inherits its base constant's rank, `sizeof:`/`maskOf:` as dump, and a `lit:`
rolls up to the best offline source that confirms its value.

Buckets:
  SHOULD-CHANGE  a higher-authority source disagrees -> fix the generator
  CONFLICT       two sources that can both see the slot disagree (must be empty)
  TIE-RESOLVED   same-file tie, decided by rule, no action needed
  INAPPLICABLE   the disagreeing entry cannot describe this slot (unconfirmed
                 in the drop's own words, value outside the class the dump
                 gives, or a namespace this build does not have)
  BOTH           the source's value ships under a sibling constant; a runtime
                 probe decides
  OVERRULED      a confirmed source disagrees but ships lower authority
  PROBED         no offline source can see the slot at all
  AGREE          a source confirms the shipped value

Usage:
  python tools/reconcile_offsets.py                  # actionable rows first
  python tools/reconcile_offsets.py --all            # every constant
  python tools/reconcile_offsets.py --check          # exit 1 on SHOULD-CHANGE/CONFLICT
  python tools/reconcile_offsets.py --report out.md  # full markdown report
  python tools/reconcile_offsets.py --json out.json  # machine-readable scores
"""
import argparse
import datetime
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdk_drop

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OFFSETS = os.path.join(ROOT, "Project", "Core", "Offsets.h")
INDEX = os.path.join(ROOT, "tools", "sdk_index.json")
DROP = sdk_drop.DROP_PATH
SDK_HPP = os.path.join(ROOT, "Project", "Core", "SDK.hpp")

# Project constant -> drop path suffix, for names that do not normalise to the
# same string. A curated alias is trusted outright; automatic matches also have
# to mention the drop's namespace in the project row to be trusted.
ALIASES = {
    "AActors": "Level::ACTORS",
    "ActorsCount": "Level::ACTOR_COUNT",
    "ActorCluster": "Level::ACTOR_CLUSTER",
    "Level_OwningWorld": "Level::OWNING_WORLD",
    "AController_PlayerState": "Controller::PLAYER_STATE",
    "AcknowledgedPawn": "Controller::ACKNOWLEDGED_PAWN",
    "Controller_Character": "Controller::CHARACTER",
    "APlayerCameraManager": "PlayerController::PLAYER_CAMERA_MANAGER",
    "APlayerState": "Pawn::PLAYER_STATE",
    "Pawn_Controller": "Pawn::CONTROLLER",
    "PlayerState_PawnPrivate": "PlayerState::PAWN_PRIVATE",
    "PlayerNamePrivate": "PlayerState::PLAYER_NAME_PRIVATE",
    "PlayerState_PlayerStatus": "PlayerState::PLAYER_STATUS",
    "PioneerPlayerState_PioneerCharacter": "PlayerState::PIONEER_CHARACTER",
    "PioneerPlayerState_CurrentPawn": "PlayerState::CURRENT_PAWN",
    "OwningGameInstance": "UWorld::GAME_INSTANCE",
    "GameState_PlayerArray": "GameState::PLAYER_ARRAY",
    "GameState_EnemyCount": "GameState::ENEMY_COUNT",
    "GameViewportClient_World": "GameViewportClient::WORLD",
    "LevelActorContainer_Actors": "LevelActorContainer::ACTORS",
    "LevelActorContainer_ActorCount": "LevelActorContainer::ACTOR_COUNT",
    "LocalPlayer_ControllerId": "LocalPlayer::CONTROLLER_ID",
    "ComponentToWorld": "SceneComponent::COMPONENT_TO_WORLD",
    "ComponentToWorld_Alt": "SceneComponent::COMPONENT_TO_WORLD",
    "LastRenderTime": "PrimitiveComponent::LAST_RENDER_TIME",
    "LastRenderTimeOnScreen": "PrimitiveComponent::LAST_RENDER_TIME_ON_SCREEN",
    "GameInstanceXorKey0Rva": "GameInstanceDecrypt::XOR_KEY_RVA",
    "GameInstanceXorKey1Rva": "GameInstanceDecrypt::XOR_KEY_RVA_2",
    "GameInstanceShuffleMaskRva": "GameInstanceDecrypt::SHUFFLE_MASK_RVA",
    "GameInstance_WorldBackRef": "GameInstanceDecrypt::WORLD_OFFSET",
    "GetObjectIdSimdMaskRva": "GetObjectIdCrypto::SIMD_MASK_RVA",
    "ObjectXorKeyRva": "ObjectXorKey::RVA",
    "XmmXorVal": "Crypto::XMM_XOR_VAL",
    "BoneV922SeedOffset": "BoneArrayDecrypt::SEED_OFFSET",
    "BoneV922SelectorOffset": "BoneArrayDecrypt::SELECTOR_OFFSET",
    "BoneV922SelectorShift": "BoneArrayDecrypt::SELECTOR_SHIFT",
    "BoneV922SelectorMask": "BoneArrayDecrypt::SELECTOR_MASK",
    "BoneV922DescriptorBase": "BoneArrayDecrypt::DESCRIPTOR_BASE",
    "BoneV922DescriptorStride": "BoneArrayDecrypt::DESCRIPTOR_STRIDE",
    "BoneV922XorKeyLo": "BoneArrayDecrypt::XOR_KEY_LO",
    "BoneV922AddKeyLo": "BoneArrayDecrypt::ADD_KEY_LO",
    "BoneV922Rol32": "BoneArrayDecrypt::ROL32_AMOUNT",
    "BoneV922Stride": "BoneArrayDecrypt::BONE_STRIDE",
    # crypto / chunks-manager block: the value match is what found these, the
    # alias records that a human read the two names and agreed they are the
    # same quantity, so the drop becomes their source instead of a bare literal.
    "GameInstanceStaticXorMask": "GameInstanceStaticDecrypt::XOR_MASK",
    "GameInstanceStaticAdd": "GameInstanceStaticDecrypt::ADD64",
    "GameInstanceStaticRot1": "GameInstanceStaticDecrypt::ROT64_1",
    "GameInstanceStaticRot2": "GameInstanceStaticDecrypt::ROT64_2",
    "TebKeyOff": "TebDecrypt::TEB_KEY_OFF",
    "ChunksManagerRva": "FName::RVA_CHUNKMGR_GLOBAL",
    "ChunksManagerXorRva": "FName::RVA_CHUNKMGR_XOR",
    "ChunksManagerAddRva": "FName::RVA_CHUNKMGR_ADD",
    "ChunksManagerRol32": "FName::CHUNKMGR_ROL32",
    "ChunksManagerNumElementsOff": "FName::MGR_NUMELEMENTS_OFF",
    "ChunksManagerNumElementsXor": "FName::MGR_NUMELEMENTS_XOR",
    "ChunksManagerArrayOff": "FName::MGR_CHUNKARRAY_OFF",
    "ChunksManagerArrayXor": "FName::MGR_CHUNKARRAY_XOR",
    "FNameShardSeedOff": "FName::SHARD_HASH_SEED_OFF",
    "FNameShardBlockBase": "FName::SHARD_BLOCK_BASE_OFF",
    "UObject_InternalIndex": "FName::UOBJECT_INTERNAL_IDX",
    "FNameEntry_WideBit": "FName::HDR_IS_WIDE_BIT",
    "GObjects_ElementsPerChunk": "FName::ITEMS_PER_CHUNK",
    "GObjects_ItemStride": "FName::FUOBJECTITEM_STRIDE",
    "GObjects_Item_Object": "FName::FUOBJECTITEM_OBJ_OFF",
    "GameStateGlobalRva": "GameState::GAME_STATE_GLOBAL_RVA",
    "FField_ClassPrivate": "FField::CLASS_PRIVATE",
    "TebSelfPtrOff": "WineTeb::SELF_PTR",
    "ExtractionPoint_State": "ExtractionPoint::STATE",
    "LockedFOV": "PlayerCameraManager::LOCKED_FOV",
}

# Worst first: this is the order the report prints in. 0/1 fail `--check`.
RANKS = [
    ("SHOULD-CHANGE", 0),
    ("CONFLICT", 1),
    ("TIE-RESOLVED", 2),
    ("INAPPLICABLE", 3),
    ("BOTH", 4),
    ("RETIRED", 5),
    ("OVERRULED", 6),
    ("PROBED", 7),
    ("AGREE", 8),
]

# Words that disqualify a drop entry: the drop's own comments use these for
# values it could not confirm.
DROP_UNTRUSTED = ("wrong", "estimated", "needs", "unverified", "heuristic",
                  "re-verify", "reverify", "not verified", "re-scan")

# Provenance markers in a shipped constant's comment.
LIVE_MARKERS = ("live-verified", "live-pinned", "user-verified", "user-provided",
                "live verified", "live-pinned slot", "live override")
DERIVED_MARKERS = ("derived",)
PROBED_MARKERS = ("no dump source", "estimated", "heuristic", "guess", "probed",
                  "previous build", "needs re-scan", "needs verification")


def normalise(text):
    return re.sub(r"[^A-Z0-9]", "", (text or "").upper())


def camel_tokens(name):
    parts = re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+", name or "")
    return [p for p in parts if len(p) > 2]


# Tokens so common in this codebase that sharing one proves nothing on its own:
# `GObjects_ItemStride 0x18` must not be corroborated by `MeshBoneInfo::STRIDE`.
GENERIC_TOKENS = {
    "stride", "size", "offset", "mask", "count", "index", "base", "ptr",
    "off", "data", "item", "value", "add", "xor", "ror", "rol", "rot",
    "shift", "key", "rva", "new", "old", "max", "min", "num", "byte",
    "bits", "type", "flag", "flags", "state", "num", "seconds", "time",
}

# Tokens that mean the constant describes a *transformed* view of a field:
# an encrypted offset, its key, its bswap. The drop retired the whole render
# time XOR scheme on 2026-09-09, so a value landing on the drop's plain field
# proves nothing about the Enc/Key row - that is a different quantity.
TRANSFORM_TOKENS = {"enc", "encrypted", "decrypt", "encrypt", "xor",
                    "key", "keys", "blend", "crypt"}

# How the drop says a constant is no longer live.
RETIRED_MARKERS = ("retired", "for history only", "no xor", "no longer",
                   "deprecated")


# ── Offsets.h: shipped values ────────────────────────────────────────────────
DECL = re.compile(
    r"^\s*constexpr\s+(?P<type>[A-Za-z_:<>\d\s]+?)\s+(?P<name>\w+)\s*=\s*"
    r"(?P<rhs>.+?);\s*//\s*(?P<comment>.*)$")

_SDK_HPP = {"loaded": False, "values": {}}


def sdk_header_value(name):
    if not _SDK_HPP["loaded"] and os.path.exists(SDK_HPP):
        text = open(SDK_HPP, encoding="utf-8").read()
        for m in re.finditer(r"constexpr\s+\w+\s+(\w+)\s*=\s*(0[xX][0-9A-Fa-f]+)", text):
            _SDK_HPP["values"][m.group(1)] = int(m.group(2), 16)
        _SDK_HPP["loaded"] = True
    return _SDK_HPP["values"].get(name)


def unparsed_constants(path, parsed):
    """Declarations in Offsets.h that score() never saw.

    A constant the report cannot parse would silently disappear from every
    bucket, so it is a failure rather than a skip."""
    missing = []
    for lineno, line in enumerate(open(path, encoding="utf-8"), 1):
        if not re.match(r"^\s*constexpr\b", line):
            continue
        m = re.match(r"^\s*constexpr\s+[A-Za-z_:<>\d\s]+?\s+(\w+)\s*=", line)
        if not m or (m.group(1) not in parsed and "(" not in line):
            missing.append((lineno, line.strip()[:96]))
        elif m.group(1) not in parsed:
            missing.append((lineno, line.strip()[:96]))
    return missing


def parse_offsets(path):
    raw, order = {}, []
    for line in open(path, encoding="utf-8"):
        m = DECL.match(line)
        if not m:
            continue
        cm = m.group("comment")
        tm = re.search(r"\[([a-z/]+)\]", cm)
        raw[m.group("name")] = {
            "type": m.group("type").strip(),
            "rhs": m.group("rhs").strip(),
            "comment": cm,
            "tag": tm.group(1) if tm else "",
            "value": None,
        }
        order.append(m.group("name"))

    for _ in range(6):
        progress = False
        for name in order:
            if raw[name]["value"] is None:
                value = resolve_rhs(raw[name]["rhs"], raw)
                if value is not None:
                    raw[name]["value"] = value
                    progress = True
        if not progress:
            break
    return raw, order


def resolve_rhs(rhs, table):
    m = re.search(r"game::offsets::(\w+)", rhs)
    if m:
        return sdk_header_value(m.group(1))

    def sub(mm):
        token = mm.group(0)
        entry = table.get(token)
        if entry and entry["value"] is not None:
            return str(entry["value"])
        if re.fullmatch(r"0[xX][0-9A-Fa-f]+", token):
            return str(int(token, 16))
        return token

    expr = re.sub(r"\b[A-Za-z_]\w*\b|0[xX][0-9A-Fa-f]+|\b\d+\b", sub, rhs)
    expr = re.sub(r"static_cast<[^>]*>", "", expr)
    expr = expr.replace("ULL", "").replace("ull", "")
    if re.search(r"[A-Za-z_]{2,}", expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - digits only
    except Exception:
        return None


# ── sources ──────────────────────────────────────────────────────────────────
class Dump:
    """The FrostDumper property index: the only source that sees reflected
    properties, their masks, and every class's real size."""

    def __init__(self, path=INDEX):
        self.entries = {}
        self.count = 0
        if os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                idx = json.load(fh)
            for bucket in ("classes", "structs"):
                self.count += len(idx.get(bucket, {}))
                for key, entry in idx.get(bucket, {}).items():
                    self.entries[key] = entry
                    self.entries.setdefault(key.rsplit(".", 1)[-1], entry)
        self.props = {}
        self.props_full = {}
        self.by_short = {}
        # `PioneerPlayerState_CurrentPawn` is the dump's way of namespacing a
        # property inside a class; the drop calls the same field `CURRENT_PAWN`.
        self.by_tail = {}
        for key, entry in self.entries.items():
            if "." in key:
                self.by_short.setdefault(key.rsplit(".", 1)[-1].lower(), []).append(
                    (key, entry))
            for pname, prop in entry.get("props", {}).items():
                if pname.startswith("Pad_") or pname.endswith("__Item"):
                    continue
                self.props.setdefault(normalise(pname), []).append((key, prop.get("off")))
                self.props_full.setdefault(normalise(pname), []).append(
                    (key, pname, prop, entry.get("size")))
                if "_" in pname:
                    self.by_tail.setdefault(
                        normalise(pname.rsplit("_", 1)[-1]), []).append(
                            (key, pname, prop, entry.get("size")))

    def class_groups(self, segment, include_contains=False):
        """Class candidates for a drop namespace segment, most precise first.

        The drop names a class the way it feels like: `World` for
        `Engine.World`, `UWorld` for the same class, `GameState` for either
        `Engine.GameState` or a game subclass. Each spelling is a separate
        group, and a resolution has to hold inside one group, so an exact name
        always wins over a suffix one. Groups are only ever *named* and
        uniqueness-checked at the point of use.
        """
        probes = [segment]
        if segment[:1] in "UAF" and len(segment) > 1:
            probes.append(segment[1:])
        groups = []
        for probe in probes:
            exact = self.by_short.get(probe.lower(), [])
            if exact:
                groups.append(exact)
            suffix = probe.lower()
            ends = [(key, entry) for short, items in self.by_short.items()
                    if short != suffix and short.endswith(suffix)
                    for key, entry in items]
            if ends:
                groups.append(ends)
            # The drop's buckets are also named after the class *family*
            # (`ExtractionPoint` for `Angelscript.SalvageExtractionPointBase`),
            # which matches too many classes to stand on its own: it is only
            # used when the project row names the class it took its value from,
            # so a source is never invented for a row that does not have one.
            if include_contains:
                inside = [(key, entry) for short, items in self.by_short.items()
                          if suffix in short and not short.endswith(suffix)
                          for key, entry in items]
                if inside:
                    groups.append(inside)
        return groups

    def short_names(self, segment):
        """Every class any spelling of this segment can refer to."""
        return [pair for group in self.class_groups(segment) for pair in group]

    def array_count(self, keys, name):
        """`X_COUNT` / `X_NUM` is the Num field of the dump's TArray X: data+0x8."""
        if not name.endswith(("_COUNT", "_NUM")):
            return None, None, ""
        want = normalise(name.rsplit("_", 1)[0])
        for pnorm, items in self.props_full.items():
            if want not in (pnorm, pnorm + "S") and pnorm not in (want, want + "S"):
                continue
            for cls, prop, info, _size in items:
                if cls in keys and "TArray" in str(info.get("type")):
                    return ("%s.%s + 0x8 (array count)" % (cls, prop),
                            (info["off"] or 0) + 8, "array count")
        return None, None, ""

    def _resolve_property(self, keys, base, name, want_mask):
        """(label, value, kind) for the property, inside this one class group."""
        for prop_name in (base, name):
            if not prop_name:
                continue
            hits = [h for h in self.props_full.get(normalise(prop_name), [])
                    if h[0] in keys]
            if hits:
                if want_mask:
                    masked = next((h for h in hits if h[2].get("mask")), None)
                    if not masked:
                        return None
                    return ("%s.%s mask" % (masked[0], masked[1]),
                            masked[2]["mask"], "mask")
                offsets = set(h[2].get("off") for h in hits)
                if len(offsets) != 1:
                    return None
                cls, prop, info, _size = hits[0]
                label = "%s.%s" % (cls, prop)
                if len(set(h[0] for h in hits)) > 1:
                    label += " (all %d classes here)" % len(set(h[0] for h in hits))
                return label, info.get("off"), "property"
            tails = [h for h in self.by_tail.get(normalise(prop_name), [])
                     if h[0] in keys]
            if len(tails) == 1:
                cls, prop, info, _size = tails[0]
                return ("%s.%s" % (cls, prop), info.get("off"),
                        "property (class-prefixed name)")
        return None

    def counterpart(self, drop_path, prefer_class=None):
        """The dump property a drop constant should describe, named exactly.

        Returns (label, offset, kind), or (None, None, reason) when the dump has
        no view of the slot - a namespace this build does not have, a property
        no class reflects, or two classes reflecting it at different offsets.
        Never a guess: the label is the class and property the value is checked
        against, so a conflict can always point at what disagrees with what.
        """
        parts = drop_path.split("::")
        name = parts[-1]
        want_mask = name.endswith("_MASK")
        base = name[:-5] if want_mask else name
        if not self.by_short:
            return None, None, "no reflected dump indexed"
        segment, groups = None, []
        for seg in reversed(parts[:-1]):
            found = self.class_groups(seg, include_contains=bool(prefer_class))
            if found:
                segment, groups = seg, found
                break
        if not groups:
            return None, None, "namespace %s has no class in the dump" % parts[0]
        for group in groups:
            keys = set(key for key, _ in group)
            if prefer_class:
                # the project row records the class its value came from, and a
                # bucket namespace can name several (`ExtractionPoint` covers a
                # dozen classes): that recorded class picks between them
                named = [k for k in keys if k == prefer_class
                         or k.endswith("." + prefer_class)]
                if named:
                    keys = set(named)
            hit = self._resolve_property(keys, base, name, want_mask)
            if hit:
                return hit
            label, value, kind = self.array_count(keys, name)
            if label:
                return label, value, kind
        # a base class's field that the drop attributes to the subclass
        hits = self.props_full.get(normalise(base), [])
        if hits and len(set(h[0] for h in hits)) == 1:
            cls, prop, info, _size = hits[0]
            return ("%s.%s (inherited)" % (cls, prop), info.get("off"),
                    "property (base class)")
        return None, None, "no %s.*.%s in the dump" % (segment, base)

    def lookup(self, spec):
        """prop:<Class>.<Property> -> (offset, mask, class size)"""
        if not spec.startswith("prop:"):
            return None, None, None
        cls, _, prop = spec[5:].rpartition(".")
        entry = self.entries.get(cls)
        if entry and prop in entry.get("props", {}):
            p = entry["props"][prop]
            return p.get("off"), p.get("mask"), entry.get("size")
        return None, None, None

    def expect(self, spec):
        """The value the dump itself gives for this spec, whatever kind it is:
        a reflected offset, a packed-bool mask, or a class size. Never a guess."""
        kind, _, arg = spec.partition(":")
        if kind == "prop":
            return self.lookup(spec)[0], "reflected property %s" % arg
        if kind == "maskOf":
            cls, _, prop = arg.rpartition(".")
            entry = self.entries.get(cls)
            if entry and prop in entry.get("props", {}):
                return entry["props"][prop].get("mask"), "%s.%s mask" % (cls, prop)
            return None, ""
        if kind == "sizeof":
            return self.size_of(arg), "sizeof(%s)" % arg
        return None, ""

    def class_named(self, name):
        entry = self.entries.get(name)
        if entry and "size" in entry:
            return entry
        return None

    def class_candidates(self, name):
        """Every class/struct the drop's spelling of this name can refer to.
        Several can share one (`GameState` the engine class,
        `Angelscript.PioneerGameState` the game one), and a size verdict has to
        hold for all of them."""
        seen, out = set(), []
        for key, entry in self.short_names(name):
            if not entry.get("size") or id(entry) in seen:
                continue
            seen.add(id(entry))
            out.append((key, entry))
        return out

    def size_of(self, name):
        entry = self.entries.get(name)
        return entry.get("size") if entry else None

    def hint(self, text):
        """A dump property the row itself names in prose (`Class.Prop`, or one
        segment of a fully-qualified `Pkg.Class.Prop`).

        A literal that records where it came from is a claim the dump can check:
        if a row says `Angelscript.PioneerGameState.EnemyCount` and ships a
        different offset, the dump wins. Without a resolvable hint nothing in
        the row's name points at a class, so the literal stands.
        """
        for run in re.finditer(r"\w+(?:\.\w+)+", text or ""):
            parts = run.group(0).split(".")
            for i in range(len(parts) - 1):
                entry = self.entries.get(parts[i])
                if entry and parts[i + 1] in entry.get("props", {}):
                    prop = entry["props"][parts[i + 1]]
                    return "%s.%s" % (parts[i], parts[i + 1]), prop.get("off")
        return None, None

    def property_at(self, prop, offset):
        """A class that reflects `prop` at `offset` - independent corroboration
        for a drop constant that names a property."""
        return next((c for c, off in self.props.get(normalise(prop), []) if off == offset), None)


class Drop:
    """sdk/sdk.txt, the CL drop, read through the one shared parser."""

    def __init__(self, path=DROP):
        self.entries = sdk_drop.parse(path)
        self.by_norm = sdk_drop.by_name(self.entries)
        self.by_value = sdk_drop.by_value(self.entries)
        self.by_suffix = {}
        for path_key in self.entries:
            self.by_suffix[path_key.split("::", 1)[-1]] = path_key

    def alias(self, target):
        """ALIASES stores a suffix; resolve it against the real paths."""
        if target in self.entries:
            return target
        return self.by_suffix.get(target)


def drop_authority(entry):
    """(rank, label) for a drop entry, judged by the drop's own comment."""
    comment = entry["comment"].lower()
    if any(word in comment for word in DROP_UNTRUSTED):
        return 1, "drop-unconfirmed"
    return (4, "drop-fresh") if entry["fresh"] else (3, "drop")


def namespace_counterpart(drop_path, dump):
    """The dump classes a drop entry's namespace refers to.

    `ArcOffsets::UWorld::GAME_INSTANCE` names UWorld; `Pickup::UI_HOVER_DATA`
    names APickup -> Angelscript.Pickup. Bucket namespaces such as `Global`
    or `Crypto` have no counterpart, which is itself the verdict: nothing in
    the dump can confirm or deny them. A short name can match several classes,
    so every candidate is returned and a size verdict has to hold for all."""
    for segment in reversed(drop_path.split("::")[:-1]):
        candidates = dump.class_candidates(segment)
        if candidates:
            return segment, candidates
    return None, []


# ── scoring ──────────────────────────────────────────────────────────────────
def provenance(name, spec, comment, rhs, table, memo):
    """(rank, label) for the shipped value, from its spec + comment."""
    if name in memo:
        return memo[name]
    memo[name] = (1, "probed")          # cycle guard
    kind = spec.split(":", 1)[0] if spec else ""
    text = comment.lower()
    if any(marker in text for marker in LIVE_MARKERS):
        result = (6, "live-pinned")
    elif kind in ("prop", "sizeof", "maskOf"):
        result = (5, "dump")
    elif kind == "sdk":
        result = (4, "header-drop")
    elif kind == "drop":
        result = (3, "drop-file")
    elif kind == "expr":
        # Offsets.h has the expression folded to literals, so read the generator
        # spec to find which constant it was derived from.
        expression = spec.split(":", 1)[1] if ":" in spec else rhs
        base = re.search(r"\b([A-Za-z_]\w*)\b", expression)
        if base and base.group(1) in table and "spec" in table[base.group(1)]:
            rank, label = provenance(base.group(1), table[base.group(1)]["spec"],
                                     table[base.group(1)]["comment"],
                                     table[base.group(1)]["rhs"], table, memo)
            result = (rank, label + "+derived")
        else:
            result = (2, "derived")
    elif any(marker in text for marker in DERIVED_MARKERS):
        result = (2, "derived")
    else:
        result = (1, "probed")
    memo[name] = result
    return result


def score(offsets, order, specs, dump, drop):
    """One row per constant: shipped value, provenance, verdict, evidence."""
    shipped_by_value = {}
    for name, entry in offsets.items():
        if entry["value"] is not None:
            shipped_by_value.setdefault(entry["value"], []).append(name)

    memo, rows, used_drop = {}, [], set()
    for name in order:
        entry = offsets[name]
        spec, section = specs.get(name, ("", ""))
        shipped = entry["value"]
        rank, label = provenance(name, spec, entry["comment"], entry["rhs"], offsets, memo)

        dump_value, dump_mask, class_size = dump.lookup(spec)
        dump_expect, dump_what = dump.expect(spec)
        if dump_expect is None:
            # a literal/derived row that records the property it came from is
            # still checkable: its own note is the dump's claim on the slot
            hint, hint_off = dump.hint(spec + " " + entry["comment"])
            if hint and hint_off is not None:
                dump_expect, dump_what = hint_off, "%s in the dump" % hint
        drop_path = None
        notes = []

        if shipped is not None:
            drop_path, _how = match_drop(name, drop, dump, shipped)
            if drop_path:
                used_drop.add(drop_path)
        drop_entry = drop.entries[drop_path] if drop_path else None
        drop_rank, drop_label = drop_authority(drop_entry) if drop_entry else (0, "none")

        # values that agree are settled first - including a renamed drop slot
        state = None
        if name in _LIVE_OVERRIDES:
            state = "AGREE"
            notes.append(_LIVE_OVERRIDES[name])
        elif dump_expect is not None and shipped == dump_expect:
            state = "AGREE"
            notes.append("dump: %s = 0x%X" % (dump_what, dump_expect))
        elif drop_entry is not None and shipped == drop_entry["value"] \
                and (dump_expect is None or dump_expect == shipped):
            # a drop entry agreeing with the shipped value cannot settle a row
            # the dump contradicts: the dump outranks the drop
            state = "AGREE"
            notes.append("confirmed by %s (%s)" % (drop_label, drop_path))
        elif state is None and shipped is not None:
            vmatch, vpaths = find_value_match(name, shipped, drop)
            if vmatch:
                state = "AGREE"
                rank, label = drop_authority(drop.entries[vmatch])[0], "drop-value"
                used_drop.update(vpaths)
                drop_path = drop_path or vmatch
                notes.append("drop carries this value as %s (renamed slot)"
                             % ", ".join(vpaths[:2]))
            elif name.endswith(("Size", "_Size")):
                cls = re.sub(r"^(?:U|A|F)", "", re.sub(r"_?[Ss]ize$", "", name))
                size = dump.size_of(cls)
                if size is not None and size == shipped:
                    state = "AGREE"
                    rank, label = max(rank, 5), "dump-size"
                    notes.append("sizeof(%s) in the dump" % cls)
            else:
                plain, prose = transformed_view(name, shipped, drop)
                if plain:
                    state = "RETIRED"
                    notes.append(
                        "the drop retired this: 0x%X is its plain %s, and the drop "
                        "says %s" % (shipped, plain.rsplit("::", 1)[-1],
                                     prose.strip()[:90] or "the transform is gone"))

        if state is None and dump_expect is not None:
            state = "SHOULD-CHANGE"
            notes.append("dump says 0x%X (%s%s)"
                         % (dump_expect, dump_what,
                            ", packed mask" if dump_what.endswith("mask") else ""))
        elif state is None and drop_entry is None:
            state = "PROBED"
            notes.append({"sdk": "from SDK.hpp game::offsets",
                          "expr": "derived expression"}.get(
                              spec.split(":", 1)[0], "no offline source can see this slot"))
        elif state is None:
            state, verdict = judge_drop(drop_path, drop_entry, drop_rank, drop_label,
                                        rank, label, dump, shipped)
            notes.append(verdict)
            others = [n for n in shipped_by_value.get(drop_entry["value"], [])
                      if n != name and (n.startswith(name + "_")
                                        or name.startswith(n + "_"))]
            if others and state in ("SHOULD-CHANGE", "CONFLICT"):
                state = "BOTH"
                notes.append("0x%X also ships as %s - a runtime probe decides"
                             % (drop_entry["value"], ", ".join(others)))
            elif state == "SHOULD-CHANGE":
                corroborated = dump.property_at(drop_path.rsplit("::", 1)[-1],
                                                drop_entry["value"])
                if corroborated:
                    notes.append("the dump reflects %s at 0x%X too"
                                 % (corroborated, drop_entry["value"]))

        rows.append({
            "name": name, "section": section, "spec": spec, "shipped": shipped,
            "state": state, "tag": entry["tag"], "rank": rank,
            "provenance": "%s(%d)" % (label, rank),
            "dump": dump_value, "drop_path": drop_path,
            "drop_value": drop_entry["value"] if drop_entry else None,
            "notes": notes,
        })
    return rows, used_drop


def relevant_namespace(path, name):
    """Does the drop constant's namespace appear in the project constant's name?

    Without this, `Offsets::UWorld` maps onto `GameViewportClient::WORLD` - the
    U-prefix strip leaves the bare word WORLD, which matches by name alone and
    would credit a live GWorld RVA to the wrong class. With it, a match needs
    the owning class in the project name, exactly like `Pickup_RootCollider`
    for `Pickup::ROOT_COLLIDER`.
    """
    own = set(t.lower() for t in camel_tokens(name))
    return bool(own & namespace_tokens(path)) if own else False


def pick_by_dump(paths, dump, shipped):
    """Of several same-named drop entries, the one the dump says this row is.

    The drop carries the same property on more than one class
    (`AUTHORITY_GAME_MODE` on both UWorld and GameState). Each candidate's
    reflected property is compared with the shipped value; a single winner is
    taken and a tie stays ambiguous - the namespace token is the fallback, not
    the verdict.
    """
    if dump is None or shipped is None or len(paths) < 2:
        return None
    scored = []
    for path in paths:
        _label, value, _kind = dump.counterpart(path)
        scored.append((0 if value == shipped else (1 if value is not None else 2),
                       path))
    scored.sort()
    # only a candidate the dump *confirms* can win, and only outright: a
    # namespace whose entries merely have counterparts somewhere else
    # (`LevelCollection::GAME_STATE` for a row aliased to `LevelCollections`)
    # must not be picked just for having one
    if scored[0][0] == 0 and scored[0][0] < scored[1][0]:
        return scored[0][1], "name+dump"
    return None


def match_drop(name, drop, dump=None, shipped=None):
    """(path, how) for the drop entry this constant maps onto."""
    if name in ALIASES:
        path = drop.alias(ALIASES[name])
        if path:
            return path, "alias"
    key = normalise(name)
    exact = drop.by_norm.get(key, [])
    picked = pick_by_dump(exact, dump, shipped)
    if picked:
        return picked
    candidates = [p for p in exact if relevant_namespace(p, name)]
    if len(candidates) == 1:
        return candidates[0], "name"
    if len(key) > 1 and key[0] in "AUF" and not candidates:
        stripped = drop.by_norm.get(key[1:], [])
        usable = [p for p in stripped if relevant_namespace(p, name)]
        if len(usable) == 1:
            return usable[0], "name-stripped"
    # <Class>_<PROPERTY>: the project spells the drop's `Class::PROPERTY` with an
    # underscore, and `normalise` strips the underscore, so the split has to
    # happen on the raw name - otherwise the whole name is looked up and
    # `Pickup_RootCollider` never finds `Pickup::ROOT_COLLIDER`. The property
    # has to match in full (not as a tail) and the class token has to appear in
    # the project name, so a bare word cannot adopt an unrelated constant.
    tail = normalise(name.rsplit("_", 1)[-1]) if "_" in name else ""
    suffix = [p for p in drop.by_norm.get(tail, [])
              if tail == normalise(p.rsplit("::", 1)[-1])
              and relevant_namespace(p, name)]
    if len(suffix) == 1:
        return suffix[0], "name-suffix"
    return None, "ambiguous" if (candidates or suffix) else "absent"


def find_value_match(name, value, drop):
    """drop entry carrying this value under a renamed constant.

    A match needs the drop entry's tokens to be a *subset* of the project
    constant's - and at least one of them to be distinctive. That is what
    rejects the coincidences: `UObject_ObjectFlags 0x8` is not corroborated by
    `UniqueNetIdRepl::NET_ID_OBJECT`, and `GObjects_ItemStride 0x18` is not
    corroborated by `MeshBoneInfo::STRIDE`, while `FField_ClassPrivate 0x60`
    is corroborated by `FField::CLASS_PRIVATE`.
    """
    own = set(t.lower() for t in camel_tokens(name))
    if not own:
        return None, []
    paths = []
    for path in drop.by_value.get(value, []):
        rank, _ = drop_authority(drop.entries[path])
        if rank < 3:
            continue
        theirs = set(t.lower() for t in camel_tokens(path.rsplit("::", 1)[-1]))
        if not theirs or not theirs <= own or not (theirs - GENERIC_TOKENS):
            continue
        # a transformed quantity (Enc/Key/Xor) is not the plain field that
        # happens to share its offset. Tokens the namespace already carries
        # (`PlayerDecrypt::FINAL_ROT`) are not evidence of a transform.
        if (own - theirs - namespace_tokens(path)) & TRANSFORM_TOKENS \
                and not (theirs & TRANSFORM_TOKENS):
            continue
        paths.append(path)
    return (paths[0], paths) if paths else (None, [])


def namespace_tokens(path):
    """Every token the drop's own namespace contributes to a path, so the
    namespace name cannot be mistaken for a marker of the quantity."""
    return set(t.lower() for segment in path.split("::")[:-1]
               for t in camel_tokens(segment))


def transformed_view(name, value, drop):
    """(plain_path, block_comment) when this constant looks like a transformed
    view of a drop field the drop itself retired."""
    own = set(t.lower() for t in camel_tokens(name))
    for path in drop.by_value.get(value, []):
        theirs = set(t.lower() for t in camel_tokens(path.rsplit("::", 1)[-1]))
        if not theirs or not theirs <= own:
            continue
        if (own - theirs - namespace_tokens(path)) & TRANSFORM_TOKENS:
            entry = drop.entries[path]
            text = (entry["comment"] + " " + entry.get("block_comment", "")).lower()
            if any(marker in text for marker in RETIRED_MARKERS):
                return path, entry.get("block_comment", "")
    return None, ""


# Verdicts for the drop-vs-dump-vs-Offsets.h axis. Each one names the source
# that decides the row; none of them is a judgement call.
DIFF_ORDER = ["DROP-CONFLICT", "PROJECT-STALE", "OUT-OF-RANGE", "DUMP-WINS",
              "DROP-WINS", "UNCONFIRMED", "STRUCT-RELATIVE", "NO-COMPARABLE",
              "AGREE", "NOT-SHIPPED-DUMP", "NOT-SHIPPED-DROP"]


def drop_diff_rows(offsets, specs, dump, drop):
    """One row per constant in the CL drop: what the drop says, what the dump
    reflects for that slot, what Offsets.h ships, and which one decides."""
    shipped_by_value = {}
    for name, entry in offsets.items():
        if entry["value"] is not None:
            shipped_by_value.setdefault(entry["value"], []).append(name)

    # index the project the same way the shipped-row pass does. A drop constant
    # can be claimed by several rows (the ComponentToWorld pair), so keep them
    # all: the verdict depends on whether *any* of them ships the source value.
    adopted = {}
    for name in offsets:
        spec = specs.get(name, ("", ""))[0]
        claims = []
        path, how = match_drop(name, drop, dump, offsets[name]["value"])
        if path:
            claims.append((path, "name/alias"))
        elif offsets[name]["value"] is not None:
            vmatch, _ = find_value_match(name, offsets[name]["value"], drop)
            if vmatch:
                claims.append((vmatch, "value"))
        if spec.startswith("drop:"):
            claims.append((drop.alias(spec[5:]) or spec[5:], "spec"))
        for path, how in claims:
            adopted.setdefault(path, []).append((name, how))

    rows = []
    for path in sorted(drop.entries):
        entry = drop.entries[path]
        value = entry["value"]
        rank, label = drop_authority(entry)
        # A name match identifies the row a drop constant describes; a value match
        # only corroborates it. So when several project rows carry the same value
        # (ComponentToWorld 0x310 and its Rotation alias, which is 0x310 too), the
        # named row is the subject of the verdict and the value-only claim cannot
        # demote it to a relative field.
        claims = sorted(adopted.get(path, []),
                        key=lambda c: 1 if c[1] == "value" else 0)
        project, how = claims[0] if claims else (None, None)
        # the project row may record the dump class its value was taken from
        # (`prop:Angelscript.Pickup.RootCollider`), which is the only thing that
        # can pick between several classes a bucket namespace matches
        prefer = ""
        for claimed, _how in claims:
            sp = specs.get(claimed, ("", ""))[0]
            if sp.startswith("prop:"):
                prefer = sp[5:].rpartition(".")[0]
                break
        counter, dump_value, kind = dump.counterpart(path, prefer)
        also = [n for n, _ in claims[1:]]
        shipped = offsets[project]["value"] if project else None
        ships_value = project is not None and (
            shipped == value or any(offsets[n]["value"] == value for n in also))
        # a struct-relative constant (FPlayerHealthInfo::HEALTH) describes an
        # offset inside a base the project already carries, so it must not be
        # diffed against an absolute class offset. Only when the dump has no view
        # of its own: with one, the dump names the slot and decides the row.
        derived = project if (project and
                              specs.get(project, ("", ""))[0].startswith("expr:")) else None
        # a row the project derives from a base it carries (`ActorsCount =
        # AActors + 8`) is only comparable when the drop says the same thing: a
        # drop constant that happens to land on a *relative* offset of another
        # struct (`LevelCollection::GAME_STATE = 0x8`) is not this slot.
        if derived and not (counter is not None and dump_value == shipped):
            base = specs[derived][0][5:]
            base_value = offsets[base]["value"] if base in offsets else None
            where = "Offsets.h.%s%s" % (
                base, " = 0x%X" % base_value if base_value is not None
                else " (unsourced)")
            if shipped == value:
                slot = "and the drop's 0x%X is that same slot" % value
            elif counter:
                slot = "while the drop's 0x%X is a relative field of %s (the dump " \
                       "puts it at 0x%X), not this row's absolute slot" % (
                           value, counter, dump_value)
            else:
                slot = "and the drop's 0x%X describes a relative field, with no " \
                       "dump counterpart to compare against" % value
            rows.append({
                "path": path, "namespace": path.split("::")[-2],
                "name": path.rsplit("::", 1)[-1], "value": value,
                "rank": rank, "label": label, "line": entry["line"],
                "counterpart": "%s (struct-relative via Offsets.h.%s)" % (
                    path.split("::")[-2], derived),
                "dump": None, "counterpart_kind": "struct-relative",
                "project": derived, "match": how,
                "shipped": offsets[derived]["value"],
                "verdict": "STRUCT-RELATIVE",
                "why": "the row is %s; %s" % (where, slot),
                "comment": entry["comment"],
            })
            continue
        # No fallback to the project row's own spec: naming a source the drop
        # constant does not describe would manufacture a comparison (the
        # `FPlayerHealthInfo::HEALTH` struct field is not `HealthComponent.Health`).
        # With no dump counterpart the row says exactly that instead.
        hedge = "the drop marks this %s" % (entry["comment"][:44] or "unconfirmed")
        if ships_value:
            # the drop's own hedge is not evidence against a source that agrees
            if dump_value is None:
                verdict = "AGREE"
                why = "shipped as Offsets.h.%s%s (no contrary source)" % (
                    project, " and " + ", ".join(also) if also else "")
            elif dump_value == value:
                verdict = "AGREE"
                why = "dump confirms %s = 0x%X (Offsets.h.%s agrees)" % (
                    counter, value, project)
            else:
                verdict = "DUMP-WINS"
                why = "Offsets.h.%s follows the drop's 0x%X; the dump reflects " \
                      "%s = 0x%X" % (project, value, counter, dump_value)
        elif project is None:
            if dump_value is None:
                verdict = "UNCONFIRMED" if rank < 3 else "NOT-SHIPPED-DROP"
                why = ("drop only, %s; %s" % (hedge, kind) if rank < 3
                       else "drop only (%s); %s" % (label, kind))
            elif dump_value == value:
                verdict = "NOT-SHIPPED-DUMP"
                why = "nothing ships this; the dump reflects %s = 0x%X" % (
                    counter, dump_value)
            else:
                verdict = "DROP-WINS"
                why = "not shipped; dump says %s = 0x%X, drop says 0x%X" % (
                    counter, dump_value, value)
        elif dump_value == value:
            # both offline sources name the slot and agree; the shipped value is
            # the odd one out, whatever the drop says about its own freshness
            verdict = "PROJECT-STALE"
            why = "Offsets.h.%s = 0x%X but drop and dump both say 0x%X (%s)" % (
                project, shipped, value, counter)
        elif dump_value is not None and shipped == dump_value:
            verdict = "DUMP-WINS"
            why = "Offsets.h.%s follows %s = 0x%X; the drop is stale" % (
                project, counter, dump_value)
        elif counter is None:
            verdict = "NO-COMPARABLE"
            why = "Offsets.h.%s = 0x%X and the drop's 0x%X has no counterpart " \
                  "here: %s" % (project, shipped, value, kind)
        elif rank < 3:
            verdict = "UNCONFIRMED"
            why = "%s; dump %s = 0x%X, Offsets.h.%s = 0x%X" % (
                hedge, counter, dump_value, project, shipped)
        elif dump_value is None:
            verdict = "PROJECT-STALE"
            why = "Offsets.h.%s = 0x%X, no dump property, drop says 0x%X" % (
                project, shipped, value)
        else:
            verdict = "DROP-CONFLICT"
            why = "Offsets.h.%s = 0x%X, dump %s = 0x%X, drop = 0x%X" % (
                project, shipped, counter, dump_value, value)

        rows.append({
            "path": path, "namespace": path.split("::")[-2],
            "name": path.rsplit("::", 1)[-1], "value": value,
            "rank": rank, "label": label, "line": entry["line"],
            "counterpart": counter, "dump": dump_value, "counterpart_kind": kind,
            "project": project, "match": how, "shipped": shipped,
            "verdict": verdict, "why": why, "comment": entry["comment"],
        })
    order = {v: i for i, v in enumerate(DIFF_ORDER)}
    rows.sort(key=lambda r: (order.get(r["verdict"], 99), r["path"]))
    return rows, adopted


def judge_drop(drop_path, drop_entry, drop_rank, drop_label, rank, label, dump, shipped):
    """Decide a row where a drop entry exists but does not match the shipped
    value. Returns (state, reason) - never 'ask a human'.

    The drop constant's own dump counterpart is resolved first: when the dump
    reflects the slot, its named property decides the row against both the drop
    and the shipped value, so a stale drop can never out-vote the build the dump
    was taken from.
    """
    namespace, candidates = namespace_counterpart(drop_path, dump)
    value = drop_entry["value"]
    counter, dump_value, _kind = dump.counterpart(drop_path)
    if dump_value is not None:
        if dump_value == shipped:
            return ("OVERRULED", "the dump reflects %s = 0x%X, which is what "
                    "Offsets.h ships; %s says 0x%X and is stale"
                    % (counter, dump_value, drop_label, value))
        if dump_value == value:
            return ("SHOULD-CHANGE", "%s and the dump agree on 0x%X (%s); "
                    "shipped value is %s" % (drop_label, value, counter, label))
        return ("CONFLICT", "three ways: %s says 0x%X, the dump reflects %s = "
                "0x%X, Offsets.h ships %s = %s"
                % (drop_label, value, counter, dump_value,
                   label, "0x%X" % shipped if shipped is not None else "nothing"))

    # A size observation is only ever a note: the drop's namespaces mix classes
    # (its `GameState` bucket carries both AGameStateBase and PioneerGameState
    # fields), so being past one class's end does not disqualify an entry. What
    # decides is what the entry can see, never a guess about which class it meant.
    note = ""
    if candidates and value and all(value >= entry["size"]
                                    for _, entry in candidates):
        note = " (0x%X is past the end of %s, which the drop's %s bucket also "
        note = note % (value, ", ".join("%s=0x%X" % (key, entry["size"])
                                        for key, entry in candidates[:2]), namespace)
        note += "holds)"

    if drop_rank < 3:
        return ("INAPPLICABLE",
                "%s says 0x%X but the drop marks it unconfirmed (%s) - not evidence%s"
                % (drop_label, value, drop_entry["comment"][:40] or "no comment",
                   note))
    if namespace is None:
        return ("INAPPLICABLE",
                "%s says 0x%X in namespace %s, which this build's dump has no "
                "class for - name collision, not this slot"
                % (drop_label, value, drop_path.split("::")[0]
                   if "::" in drop_path else drop_path))
    if drop_rank > rank:
        return ("SHOULD-CHANGE", "%s says 0x%X, shipped value is %s%s"
                % (drop_label, value, label, note))
    if drop_rank == rank and label in ("header-drop", "live-pinned"):
        return ("TIE-RESOLVED",
                "%s says 0x%X, equal rank as the shipped %s; the raw drop is "
                "the single source of truth over the hand-copied header - "
                "port the value or re-mark it live%s" % (drop_label, value, label, note))
    if drop_rank == rank:
        return ("CONFLICT", "%s says 0x%X, shipped value is %s (equal authority, "
                            "both see the slot)%s" % (drop_label, value, label, note))
    return ("OVERRULED", "%s says 0x%X - lower authority than the shipped %s%s"
            % (drop_label, value, label, note))


SOURCES = ("live", "dump", "drop", "header", "derived", "probed")


def source_of(label):
    """Map a provenance label onto the scoreboard source it belongs to.

    The head of the label names the source (`header-drop` is the header, not the
    drop), and a derived constant is credited to what it derives from."""
    head = re.split(r"[-+]", label)[0]
    return head if head in SOURCES else "probed"


def classify(rows):
    """Per-source scoreboard: what each source decided, won and lost."""
    board = {s: {"pins": 0, "wins": 0, "loses": 0, "silent": 0} for s in SOURCES}
    for r in rows:
        state = r["state"]
        if state == "AGREE":
            board[source_of(r["provenance"].split("(")[0])]["pins"] += 1
        elif state == "SHOULD-CHANGE":
            board["drop"]["wins"] += 1
        elif state in ("OVERRULED", "INAPPLICABLE", "TIE-RESOLVED"):
            board["drop"]["loses"] += 1
        elif state == "PROBED":
            board["probed"]["silent"] += 1
    return board


_LIVE_OVERRIDES = {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="print every constant")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 on SHOULD-CHANGE or CONFLICT")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the summary (for build logs)")
    ap.add_argument("--drop-diff", action="store_true",
                    help="per-constant diff of the whole CL drop against "
                         "Offsets.h and the dump")
    ap.add_argument("--report", help="write a markdown report here")
    ap.add_argument("--json", dest="json_out", help="write the scores as JSON here")
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_offsets as go

    _LIVE_OVERRIDES.clear()
    _LIVE_OVERRIDES.update(go.LIVE_OVERRIDES)

    specs = {}
    for title, entries in go.SECTIONS:
        for name, _ctype, spec, _note in entries:
            specs[name] = (spec, title)

    offsets, order = parse_offsets(OFFSETS)
    dump, drop = Dump(), Drop()
    missing = unparsed_constants(OFFSETS, offsets)
    rows, used_drop = score(offsets, order, specs, dump, drop)

    rank_of = dict(RANKS)
    rows.sort(key=lambda r: (rank_of.get(r["state"], 99), r["name"]))

    width = max((len(r["name"]) for r in rows), default=10)
    if not args.quiet:
        print("%-*s %-12s %-13s %-18s %s" % (
            width, "constant", "shipped", "state", "source", "note"))
        print("-" * (width + 72))
        for r in rows:
            if not args.all and r["state"] == "AGREE":
                continue
            shipped = "0x%X" % r["shipped"] if r["shipped"] is not None else "-"
            src = r["drop_path"] or (r["spec"] if r["spec"].startswith("prop:") else r["provenance"])
            print("%-*s %-12s %-13s %-18s %s" % (
                width, r["name"], shipped, r["state"], src, "; ".join(r["notes"])[:104]))

    counts = {}
    for r in rows:
        counts[r["state"]] = counts.get(r["state"], 0) + 1

    board = classify(rows)
    unclaimed = [p for p in drop.entries if p not in used_drop]
    if not args.quiet:
        print()
        print("sources: dump=%d classes/structs  drop=%d constants (%d namespaces)  "
              "sdk.hpp=%s" % (dump.count, len(drop.entries),
                              len(set(e["scope"] for e in drop.entries.values())),
                              "ok" if _SDK_HPP["values"] else "n/a"))
        print("authority: live(6) > dump(5) > drop-fresh(4) = header(4) > drop(3) > "
              "derived(2) > probed(1)")
        print("buckets: " + "  ".join("%s=%d" % (k, counts[k])
                                      for k, _ in RANKS if k in counts))
        print("scoreboard: " + "  ".join(
            "%s[pins=%d win=%d lose=%d]" % (k, v["pins"], v["wins"], v["loses"])
            for k, v in sorted(board.items()) if any(v.values())))
        print("drop constants not referenced by Offsets.h: %d of %d"
              % (len(unclaimed), len(drop.entries)))

    if args.drop_diff:
        diff, _adopted = drop_diff_rows(offsets, specs, dump, drop)
        print()
        print("drop diff: %d constants in sdk/sdk.txt (%d namespaces)" % (
            len(diff), len(set(r["namespace"] for r in diff))))
        print("%-19s %-58s %-13s %-30s %s" % (
            "verdict", "drop constant", "shipped", "source of truth", "Offsets.h"))
        print("-" * 150)
        for r in diff:
            col = "0x%X" % r["value"]
            if r["counterpart"] and r["dump"] is not None:
                src = "%s = 0x%X" % (r["counterpart"], r["dump"])
            else:
                src = "(drop) %s" % (r["counterpart_kind"] or "no dump view")
            print("%-19s %-58s %-13s %-30s %s" % (
                r["verdict"], r["path"][:58], col,
                src[:30], (r["project"] or "-") + (
                    " = 0x%X" % r["shipped"] if r["shipped"] is not None else "")))
        tally = {}
        for r in diff:
            tally[r["verdict"]] = tally.get(r["verdict"], 0) + 1
        print()
        print("drop-diff buckets: " + "  ".join(
            "%s=%d" % (v, tally[v]) for v in DIFF_ORDER if v in tally))

    if args.report:
        write_report(args.report, rows, counts, drop, unclaimed, board, dump.count,
                     drop_diff_rows(offsets, specs, dump, drop)[0])
        print("report: %s" % args.report)
    if args.json_out:
        write_json(args.json_out, rows, counts, board)
        print("scores: %s" % args.json_out)

    if missing:
        print("%d declaration(s) in Offsets.h could not be scored:" % len(missing))
        for lineno, text in missing:
            print("  line %d: %s" % (lineno, text))

    actionable = [r for r in rows if r["state"] in ("SHOULD-CHANGE", "CONFLICT")]
    if missing and args.check:
        print("\nOffsets.h has %d unscored declaration(s) - fix the generator output."
              % len(missing))
        return 1
    if actionable and args.check:
        print("\n%d actionable disagreement(s):" % len(actionable))
        for r in actionable:
            print("  %s = 0x%X: %s" % (r["name"], r["shipped"] or 0, "; ".join(r["notes"])))
        return 1
    return 0


def write_json(path, rows, counts, board):
    payload = {
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "buckets": counts,
        "scoreboard": board,
        "constants": [{k: v for k, v in r.items() if k != "notes"} for r in rows],
    }
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=1, sort_keys=True)


def write_report(path, rows, counts, drop, unclaimed, board, dump_count, diff=None):
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    lines = [
        "# Offset reconciliation", "",
        "Generated %s by `tools/reconcile_offsets.py`." % stamp, "",
        "Every constant in `Project/Core/Offsets.h` is scored against every offline "
        "source. No row needs a human to pick a winner: a disagreement is decided by "
        "the two confidence scores, and `tools/gen_offsets.py --check` fails the "
        "build when a source that can see a slot outranks the shipped value.", "",
        "Sources: the FrostDumper index of the 2026-09-22 build (`tools/sdk_index.json`, "
        "%d classes/structs), the CL drop `sdk/sdk.txt` (%d constants across %d "
        "namespaces), `Project/Core/SDK.hpp` `game::offsets`."
        % (dump_count, len(drop.entries),
           len(set(e["scope"] for e in drop.entries.values()))), "",
        "| bucket | constants |", "| --- | --- |",
    ]
    for key, _ in RANKS:
        if key in counts:
            lines.append("| %s | %d |" % (key, counts[key]))

    lines += ["", "## Authority ladder", "",
              "| score | source | what it can see |", "| --- | --- | --- |",
              "| 6 | live | pinned against the running target; no static source carries it |",
              "| 5 | dump | reflected UPROPERTY offset + packed-bool mask, and every class size |",
              "| 4 | drop (2026-09-22) | the CL drop's own re-checked entry |",
              "| 4 | header | `SDK.hpp` `game::offsets`, the value the project hand-copied |",
              "| 3 | drop | the CL drop's entry left on its older CL value |",
              "| 2 | derived | arithmetic or alias over a higher-ranked constant |",
              "| 1 | probed | nothing offline can see this slot |", "",
              "## Scoreboard", "",
              "| source | pins | wins | loses |", "| --- | --- | --- | --- |"]
    for source, stats in sorted(board.items()):
        if any(stats.values()):
            lines.append("| %s | %d | %d | %d |" % (source, stats["pins"],
                                                    stats["wins"], stats["loses"]))

    def table(title, chosen, intro=""):
        lines.extend(["", "## " + title, ""])
        if intro:
            lines.extend([intro, ""])
        lines.extend(["| constant | shipped | source value | provenance | verdict |",
                      "| --- | --- | --- | --- | --- |"])
        for r in rows:
            if r["state"] not in chosen:
                continue
            src_value = ("0x%X" % r["drop_value"]) if r["drop_value"] is not None else (
                "0x%X" % r["dump"] if r["dump"] is not None else "-")
            lines.append("| %s | %s | %s | %s | %s |" % (
                r["name"],
                "0x%X" % r["shipped"] if r["shipped"] is not None else "-",
                src_value, r["provenance"], "; ".join(r["notes"])))

    table("Fix these (a higher-authority source disagrees)", {"SHOULD-CHANGE", "CONFLICT"},
          "The shipped value is outranked by a source that can see the slot. "
          "`--check` fails until the generator carries the source value.")
    table("Decided without you (the disagreeing source cannot see the slot)",
          {"INAPPLICABLE", "TIE-RESOLVED", "OVERRULED"},
          "The entry is unconfirmed in the drop's own words, points past the end of "
          "the class the dump gives, names a namespace this build does not have, or "
          "ships lower authority than a live-pinned value. The shipped value stands.")
    table("Already resolved in the build (both values ship)", {"BOTH"},
          "The source's value ships under another constant; a runtime probe decides.")
    table("No offline source can see these", {"PROBED"},
          "Native (non-UPROPERTY) fields, data RVAs and crypto constants with no "
          "reflected property and no drop entry. Only the running game can confirm them.")

    if diff:
        tally = {}
        for r in diff:
            tally[r["verdict"]] = tally.get(r["verdict"], 0) + 1
        lines += ["", "## Every constant in the CL drop", "",
                  "%d constants, each with the source that decides it: the dump "
                  "property it should describe (named), the value Offsets.h ships, "
                  "and the resulting verdict. No row needs a judgement call." % len(diff),
                  "", "| verdict | constants |", "| --- | --- |"]
        for v in DIFF_ORDER:
            if v in tally:
                lines.append("| %s | %d |" % (v, tally[v]))
        lines += ["", "| verdict | drop constant | drop | dump source | dump | "
                  "Offsets.h row | shipped | why |",
                  "| --- | --- | --- | --- | --- | --- | --- | --- |"]
        for r in diff:
            lines.append("| %s | %s | 0x%X | %s | %s | %s | %s | %s |" % (
                r["verdict"], r["path"], r["value"], r["counterpart"] or "-",
                "0x%X" % r["dump"] if r["dump"] is not None else "-",
                r["project"] or "-",
                "0x%X" % r["shipped"] if r["shipped"] is not None else "-",
                r["why"]))

    lines += ["", "## Drop constants not adopted by the project", "",
              "%d of %d constants in the drop are not referenced by `Offsets.h`."
              % (len(unclaimed), len(drop.entries)), "",
              "| drop path | value | note |", "| --- | --- | --- |"]
    for p in sorted(unclaimed):
        e = drop.entries[p]
        lines.append("| %s | 0x%X | %s |" % (p, e["value"], e["comment"][:60]))

    lines += ["", "## Full table", "",
              "| constant | shipped | state | source | provenance | verdict |",
              "| --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        lines.append("| %s | %s | %s | %s | %s | %s |" % (
            r["name"],
            "0x%X" % r["shipped"] if r["shipped"] is not None else "-",
            r["state"], r["drop_path"] or r["spec"], r["provenance"],
            "; ".join(r["notes"])))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.exit(main())
