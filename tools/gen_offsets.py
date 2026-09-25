#!/usr/bin/env python3
"""Generate Project/Core/Offsets.h from the dumped SDK (sdk/).

Every constant is either:

  prop:<Class>.<Property>   offset taken from the FrostDumper dump
                            (sdk/SDK.txt, indexed by tools/sdk_index.py)
  drop:<namespace::NAME>    value taken from the CL drop sdk/sdk.txt
                            (native fields, data RVAs, crypto constants)
  maskOf:<Class>.<Property> packed-bool mask from the dump
  sizeof:<Class>            class/struct size from the dump
  lit:<hex>                 nothing offline can see this slot: a hand-probed
                            value kept until a source or the live target pins it
  expr:<text>               alias or arithmetic over other constants
  sdk:<NAME>                re-exported from Project/Core/SDK.hpp
                            (the FName/decrypt drop: game::offsets + key tables)

Usage:
  python tools/sdk_index.py build      # refresh the index after a new dump
  python tools/gen_offsets.py          # rewrite Project/Core/Offsets.h
  python tools/gen_offsets.py --check   # verify without writing
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdk_drop

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
INDEX = os.path.join(ROOT, "tools", "sdk_index.json")
OUT = os.path.join(ROOT, "Project", "Core", "Offsets.h")

# Constants the live target has pinned to a value no static source carries.
# GWorld is deliberately not overridden here: sdk.txt defines the authoritative
# 0x10839A98 global and the ladder performs its documented intermediary deref.
LIVE_OVERRIDES = {
    # ComponentToWorld is not a UPROPERTY on this build, so the dump has no
    # value for it. ArcOffsets (2026-09-22) pins the block at 0x310 with the
    # translation at +0x20 => 0x330, and our own probe agrees: every bot root
    # read at the old 0x2D0 slot came back implausible (pos_refresh bFail ==
    # bots, 100%) while the 0x310 slot resolved. 0x2D0 stays as the probe's
    # alternate for older builds.
    "ComponentToWorld": ("ArcOffsets::SceneComponent::COMPONENT_TO_WORLD = 0x310 "
                         "(v20260922 dumper + user); translation at +0x20 => 0x330"),
    # Neither render stamp is reflected, so the dump cannot see them either.
    # CL-1372005 made both PLAIN floats again; the vis check user-provided
    # 2026-09-23 pins them at 0x480/0x484 (the drop's 0x4C0/0x4C4 slot reads
    # zero on the current build). 0x4C8/0x4CC is the retired XOR-era pair and
    # reading it left visibility at "unknown".
    "LastRenderTime": "vis check user-provided 2026-09-23: PrimitiveComponent::LastRenderTime = 0x480 (plain float; drop's 0x4C0 reads zero now)",
    "LastRenderTimeOnScreen": "vis check user-provided 2026-09-23: LastRenderTimeOnScreen = 0x484 (plain float; was 0x4C4)",
    # The dump types HealthComponent.CachedHealth (0x700) and MaxHealth (0x378)
    # as FName, so both reads returned a name index instead of a number. The
    # live-verified doubles are CURRENT_HEALTH 0x6D0 and MAX_HEALTH 0x348 (the
    # latter inside the dump's Pad_0340 hole).
    "Health": "ArcOffsets::HealthComponent::CURRENT_HEALTH = 0x6D0 (the 0x700 dump field is an FName shadow)",
    "MaxHealth": "ArcOffsets::HealthComponent::MAX_HEALTH = 0x348 (native, in the dump's Pad_0340 hole)",
}

# (name, cpp type, value spec, comment)
SECTIONS = [
    ("SDK drop: every game::offsets value is mirrored here", [
        ("UWorld", "std::ptrdiff_t", "sdk:GWORLD", "GWORLD global"),
        ("GNamePoolRva", "uint64_t", "sdk:GNAMES", "GNAMES pool global"),
        ("FNameKeyTableRva", "uint64_t", "sdk:KEYTABLE", "FName key table"),
        ("FStringVerificationRva", "uint64_t", "sdk:FString_Verification_Offset",
         "FString verification data"),
    ]),
    ("Globals only the 20260922 dump carries", [
        ("GObjectsRva", "uint64_t", "lit:0x10C56FE0",
         "OFFSET_GOBJECTS (Dumpspace OffsetsInfo / dump header)"),
        ("ProcessEventRva", "uint64_t", "lit:0x5AF560", "OFFSET_PROCESSEVENT"),
        ("ProcessEventIndex", "uint32_t", "lit:0x4C", "INDEX_PROCESSEVENT"),
    ]),
    ("Reflection layout (FField/FProperty walkers)", [
        ("UStruct_SuperStruct", "std::ptrdiff_t", "prop:CoreUObject.Struct.SuperStruct",
         "UStruct::SuperStruct"),
        ("UStruct_PropertyLink", "std::ptrdiff_t", "prop:CoreUObject.Struct.Children",
         "UStruct::Children (the FField chain head)"),
        ("FProperty_PropertyLinkNext", "std::ptrdiff_t", "prop:CoreUObject.Field.Next",
         "FField::Next"),
        ("FObjectPropertyBase_PropertyClass", "std::ptrdiff_t",
         "prop:CoreUObject.ObjectPropertyBase.PropertyClass",
         "FObjectPropertyBase::PropertyClass"),
        ("FField_ClassPrivate", "std::ptrdiff_t", "lit:0x60",
         "FField::ClassPrivate (FField is not reflected — no dump source)"),
        ("FFieldClass_SuperClass", "std::ptrdiff_t", "lit:0x40",
         "FFieldClass::SuperClass (FFieldClass is not reflected — no dump source)"),
    ]),
    ("Not covered by the dump (project-local / non-reflected)", [
        ("GameStateGlobalRva", "uint64_t", "lit:0xDCA7C88",
         "UNVERIFIED: no GameState global in the 20260922 dump"),
        ("FNameKeystreamLegacyRva", "uint64_t", "lit:0xE2997F4", "legacy v818 keystream"),
        ("FNameBlockMaskRva", "uint64_t", "lit:0xB523C50", "legacy SSE block mask"),
        ("GUObjectArrayChunksRva", "uint64_t", "expr:GObjectsRva",
         "== GObjectsRva (old drop's name for the same global)"),
        ("GObjPshufbMaskRva", "uint64_t", "lit:0xAD97CC0",
         "16-byte PSHUFB mask (data RVA — no dump source)"),
        ("FFieldNameKey0Rva", "uint64_t", "lit:0xE7B5330", "FField name key (data RVA)"),
        ("FFieldNameKey1Rva", "uint64_t", "lit:0xE6F0554", "FField name key (data RVA)"),
        ("GameInstanceXorKey0Rva", "uint64_t", "lit:0xB06C380", "GI pointer XOR key low"),
        ("GameInstanceXorKey1Rva", "uint64_t", "drop:GameInstanceDecrypt::XOR_KEY_RVA_2", "GI pointer XOR key high"),
        ("GameInstanceShuffleMaskRva", "uint64_t", "lit:0xB09C350", "GI PSHUFB mask"),
        ("GameInstance_WorldBackRef", "std::ptrdiff_t", "lit:0x2F0",
         "GI -> UWorld back-reference (non-UPROPERTY)"),
        ("PlayerNameSimdMaskRva", "uint64_t", "drop:GetObjectIdCrypto::SIMD_MASK_RVA", "player-name PSHUFB mask"),
    ]),
    ("UObjectBase (engine-invariant layout)", [
        ("UObject_ObjectFlags", "std::ptrdiff_t", "lit:0x08", "UObjectBase::ObjectFlags"),
        ("RF_BeginDestroyed", "uint32_t", "lit:0x00800000", "EObjectFlags"),
        ("RF_FinishDestroyed", "uint32_t", "lit:0x01000000", "EObjectFlags"),
    ]),
    ("UWorld", [
        ("PersistentLevel", "std::ptrdiff_t", "prop:Engine.World.PersistentLevel", ""),
        ("StreamingLevels", "std::ptrdiff_t", "prop:Engine.World.StreamingLevels", ""),
        ("Levels", "std::ptrdiff_t", "prop:Engine.World.Levels", ""),
        ("OwningGameInstance", "std::ptrdiff_t", "drop:ArcOffsets::UWorld::GAME_INSTANCE",
         "SDK drop: encrypted UWorld::OwningGameInstance slot"),
        ("LevelCollections", "std::ptrdiff_t", "prop:Engine.World.LevelCollections",
         "TArray<FLevelCollection>"),
        ("AuthorityGameMode", "std::ptrdiff_t", "prop:Engine.World.AuthorityGameMode", ""),
        ("PhysicsField", "std::ptrdiff_t", "prop:Engine.World.PhysicsField", ""),
        ("LevelCollection_GameState", "std::ptrdiff_t",
         "prop:Engine.LevelCollection.GameState", "FLevelCollection::GameState"),
        ("LevelCollection_PersistentLevel", "std::ptrdiff_t",
         "prop:Engine.LevelCollection.PersistentLevel", "FLevelCollection::PersistentLevel"),
        ("LevelCollection_Stride", "std::ptrdiff_t", "sizeof:Engine.LevelCollection",
         "FLevelCollection element stride"),
        ("GameState", "std::ptrdiff_t", "expr:LevelCollections",
         "UWorld has no direct GameState field — use LevelCollections"),
    ]),
    ("GameInstance / LocalPlayer", [
        ("LocalPlayers", "std::ptrdiff_t", "drop:ArcOffsets::GameInstance::LOCAL_PLAYERS",
         "SDK drop: UGameInstance::LocalPlayers"),
        ("LocalPlayer_PlayerController", "std::ptrdiff_t",
         "prop:Engine.Player.PlayerController", "ULocalPlayer inherits UPlayer"),
        ("LocalPlayer_ControllerId", "std::ptrdiff_t",
         "prop:Engine.LocalPlayer.ControllerId", ""),
    ]),
    ("ULevel", [
        # ULevel::Actors is not a UPROPERTY, so the property index has no entry
        # for it — but the dumped SDK's own class definition does:
        #   sdk/CppSDK/SDK/Level_classes.hpp
        #     class TArray<class AActor*> Actors;  // 0x0110(0x0010)
        ("AActors", "std::ptrdiff_t", "lit:0x110",
         "ULevel::Actors TArray<AActor*> data ptr — Level_classes.hpp (non-UPROPERTY)"),
        ("ActorsCount", "std::ptrdiff_t", "expr:AActors + 0x8",
         "TArray num (ULevel::Actors + 0x8)"),
        ("Level_OwningWorld", "std::ptrdiff_t", "prop:Engine.Level.OwningWorld", ""),
        ("ActorCluster", "std::ptrdiff_t", "prop:Engine.Level.ActorCluster",
         "the live actor path on this build"),
        ("LevelActorContainer_Actors", "std::ptrdiff_t",
         "prop:Engine.ActorContainer.Actors", "ULevelActorContainer::Actors"),
        ("LevelActorContainer_ActorCount", "std::ptrdiff_t",
         "expr:LevelActorContainer_Actors + 0x8", "TArray num"),
    ]),
    ("GameState", [
        ("GameState_PlayerArray", "std::ptrdiff_t", "prop:Engine.GameStateBase.PlayerArray", ""),        ("GameState_EnemyCount", "std::ptrdiff_t", "prop:Angelscript.PioneerGameState.EnemyCount",
         "was lit:0x0940 — the dump reflects it at 0xA00"),
        ("GameState_PickupCount", "std::ptrdiff_t",
         "prop:Angelscript.PioneerGameState.PickupCount", ""),
        ("GameState_GamePhase", "std::ptrdiff_t",
         "prop:Angelscript.PioneerGameState.GamePhase", ""),
    ]),
    ("GameViewportClient", [
        ("GameViewportClient_World", "std::ptrdiff_t",
         "prop:Engine.GameViewportClient.World", ""),
    ]),
    ("Controller / PlayerController", [
        ("AcknowledgedPawn", "std::ptrdiff_t", "drop:ArcOffsets::Controller::ACKNOWLEDGED_PAWN",
         "SDK drop: AController::Pawn — the authoritative pawn slot"),
        ("AcknowledgedPawn_Fallback", "std::ptrdiff_t", "lit:0x3D8",
         "heuristic fallback only (now AController+0x3D8 is inside Character)"),
        ("Controller_Character", "std::ptrdiff_t", "prop:Engine.Controller.Character", ""),
        ("ControlRotation", "std::ptrdiff_t", "drop:ArcOffsets::Controller::CONTROL_ROTATION",
         "SDK drop: AController::ControlRotation"),
        ("AController_PlayerState", "std::ptrdiff_t", "drop:ArcOffsets::Controller::PLAYER_STATE",
         "SDK drop: AController::PlayerState"),
        ("APlayerCameraManager", "std::ptrdiff_t", "lit:0x4D0",
         "non-UPROPERTY on PlayerController — live-pinned slot"),
        ("PlayerController_bIsLocalPlayerController", "std::ptrdiff_t",
         "prop:Engine.PlayerController.bIsLocalPlayerController",
         "APlayerController::bIsLocalPlayerController — the engine's own local-PC flag"),
        ("PlayerController_bIsLocalPlayerController_Mask", "uint8_t", "lit:0x1",
         "bIsLocalPlayerController mask (declared bool mask=0x1 in the dump)"),
        ("APlayerState", "std::ptrdiff_t", "drop:ArcOffsets::Pawn::PLAYER_STATE",
         "SDK drop: APawn::PlayerState"),
        ("PlayerNamePrivate", "std::ptrdiff_t", "prop:Engine.PlayerState.PlayerNamePrivate", ""),
        ("PlayerState_PawnPrivate", "std::ptrdiff_t", "prop:Engine.PlayerState.PawnPrivate", ""),
        ("PlayerState_PlayerStatus", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerState.PlayerStatus", ""),
        ("PioneerPlayerState_PioneerCharacter", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerState.PioneerCharacter", ""),
        ("PioneerPlayerState_CurrentPawn", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerState.CurrentPawn", ""),
        ("Pawn_Controller", "std::ptrdiff_t", "drop:ArcOffsets::Pawn::CONTROLLER",
         "SDK drop: APawn::Controller"),
        ("PlayerNameOnPawn", "std::ptrdiff_t", "lit:0x438",
         "no longer a name slot on this build — heuristic only"),
    ]),
    ("PlayerCameraManager (TViewTarget @ 0x460, FMinimalViewInfo inside POV)", [
        ("ViewTarget", "std::ptrdiff_t", "prop:Engine.PlayerCameraManager.ViewTarget",
         "TViewTarget (active)"),
        ("PendingViewTarget", "std::ptrdiff_t",
         "prop:Engine.PlayerCameraManager.PendingViewTarget", "TViewTarget (blend target)"),
        ("CameraPOV_Location", "std::ptrdiff_t",
         "expr:ViewTarget + 0x10 + 0x10", "TViewTarget::POV + FMinimalViewInfo::Location"),
        ("CameraPOV_Rotation", "std::ptrdiff_t",
         "expr:ViewTarget + 0x10 + 0x38", "TViewTarget::POV + FMinimalViewInfo::Rotation"),
        ("CameraPOV_FOV", "std::ptrdiff_t",
         "expr:ViewTarget + 0x10 + 0x60", "TViewTarget::POV + FMinimalViewInfo::FOV"),
        ("CameraLocation", "std::ptrdiff_t", "expr:CameraPOV_Location", ""),
        ("CameraRotation", "std::ptrdiff_t", "expr:CameraPOV_Rotation", ""),
        ("CameraFOV", "std::ptrdiff_t", "expr:CameraPOV_FOV", ""),
        ("PCOwner", "std::ptrdiff_t", "prop:Engine.PlayerCameraManager.PCOwner", ""),
        ("DefaultFOV", "std::ptrdiff_t", "prop:Engine.PlayerCameraManager.DefaultFOV", ""),
        ("LockedFOV", "std::ptrdiff_t", "lit:0x43C",
         "sdk.txt drop: APlayerCameraManager::LockedFOV (2026-09-22)"),
    ]),
    ("Pointer / global crypto (sdk.txt drop, 2026-09-22)", [
        ("UWorldGlobalIntermediary", "std::ptrdiff_t", "lit:0x0",
         "the GWorld global is [RVA] -> intermediary -> [+0x00] = UWorld (double deref)"),
        ("XmmXorVal", "uint64_t", "lit:0xA738DD8241D227C2",
         "ArcOffsets::Crypto::XMM_XOR_VAL - decrypt_pointer_general XOR lane"),
        ("ObjectXorKeyRva", "uint64_t", "lit:0xD91F885",
         "ArcOffsets::ObjectXorKey::RVA - decrypt_object_ptr XOR key"),
        ("GetObjectIdSimdMaskRva", "uint64_t", "lit:0xAD2FC50",
         "ArcOffsets::GetObjectIdCrypto::SIMD_MASK_RVA"),
        ("GameInstanceStaticStageArrayRva", "uint64_t", "drop:GameInstanceStaticDecrypt::STAGE_ARRAY_RVA",
         "GameInstanceStaticDecrypt::STAGE_ARRAY_RVA - xmmword_150F35650 stage blocks"),
        ("GameInstanceStaticPshufbMaskRva", "uint64_t", "drop:GameInstanceStaticDecrypt::PSHUFB_MASK_RVA",
         "GameInstanceStaticDecrypt::PSHUFB_MASK_RVA - qword_14DDFC1D0"),
        ("GameInstanceStaticXorMask", "uint64_t", "drop:GameInstanceStaticDecrypt::XOR_MASK",
         "GameInstanceStaticDecrypt::XOR_MASK"),
        ("GameInstanceStaticAdd", "uint64_t", "drop:GameInstanceStaticDecrypt::ADD64",
         "GameInstanceStaticDecrypt::ADD64"),
        ("GameInstanceStaticRot1", "uint32_t", "drop:GameInstanceStaticDecrypt::ROT64_1",
         "GameInstanceStaticDecrypt::ROT64_1"),
        ("GameInstanceStaticRot2", "uint32_t", "drop:GameInstanceStaticDecrypt::ROT64_2",
         "GameInstanceStaticDecrypt::ROT64_2"),
        ("GameInstanceStaticResultDeref", "std::ptrdiff_t", "drop:GameInstanceStaticDecrypt::RESULT_DEREF",
         "GameInstanceStaticDecrypt::RESULT_DEREF - result = *(seed_ptr + 24)"),
        ("PlayerDecrypt_LocalPlayerOffset", "std::ptrdiff_t", "lit:0x4B0",
         "APlayerController+0x4B0 - encrypted ULocalPlayer (APlayerController::GetLocalPlayer, sub_3680940)"),
        ("PlayerDecrypt_BlendXorMask", "uint64_t", "drop:PlayerDecrypt::BLEND_XOR_MASK",
         "BLEND_XOR_MASK - the xmmword AND/ANDNOT pair collapses to one XOR"),
        ("PlayerDecrypt_LaneRot", "uint32_t", "drop:PlayerDecrypt::LANE_ROT", "LANE_ROT - ROL32 per 32-bit lane"),
        ("PlayerDecrypt_FinalRot", "uint32_t", "drop:PlayerDecrypt::FINAL_ROT", "FINAL_ROT - ROL64 of the blended qword"),
        ("OuterDecrypt_SlotBaseOff", "std::ptrdiff_t", "drop:OuterDecrypt::SLOT_BASE_OFF",
         "OuterDecrypt::SLOT_BASE_OFF - 4 encrypted UObject slots"),
        ("OuterDecrypt_SlotStride", "std::ptrdiff_t", "drop:OuterDecrypt::SLOT_STRIDE", "OuterDecrypt::SLOT_STRIDE"),
        ("OuterDecrypt_HashPrime", "uint32_t", "drop:OuterDecrypt::HASH_PRIME", "OuterDecrypt::HASH_PRIME (FNV 32)"),
        ("OuterDecrypt_HashAdd", "uint32_t", "drop:OuterDecrypt::HASH_ADD", "OuterDecrypt::HASH_ADD"),
        ("OuterDecrypt_HashRot1", "uint32_t", "drop:OuterDecrypt::HASH_ROT_1", "OuterDecrypt::HASH_ROT_1"),
        ("OuterDecrypt_HashRot2", "uint32_t", "drop:OuterDecrypt::HASH_ROT_2", "OuterDecrypt::HASH_ROT_2"),
        ("OuterDecrypt_XorMask", "uint64_t", "drop:OuterDecrypt::XOR_MASK", "OuterDecrypt::XOR_MASK"),
        ("OuterDecrypt_LaneRot", "uint32_t", "drop:OuterDecrypt::LANE_ROT", "OuterDecrypt::LANE_ROT - ROL32 per lane"),
        ("OuterDecrypt_FinalRot", "uint32_t", "drop:OuterDecrypt::FINAL_ROT", "OuterDecrypt::FINAL_ROT - ROL64"),
        ("OuterDecrypt_HashSeedOff", "std::ptrdiff_t", "lit:0x10",
         "literal in OuterDecrypt::decrypt_outer (sub_1439B00C0): hashes (obj + 0x10)"),
        ("OuterDecrypt_HashShr", "uint32_t", "lit:0x10",
         "literal in OuterDecrypt::decrypt_outer: slot = ((h ^ (h >> 0x10)) & 3) ^ 2"),
        ("OuterDecrypt_SlotMask", "uint32_t", "lit:0x3",
         "literal in OuterDecrypt::decrypt_outer: slot = ((h ^ (h >> 0x10)) & 3) ^ 2"),
        ("OuterDecrypt_SlotXor", "uint32_t", "lit:0x2",
         "literal in OuterDecrypt::decrypt_outer: slot = ((h ^ (h >> 0x10)) & 3) ^ 2"),
        ("TebKeyOff", "std::ptrdiff_t", "drop:TebDecrypt::TEB_KEY_OFF", "TebDecrypt::TEB_KEY_OFF"),
        ("TebSelfPtrOff", "std::ptrdiff_t", "lit:0x30", "TebDecrypt::TEB_SELF_PTR_OFF"),
        ("TebWordRor", "uint32_t", "drop:TebDecrypt::WORD_ROR", "TebDecrypt::WORD_ROR"),
        ("TebQwordRor", "uint32_t", "drop:TebDecrypt::QWORD_ROR", "TebDecrypt::QWORD_ROR"),
    ]),
    ("GUObjectArray chunks manager (sdk.txt drop, 2026-09-22)", [
        ("ChunksManagerRva", "uint64_t", "drop:FName::RVA_CHUNKMGR_GLOBAL",
         "RVA_CHUNKMGR_GLOBAL - encrypted chunks-manager pointer"),
        ("ChunksManagerXorRva", "uint64_t", "drop:FName::RVA_CHUNKMGR_XOR", "RVA_CHUNKMGR_XOR"),
        ("ChunksManagerAddRva", "uint64_t", "drop:FName::RVA_CHUNKMGR_ADD", "RVA_CHUNKMGR_ADD"),
        ("ChunksManagerRol32", "uint32_t", "drop:FName::CHUNKMGR_ROL32", "CHUNKMGR_ROL32"),
        ("ChunksManagerNumElementsOff", "std::ptrdiff_t", "drop:FName::MGR_NUMELEMENTS_OFF", "MGR_NUMELEMENTS_OFF"),
        ("ChunksManagerNumElementsXor", "uint32_t", "drop:FName::MGR_NUMELEMENTS_XOR", "MGR_NUMELEMENTS_XOR"),
        ("ChunksManagerArrayOff", "std::ptrdiff_t", "drop:FName::MGR_CHUNKARRAY_OFF", "MGR_CHUNKARRAY_OFF"),
        ("ChunksManagerArrayXor", "uint64_t", "drop:FName::MGR_CHUNKARRAY_XOR", "MGR_CHUNKARRAY_XOR"),
    ]),
    ("FName pipeline v20260922 (sdk.txt drop constants)", [
        ("FNameBlockXor", "uint64_t", "drop:FName::BLOCK_XOR", "BLOCK_XOR"),
        ("FNameBlockRol64", "uint32_t", "drop:FName::BLOCK_ROL64", "BLOCK_ROL64"),
        ("FNameShardHashAdd", "uint32_t", "drop:FName::SHARD_HASH_ADD", "SHARD_HASH_ADD"),
        ("FNameShardSeedOff", "std::ptrdiff_t", "drop:FName::SHARD_HASH_SEED_OFF", "SHARD_HASH_SEED_OFF"),
        ("FNameShardBlockBase", "std::ptrdiff_t", "drop:FName::SHARD_BLOCK_BASE_OFF", "SHARD_BLOCK_BASE_OFF"),
        ("FNameShardBlockStride", "std::ptrdiff_t", "drop:FName::SHARD_BLOCK_STRIDE", "SHARD_BLOCK_STRIDE"),
        ("FNameShardRolA", "uint32_t", "drop:FName::SHARD_ROL_A", "SHARD_ROL_A"),
        ("FNameShardRolB", "uint32_t", "drop:FName::SHARD_ROL_B", "SHARD_ROL_B"),
        ("FNameShardFinalShr", "uint32_t", "drop:FName::SHARD_FINAL_SHR", "SHARD_FINAL_SHR"),
        ("FNameFnvAdd", "uint64_t", "drop:FName::FNV_ADD", "FNV_ADD"),
        ("FNameFnvRol1", "uint32_t", "drop:FName::FNV_ROL1", "FNV_ROL1"),
        ("FNameFnvRol2", "uint32_t", "drop:FName::FNV_ROL2", "FNV_ROL2"),
        ("FNameKeystreamBaseIdx", "uint32_t", "drop:FName::KEYSTREAM_BASE_IDX", "KEYSTREAM_BASE_IDX"),
        ("FNameKeystreamCount", "uint32_t", "drop:FName::KEYSTREAM_COUNT", "KEYSTREAM_COUNT (u16 words)"),
        ("FNameNarrowKeyShift", "uint32_t", "drop:FName::NARROW_KEY_SHIFT", "NARROW_KEY_SHIFT"),
        ("FNameSlotBaseOff", "std::ptrdiff_t", "drop:FName::SLOT_BASE_OFF", "SLOT_BASE_OFF - UObject encrypted slot base"),
        ("FNameSlotStride", "std::ptrdiff_t", "drop:FName::SLOT_STRIDE", "SLOT_STRIDE"),
        ("FNameHeaderLenShift", "uint32_t", "lit:13",
         "length = (hdr >> 13) | ((hdr >> 3) & 0x3F8) - v20260922 header layout"),
    ]),
    ("Bones v922 (sdk.txt drop, CL-1389382)", [
        ("BoneV922SeedOffset", "std::ptrdiff_t", "lit:0x7B0",
         "BoneArrayDecrypt::SEED_OFFSET - 16-byte XMM seed"),
        ("BoneV922SelectorOffset", "std::ptrdiff_t", "lit:0x848",
         "BoneArrayDecrypt::SELECTOR_OFFSET - descriptor selector dword"),
        ("BoneV922SelectorShift", "uint32_t", "lit:15", "SELECTOR_SHIFT"),
        ("BoneV922SelectorMask", "uint32_t", "lit:1", "SELECTOR_MASK"),
        ("BoneV922DescriptorBase", "std::ptrdiff_t", "lit:0x18",
         "DESCRIPTOR_BASE - descriptor = Base + 0x18 + 0x10 * Selector"),
        ("BoneV922DescriptorStride", "std::ptrdiff_t", "lit:0x10", "DESCRIPTOR_STRIDE"),
        ("BoneV922XorKeyLo", "uint64_t", "lit:0xC05F21012B46E100", "XOR_KEY_LO"),
        ("BoneV922AddKeyLo", "uint64_t", "lit:0x3FA0DEFFD4B91F00", "ADD_KEY_LO"),
        ("BoneV922Rol32", "uint32_t", "lit:3", "ROL32_AMOUNT (pslld 3 | psrld 29)"),
        ("BoneV922Stride", "std::ptrdiff_t", "lit:0x60", "BONE_STRIDE"),
    ]),
    ("Actor", [
        ("RootComponent", "std::ptrdiff_t", "prop:Engine.Actor.RootComponent", ""),
        ("ClassDefaultObject", "std::ptrdiff_t", "lit:0x70",
         "no dump source — int32 read on actor keys"),
        ("ClassDefaultObjectAlt", "std::ptrdiff_t", "lit:0x78", "no dump source"),
        ("ActorTypeId", "std::ptrdiff_t", "lit:0xB0", "no dump source"),
        ("IsRenderedTime", "std::ptrdiff_t", "lit:0x1F0", "no dump source"),
        ("ActorID", "std::ptrdiff_t", "lit:0x18", "no dump source"),
        ("ActorOwner", "std::ptrdiff_t", "prop:Engine.Actor.Owner", ""),
        ("ActorInstigator", "std::ptrdiff_t", "prop:Engine.Actor.Instigator", ""),
        ("Actor_bHiddenByte", "std::ptrdiff_t", "prop:Engine.Actor.bHidden",
         "byte holding bHidden"),
        ("Actor_bHiddenMask", "uint8_t", "maskOf:Engine.Actor.bHidden", ""),
        ("Actor_FlagsDd", "std::ptrdiff_t", "prop:Engine.Actor.bActorEnableCollision",
         "byte holding bActorEnableCollision / bActorIsBeingDestroyed"),
        ("Actor_bActorEnableCollisionMask", "uint8_t",
         "maskOf:Engine.Actor.bActorEnableCollision", ""),
        ("Actor_bActorIsBeingDestroyedMask", "uint8_t",
         "maskOf:Engine.Actor.bActorIsBeingDestroyed", ""),
        ("ReplicatedMovement", "std::ptrdiff_t", "prop:Engine.Actor.ReplicatedMovement", ""),
        ("RepMov_LinearVelocity", "std::ptrdiff_t",
         "prop:Engine.RepMovement.LinearVelocity", "offset within FRepMovement"),
        ("Actor_InstanceComponents", "std::ptrdiff_t",
         "prop:Engine.Actor.InstanceComponents", ""),
    ]),
    # CoreUObject.Transform - the dump's own FTransform. The CTW slot cannot be
    # sourced from the dump (not a UPROPERTY), but its *layout* can, and the layout
    # is what makes the live pin checkable: Quat 0x00, Translation 0x20, pad 0x38,
    # Scale3D 0x40, size 0x60.
    ("FTransform layout (CoreUObject.Transform from the dump)", [
        ("Transform_Rotation", "std::ptrdiff_t", "prop:CoreUObject.Transform.Rotation",
         "FTransform::Rotation (Quat, 0x20 bytes)"),
        ("Transform_Translation", "std::ptrdiff_t",
         "prop:CoreUObject.Transform.Translation",
         "FTransform::Translation (FVector3d) - the live world position"),
        ("Transform_Scale3D", "std::ptrdiff_t", "prop:CoreUObject.Transform.Scale3D",
         "FTransform::Scale3D (FVector3d)"),
        ("Transform_Size", "std::ptrdiff_t", "sizeof:CoreUObject.Transform",
         "sizeof(FTransform) = Quat 0x20 + Translation 0x18 + pad 0x8 + Scale3D 0x18"),
    ]),
    ("SceneComponent", [
        ("RelativeLocation", "std::ptrdiff_t", "prop:Engine.SceneComponent.RelativeLocation", ""),
        # CONFLICT, unresolved: ArcOffsets::SceneComponent::RELATIVE_LOCATION
        # says 0x2A8 ("live-verified 2026-09-14, was 0x260") and the dump says
        # 0x268. Nothing in this repo can settle it without the live target, so
        # both are named and the dump value stays primary.
        ("RelativeLocation_Alt", "std::ptrdiff_t", "lit:0x2A8",
         "ArcOffsets::SceneComponent::RELATIVE_LOCATION = 0x2A8 (conflicts with the dump's 0x268)"),
        ("RelativeRotation", "std::ptrdiff_t", "prop:Engine.SceneComponent.RelativeRotation", ""),
        ("ComponentVelocity", "std::ptrdiff_t", "prop:Engine.SceneComponent.ComponentVelocity", ""),
        ("ComponentToWorld", "std::ptrdiff_t", "lit:0x310",
         "v20260922 FTransform block; translation at +0x20 => 0x330"),
        ("ComponentToWorld_Alt", "std::ptrdiff_t", "lit:0x2D0",
         "previous build's slot, alternate candidate for the runtime probe"),
        ("ComponentToWorld_Rotation", "std::ptrdiff_t",
         "expr:ComponentToWorld + Transform_Rotation",
         "FTransform::Rotation (Quat) inside the block"),
        ("ComponentToWorld_Translation", "std::ptrdiff_t",
         "expr:ComponentToWorld + Transform_Translation",
         "FTransform::Translation (FVector3d) inside the block"),
        ("ComponentToWorld_Scale3D", "std::ptrdiff_t",
         "expr:ComponentToWorld + Transform_Scale3D",
         "FTransform::Scale3D (FVector3d) inside the block"),
        ("WorldLocation", "std::ptrdiff_t", "expr:ComponentToWorld_Translation",
         "the live world position (ComponentToWorld + 0x20, FVector3d LWC)"),
        ("Scene_bVisibleByte", "std::ptrdiff_t", "prop:Engine.SceneComponent.bVisible", ""),
        ("Scene_bVisibleMask", "uint8_t", "maskOf:Engine.SceneComponent.bVisible", ""),
        ("Scene_bHiddenInGameByte", "std::ptrdiff_t", "prop:Engine.SceneComponent.bHiddenInGame", ""),
        ("Scene_bHiddenInGameMask", "uint8_t", "maskOf:Engine.SceneComponent.bHiddenInGame", ""),
        ("AttachChildren", "std::ptrdiff_t", "prop:Engine.SceneComponent.AttachChildren", ""),
    ]),
    ("Primitive / skinned mesh", [
        ("BoundsScale", "std::ptrdiff_t", "prop:Engine.PrimitiveComponent.BoundsScale", ""),
        # Neither render stamp is a UPROPERTY, so the dump cannot see them. The
        # pair is PLAIN floats (CL-1372005) and moves with the build: the vis
        # check user-provided 2026-09-23 pins them at 0x480/0x484 (the drop's
        # 0x4C0/0x4C4 reads zero on the current build). The old 0x4C8/0x4CC
        # pair is the retired XOR-era slot the LRTS raw path once compared.
        ("LastRenderTime", "std::ptrdiff_t", "lit:0x480",
         "vis check user-provided 2026-09-23: PrimitiveComponent::LastRenderTime = 0x480 (plain float)"),
        ("LastRenderTimeOnScreen", "std::ptrdiff_t", "lit:0x484",
         "vis check user-provided 2026-09-23: LastRenderTimeOnScreen = 0x484 (plain float)"),
        ("LastSubmitTime", "std::ptrdiff_t", "expr:LastRenderTime",
         "alias kept for the LRTS raw fast-path (== LastRenderTime)"),
        ("VisibilityBasedAnimTickOption", "std::ptrdiff_t",
         "prop:Engine.SkinnedMeshComponent.VisibilityBasedAnimTickOption", ""),
        ("bRecentlyRendered", "std::ptrdiff_t", "prop:Engine.SkinnedMeshComponent.bRecentlyRendered",
         "byte holding bRecentlyRendered"),
        ("bRecentlyRenderedMask", "uint8_t",
         "maskOf:Engine.SkinnedMeshComponent.bRecentlyRendered", ""),
    ]),
    ("Bone decrypt slots (non-reflected — no dump source)", [
        ("Encrypted", "std::ptrdiff_t", "lit:0x7B0", "bone pointer decrypt seed"),
        ("Encrypted_Legacy", "std::ptrdiff_t", "lit:0x790", "previous build's seed slot"),
        ("LodSelect", "std::ptrdiff_t", "lit:0x7D0", "LOD selector"),
        ("LodSelect_Legacy", "std::ptrdiff_t", "lit:0x7D0", "previous LOD slot"),
    ]),
    ("Mesh / character", [
        # CONFLICT, unresolved: ArcOffsets::Actor::MESH says 0x4A0 ("was
        # 0x450") where the dump says 0x440, and the skinned mesh asset is
        # 0x740 there vs 0x720 in the dump. Both names are exposed so a probe
        # can pick live instead of us guessing.
        ("USkeletalMeshComponent_Alt", "std::ptrdiff_t", "lit:0x4A0",
         "ArcOffsets::Actor::MESH = 0x4A0 (conflicts with the dump's 0x440)"),
        ("SkeletalMeshAsset_Alt", "std::ptrdiff_t", "lit:0x740",
         "ArcOffsets::SkeletalMeshComponent::SKELETAL_MESH = 0x740 (conflicts with the dump's 0x720)"),
        ("USkeletalMeshComponent", "std::ptrdiff_t", "prop:Engine.Character.Mesh",
         "ACharacter::Mesh"),
        ("SkeletalMeshAsset", "std::ptrdiff_t", "prop:Engine.SkinnedMeshComponent.SkeletalMesh", ""),
        ("bNoSkeletonUpdate", "std::ptrdiff_t",
         "prop:Engine.SkeletalMeshComponent.bNoSkeletonUpdate", ""),
        ("bNoSkeletonUpdateMask", "uint8_t",
         "maskOf:Engine.SkeletalMeshComponent.bNoSkeletonUpdate", ""),
        ("bForceRefpose", "std::ptrdiff_t",
         "prop:Engine.SkeletalMeshComponent.bForceRefpose", ""),
        ("bForceRefposeMask", "uint8_t",
         "maskOf:Engine.SkeletalMeshComponent.bForceRefpose", ""),
        ("CharacterMovement", "std::ptrdiff_t", "prop:Engine.Character.CharacterMovement", ""),
        ("Character_CapsuleComponent", "std::ptrdiff_t",
         "prop:Engine.Character.CapsuleComponent", ""),
        ("CMC_LastUpdateLocation", "std::ptrdiff_t",
         "prop:Engine.CharacterMovementComponent.LastUpdateLocation", ""),
        ("StateInterpolator", "std::ptrdiff_t",
         "prop:EmbarkCharacter.EmbarkCharacterBase.StateInterpolatorComponent", ""),
        ("ReplicatedRootTransform", "std::ptrdiff_t", "lit:0x1F8",
         "struct field on the interpolator state — no dump source"),
        ("Velocity", "std::ptrdiff_t", "prop:Engine.MovementComponent.Velocity", ""),
        ("PioneerCharacterMovement", "std::ptrdiff_t",
         "prop:PioneerGameplay.PioneerCharacterBase.PioneerCharacterMovement", ""),
        ("HealthComponent", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerCharacter.HealthComponent", ""),
        ("InventoryComponent", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerCharacter.InventoryComponent", ""),
        ("EmbarkMesh", "std::ptrdiff_t",
         "prop:EmbarkCharacter.EmbarkCharacterBase.EmbarkMesh", ""),
    ]),
    ("Health", [
        ("HealthInfo", "std::ptrdiff_t", "lit:0x550",
         "derived block on PioneerPlayerState — no dump source"),
        ("PlayerState_Health", "std::ptrdiff_t", "expr:HealthInfo", ""),
        ("PlayerState_MaxHealth", "std::ptrdiff_t", "lit:0x558", "derived — no dump source"),
        ("PlayerState_Armor", "std::ptrdiff_t", "lit:0x560", "derived — no dump source"),
        ("PlayerState_MaxArmor", "std::ptrdiff_t", "lit:0x568", "derived — no dump source"),
        # Value first, then the dump field it shadows: the dump types
        # CachedHealth as FName, so reading its slot returned a name index.
        ("Health", "std::ptrdiff_t", "lit:0x6D0",
         "ArcOffsets::HealthComponent::CURRENT_HEALTH = 0x6D0 (live-verified; was the 0x700 FName shadow)"),
        ("HealthNameShadow", "std::ptrdiff_t", "prop:Angelscript.HealthComponent.CachedHealth",
         "FName on this build — value read is a name index, not a float"),
        ("MaxHealth", "std::ptrdiff_t", "lit:0x348",
         "ArcOffsets::HealthComponent::MAX_HEALTH = 0x348 (native, inside the dump's Pad_0340 hole)"),
        ("Shield", "std::ptrdiff_t", "prop:Angelscript.HealthComponent.Armor",
         "FArmorSlot block"),
        ("ShieldMax", "std::ptrdiff_t", "expr:Shield + 0x10",
         "FArmorSlot::CurrentMaxArmor"),
        ("MaxDBNO", "std::ptrdiff_t", "prop:Angelscript.HealthComponent.MaxDBNOHealth", "FName"),
        ("TeamID", "std::ptrdiff_t", "prop:EmbarkCharacter.EmbarkCharacterBase.TeamId", ""),
    ]),
    ("Inventory / weapons", [
        ("CurrentItemActors", "std::ptrdiff_t",
         "prop:Angelscript.InventoryComponent.CurrentItemActors", ""),
        ("LocalCurrentItemActors", "std::ptrdiff_t",
         "prop:Angelscript.InventoryComponent.LocalCurrentItemActors", ""),
        ("EquippedPrimaryItem", "std::ptrdiff_t",
         "prop:Angelscript.InventoryComponent.EquippedArmor", "reads the equipped armor slot"),
        ("WeaponClip", "std::ptrdiff_t", "prop:Angelscript.BeamFirearmActor.ClipSize", ""),
        ("WeaponQuality", "std::ptrdiff_t", "prop:Angelscript.BeamFirearmActor.WeaponQuality", ""),
        ("StowedWeaponSlot0", "std::ptrdiff_t",
         "prop:Angelscript.InventoryComponent.StowedWeapon0", ""),
        ("StowedWeaponSlot1", "std::ptrdiff_t",
         "prop:Angelscript.InventoryComponent.StowedWeapon1", ""),
        ("EquippedArmor", "std::ptrdiff_t",
         "prop:Angelscript.InventoryComponent.EquippedArmor", ""),
    ]),
    ("Loot / containers", [
        ("LootInteractionComponent", "std::ptrdiff_t",
         "prop:Angelscript.LootContainerSingle.LootInteractionComponent", ""),
        ("LootContainer_ItemContainer", "std::ptrdiff_t", "lit:0xBD8",
         "working runtime profile: LootContainer item-container field"),
        ("LootInteraction_Container", "std::ptrdiff_t",
         "expr:LootInteractionComponent", "same slot"),
        ("SalvageContainer_ChosenMesh", "std::ptrdiff_t",
         "prop:Angelscript.SalvageContainerSingle.ChosenMesh", ""),
        ("LootInteraction_Searched", "std::ptrdiff_t", "lit:0xBD8",
         "working runtime profile: loot searched/state field"),
        ("SimpleLootActivity_LootInteraction", "std::ptrdiff_t", "lit:0x480", "no dump source"),
        ("SimpleLootActivity_LootStateMachine", "std::ptrdiff_t", "lit:0x4A8", "no dump source"),
        ("SimpleLootActivity_ItemContainer", "std::ptrdiff_t", "lit:0x498", "no dump source"),
        ("SalvageContainer_MeshVariants", "std::ptrdiff_t",
         "prop:Angelscript.SalvageContainerSingle.MeshVariants", ""),
        ("ItemDataAsset", "std::ptrdiff_t", "lit:0x898", "no dump source"),
        ("BP_PickupBase_SpawnItems", "std::ptrdiff_t", "prop:Angelscript.Pickup.SpawnItems", ""),
        ("ItemContainer_OpenTime", "std::ptrdiff_t",
         "prop:Angelscript.ItemContainerComponent.OpenTime", ""),
        ("ConstructableItemContainer_OpenTime", "std::ptrdiff_t",
         "prop:Angelscript.ConstructableItemContainerComponent.OpenTime", ""),
        ("ItemDataAsset_OverrideItemAssetId", "std::ptrdiff_t", "lit:0x120", "no dump source"),
        ("ItemDataAsset_bOverrideItemAssetId", "std::ptrdiff_t", "lit:0x118", "no dump source"),
        ("UIHoverData", "std::ptrdiff_t", "lit:0x620",
         "sdk.txt Pickup::UI_HOVER_DATA (FItemUIHoverData inline block; 0x550 was the pre-20260922 probe)"),
        ("UIHoverData_Pickup", "std::ptrdiff_t", "expr:UIHoverData",
         "same APickup field; compatibility alias for older call sites"),
        ("Pickup_RootCollider", "std::ptrdiff_t", "prop:Angelscript.Pickup.RootCollider",
         "the dump reflects it here and the CL drop agrees (was lit:0x470)"),
        ("Pickup_Interaction", "std::ptrdiff_t", "prop:Angelscript.Pickup.Interaction",
         "the dump reflects it here and the CL drop agrees (was lit:0x488)"),
        ("Pickup_DefaultPickupDataAsset", "std::ptrdiff_t",
         "prop:Angelscript.Pickup.DefaultPickupDataAsset",
         "the dump reflects it here and the CL drop agrees (was lit:0x498)"),
        ("Pickup_ContainedItem_BB", "std::ptrdiff_t", "lit:0x4A0", "no dump source"),
        ("BBItem_DataAssetIndex", "std::ptrdiff_t", "lit:0x0", "struct field"),
        ("BBItem_AmountValue", "std::ptrdiff_t", "lit:0x4 + 0xC", "struct field"),

        # FItemUIHoverData (sdk.txt): per-stack hover tooltip fields read for the
        # crate-contents preview. Drop-only struct - no dump source.
        ("ItemUIHoverData_DisplayName", "std::ptrdiff_t", "lit:0x0",
         "sdk.txt ItemUIHoverData::DISPLAY_NAME - FText per-stack display name"),
        ("ItemUIHoverData_Amount", "std::ptrdiff_t", "lit:0x18",
         "sdk.txt ItemUIHoverData::AMOUNT - int32 stack amount"),
        ("ItemUIHoverData_MaxStack", "std::ptrdiff_t", "lit:0x1C",
         "sdk.txt ItemUIHoverData::MAX_STACK - int32 max stack"),
        ("ItemUIHoverData_DataAsset", "std::ptrdiff_t", "lit:0x20",
         "sdk.txt ItemUIHoverData::DATA_ASSET - UItemDataAsset*"),
        ("ItemUIHoverData_Size", "std::ptrdiff_t", "lit:0x28",
         "sizeof(FItemUIHoverData): FText 0x18 + amount + max stack + ptr"),
        ("Pickup_VisibleAmount", "std::ptrdiff_t", "lit:0x4D0",
         "sdk.txt Pickup::VISIBLE_AMOUNT (estimated) - visible stack amount"),
        ("Interaction_bIsActiveByte", "std::ptrdiff_t", "lit:0x137", "no dump source"),
        ("Interaction_bIsActiveMask", "uint8_t", "lit:0x8", ""),
        ("Interaction_CurrentInteractionState", "std::ptrdiff_t",
         "prop:Angelscript.BaseInteractionComponent.CurrentInteractionState", ""),
    ]),
    ("Constructables", [
        ("bIsBreaked", "std::ptrdiff_t",
         "prop:Angelscript.PioneerConstructablePawn.bIsDestroyed", ""),
        ("Constructable_EnemyTypeDataAsset", "std::ptrdiff_t",
         "prop:Angelscript.PioneerConstructablePawn.EnemyTypeDataAsset", ""),
        ("Constructable_AITemplateData", "std::ptrdiff_t",
         "prop:Angelscript.PioneerConstructablePawn.AITemplateData", ""),
        ("Constructable_bIsDestroyed", "std::ptrdiff_t",
         "prop:Angelscript.PioneerConstructablePawn.bIsDestroyed", ""),
    ]),
    ("Extraction / static mesh / misc", [
        ("ExtractionPoint_State", "std::ptrdiff_t",
         "prop:Angelscript.SalvageExtractionPointBase.State",
         "the drop's 0xBD2 is the pre-shift block; the dump reflects the state here"),
        ("ExtractionPoint_ExtractionInfo", "std::ptrdiff_t",
         "prop:Angelscript.SalvageExtractionPointBase.ExtractionInfo",
         "FExtractionInfo { double startedTs; double time; } - timer struct"),
        ("ExtractionPoint_bIsEnabled", "std::ptrdiff_t",
         "prop:Angelscript.SalvageExtractionPointBase.bIsEnabled", ""),
        ("ExtractionPoint_StateChangeTimestamp", "std::ptrdiff_t",
         "prop:Angelscript.SalvageExtractionPointBase.StateChangeTimestamp", ""),
        ("ExtractionPoint_TimeLeftAfterShutdown", "std::ptrdiff_t",
         "prop:Angelscript.SalvageExtractionPoint.TimeLeftAfterShutdown",
         "config window (double seconds)"),
        ("ExtractionPoint_TimeLeftAfterStartup", "std::ptrdiff_t",
         "prop:Angelscript.SalvageExtractionPoint.TimeLeftAfterStartup",
         "config window (double seconds)"),
        ("StaticMesh", "std::ptrdiff_t", "prop:Engine.StaticMeshComponent.StaticMesh", ""),
        ("StaticMesh_ExtendedBounds", "std::ptrdiff_t",
         "prop:Engine.StaticMesh.ExtendedBounds", "FBoxSphereBounds on the asset"),
        ("StaticMeshLegacy", "std::ptrdiff_t", "lit:0x718", "previous build's slot"),
        ("Mesh_LastRenderTimeEnc", "std::ptrdiff_t", "lit:0x4BC", "no dump source"),
        ("Mesh_LastRenderTimeOnScreenEnc", "std::ptrdiff_t", "lit:0x4C4", "no dump source"),
        ("Mesh_LastRenderTimeKey", "uint32_t", "lit:0x5AB299E0", "XOR key"),
        ("Mesh_LastRenderTimeOnScreenKey", "uint32_t", "lit:0xA83E5CBE", "XOR key"),
        ("UActorComponent_WorldPrivate", "std::ptrdiff_t",
         "drop:PrimitiveComponent::WORLD_PRIVATE",
         "the drop found the UWorld ptr at +0x140 (live-verified 2026-08-20, was 0x148)"),
        ("UWorld_TimeSeconds", "std::ptrdiff_t", "drop:UWorld::TIME_SECONDS",
         "the drop re-verified it at +0x240 on 2026-09-09 (was 0x8B0); the dump pads the World tail"),
    ]),
    # FRepMovement — the dump's own struct, and the one world position on an
    # actor that is a UPROPERTY (so it is reflection-verified, unlike
    # ComponentToWorld, which is not a UPROPERTY on this build and therefore has
    # no dump source).
    ("Replicated movement (Engine.RepMovement from the dump)", [
        ("RepMovement_Location", "std::ptrdiff_t", "prop:Engine.RepMovement.Location",
         "FRepMovement::Location — LWC double Vector, actor-relative to here"),
        ("RepMovement_Rotation", "std::ptrdiff_t", "prop:Engine.RepMovement.Rotation",
         "FRepMovement::Rotation"),
        ("RepMovement_AngularVelocity", "std::ptrdiff_t",
         "prop:Engine.RepMovement.AngularVelocity", "FRepMovement::AngularVelocity"),
        ("RepMovement_ServerFrame", "std::ptrdiff_t", "prop:Engine.RepMovement.ServerFrame",
         "FRepMovement::ServerFrame"),
        ("RepMovement_bRepPhysicsByte", "std::ptrdiff_t", "prop:Engine.RepMovement.bRepPhysics",
         "byte holding FRepMovement::bRepPhysics"),
        ("RepMovement_bRepPhysicsMask", "uint8_t", "lit:0x02",
         "FRepMovement::bRepPhysics mask"),
    ]),
    # ---- Intel features (loadout / DBNO / look arrows / squads / bot vision /
    # part damage / raid clock / activity feed / map radar). All drop-sourced.
    ("Intel: loadout (InventoryComponent / stowed layout / weapon actors)", [
        ("Inventory_Loadout", "std::ptrdiff_t", "drop:InventoryComponent::LOADOUT",
         "FInventoryLoadout block start"),
        ("Inventory_Backpack", "std::ptrdiff_t", "drop:InventoryComponent::BACKPACK",
         "backpack container slot (UItemContainer*)"),
        ("Inventory_Belt", "std::ptrdiff_t", "drop:InventoryComponent::BELT",
         "belt container slot (UItemContainer*)"),
        ("Inventory_SafePouch", "std::ptrdiff_t", "drop:InventoryComponent::SAFE_POUCH",
         "safe-pouch item slot (UItemBase*)"),
        ("StowedInfo_ItemDataAsset", "std::ptrdiff_t", "drop:StowedWeaponLayout::ITEM_DATA_ASSET",
         "FStowedWeaponInfo::ItemDataAsset - DA_WeaponVisuals names the gun"),
        ("StowedInfo_Quality", "std::ptrdiff_t", "drop:StowedWeaponLayout::WEAPON_QUALITY",
         "FStowedWeaponInfo::WeaponQuality (int32 0-3 = I-IV)"),
        ("WeaponActor_ClipSize", "std::ptrdiff_t", "drop:WeaponActor::CLIP_SIZE",
         "WeaponActor::ClipSize (uint16)"),
        ("WeaponActor_Quality", "std::ptrdiff_t", "drop:WeaponActor::WEAPON_QUALITY",
         "WeaponActor::WeaponQuality (uint16)"),
        ("Inventory_StowedToolActor", "std::ptrdiff_t",
         "drop:InventoryComponent::STOWED_TOOL_ACTOR",
         "stowed tool slot (AStowedWeaponActor* - radar/knife/flashlight)"),
        ("StowedInfo_Size", "std::ptrdiff_t", "drop:StowedWeaponLayout::STRUCT_SIZE",
         "sizeof(FStowedWeaponInfo) stride pin"),
        ("ItemContainer_ItemLimit", "std::ptrdiff_t",
         "prop:Angelscript.ItemContainerComponent.ItemLimit",
         "UItemContainer::ItemLimit - belt/backpack slot capacity"),
        ("ItemBase_Quality", "std::ptrdiff_t", "drop:ItemBase::QUALITY_LEVEL",
         "EItemRarity (0=common,1=rare,2=epic,3=legendary) on the item/CDO"),
        ("UClass_DefaultObjectSlot", "std::ptrdiff_t",
         "prop:CoreUObject.Class.ClassDefaultObject",
         "UClass::ClassDefaultObject slot - rarity chain reads the CDO. The drop's "
         "ArcOffsets::UClass::DEFAULT_OBJECT 0x130 is the pre-shift CL-1341255 slot "
         "(0x130 is EClassCastFlags now) and the dump reflects the CDO here - same "
         "pattern as ExtractionPoint_State"),
        ("PickupDataAsset_ResolvedItemClass", "std::ptrdiff_t",
         "drop:PickupDataAsset::RESOLVED_ITEM_CLASS",
         "pickup data asset -> resolved UItemBase class (rarity naming chain)"),
        ("Pickup_DefaultDataAsset", "std::ptrdiff_t",
         "drop:Pickup::DEFAULT_PICKUP_DATA_ASSET",
         "APickup::DefaultPickupDataAsset - the rarity chain's entry point"),
    ]),
    ("Intel: container state (socket parts / searched / dispenser)", [
        ("LootContainer_SocketMesh", "std::ptrdiff_t",
         "drop:LootContainerSingle::SOCKET_LOOT_CONTAINER_MESH",
         "UStaticMeshComponent* carrying the socket loot parts"),
        ("LootInteract_OpenedByte", "std::ptrdiff_t",
         "prop:Angelscript.LootInteractionComponent.bHasBeenOpened",
         "bHasBeenOpened byte - the drop's BYTE_HAS_BEEN_OPENED 0x8D8 is the "
         "pre-shift slot; the dump reflects it here (same pattern as "
         "ExtractionPoint_State)"),
        ("LootInteract_OpenedMask", "uint8_t",
         "drop:LootInteractionComponent::MASK_HAS_BEEN_OPENED",
         "bHasBeenOpened bit mask"),
        ("LootInteract_AcquisitionMethod", "std::ptrdiff_t",
         "drop:LootInteractionComponent::LOOT_ACQUISITION_METHOD",
         "LootAcquisitionMethod enum byte - values are not in the dump, so it "
         "only contributes a plausibility gate"),
        ("LootInteract_DispenserLocations", "std::ptrdiff_t",
         "drop:LootInteractionComponent::DISPENSER_LOCATIONS",
         "TArray<Vector> dispenser drop ports (dump-verified)"),
        ("LootInteract_PingIconOffset", "std::ptrdiff_t",
         "drop:LootInteractionComponent::LOOT_PING_ICON_OFFSET",
         "Vector loot-ping icon offset - block coherence gate (dump-verified)"),
    ]),
    ("Intel: DBNO / revive (FPlayerHealthInfo + interaction timers)", [
        ("PlayerHealthInfoBase", "std::ptrdiff_t", "drop:PlayerState::HEALTH_INFO_BASE",
         "FPlayerHealthInfo inline block on PioneerPlayerState"),
        ("PHI_DBNOByte", "std::ptrdiff_t", "drop:PlayerHealthInfo::B_IS_DBNO",
         "byte in FPlayerHealthInfo holding bIsDBNO"),
        ("PHI_Health", "std::ptrdiff_t", "drop:PlayerHealthInfo::HEALTH",
         "double current health - block-anchor sanity gate"),
        ("PHI_MaxHealth", "std::ptrdiff_t", "drop:PlayerHealthInfo::MAX_HEALTH",
         "double max health - block-anchor sanity gate"),
        ("PHI_BrokenArmorByte", "std::ptrdiff_t", "drop:PlayerHealthInfo::B_HAS_BROKEN_ARMOR",
         "byte holding bHasBrokenArmor"),
        ("HC_MaxDBNOHealth", "std::ptrdiff_t", "drop:HealthComponent::MAX_DBNO_HEALTH",
         "HealthComponent::MaxDBNOHealth (double)"),
        ("Interact_DBNOTimerFloats", "std::ptrdiff_t",
         "drop:BaseInteractionComponent::DBNO_TIMER_FLOATS", "bleedout timer float pair"),
        ("Interact_DBNOTimerTicks", "std::ptrdiff_t",
         "drop:BaseInteractionComponent::DBNO_TIMER_TICKS", "bleedout timer tick pair"),
        ("Interact_DefibTimerFloats", "std::ptrdiff_t",
         "drop:BaseInteractionComponent::DEFIB_TIMER_FLOATS", "revive/defib timer float pair"),
        ("Interact_DefibTimerTicks", "std::ptrdiff_t",
         "drop:BaseInteractionComponent::DEFIB_TIMER_TICKS", "revive/defib timer tick pair"),
    ]),
    ("Intel: look direction (PlayerCameraManager view clamps)", [
        ("PCM_ViewPitchMax", "std::ptrdiff_t", "drop:PlayerCameraManager::VIEW_PITCH_MAX",
         "APlayerCameraManager::ViewPitchMax - rotation sanity clamp"),
        ("PCM_ViewPitchMin", "std::ptrdiff_t", "drop:PlayerCameraManager::VIEW_PITCH_MIN",
         "APlayerCameraManager::ViewPitchMin"),
        ("PCM_ViewYawMax", "std::ptrdiff_t", "drop:PlayerCameraManager::VIEW_YAW_MAX",
         "APlayerCameraManager::ViewYawMax"),
        ("PCM_ViewYawMin", "std::ptrdiff_t", "drop:PlayerCameraManager::VIEW_YAW_MIN",
         "APlayerCameraManager::ViewYawMin"),
    ]),
    ("Intel: squads and identity (platform id + squad arrays)", [
        ("PS_PlatformIdComponent", "std::ptrdiff_t",
         "prop:EmbarkGameplay.EmbarkPlayerStateBase.PlatformIdComponent",
         "UEmbarkPlatformIdComponent* on AEmbarkPlayerStateBase"),
        ("PlatformId_Repl", "std::ptrdiff_t",
         "prop:EmbarkGameplay.EmbarkPlatformIdComponent.PlatformId",
         "FUniqueNetIdRepl (0x30 bytes)"),
        ("NetIdRepl_Object", "std::ptrdiff_t", "drop:UniqueNetIdRepl::NET_ID_OBJECT",
         "TVariant shared-ptr alternative object"),
        ("NetIdRepl_TypeIndex", "std::ptrdiff_t", "drop:UniqueNetIdRepl::TYPE_INDEX",
         "TVariant type index byte (0 = shared-ptr alternative)"),
        ("NetId_Payload", "std::ptrdiff_t", "drop:UniqueNetId::PAYLOAD",
         "FUniqueNetId payload - Steam CSteamID uint64"),
        ("PS_Squad", "std::ptrdiff_t", "prop:EmbarkGameplay.EmbarkPlayerStateBase.Squad",
         "UEmbarkSquad* - real squad membership"),
        ("EmbarkGS_SquadsArray", "std::ptrdiff_t", "drop:EmbarkGameStateBase::SQUADS_ARRAY",
         "TArray<UEmbarkSquad*>"),
        ("EmbarkGS_AllSquads", "std::ptrdiff_t", "drop:EmbarkGameStateBase::ALL_SQUADS",
         "TArray<UEmbarkSquad*>"),
        ("EmbarkGS_MatchState", "std::ptrdiff_t", "drop:EmbarkGameStateBase::MATCH_STATE",
         "FName tag (InProgress/WaitingToStart/...)"),
        ("PS_PlayerNamePrivate2", "std::ptrdiff_t", "drop:PlayerState::PLAYER_NAME_PRIVATE_2",
         "second name slot - fallback when PlayerNamePrivate reads empty"),
        ("PS_BotStateByte", "std::ptrdiff_t", "prop:Engine.PlayerState.bIsABot",
         "packed bool byte - the drop's B_IS_A_BOT_BYTE 0x3DA is the pre-shift "
         "slot; the dump reflects the bools here (masks match bit for bit)"),
        ("PS_BotStateBotMask", "uint8_t", "drop:PlayerState::B_IS_A_BOT_MASK",
         "bIsABot bit mask"),
        ("PS_BotStateSpectatorMask", "uint8_t", "drop:PlayerState::B_IS_SPECTATOR_MASK",
         "bIsSpectator bit mask"),
        ("PS_FinishedRoundByte", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerState.bFinishedRound",
         "bFinishedRound bool byte - the drop's B_FINISHED_ROUND 0x5B8 is the "
         "pre-shift slot"),
        ("PS_FinishedRoundMask", "uint8_t",
         "maskOf:Angelscript.PioneerPlayerState.bFinishedRound",
         "bFinishedRound bit mask"),
        ("PS_AchievementComponent", "std::ptrdiff_t",
         "prop:Angelscript.PioneerPlayerState.AchievementComponent",
         "PioneerAchievementPlayerStateComponent* - the drop's "
         "ACHIEVEMENT_COMPONENT 0x5C0 is the pre-shift slot; doubles as the "
         "PioneerPlayerState block coherence anchor"),
        ("NetIdRepl_ReplBytesData", "std::ptrdiff_t", "drop:UniqueNetIdRepl::REPL_BYTES_DATA",
         "TArray<uint8> ReplicationBytes.Data - FAccountId inline alternative"),
        ("NetIdRepl_ReplBytesCount", "std::ptrdiff_t", "drop:UniqueNetIdRepl::REPL_BYTES_COUNT",
         "TArray<uint8> ReplicationBytes.Num"),
    ]),
    ("Intel: bot vision and alertness (AIPerception + AIStateService)", [
        ("AIController_Perception", "std::ptrdiff_t", "drop:AIController::PERCEPTION_COMPONENT",
         "UAIPerceptionComponent* on the bot's AIController"),
        ("AIPerception_SensesConfig", "std::ptrdiff_t",
         "drop:AIPerceptionComponent::SENSES_CONFIG", "TArray<UAISenseConfig*>"),
        ("AISight_SightRadius", "std::ptrdiff_t", "drop:AISenseConfigSight::SIGHT_RADIUS",
         "UAISenseConfigSight::SightRadius (float, cm)"),
        ("AISight_LoseSightRadius", "std::ptrdiff_t",
         "drop:AISenseConfigSight::LOSE_SIGHT_RADIUS", "LoseSightRadius (float, cm)"),
        ("AISight_PeripheralDeg", "std::ptrdiff_t",
         "drop:AISenseConfigSight::PERIPHERAL_VISION_DEG",
         "PeripheralVisionAngle (float, half-angle degrees)"),
        ("Constructable_AIStateService", "std::ptrdiff_t",
         "drop:PioneerConstructablePawn::AI_STATE_SERVICE",
         "UPioneerConstructableAIStateService*"),
        ("AIState_Alertness", "std::ptrdiff_t", "drop:AIStateService::ALERTNESS",
         "uint8 alertness (0=Idle .. 3=Combat)"),
        ("AIState_CombatPhase", "std::ptrdiff_t", "drop:AIStateService::COMBAT_PHASE",
         "uint8 combat phase (search/engage)"),
        ("AIState_SightRange", "std::ptrdiff_t", "drop:AIStateService::SIGHT_RANGE",
         "float sight range (cm)"),
        ("AIState_SightHalfAngle", "std::ptrdiff_t", "drop:AIStateService::SIGHT_HALF_ANGLE",
         "uint8 compressed degrees (half angle)"),
    ]),
    ("Intel: per-part damage (HealthService + style drivers)", [
        ("Constructable_HealthService", "std::ptrdiff_t",
         "drop:PioneerConstructablePawn::HEALTH_SERVICE",
         "UConstructableHealthServiceComponent*"),
        ("PartHpArray", "std::ptrdiff_t", "drop:HealthService::PART_HP_ARRAY",
         "TArray<float> per-part hp fraction @ +0x280"),
        ("StyleDrivers", "std::ptrdiff_t", "drop:ConstructableBase::ALL_STYLE_DRIVERS",
         "TArray style-driver components (per-part mesh style)"),
        ("Style_IsDestroyedByte", "std::ptrdiff_t",
         "drop:ConstructableStaticMeshStyle::IS_DESTROYED", "per-part destroyed flag byte"),
        ("Style_DestroyedReasonByte", "std::ptrdiff_t",
         "drop:ConstructableStaticMeshStyle::DESTROYED_REASON", "per-part destroyed reason byte"),
        ("Style_PartIdRange", "std::ptrdiff_t",
         "drop:ConstructableStaticMeshStyle::PART_ID_RANGE", "part id range in PART_HP_ARRAY"),
    ]),
    ("Intel: raid clock and match state (FStageInfo + PioneerGameState)", [
        ("GameState_StageInfoRef", "std::ptrdiff_t", "drop:GameState::STAGE_INFO",
         "FStageInfo inline block on PioneerGameState"),
        ("StageInfo_TimeLeftOff", "std::ptrdiff_t", "drop:StageInfo::TIME_LEFT",
         "FStageInfo::TimeLeft - raid clock seconds remaining"),
        ("StageInfo_GraceTimeOff", "std::ptrdiff_t", "drop:StageInfo::GRACE_TIME",
         "FStageInfo::GraceTime - post-raid grace seconds"),
        ("EmbarkGS_ElapsedTime", "std::ptrdiff_t", "drop:EmbarkGameStateBase::ELAPSED_TIME",
         "int32 seconds since match start"),
    ]),
    ("Intel: activity feed (interaction state + instigators)", [
        ("Interact_ActiveInstigator", "std::ptrdiff_t",
         "drop:BaseInteractionComponent::ACTIVE_INSTIGATOR",
         "who is running the interaction right now"),
        ("Interact_DefaultInstigator", "std::ptrdiff_t",
         "drop:BaseInteractionComponent::DEFAULT_INSTIGATOR", "default instigator slot"),
    ]),
    ("Intel: map radar (WorldPartitionMiniMap + level settings)", [
        ("MiniMap_UnitsPerPixel", "std::ptrdiff_t",
         "drop:WorldPartitionMiniMap::WORLD_UNITS_PER_PIXEL",
         "world units per minimap pixel - authoritative radar scale"),
        ("MiniMap_WorldBounds", "std::ptrdiff_t",
         "drop:WorldPartitionMiniMap::MINIMAP_WORLD_BOUNDS", "FBox world bounds"),
        ("MiniMap_TileSize", "std::ptrdiff_t", "drop:WorldPartitionMiniMap::MINIMAP_TILE_SIZE",
         "minimap tile size"),
        ("MapLevel_HasUnderground", "std::ptrdiff_t",
         "drop:MapWidgetLevelSettings::HAS_UNDERGROUND", "underground floor exists"),
        ("MapLevel_WorldSize", "std::ptrdiff_t", "drop:MapWidgetLevelSettings::WORLD_SIZE",
         "per-level map world size"),
        ("MapLevel_WorldPosition", "std::ptrdiff_t",
         "drop:MapWidgetLevelSettings::WORLD_POSITION", "per-level map world origin"),
        ("MapLevel_Stride", "std::ptrdiff_t", "drop:MapWidgetLevelSettings::STRIDE",
         "per-level settings element stride"),
    ]),
    # Container layout straight out of the dumped SDK's own core headers
    # (sdk/CppSDK/SDK/Basic.hpp and sdk/CppSDK/SDK/CoreUObject_classes.hpp).
    # These are engine internals, not UPROPERTYs, so the property dump does not
    # carry them — the SDK's own struct definitions do.
    ("SDK container layout (sdk/CppSDK/SDK/Basic.hpp + CoreUObject_classes.hpp)", [
        ("UObject_ClassPrivate", "std::ptrdiff_t", "lit:0x20",
         "UObject::Class — CoreUObject_classes.hpp: class UObject, size 0xA0"),
        ("UObject_InternalIndex", "std::ptrdiff_t", "drop:FName::UOBJECT_INTERNAL_IDX",
         "UObject::Index — CoreUObject_classes.hpp"),
        ("UObject_NamePrivate", "std::ptrdiff_t", "lit:0x98",
         "UObject::Name — FName (u32 comparison index + u32 number)"),
        ("UObject_OuterPrivate", "std::ptrdiff_t", "lit:0xA0",
         "UObject::Outer — CoreUObject_classes.hpp"),
        ("UObject_Size", "std::ptrdiff_t", "sizeof:CoreUObject.Object",
         "sizeof(UObject)"),
        ("FNamePool_Blocks", "std::ptrdiff_t", "lit:0x40",
         "FNamePool::Blocks[0x2000] — Basic.hpp (CurrentBlock 0x38, Cursor 0x3C)"),
        ("FNamePool_EntryStride", "std::ptrdiff_t", "lit:0x02",
         "FNamePool::FNameEntryStride — Basic.hpp"),
        ("FNamePool_BlockOffsetBits", "std::ptrdiff_t", "lit:0x10",
         "FNamePool::FNameBlockOffsetBits — 0x10000 entries per block"),
        ("FNameEntry_WideBit", "uint16_t", "drop:FName::HDR_IS_WIDE_BIT",
         "FNameEntryHeader::bIsWide — Basic.hpp"),
        ("FNameEntry_LenShift", "uint16_t", "lit:0x0006",
         "FNameEntryHeader::Len bit index — Basic.hpp"),
        ("FNameEntry_LenMask", "uint16_t", "lit:0x03FF",
         "FNameEntryHeader::Len (10 bits) — Basic.hpp"),
        ("FNameEntry_TextOffset", "std::ptrdiff_t", "lit:0x02",
         "FNameEntry::Name (FStringData) — char/wchar_t after the u16 header"),
        ("GObjects_ElementsPerChunk", "uint32_t", "drop:FName::ITEMS_PER_CHUNK",
         "TUObjectArray::ElementsPerChunk — Basic.hpp"),
        ("GObjects_ItemStride", "std::ptrdiff_t", "drop:FName::FUOBJECTITEM_STRIDE",
         "sizeof(FUObjectItem) — Basic.hpp"),
        ("GObjects_Item_Object", "std::ptrdiff_t", "drop:FName::FUOBJECTITEM_OBJ_OFF",
         "FUObjectItem::Object — Basic.hpp"),
        ("GObjects_NumElements", "std::ptrdiff_t", "lit:0x0C",
         "TUObjectArray::NumElements — Basic.hpp"),
    ]),
]


def load_index():
    with open(INDEX) as fh:
        return json.load(fh)


_DROP = {}


def drop_value(path_key):
    """Look up a CL drop constant by its namespace::NAME path (the ArcOffsets::
    prefix may be omitted - the drop's outermost namespace is not part of the
    constant's identity when reading it)."""
    if not _DROP:
        _DROP.update(sdk_drop.parse())
    if path_key in _DROP:
        return _DROP[path_key]
    matches = [k for k in _DROP if k == "ArcOffsets::" + path_key]
    if len(matches) == 1:
        return _DROP[matches[0]]
    for k in _DROP:
        if k.split("::", 1)[-1] == path_key:
            matches.append(k)
    if len(matches) == 1:
        return _DROP[matches[0]]
    raise SystemExit("drop constant %s not found (%d candidates)" % (path_key, len(matches)))


def resolve(idx, spec):
    """Return (value, comment, is_dump_sourced)."""
    kind, _, arg = spec.partition(":")
    buckets = {"classes": idx["classes"], "structs": idx["structs"]}
    if kind == "prop":
        cls, _, prop = arg.rpartition(".")
        for bucket in buckets.values():
            c = bucket.get(cls)
            if not c:
                continue
            info = c["props"].get(prop)
            if not info:
                raise SystemExit("property %s.%s not in dump" % (cls, prop))
            note = "0x%X" % info["off"]
            extra = ""
            if info.get("mask"):
                extra = " (byte 0x%X mask 0x%X)" % (info["off"], info["mask"])
            return info["off"], "%s.%s = 0x%X%s" % (cls, prop, info["off"], extra), True
        raise SystemExit("class %s not in dump" % cls)
    if kind == "maskOf":
        cls, _, prop = arg.rpartition(".")
        for bucket in buckets.values():
            c = bucket.get(cls)
            if c and prop in c["props"]:
                info = c["props"][prop]
                if not info.get("mask"):
                    raise SystemExit("%s.%s has no mask" % (cls, prop))
                return info["mask"], "%s.%s mask" % (cls, prop), True
        raise SystemExit("class %s not in dump" % cls)
    if kind == "sizeof":
        for bucket in buckets.values():
            c = bucket.get(arg)
            if c:
                return c["size"], "sizeof(%s) from dump" % arg, True
        raise SystemExit("class %s not in dump" % arg)
    if kind == "drop":
        entry = drop_value(arg)
        fresh = " (drop re-checked 2026-09-22)" if entry["fresh"] else ""
        return (entry["value"], "sdk/sdk.txt %s = 0x%X%s"
                % (entry["path"], entry["value"], fresh), True)
    if kind == "lit":
        return None, arg, False
    if kind in ("sdk", "expr"):
        return None, arg, False
    raise SystemExit("bad spec: %s" % spec)


def emit_expr_text(spec, values, name, ctype="std::ptrdiff_t"):
    """Turn expr/lit/sdk specs into the C++ right-hand side."""
    kind, _, arg = spec.partition(":")
    if kind == "lit":
        return arg
    if kind == "sdk":
        return "static_cast<%s>(game::offsets::%s)" % (ctype, arg)
    if kind == "expr":
        return arg
    # prop/maskOf/sizeof resolved numerically in the caller
    return "0x%X" % values[name]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    idx = load_index()
    meta = idx.get("meta", {})

    # resolve every spec first
    values, comments, generated_expr = {}, {}, {}
    order = []
    for _, entries in SECTIONS:
        for name, ctype, spec, note in entries:
            val, comment, is_dump = resolve(idx, spec)
            values[name] = val
            comments[name] = (comment, note, is_dump, spec, ctype)
            order.append(name)

    def numeric(name):
        v = values.get(name)
        return v

    for name in order:
        comment, note, is_dump, spec, ctype = comments[name]
        if not is_dump and spec.startswith("expr:"):
            expr = spec[5:]
            # substitute known constants (identifiers) with their resolved values
            def sub(m):
                base, _, off = m.group(1), m.group(2), m.group(3)
                v = numeric(base) if not base.startswith(("0x", "0X")) else int(base, 16)
                if v is None:
                    return m.group(0)
                if off:
                    v += int(off, 16)
                return "0x%X" % v
            replaced = re.sub(r"\b([A-Za-z_]\w*|0x[0-9A-Fa-f]+)\s*(\+)\s*(0x[0-9A-Fa-f]+)", sub, expr)
            if re.fullmatch(r"0x[0-9A-Fa-f]+", replaced.strip()):
                values[name] = int(replaced, 16)
            else:
                generated_expr[name] = replaced

    lines = []
    lines.append("#pragma once")
    lines.append('#include "SDK.hpp"   // adopted SDK drop - game::offsets is the source of truth')
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("// GENERATED by tools/gen_offsets.py from the dumped SDK - do not hand-edit.")
    lines.append("// Dump: %s | %s | image %s | %s" % (
        meta.get("Game build", "?"), meta.get("Game updated", "?"),
        meta.get("Image size", "?"), meta.get("FName pipeline", "?")))
    lines.append("// Every constant below carries its source: <Class>.<Property> offsets come")
    lines.append("// straight from the dump; slots marked DERIVED/lit have no reflecting property")
    lines.append("// on this build and keep their probed value. [live] marks a value pinned")
    lines.append("// against the running target because no dump carries it.")
    for name, why in sorted(LIVE_OVERRIDES.items()):
        lines.append("// [live] %s - %s" % (name, why))
    lines.append("")
    lines.append("namespace Offsets {")
    for title, entries in SECTIONS:
        lines.append("    // -- %s " % title + "-" * max(0, 60 - len(title)))
        for name, ctype, spec, note in entries:
            rhs = generated_expr.get(name) or emit_expr_text(spec, values, name, ctype)
            comment, nnote, is_dump, _, _ = comments[name]
            text = note or nnote
            if is_dump:
                tag = ""
            elif name in LIVE_OVERRIDES:
                tag = "  [live]"
            else:
                tag = "  [lit/probed]"
            lines.append("    constexpr %-14s %-38s = %-44s // %s%s" % (
                ctype, name, rhs + ";", comment + (" - " + text if text else ""), tag))
        lines.append("")

    # value check: dump-sourced offsets must sit inside their class
    problems = []
    for name, v in values.items():
        spec = comments[name][3]
        if v is None or not spec.startswith("prop:"):
            continue
        cls, _, prop = spec[5:].rpartition(".")
        info = None
        for bucket in (idx["classes"], idx["structs"]):
            if cls in bucket and prop in bucket[cls]["props"]:
                info = bucket[cls]
        size = info["size"] if info else None
        if size and v >= size:
            problems.append("%s: 0x%X >= sizeof(%s)=0x%X" % (name, v, cls, size))

    lines.append("}")
    content = "\n".join(lines).rstrip().replace("\u2014", "-") + "\n"

    if problems:
        print("OUT OF RANGE:")
        for p in problems:
            print("  " + p)

    n_dump = sum(1 for c in comments.values() if c[2])
    print("constants: %d (%d dump-sourced, %d literal/derived)" % (
        len(comments), n_dump, len(comments) - n_dump))

    if args.check:
        if os.path.exists(OUT):
            with open(OUT, encoding="utf-8") as fh:
                old = fh.read()
            print("differences:" if old != content else "no changes")
            if old != content:
                # the header is generated, so any difference means it was either
                # hand-edited or not regenerated after a spec change - both are
                # failures, not warnings
                problems.append("Offsets.h differs from the generated content")
                import difflib
                for line in list(difflib.unified_diff(
                        old.splitlines(), content.splitlines(),
                        "Offsets.h(old)", "Offsets.h(new)", lineterm=""))[:80]:
                    print(line)
        # the IDA map shipped in sdk/IDAMappings/ is an independent source for
        # the four data globals -- reconcile against it when it is present
        if not problems:
            try:
                sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                import idmap as idmap_tool
                print()
                if idmap_tool.check(idmap_tool.Idmap(idmap_tool.find_file())) != 0:
                    problems.append("idmap: data globals out of sync with Offsets.h")
            except SystemExit:
                pass
            except Exception as exc:      # tool is optional; never block on it
                print("idmap check skipped: %s" % exc)

        # sdk/bigger sdk/ is a second, independent Dumper-7 dump of the same
        # build - the data globals, the native reflection layout and every
        # class size get a second opinion from it. Optional like the idmap: a
        # checkout without the big dump simply skips it.
        if not problems:
            try:
                sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                import sdk7_index as sdk7
                print()
                if sdk7.check(sdk7.load()) != 0:
                    problems.append(
                        "sdk7: constants out of sync with the Dumper-7 dump")
            except SystemExit:
                pass
            except Exception as exc:      # dump is optional; never block on it
                print("sdk7 check skipped: %s" % exc)

        # Every constant scored against every offline source: a higher-authority
        # disagreement is a failure here, so a wrong offset cannot be merged just
        # because it compiles.
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import reconcile_offsets as reconcile
        print()
        saved_argv = sys.argv
        sys.argv = ["reconcile_offsets.py", "--check", "--quiet"]
        try:
            if reconcile.main() != 0:
                problems.append("reconcile: actionable offset disagreement")
        finally:
            sys.argv = saved_argv
        return 1 if problems else 0

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(content)
    print("wrote %s" % OUT)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
