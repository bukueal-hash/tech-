#!/usr/bin/env python3
"""sdk_gap_report.py — what is still in sdk/sdk.txt that the project does NOT use.

Reads tools/sdk_index.json (the parsed drop) + scans sdk/sdk.txt for the crypto
pipeline functions, checks every name against Project/ + tools/ sources, and
writes sdk_gap_report.html: the full unused inventory with a one-line
description per namespace and the drop's own per-constant comments.

Usage match is a case-insensitive substring test on the raw name OR its
de-underscored form (SCREAMING_SNAKE in the drop vs Camel/pascal in the
generated Offsets.h), and the first hit is shown for transparency.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INDEX = ROOT / "tools" / "sdk_index.json"
DROP = ROOT / "sdk" / "sdk.txt"
OUT = ROOT / "sdk_gap_report.html"
DISP = ROOT / "tools" / "gap_dispositions.json"

SCAN_DIRS = ["Project", "tools"]
SCAN_EXT = {".h", ".hpp", ".cpp", ".py", ".inl"}
SKIP_PARTS = {"thirdparty", "build", "data", ".git", "packages"}

# One-liner per namespace: what the group is for in the game.
NS_BLURB = {
    "offsets": "game::offsets — raw RVAs for the name/object/property crypto (GWorld, GNames, KEYTABLE, UStruct/FField link offsets).",
    "detail": "game::detail — crypto primitives: rotates, GF(2) CLMUL, PSHUFB shuffle, and the slot/shard hash selectors.",
    "gasm": "game::gasm — the per-structure decode pipelines: UObject slots, FName pool blocks, FName strings, FString bytes, FField names, FProperty offsets, bone-array table address.",
    "Global": "Global singleton RVAs (UWorld base pointer).",
    "Crypto": "Pointer-decrypt XOR key (arc_decrypt XMM pipeline).",
    "GetObjectIdCrypto": "PlayerNameDecryptor/GetObjectId SIMD mask key.",
    "GameInstanceDecrypt": "get_game_instance SIMD decrypt (XOR keys, shuffle mask, validity window).",
    "GameInstanceStaticDecrypt": "GameInstance static-decrypt variant (K1/K2 CLMUL, FNV, PSHUFB).",
    "GameInstanceTlsDecrypt": "GameInstance TLS decrypt (TLS key table, XMM input, hash selector).",
    "TebDecrypt": "Wine TEB pointer decrypt (word/qword rotates keyed off TEB).",
    "WineTeb": "Wine/Proton TEB layout (gs_base self ptr, TLS pointer).",
    "PlayerDecrypt": "LocalPlayer pointer decrypt (lane XOR, final rotate).",
    "OuterDecrypt": "Encrypted UObject Outer at object+0x20 — outer-chain walks to the owning actor.",
    "ObjectXorKey": "ObjectProperty pointer XOR key RVA.",
    "BoneArrayDecrypt": "SkeletalMesh bone-array table decrypt (seed@0x7B0, selector@0x848, descriptor table).",
    "FName": "The v20260922 FName pipeline port (name pool, paired-LCG string keystream, CLMUL object slots, FField names, chunks-manager GUObjectArray).",
    "FField": "Runtime reflection FField layout (enc name, owner/next/class, property flags, bool bitfields).",
    "FFieldCrypto": "FField name/offset crypto constants (ADD chain, offset XOR key).",
    "UStruct": "UStruct layout (super struct, child properties, properties size).",
    "UClass": "UClass::ClassDefaultObject slot.",
    "UWorld": "UWorld layout (persistent level, levels, level collections, game instance, time, physics field).",
    "Level": "ULevel actor array (Actors/Count/Max, owning world, GC cluster).",
    "LevelStreaming": "ULevelStreaming loaded level + LevelTransform (runtime-only transform for streamed maps).",
    "LevelActorContainer": "GC actor cluster container (secondary actor list).",
    "LevelCollection": "FLevelCollection element (game state, net drivers, levels).",
    "GameState": "AGameState/PioneerGameState (player array, game phase, enemy/pickup counts, stage info, squad list).",
    "EmbarkGameStateBase": "Embark-extended game state (all squads, elapsed/replicated time, match state).",
    "StageInfo": "FStageInfo (raid timer: time left, grace time).",
    "GameInstance": "UGameInstance local-players array slot.",
    "GameViewportClient": "Viewport client (world/gameinstance back-pointers, console, splitscreen).",
    "LocalPlayer": "ULocalPlayer (player controller, FOV/cull fields, controller id).",
    "PlayerController": "APlayerController (player state, acknowledged pawn, control rotation, camera manager).",
    "Controller": "AController base (player state, character, control rotation).",
    "PlayerState": "APlayerState/Embark/Pioneer extensions (pawn private, names, squad, bot/spectator bits, platform id).",
    "PlayerHealthInfo": "Player health struct (health/max, armor, DBNO flag, broken armor).",
    "PlayerCameraManager": "Camera manager (FOVs, view targets, pitch/yaw/roll clamps, POV).",
    "MinimalViewInfo": "FMinimalViewInfo (camera location/rotation/FOV/near-far/post-process).",
    "TViewTarget": "FTViewTarget (target actor + POV).",
    "Pawn": "APawn (player state, controller, mesh, health component, remote view pitch).",
    "Character": "ACharacter movement-state bits (crouch, replicated movement mode).",
    "Actor": "AActor (owner, root component, mesh, hidden byte/mask, render timestamps).",
    "Controller ": "see Controller.",
    "SceneComponent": "USceneComponent (attach children, relative transform, component-to-world, velocity, visible/mobility bits).",
    "SkeletalMeshComponent": "USkinnedMeshComponent (anim instance, bone caches, leader pose, ref-pose/pause bits, skeletal mesh asset).",
    "SkeletalMesh": "USkeletalMesh asset (physics asset, RefSkeleton probe window).",
    "PhysicsAsset": "UPhysicsAsset body setups array.",
    "BodySetup": "USkeletalBodySetup (bone name, agg geom, implicit object).",
    "MeshBoneInfo": "FMeshBoneInfo layout (name/parent/stride) for RefSkeleton parsing.",
    "BoneArrayDecrypt ": "see BoneArrayDecrypt.",
    "PrimitiveComponent": "Primitive render state (bounds scale, body instance, last render time, world private).",
    "HealthComponent": "UHealthServiceComponent (current/max health + armor, DBNO/death delegates, cached health).",
    "HealthService": "UConstructableHealthServiceComponent (per-part HP array for robot part damage, destruction chains).",
    "InventoryComponent": "Player inventory (loadout, belt, backpack, safe pouch, stowed weapons, equipped armor).",
    "StowedWeaponLayout": "Stowed weapon slot layout (item data, quality, stowed actor).",
    "WeaponActor": "Weapon actor (clip size, weapon quality).",
    "ItemBase": "AItemBase quality level.",
    "ItemUIHoverData": "Item hover tooltip (amount, data asset, display name, max stack).",
    "Pickup": "APickup (default pickup data, interaction, root collider, spawn items, visible amount).",
    "PickupDataAsset": "Pickup data asset resolved item class.",
    "LootContainerSingle": "Loot container (item container + loot interaction components, socket mesh).",
    "LootInteractionComponent": "Loot interaction (opened byte+mask, dispenser locations, acquisition method).",
    "BaseInteractionComponent": "Interaction base (instigator, interaction state, DBNO/defib timer floats+ticks).",
    "QuestInteractable": "Quest interactable actors (interaction components + validate float).",
    "InteractQuestComponent": "InteractQuestComponent (relevant player ids = quest active, interact-access list, owner probes).",
    "ExtractionPoint": "SalvageExtractionPointBase/Point (ExtractionInfo timer struct, state enum, enabled bit, config windows).",
    "PioneerConstructablePawn": "Robot/bot pawn (AI state/template, capability/collision/debris services, enemy type data asset, bIsDestroyed).",
    "ConstructableBase": "Constructable base (all services/style drivers, construction stream, style state).",
    "ConstructableServiceComponentBase": "Constructable service base (fully-constructed bits, service pump flags).",
    "ConstructableStaticMeshStyle": "Static mesh style (destroyed reason, is destroyed, part tags/id range, resistance group).",
    "AIController": "AAIController (perception, brain, path following, blackboard).",
    "AIPerceptionComponent": "AI perception (senses config, dominant sense, AI owner).",
    "AISenseConfigSight": "AI sight config (sight/lose-sight radius, peripheral vision, affiliation detection).",
    "AIStateService": "Bot alertness/combat/emotion state (alertness, combat phase, sight overrides).",
    "GeometryCollection": "Chaos geometry collection (GC component, rest transforms, simulating flag).",
    "GuTriangleMesh": "PhysX triangle mesh (vertices/indices/AABB/16-bit flag).",
    "PxHeightField": "PhysX heightfield (rows/cols/samples) for landscape collision reads.",
    "FHeightfieldRef": "Heightfield reference (rb pointer, refcount).",
    "FBodyInstance": "FBodyInstance actor handle.",
    "Landscape": "Landscape actor (collision components, component size quads, num subsections).",
    "LandscapeHeightfieldCollisionComponent": "Landscape collision (cached local box, heightfield ref, render component ref).",
    "StaticMeshGeometry": "Static mesh render geometry (position/index buffers, LODs, body setup, flag bytes).",
    "MapWidget": "UMG map widget mappings root.",
    "MapWidgetMappings": "Map widget component template / fallback widget.",
    "MapWidgetLevelSettings": "Per-level map settings (texture, material, world size/position, underground flag).",
    "WorldPartitionMiniMap": "AWorldPartitionMiniMap (minimap texture, world-units-per-pixel, tile size, world bounds).",
    "DirectionalLightComponent": "Sun/moon directional light (cascade/shadow shafts, atmosphere flags, cloud shadows).",
    "LightComponent": "Light component (attenuation, IES, shadows, channels, temperature).",
    "LightComponentBase": "Light base (intensity, light color, samples per pixel).",
    "SkyLightComponent": "Sky light (lower-hemisphere, real-time capture, blend).",
    "SkyAtmosphereComponent": "Sky atmosphere (Rayleigh/Mie, absorption, ground albedo, transform mode).",
    "ExponentialHeightFog": "Height fog actor enabled bit.",
    "ExponentialHeightFogComponent": "Height fog (density/falloff, in-scattering, start distance).",
    "VolumetricCloudComponent": "Volumetric cloud (layer altitude/extinction, phase G, shadow tracing).",
    "Pioneer_Sky_C": "Pioneer sky blueprint (time-of-day lights, exposure min/max, moon/sun intensities).",
    "EmbarkSkyActor": "Embark sky actor (time of day, lighting multiplier, moon color/intensity).",
    "PostProcessComponent": "Post-process component (settings, blend radius/weight, priority).",
    "PostProcessVolume": "Post-process volume (unbound, blend radius, priority).",
    "FPostProcessSettings": "Full FPostProcessSettings field map (bloom, exposure, DOF, color grading, Lumen, AA...).",
    "CapsuleComponent": "Capsule half-height/radius slots (box sizing for ESP).",
    "UniqueNetIdRepl": "FUniqueNetIdRepl layout (net id object, type index, replication bytes).",
    "UniqueNetId": "FUniqueNetId payload (SteamID64 / console id string).",
    "PlatformIdComponent": "UEmbarkPlatformIdComponent platform id slot (SteamID64 replicates to all).",
    "PioneerPC": "APioneerPlayerController (contexts disabled on menu open = UI-open detector).",
    "PioneerPlayerCharacter": "Player character extensions (DBNO/flashlight/follow-camera/inventory/team components).",
    "Engine": "UEngine::GameInstance slot.",
    "ArcOffsets": "Aggregate root of the drop.",
}


def norm(name: str) -> str:
    return re.sub(r"[^a-z0-9]", "", name.lower())


def camel(name: str) -> str:
    return norm(name)


def load_sources():
    blobs = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if p.suffix.lower() not in SCAN_EXT or not p.is_file():
                continue
            if any(s in p.parts for s in SKIP_PARTS):
                continue
            try:
                blobs.append((str(p.relative_to(ROOT)), p.read_text(errors="ignore")))
            except OSError:
                pass
    return blobs


def find_use(raw_name: str, nname: str, cname: str, blobs):
    """Return first usage location or None. Case-insensitive raw OR de-underscored."""
    for path, text in blobs:
        low = text.lower()
        if raw_name.lower() in low or (len(cname) > 3 and cname in low):
            m = re.search(re.escape(raw_name), text, re.IGNORECASE)
            line = text.count("\n", 0, m.start()) + 1 if m else 0
            return f"{path}:{line}"
    return None


def parse_drop_functions(text: str):
    """Function definitions in the crypto blocks of sdk.txt."""
    names = {}
    pat = re.compile(
        r"^\s*(?:static\s+|inline\s+)*[\w:<>,\s\*&~]+?\b([a-z_][a-z0-9_]*)\s*\([^;{]*\)\s*\{?",
        re.MULTILINE,
    )
    kw = {"if", "for", "while", "switch", "return", "sizeof", "alignof"}
    for m in pat.finditer(text):
        name = m.group(1)
        if name in kw or name.startswith("_"):
            continue
        line = text.count("\n", 0, m.start()) + 1
        names.setdefault(name, line)
    return names


FN_BLURB = {
    "rol32": "32-bit rotate left (crypto primitive).",
    "rol64": "64-bit rotate left (crypto primitive).",
    "clmul64_low": "portable 64x64 GF(2) carry-less multiply, low half (stands in for PCLMULQDQ).",
    "shuffle64": "PSHUFB-style byte shuffle of a u64 (mask {6,5,1,2,7,3,4,0}).",
    "uobject_selector": "per-address slot hash: picks which of 4 encrypted slots holds Name/Class/Outer.",
    "decode_uobject_slot": "CLMUL double-mix decode of one encrypted UObject slot (name/class/outer).",
    "fname_pool_selector": "shard hash: picks the two encoded block descriptors for a name entry.",
    "decode_fname_pool_first": "block decode #1 (AND/OR/XOR mask -> ROL64(19) -> PSHUFB).",
    "decode_fname_pool_second": "block decode #2 (XOR -> ROL64(19) -> PSHUFB).",
    "decode_uobject_nameprivate": "object NamePrivate (index+number) via slot selector^2 + ROL64(32).",
    "decode_uobject_classprivate": "object ClassPrivate pointer via the hashed slot.",
    "decode_fname_pool_address": "FNameEntry* from the two decoded blocks + FNV mix (Entry = V14 + (V15^Fv) + 2*NameOff).",
    "decode_fname_header_len": "FNameEntry header length decode (wide bit + packed length).",
    "decode_fname": "narrow FName string decrypt (paired-LCG keystream, K=length+0xADF).",
    "decode_fname_wide_v922": "wide FName string decrypt (full u16 keystream XOR, seed len+2783).",
    "decode_fstring_byte": "FString byte pipeline (state = ROL14(state*P+ADD)+state; printable remap chain).",
    "decode_fstring": "in-place FString decrypt (narrow + wide overloads).",
    "decode_ffield_nameprivate": "FField NamePrivate decode (ADD/ROL chain, second qword XOR).",
    "decode_ffieldclass_name": "FFieldClass name decode (u16 rotate + shuffle + ROL64(32)).",
    "decode_ffproperty_offset": "FProperty offset decode (byte-swap of XOR 0xDE28E18D).",
    "decode_bonearray_table_address": "bone-array table address (XMM XOR/ROL32/ADD per dword at mesh+0x7B0).",
    "ClmulLo": "GF(2) carry-less multiply low (v922 port).",
    "Pshufb64": "byte shuffle (v922 port).",
    "DecodeBlock": "name-pool block decode (XOR -> ROL64(19) -> PSHUFB).",
    "DecodeSlotLo": "UObject slot CLMUL decode (v922 port).",
    "DecodeFFieldName": "FField name ADD/ROL decode (v922 port).",
    "ShardHash": "name-pool shard hash (3 imul rounds + SHR5).",
    "obj_slot_hash": "UObject slot hash (4 imul rounds from addr+0x10).",
    "obj_slot_index_base": "slot index base (low-byte ^ byte16 & 3).",
    "obj_name_slot": "Name slot index (base ^ 2).",
    "obj_class_slot": "Class slot index (base + 0).",
    "obj_outer_slot": "Outer slot index (base + 1).",
    "InitFNameState": "loads the 256-word keystream from RVA_KEYSTREAM and primes the pipeline.",
    "ReadSlotDecoded": "read+decode one encrypted 16-byte slot at obj+0x20+slot*0x20.",
    "ReadSlotDecodedAsPtr": "decoded slot re-rotated to pointer form (outer/class).",
    "ReadSlotRaw": "raw slot qword read.",
    "FindFNameSlot": "resolve the actor's NamePrivate slot (index+number packed u64).",
    "ResolveNamePtr": "CI -> FNameEntry* via chunk/shard/blocks + FNV (the pool walk).",
    "DecryptNameString": "FNameEntry -> string (paired-LCG keystream, narrow + wide).",
    "is_sane_name": "printability gate for decoded names.",
    "CachedNameString": "name decode with the global index cache.",
    "ToString": "CI -> string (cached).",
    "GetActorFNameString": "actor -> name string (hashed slot + 4-slot fallback).",
    "GetActorFNameId": "actor -> name comparison index.",
    "GetActorFNameNumber": "actor -> name number (instance suffix).",
    "GetClassPtr": "object -> ClassPrivate pointer (hashed slot + fallbacks).",
    "GetObjectClassName": "object -> class name string.",
    "DecryptChunksManager": "GUObjectArray chunks-manager decrypt (per-dword XOR/ROL32/ADD from 3 RVA keys).",
    "ReadGUObjectArrayNumElements": "live object count from the decrypted manager (bswap+XOR sanity window).",
    "FindChunksArray": "chunk array pointer (bswap64 XOR) for full object enumeration.",
    "SimdDecryptBlob": "stub: SIMD blob decrypt (unimplemented in the drop).",
    "decrypt_ffieldclass_name_context": "register context struct for the FFieldClass name decode.",
}


def main(argv=None):
    ap = argparse.ArgumentParser(description="sdk.txt gap report")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 while any unused constant/function lacks a disposition")
    ap.add_argument("--strict", action="store_true",
                    help="also exit 1 for declared-but-not-yet-referenced items")
    args = ap.parse_args(argv)

    if not INDEX.exists() or not DROP.exists():
        print("missing sdk_index.json or sdk/sdk.txt", file=sys.stderr)
        return 1
    idx = json.loads(INDEX.read_text(errors="ignore"))
    drop_text = DROP.read_text(errors="ignore")
    blobs = load_sources()

    disp = json.loads(DISP.read_text(errors="ignore")) if DISP.exists() else {}
    ns_disp = {k.strip(): v for k, v in (disp.get("namespaces") or {}).items()}
    const_disp = disp.get("constants") or {}
    fn_disp = disp.get("functions") or {}

    def fate_for(key, ns):
        d = const_disp.get(key)
        if d is None:
            d = ns_disp.get((ns or "").strip())
        return d

    consts = idx.get("drop", {}).get("constants", {}) or idx.get("constants", {})
    rows_used, rows_unused = [], []
    for key in sorted(consts, key=lambda k: (consts[k].get("namespace", ""), k)):
        info = consts[key]
        name = key.split("::")[-1]
        ns = info.get("namespace", "?")
        n, c = norm(name), camel(name)
        use = find_use(name, n, c, blobs)
        row = {
            "key": key,
            "name": name,
            "ns": ns,
            "value": info.get("value"),
            "comment": info.get("comment", ""),
            "line": info.get("line", 0),
            "use": use,
            "fate": ({"fate": "wired", "note": ""} if use else fate_for(key, ns)),
        }
        (rows_used if use else rows_unused).append(row)

    fns = parse_drop_functions(drop_text)
    fn_used, fn_unused = [], []
    for name, line in sorted(fns.items()):
        use = find_use(name, norm(name), camel(name), blobs)
        row = {"name": name, "line": line, "use": use,
               "blurb": FN_BLURB.get(name, ""),
               "fate": ({"fate": "wired", "note": ""} if use else fn_disp.get(name))}
        (fn_used if use else fn_unused).append(row)

    by_ns = {}
    for r in rows_unused:
        by_ns.setdefault(r["ns"], []).append(r)

    def _unaccounted(r):
        f = r.get("fate")
        return f is None or (f.get("fate") == "waived" and not f.get("note"))

    unaccounted = ([r["key"] for r in rows_unused if _unaccounted(r)] +
                   [r["name"] for r in fn_unused if _unaccounted(r)])
    declared = ([r["key"] for r in rows_unused
                 if r.get("fate") and r["fate"].get("fate") not in ("waived", "wired")] +
                [r["name"] for r in fn_unused
                 if r.get("fate") and r["fate"].get("fate") not in ("waived", "wired")])
    waived_n = sum(1 for r in rows_unused
                   if (r.get("fate") or {}).get("fate") == "waived")

    def fate_html(r):
        f = r.get("fate")
        if not f:
            return '<span style="color:#e07a7a">unaccounted</span>'
        kind = f.get("fate", "?")
        note = f.get("note", "")
        color = {"waived": "#77808f", "wired": "#69c37a"}.get(kind, "#e8c987")
        return (f'<span style="color:{color}">{esc(kind)}</span>'
                + (f' - <span class="muted">{esc(note)}</span>' if note else ""))

    def esc(s):
        return (str(s).replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;").replace('"', "&quot;"))

    def val_hex(v):
        if isinstance(v, int):
            if v < 0:
                v &= (1 << 64) - 1
            return f"0x{v:X}"
        return esc(v)

    parts = []
    parts.append("""<!DOCTYPE html><html><head><meta charset="utf-8">
<title>sdk.txt gap report</title><style>
body{font:13px/1.5 system-ui,Segoe UI,sans-serif;background:#14161a;color:#d8dce4;margin:0;padding:24px}
h1{font-size:20px;margin:0 0 4px} h2{font-size:16px;margin:28px 0 8px;border-bottom:1px solid #2a2f38;padding-bottom:4px}
h3{font-size:14px;margin:18px 0 4px;color:#9fd0ff}
.sub{color:#8a93a3;margin:0 0 16px}
.cards{display:flex;gap:12px;flex-wrap:wrap;margin:12px 0 20px}
.card{background:#1b1f26;border:1px solid #2a2f38;border-radius:8px;padding:10px 16px;min-width:130px}
.card b{display:block;font-size:22px;color:#9fd0ff}
.card span{color:#8a93a3;font-size:11px}
table{border-collapse:collapse;width:100%;margin:6px 0 18px}
th,td{border-bottom:1px solid #242932;padding:4px 8px;text-align:left;vertical-align:top}
th{color:#8a93a3;font-weight:600;font-size:11px;text-transform:uppercase}
td.n{font-family:Consolas,monospace;color:#e8c987;white-space:nowrap}
td.v{font-family:Consolas,monospace;color:#8fd48f;white-space:nowrap}
td.u{font-family:Consolas,monospace;color:#7ec3e8;font-size:11px}
tr:hover td{background:#1b1f26}
.blurb{color:#a9b2c1;margin:2px 0 6px}
.used{color:#69c37a} .muted{color:#77808f}
#filter{width:100%;max-width:420px;padding:8px 10px;background:#1b1f26;border:1px solid #2a2f38;border-radius:6px;color:#d8dce4;margin:8px 0 16px}
.fn td:first-child{font-family:Consolas,monospace;color:#e8c987;white-space:nowrap}
</style></head><body>""")
    parts.append("<h1>sdk.txt — what the project does NOT use yet</h1>")
    parts.append('<p class="sub">Generated by <code>tools/sdk_gap_report.py</code> from '
                 '<code>tools/sdk_index.json</code> + a name scan of Project/ and tools/. '
                 'Matching is case-insensitive on the raw name or its de-underscored form; '
                 'the first hit is shown for used items. Descriptions come from the drop\'s '
                 'own comments where present, otherwise from the namespace blurb.</p>')
    parts.append('<input id="filter" placeholder="filter items (name, comment, namespace)…" oninput="applyFilter()">')
    parts.append('<div class="cards">')
    parts.append(f'<div class="card"><b>{len(unaccounted)}</b><span>unaccounted (goal 0)</span></div>')
    parts.append(f'<div class="card"><b>{len(declared)}</b><span>declared, pending wiring</span></div>')
    parts.append(f'<div class="card"><b>{waived_n}</b><span>waived with reason</span></div>')
    parts.append(f'<div class="card"><b>{len(rows_unused)}</b><span>constants unused</span></div>')
    parts.append(f'<div class="card"><b>{len(rows_used)}</b><span>constants used</span></div>')
    parts.append(f'<div class="card"><b>{len(consts)}</b><span>constants total</span></div>')
    parts.append(f'<div class="card"><b>{len(fn_unused)}</b><span>pipeline fns unused</span></div>')
    parts.append(f'<div class="card"><b>{len(fn_used)}</b><span>pipeline fns used</span></div>')
    parts.append(f'<div class="card"><b>{len(by_ns)}</b><span>namespaces w/ gaps</span></div>')
    parts.append('</div>')

    parts.append("<h2>Unused crypto-pipeline functions</h2>")
    parts.append('<table class="fn"><tr><th>function</th><th>sdk.txt line</th><th>disposition</th><th>what it does</th></tr>')
    for r in fn_unused:
        parts.append(f'<tr data-f="1"><td>{esc(r["name"])}</td><td>{r["line"]}</td>'
                     f'<td>{fate_html(r)}</td><td>{esc(r["blurb"])}</td></tr>')
    if not fn_unused:
        parts.append('<tr><td colspan="4" class="muted">none - every pipeline function is wired</td></tr>')
    parts.append("</table>")

    parts.append("<h2>Unused constants by namespace</h2>")
    for ns in sorted(by_ns, key=lambda k: (-len(by_ns[k]), k)):
        items = by_ns[ns]
        blurb = NS_BLURB.get(ns) or NS_BLURB.get(ns.strip()) or f"SDK constants for {ns}."
        parts.append(f'<h3>{esc(ns)} — {len(items)} unused</h3>')
        parts.append(f'<div class="blurb">{esc(blurb)}</div>')
        parts.append('<table><tr><th>constant</th><th>value</th><th>disposition</th><th>what it is</th></tr>')
        for r in items:
            desc = r["comment"] or blurb
            parts.append(f'<tr data-f="1"><td class="n">{esc(r["name"])}</td>'
                         f'<td class="v">{val_hex(r["value"])}</td>'
                         f'<td>{fate_html(r)}</td><td>{esc(desc)}</td></tr>')
        parts.append("</table>")

    parts.append("<h2 class='muted'>Appendix — constants already used</h2>")
    parts.append('<table><tr><th>constant</th><th>value</th><th>matched at</th></tr>')
    for r in rows_used:
        parts.append(f'<tr data-f="1"><td class="n">{esc(r["key"])}</td>'
                     f'<td class="v">{val_hex(r["value"])}</td>'
                     f'<td class="u">{esc(r["use"])}</td></tr>')
    parts.append("</table>")

    parts.append("""<script>
function applyFilter(){
  const q = document.getElementById('filter').value.toLowerCase();
  document.querySelectorAll('tr[data-f]').forEach(tr => {
    tr.style.display = !q || tr.innerText.toLowerCase().includes(q) ? '' : 'none';
  });
}
</script></body></html>""")
    OUT.write_text("\n".join(parts), encoding="utf-8")
    print(f"wrote {OUT}")
    print(f"constants: {len(consts)} total, {len(rows_used)} used, {len(rows_unused)} unused")
    print(f"functions: {len(fns)} found, {len(fn_used)} used, {len(fn_unused)} unused")
    print(f"dispositions: {waived_n} waived constants, {len(declared)} declared pending, "
          f"{len(unaccounted)} unaccounted")
    if unaccounted:
        print("unaccounted (add tools/gap_dispositions.json entries):")
        for k in unaccounted[:25]:
            print(f"  {k}")
        if len(unaccounted) - 25 > 0:
            print(f"  ... and {len(unaccounted) - 25} more")
    if args.check and unaccounted:
        print("gap check FAILED: %d item(s) without a disposition" % len(unaccounted))
        return 1
    if args.strict and declared:
        print("gap strict FAILED: %d declared item(s) not yet referenced in code" % len(declared))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
