#pragma once
#include <cstddef>
#include <cstdint>

namespace Offsets {
    // ── Globals (CL-1341255 2026-08-18 — forum qwe900 live-pinned) ─────────────
    constexpr std::ptrdiff_t UWorld = 0xE782D78;
    constexpr uint64_t GameStateGlobalRva = 0xDCA7C88ULL;  // UNVERIFIED for CL-1341255
    // CL-1341255 / v20260818 FName pipeline (see SteamDecrypt.hpp GNames):
    constexpr uint64_t GNamePoolRva = 0xE35AB00ULL;       // RVA_GNAMEPOOL
    constexpr uint64_t FNameKeyTableRva = 0xE2997F4ULL;   // RVA_KEYSTREAM
    constexpr uint64_t FNameBlockMaskRva = 0xB523C50ULL;  // legacy SSE block mask — Dumper fallback for older builds
    constexpr uint64_t GUObjectArrayChunksRva = 0xE80BA10ULL;  // dumper relaxed validated
    constexpr uint64_t FFieldNameKey0Rva = 0xE7B5330ULL;
    constexpr uint64_t FFieldNameKey1Rva = 0xE6F0554ULL;
    // CL-1341255 GameInstance SIMD decrypt (UWorld+0x478 is encrypted — plain read returns 0):
    constexpr uint64_t GameInstanceXorKey0Rva = 0xB06C380ULL;     // XOR key 0 (low qword)
    constexpr uint64_t GameInstanceXorKey1Rva = 0xB06C390ULL;     // XOR key 1 (high qword)
    constexpr uint64_t GameInstanceShuffleMaskRva = 0xB09C350ULL; // 16-byte PSHUFB mask
    constexpr std::ptrdiff_t GameInstance_WorldBackRef = 0x2F0;   // GI struct → UWorld (validation)
    // CL-1341255 player-name SIMD decrypt (replaces hardcoded scramble key):
    constexpr uint64_t PlayerNameSimdMaskRva = 0xAD2FC50ULL;      // 16-byte PSHUFB mask (read from game base)

    // ── UObjectBase (standard UE layout; not game-specific dump) ─────────────
    constexpr std::ptrdiff_t UObject_ObjectFlags = 0x08;
    constexpr uint32_t RF_BeginDestroyed = 0x00800000u;
    constexpr uint32_t RF_FinishDestroyed = 0x01000000u;

    // ── UWorld (SDK dump Class: World — CL-1341255) ─────────────────────────
    constexpr std::ptrdiff_t PersistentLevel = 0x110;
    constexpr std::ptrdiff_t StreamingLevels = 0x158;
    constexpr std::ptrdiff_t Levels = 0x348;
    constexpr std::ptrdiff_t OwningGameInstance = 0x478;
    constexpr std::ptrdiff_t LevelCollections = 0x240;          // SDK validated (stride 0x78)
    constexpr std::ptrdiff_t AuthorityGameMode = 0x230;
    constexpr std::ptrdiff_t PhysicsField = 0x4E0;

    // LevelCollection (size 0x78 — stride unchanged) — SDK validated
    constexpr std::ptrdiff_t LevelCollection_GameState = 0x08;
    constexpr std::ptrdiff_t LevelCollection_PersistentLevel = 0x20;
    constexpr std::ptrdiff_t LevelCollection_Stride = 0x78;

    // UWorld has no direct GameState field — use LevelCollections.
    constexpr std::ptrdiff_t GameState = LevelCollections;

    // ── GameInstance / LocalPlayer ───────────────────────────────────────────
    constexpr std::ptrdiff_t LocalPlayers = 0x138;              // SDK validated
    constexpr std::ptrdiff_t LocalPlayer_PlayerController = 0xA0; // forum qwe900 confirmed CL-1341255
    constexpr std::ptrdiff_t LocalPlayer_ControllerId = 0x270;  // SDK validated

    // ── Level (SDK dump Class: Level) ────────────────────────────────────────
    // Both actor-array paths exist on CL-1341255: the direct Level TArray
    // (forum live-pinned data@0x108 / num@0x110 / max@0x114)
    // and ActorCluster → LevelActorContainer::Actors.
    constexpr std::ptrdiff_t AActors = 0x108;                   // forum live-pinned
    constexpr std::ptrdiff_t ActorsCount = 0x110;               // forum live-pinned
    constexpr std::ptrdiff_t Level_OwningWorld = 0x130;         // SDK validated
    constexpr std::ptrdiff_t ActorCluster = 0x150;              // SDK validated

    // LevelActorContainer (via Level::ActorCluster pointer) — SDK validated
    constexpr std::ptrdiff_t LevelActorContainer_Actors = 0x98;
    constexpr std::ptrdiff_t LevelActorContainer_ActorCount = 0xA0;

    // ── GameState ────────────────────────────────────────────────────────────
    constexpr std::ptrdiff_t GameState_PlayerArray = 0x3F8;     // SDK validated (GameStateBase)
    constexpr std::ptrdiff_t GameState_EnemyCount = 0x0940;     // SDK validated

    // ── GameViewportClient ───────────────────────────────────────────────────
    constexpr std::ptrdiff_t GameViewportClient_World = 0x1A0;  // SDK validated

    // ── PlayerController / Controller ────────────────────────────────────────
    // SDK dump (buildid 24710327) Class: AController — Pawn @ 0x3F0, StateName @ 0x3D8.
    // 0x3F0 is the real pawn slot and is what the working builds used; the 0x3D8
    // change (a forum hunch) broke every PC-resolution path because the
    // validators read the primary slot only. Keep 0x3D8 as fallback only.
    constexpr std::ptrdiff_t AcknowledgedPawn = 0x3F0;             // SDK dump: AController.Pawn (validated)
    constexpr std::ptrdiff_t AcknowledgedPawn_Fallback = 0x3D8;    // StateName slot per SDK — heuristic fallback only
    constexpr std::ptrdiff_t Controller_Character = 0x408;         // SDK validated (AController.Character)
    constexpr std::ptrdiff_t ControlRotation = 0x438;              // SDK validated
    constexpr std::ptrdiff_t AController_PlayerState = 0x3C0;   // SDK validated
    // The game keeps this non-UPROPERTY pointer outside the generated SDK; retain
    // the discovered slot only as a direct-read fallback. FName/PCOwner discovery
    // remains authoritative when this slot is encrypted or moved.
    // LIVE-PINNED: debug-c190fb.log TracePcDiscovery shows PC+0x4D0 returning a
    // stable valid APlayerCameraManager ptr on every raid session (cached path,
    // 640+ traces: pcm=1258A8040 / 12ABDF0080 / 1292330080 / 12A65A2070), and the
    // same read passes ControllerHasValidPcm (valid ptr + PcmLivePovLooksSane).
    constexpr std::ptrdiff_t APlayerCameraManager = 0x4D0;
    constexpr std::ptrdiff_t APlayerState = 0x3D0;              // SDK validated (APawn.PlayerState)
    constexpr std::ptrdiff_t PlayerNamePrivate = 0x448;         // SDK validated (APlayerState)
    constexpr std::ptrdiff_t PlayerState_PawnPrivate = 0x428;   // SDK validated
    constexpr std::ptrdiff_t PlayerState_PlayerStatus = 0x530;  // SDK validated (PioneerPlayerState)
    constexpr std::ptrdiff_t Pawn_Controller = 0x3E8;           // SDK validated (APawn.Controller)
    constexpr std::ptrdiff_t PlayerNameOnPawn = 0x438;          // stale on CL-1341255 (== Character.Mesh); graceful heuristic only

    // ── PlayerCameraManager (CL-1341255 — FrostSDK dump from paste) ───────────
    // FrostSDK dump: PendingViewTarget @ 0x430 (labeled Pending but is ACTUAL
    // active ViewTarget — FrostSDK swapped labels). ViewTarget @ 0xC80.
    // POV = ViewTarget + 0x10 (FTViewTarget prefix) + FMinimalViewInfo offsets.
    // FMinimalViewInfo: Location @ +0x10, Rotation @ +0x30, FOV @ +0x58.
    // FrostSDK labels are SWAPPED vs CppSDK: 0xC80 is the ACTIVE ViewTarget
    // (all POV fields populated), 0x430 is Pending (Rotation zero during normal play).
    constexpr std::ptrdiff_t ViewTarget = 0xC80;               // ACTIVE ViewTarget base (confirmed by working Rotation)
    constexpr std::ptrdiff_t PendingViewTarget = 0x430;        // Pending/inactive ViewTarget base
    constexpr std::ptrdiff_t CameraPOV_Location = 0xCA0;       // ViewTarget(0xC80) + FTViewTarget(0x10) + Location(0x10)
    constexpr std::ptrdiff_t CameraPOV_Rotation = 0xCC0;       // ViewTarget(0xC80) + FTViewTarget(0x10) + Rotation(0x30)
    constexpr std::ptrdiff_t CameraPOV_FOV = 0xCE8;            // ViewTarget(0xC80) + FTViewTarget(0x10) + FOV(0x58)
    constexpr std::ptrdiff_t CameraLocation = CameraPOV_Location;
    constexpr std::ptrdiff_t CameraRotation = CameraPOV_Rotation;
    constexpr std::ptrdiff_t CameraFOV = CameraPOV_FOV;
    constexpr std::ptrdiff_t PCOwner = 0x3D0;                   // SDK 0x3A8 + 0x28 non-UPROPERTY shift
    constexpr std::ptrdiff_t DefaultFOV = 0x3E8;               // SDK 0x3C0 + 0x28 non-UPROPERTY shift

    // ── Actor (SDK dump Class: Actor) ────────────────────────────────────────
    constexpr std::ptrdiff_t RootComponent = 0x238;             // SDK validated
    constexpr std::ptrdiff_t ClassDefaultObject = 0x70;
    constexpr std::ptrdiff_t ClassDefaultObjectAlt = 0x78;
    constexpr std::ptrdiff_t ActorTypeId = 0xB0;
    constexpr std::ptrdiff_t IsRenderedTime = 0x1F0;
    constexpr std::ptrdiff_t ActorID = 0x18;
    constexpr std::ptrdiff_t ActorOwner = 0x1C8;                // SDK validated
    constexpr std::ptrdiff_t ActorInstigator = 0x218;           // SDK validated (Instigator)
    constexpr std::ptrdiff_t Actor_bHiddenByte = 0xE0;          // SDK validated
    constexpr uint8_t Actor_bHiddenMask = 0x10;
    constexpr std::ptrdiff_t Actor_FlagsDd = 0xE4;              // SDK validated
    constexpr uint8_t Actor_bActorEnableCollisionMask = 0x10;
    constexpr uint8_t Actor_bActorIsBeingDestroyedMask = 0x20;
    constexpr std::ptrdiff_t ReplicatedMovement = 0x158;        // SDK validated
    constexpr std::ptrdiff_t RepMov_LinearVelocity = 0x0;      // offset within FRepMovement
    constexpr std::ptrdiff_t Actor_InstanceComponents = 0x388;  // SDK validated

    // ── SceneComponent (SDK dump — unchanged from CL-1335610) ────────────────
    constexpr std::ptrdiff_t RelativeLocation = 0x260;          // SDK validated
    constexpr std::ptrdiff_t RelativeRotation = 0x278;          // SDK validated
    constexpr std::ptrdiff_t ComponentVelocity = 0x2A8;         // SDK validated
    constexpr std::ptrdiff_t ComponentToWorld = 0x370;          // forum live-pinned (was 0x228)
    constexpr std::ptrdiff_t WorldLocation = 0x390;             // forum live-pinned (CTW + 0x20 = translation)
    constexpr std::ptrdiff_t Scene_bVisibleByte = 0x2C0;        // SDK validated
    constexpr uint8_t Scene_bVisibleMask = 0x20;
    constexpr std::ptrdiff_t Scene_bHiddenInGameByte = 0x2C1;   // SDK validated
    constexpr uint8_t Scene_bHiddenInGameMask = 0x10;
    constexpr std::ptrdiff_t AttachChildren = 0x1E8;            // SDK validated

    constexpr std::ptrdiff_t BoundsScale = 0x4B8;               // SDK validated
    constexpr std::ptrdiff_t LastSubmitTime = 0x4C4;
    constexpr std::ptrdiff_t LastRenderTime = LastSubmitTime + 0x4;
    constexpr std::ptrdiff_t LastRenderTimeOnScreen = LastSubmitTime + 0x8;
    constexpr std::ptrdiff_t VisibilityBasedAnimTickOption = 0x0994; // SDK validated
    constexpr std::ptrdiff_t bRecentlyRendered = 0x997;         // SDK CL-1341255 SkinnedMeshComponent

    // ── Bone decrypt (CL-1341255 v818: seed@0x7B0, lod@0x7F8) ─────────────────
    constexpr std::ptrdiff_t Encrypted = 0x7B0;                 // bone pointer decrypt seed (v818)
    constexpr std::ptrdiff_t Encrypted_Legacy = 0x790;          // previous build's seed slot
    constexpr std::ptrdiff_t LodSelect = 0x7D0;                 // CL-1341255 (esasiolan + texaftertex confirm)
    constexpr std::ptrdiff_t LodSelect_Legacy = 0x7D0;          // previous LOD slot

    // ── Mesh / character ─────────────────────────────────────────────────────
    constexpr std::ptrdiff_t USkeletalMeshComponent = 0x438;    // SDK validated (Character.Mesh)
    constexpr std::ptrdiff_t SkeletalMeshAsset = 0x740;         // SDK CL-1341255 SkinnedMeshComponent.SkeletalMesh (was 0xAC0 — stale)
    constexpr std::ptrdiff_t bNoSkeletonUpdate = 0xC51;         // SDK validated (mask 0x10)
    constexpr std::ptrdiff_t bForceRefpose = 0xC52;             // SDK validated (mask 0x1)
    constexpr std::ptrdiff_t CharacterMovement = 0x440;         // SDK validated
    constexpr std::ptrdiff_t Velocity = 0x160;                  // SDK: UMovementComponent.Velocity (was 0x168 — stale)
    constexpr std::ptrdiff_t PioneerCharacterMovement = 0xB48;  // SDK validated
    constexpr std::ptrdiff_t HealthComponent = 0xDD8;           // SDK validated
    constexpr std::ptrdiff_t InventoryComponent = 0xCB0;        // SDK CL-1341255 PioneerPlayerCharacter (was 0xCA0 — stale)
    constexpr std::ptrdiff_t EmbarkMesh = 0x7E8;                // SDK validated

    // ── Health ────────────────────────────────────────────────────────────────
    constexpr std::ptrdiff_t HealthInfo = 0x550;                // derived (+0x20; non-UPROPERTY)
    constexpr std::ptrdiff_t PlayerState_Health = HealthInfo;
    constexpr std::ptrdiff_t PlayerState_MaxHealth = 0x558;     // derived (+0x20)
    constexpr std::ptrdiff_t PlayerState_Armor = 0x560;         // derived (+0x20)
    constexpr std::ptrdiff_t PlayerState_MaxArmor = 0x568;      // derived (+0x20)
    constexpr std::ptrdiff_t Health = 0x678;                    // SDK validated (HealthComponent::CachedHealth)
    constexpr std::ptrdiff_t MaxHealth = 0x308;                 // SDK validated
    constexpr std::ptrdiff_t Shield = 0x150;                    // == HealthComponent.Armor @ 0x150
    constexpr std::ptrdiff_t ShieldMax = 0x160;                 // SDK validated: FArmorSlot.CurrentMaxArmor @ Armor+0x10
    constexpr std::ptrdiff_t MaxDBNO = 0x310;                   // SDK validated
    constexpr std::ptrdiff_t TeamID = 0x822;                    // live-pinned on UEmbarkCharacterBase; actual range 4-10 (NOT SDK enum 0-2)

    constexpr std::ptrdiff_t CurrentItemActors = 0x4B0;         // SDK CL-1341255 InventoryComponent (was 0x4A0 — stale)
    constexpr std::ptrdiff_t LocalCurrentItemActors = 0x4D0;    // SDK CL-1341255 (was 0x4C0 — stale)
    constexpr std::ptrdiff_t EquippedPrimaryItem = 0x518;       // reads EquippedArmor (was 0x500 — padding)
    constexpr std::ptrdiff_t WeaponClip = 0x4A0;                // SDK CL-1341255 WeaponActor.CLIP_SIZE (was 0x470 — stale)
    constexpr std::ptrdiff_t WeaponQuality = 0x4A2;             // SDK CL-1341255 (was 0x472 — stale)

    constexpr std::ptrdiff_t StowedWeaponSlot0 = 0x330;         // SDK CL-1341255 STOWED_WEAPON_0 (was 0x340 — stale)
    constexpr std::ptrdiff_t StowedWeaponSlot1 = 0x370;         // SDK CL-1341255 STOWED_WEAPON_1 (was 0x380 — stale)
    constexpr std::ptrdiff_t EquippedArmor = 0x518;

    // ── Loot / containers ────────────────────────────────────────────────────
    constexpr std::ptrdiff_t LootInteractionComponent = 0xB58;  // SDK validated
    constexpr std::ptrdiff_t LootContainer_ItemContainer = 0xB60; // SDK validated
    constexpr std::ptrdiff_t LootInteraction_Container = 0xB58; // SDK validated (== LootInteractionComponent)
    constexpr std::ptrdiff_t SalvageContainer_ChosenMesh = 0xC90; // SDK validated
    constexpr std::ptrdiff_t LootInteraction_Searched = 0x8D8;  // SDK validated
    constexpr std::ptrdiff_t SimpleLootActivity_LootInteraction = 0x480;
    constexpr std::ptrdiff_t SimpleLootActivity_LootStateMachine = 0x4A8;
    constexpr std::ptrdiff_t SimpleLootActivity_ItemContainer = 0x498;
    constexpr std::ptrdiff_t SalvageContainer_MeshVariants = 0xC58; // SDK validated
    constexpr std::ptrdiff_t ItemDataAsset = 0x898;             // dump: WorldItemBase.ItemDataAsset
    constexpr std::ptrdiff_t BP_PickupBase_SpawnItems = 0x550;  // SDK validated
    constexpr std::ptrdiff_t ItemContainer_OpenTime = 0x540;    // SDK validated
    constexpr std::ptrdiff_t ConstructableItemContainer_OpenTime = 0x4D0; // SDK validated
    constexpr std::ptrdiff_t ItemDataAsset_OverrideItemAssetId = 0x120;
    constexpr std::ptrdiff_t ItemDataAsset_bOverrideItemAssetId = 0x118;
    constexpr std::ptrdiff_t UIHoverData = 0x550;
    constexpr std::ptrdiff_t UIHoverData_Pickup = 0x5F8;

    constexpr std::ptrdiff_t Pickup_RootCollider = 0x470;       // SDK validated
    constexpr std::ptrdiff_t Pickup_Interaction = 0x488;        // SDK validated
    constexpr std::ptrdiff_t Pickup_DefaultPickupDataAsset = 0x498; // SDK validated
    constexpr std::ptrdiff_t Pickup_ContainedItem_BB = 0x4A0;   // SDK validated
    constexpr std::ptrdiff_t BBItem_DataAssetIndex = 0x0;
    constexpr std::ptrdiff_t BBItem_AmountValue = 0x4 + 0xC;
    constexpr std::ptrdiff_t Interaction_bIsActiveByte = 0x137;
    constexpr uint8_t Interaction_bIsActiveMask = 0x8;
    constexpr std::ptrdiff_t Interaction_CurrentInteractionState = 0x47D; // SDK ref +0x10 (was 0x46D — stale)

    constexpr std::ptrdiff_t bIsBreaked = 0x1220;               // SDK ref PioneerConstructablePawn.bIsDestroyed @ 0x1220
    constexpr std::ptrdiff_t Constructable_EnemyTypeDataAsset = 0x11B0; // SDK ref (was 0x11A0 — stale)
    constexpr std::ptrdiff_t Constructable_AITemplateData = 0x11A0;     // SDK ref (was 0x1190 — stale)
    constexpr std::ptrdiff_t Constructable_bIsDestroyed = 0x1220;       // SDK ref (was 0x1210 — stale)

    // ── Extraction points (ASalvageExtractionPointBase — SDK validated) ───────
    constexpr std::ptrdiff_t ExtractionPoint_State = 0x0BD2;            // EExtractionState @ 0x0BD2 (Net, RepNotify)

    // ── Static mesh collision (StaticMeshComponent / UStaticMesh — CL-1341255) ─
    constexpr std::ptrdiff_t StaticMesh = 0x728;                // SDK validated
    constexpr std::ptrdiff_t StaticMeshLegacy = 0x718;          // previous build's slot (Dumper fallback)

    // ── Encrypted render times / component internals ──────────────────────────
    constexpr std::ptrdiff_t Mesh_LastRenderTimeEnc = 0x4BC;
    constexpr std::ptrdiff_t Mesh_LastRenderTimeOnScreenEnc = 0x4C4;  // SDK ref PrimitiveComponent (was 0x4C8 — stale)
    constexpr uint32_t Mesh_LastRenderTimeKey = 0x5AB299E0u;    // SDK ref LAST_RENDER_TIME_XOR
    constexpr uint32_t Mesh_LastRenderTimeOnScreenKey = 0xA83E5CBEu;  // SDK ref LAST_RENDER_TIME_ON_SCREEN_XOR (was 0xE1664254 — stale)
    constexpr std::ptrdiff_t UActorComponent_WorldPrivate = 0x148; // UNVERIFIED (non-UPROPERTY)
    // Live-verified by WorldScan::StartTimeSecondsProbe on CL-1341255: read
    // 480.526 and advanced 1.003 per wall second. RealTimeSeconds sits 0x10
    // later at 0x8C0 running ~6.7s ahead, matching UE5 declaration order.
    // 0xA28 was wrong and read zero, which is what stalled LRTS.
    constexpr std::ptrdiff_t UWorld_TimeSeconds = 0x8B0;
}
