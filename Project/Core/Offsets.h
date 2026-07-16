#pragma once
#include <cstddef>
#include <cstdint>

namespace Offsets {
    // ── Globals (CL-1315578) ─────────────────────────────────────────────────
    constexpr std::ptrdiff_t UWorld = 0xE91A288;
    constexpr uint64_t GameStateGlobalRva = 0xDCA7C88ULL;
    constexpr uint64_t GNamePoolRva = 0xE4F2A00ULL;
    constexpr uint64_t FNameKeyTableRva = 0xE4318DCULL;
    constexpr uint64_t FNameBlockMaskRva = 0xB523C50ULL;
    constexpr uint64_t GUObjectArrayChunksRva = 0xE3B61C0ULL;
    constexpr uint64_t GObjPshufbMaskRva = 0xAD97CC0ULL;

    // ── UObjectBase (standard UE layout; not game-specific dump) ─────────────
    /** EObjectFlags ObjectFlags — forum "SleepingInPool" concept maps here / GUObjectItem. */
    constexpr std::ptrdiff_t UObject_ObjectFlags = 0x08;
    // Common RF bits (probe-only; do not treat as deplete until log-proven):
    constexpr uint32_t RF_BeginDestroyed = 0x00800000u;
    constexpr uint32_t RF_FinishDestroyed = 0x01000000u;

    // ── UWorld ───────────────────────────────────────────────────────────────
    constexpr std::ptrdiff_t PersistentLevel = 0x158;
    constexpr std::ptrdiff_t StreamingLevels = 0x178;
    constexpr std::ptrdiff_t Levels = 0x4A0;
    constexpr std::ptrdiff_t UWorld_InstanceTime = 0x1E0;
    constexpr std::ptrdiff_t OwningGameInstance = 0x4D8;
    constexpr std::ptrdiff_t LevelCollections = 0x370;
    constexpr std::ptrdiff_t AuthorityGameMode = 0x3F0;
    constexpr std::ptrdiff_t PhysicsField = 0x560;
    constexpr std::ptrdiff_t UWorldSeconds = 0x778;

    // LevelCollection (size 0x78)
    constexpr std::ptrdiff_t LevelCollection_GameState = 0x08;
    constexpr std::ptrdiff_t LevelCollection_NetDriver = 0x10;
    constexpr std::ptrdiff_t LevelCollection_DemoNetDriver = 0x18;
    constexpr std::ptrdiff_t LevelCollection_PersistentLevel = 0x20;
    constexpr std::ptrdiff_t LevelCollection_Levels = 0x28;
    constexpr std::ptrdiff_t LevelCollection_Stride = 0x78;

    // UWorld has no direct GameState field on CL-1315578 — use LevelCollections.
    // Offsets::GameState kept as LevelCollections base for ResolveGameStateFromWorld.
    constexpr std::ptrdiff_t GameState = LevelCollections;

    // ── GameInstance / LocalPlayer ───────────────────────────────────────────
    constexpr std::ptrdiff_t LocalPlayers = 0x120;
    constexpr std::ptrdiff_t LocalPlayer_PlayerController = 0x00A0;
    constexpr std::ptrdiff_t LocalPlayer_ControllerId = 0x0270;

    // ── Level ────────────────────────────────────────────────────────────────
    constexpr std::ptrdiff_t ActorsCount = 0x110;
    constexpr std::ptrdiff_t AActors = 0x108;
    constexpr std::ptrdiff_t OwningActor = 0x108;
    constexpr std::ptrdiff_t ActorsMax = 0x114;
    constexpr std::ptrdiff_t Level_OwningWorld = 0x130;
    constexpr std::ptrdiff_t ActorCluster = 0x150;

    // LevelActorContainer
    constexpr std::ptrdiff_t LevelActorContainer_Actors = 0x98;
    constexpr std::ptrdiff_t LevelActorContainer_ActorCount = 0xA8;

    // ── GameState ────────────────────────────────────────────────────────────
    constexpr std::ptrdiff_t GameState_PlayerArray = 0x0480;
    constexpr std::ptrdiff_t GameState_GameModeClass = 0x03C0;
    constexpr std::ptrdiff_t GameState_AuthorityGameMode = 0x03C8;
    constexpr std::ptrdiff_t GameState_SpectatorClass = 0x03D0;
    constexpr std::ptrdiff_t GameState_PlayerStates = 0x0940;
    constexpr std::ptrdiff_t GameState_GamePhase = 0x0950;
    constexpr std::ptrdiff_t GameState_EnemyCount = 0x0980;
    constexpr std::ptrdiff_t GameState_PickupCount = 0x0984;

    // ── GameViewportClient ───────────────────────────────────────────────────
    constexpr std::ptrdiff_t GameViewportClient_World = 0x0178;

    // ── PlayerController ─────────────────────────────────────────────────────
    constexpr std::ptrdiff_t AcknowledgedPawn = 0x3E0;
    constexpr std::ptrdiff_t ControlRotation = 0x418;
    constexpr std::ptrdiff_t AController_PlayerState = 0x3A8;
    constexpr std::ptrdiff_t LocalAckPlayerState = 0x3F0; // CHARACTER on PC (help)
    constexpr std::ptrdiff_t APlayerCameraManager = 0x4E0;
    constexpr std::ptrdiff_t APlayerState = 0x3A8; // Actor/PC PlayerState (restore world cache)
    constexpr std::ptrdiff_t PlayerNamePrivate = 0x440;
    constexpr std::ptrdiff_t PlayerNameOnPawn = 0x438;
    constexpr std::ptrdiff_t PlayerState_PawnPrivate = 0x410; // probed local; EntityList uses kPsPawnPrivate 0x418 (#24)
    // COLLISION @0x548: DBNO status vs max-armor — only MaxArmor is read today (#22/#27).
    constexpr std::ptrdiff_t PlayerState_PlayerStatus = 0x548; // Alive=0 DBNO=1 — needs raid confirm
    constexpr std::ptrdiff_t Pawn_Controller = 0x3D8;

    // ── PlayerCameraManager ──────────────────────────────────────────────────
    // help/esp.txt Step 5 (CL-1315578 live): ViewTarget = 0x4B0 — do NOT use SDK 0x4A0.
    // Absolute POV base = ViewTarget + 0x10 (= 0x4C0). Composition stays Loc/Rot/FOV @ 0x00/0x28/0x50.
    constexpr std::ptrdiff_t CameraCachePrivate = 0x4B0;
    constexpr std::ptrdiff_t ViewTarget = 0x04B0;   // help/esp.txt live; SDK 0x4A0 is stale
    constexpr std::ptrdiff_t ViewTargetTarget = ViewTarget;
    constexpr std::ptrdiff_t ViewTargetPOV = 0x10;
    constexpr std::ptrdiff_t CameraCachePOV = 0x10;

    // POV relative (help/esp.txt): Location +0x00, Rotation +0x28, FOV +0x50.
    // Do NOT add ViewTargetPOV(0x10) again here — that double-shift made Cam Loc Y/Z=0, Rot 0, FOV fallback 90.
    constexpr std::ptrdiff_t CameraPOV_Location = 0x00;
    constexpr std::ptrdiff_t CameraPOV_Rotation = 0x28;
    constexpr std::ptrdiff_t CameraPOV_FOV = 0x50;
    constexpr std::ptrdiff_t CameraLocation = CameraPOV_Location;
    constexpr std::ptrdiff_t CameraRotation = CameraPOV_Rotation;
    constexpr std::ptrdiff_t CameraFOV = CameraPOV_FOV;
    constexpr std::ptrdiff_t POV_Location = CameraPOV_Location;
    constexpr std::ptrdiff_t POV_Rotation = CameraPOV_Rotation;
    constexpr std::ptrdiff_t POV_FOV = CameraPOV_FOV;
    constexpr std::ptrdiff_t PCOwner = 0x0420;
    constexpr std::ptrdiff_t DefaultFOV = 0x0430;
    constexpr std::ptrdiff_t LockedFOV = 0x3C4;
    constexpr std::ptrdiff_t DefaultOrthoWidth = 0x70;

    // ── Actor ────────────────────────────────────────────────────────────────
    constexpr std::ptrdiff_t RootComponent = 0x218;
    constexpr std::ptrdiff_t ClassDefaultObject = 0x70;
    constexpr std::ptrdiff_t ClassDefaultObjectAlt = 0x78;
    constexpr std::ptrdiff_t ActorTypeId = 0xB0;
    constexpr std::ptrdiff_t IsRenderedTime = 0x1F0;
    constexpr std::ptrdiff_t ActorID = 0x18;
    constexpr std::ptrdiff_t ActorOwner = 0x1c0;
    constexpr std::ptrdiff_t ActorInstigator = 0x210;
    constexpr std::ptrdiff_t Actor_bHiddenByte = 0xd9;
    constexpr uint8_t Actor_bHiddenMask = 0x1;
    constexpr std::ptrdiff_t Actor_FlagsDd = 0xdd;
    constexpr uint8_t Actor_bActorEnableCollisionMask = 0x1;
    constexpr uint8_t Actor_bActorIsBeingDestroyedMask = 0x2;
    constexpr std::ptrdiff_t ReplicatedMovement = 0x148; // velocity (Aimbot); position readers use actor+0x150 (#23)
    constexpr std::ptrdiff_t RepMov_LinearVelocity = 0x0;
    constexpr std::ptrdiff_t Actor_InstanceComponents = 0x328;

    // ── SceneComponent ───────────────────────────────────────────────────────
    constexpr std::ptrdiff_t RelativeLocation = 0x218;
    constexpr std::ptrdiff_t RelativeRotation = 0x230;
    constexpr std::ptrdiff_t RelativeScale3D = 0x248;
    constexpr std::ptrdiff_t ComponentVelocity = 0x260;
    constexpr std::ptrdiff_t ComponentToWorld = 0x330;
    constexpr std::ptrdiff_t WorldLocation = 0x350; // CTW + 0x20
    constexpr std::ptrdiff_t Scene_bVisibleByte = 0x278;
    constexpr uint8_t Scene_bVisibleMask = 0x20;
    constexpr std::ptrdiff_t Scene_bHiddenInGameByte = 0x279;
    constexpr uint8_t Scene_bHiddenInGameMask = 0x10;

    constexpr std::ptrdiff_t BoundsScale = 0x438;
    constexpr std::ptrdiff_t LastSubmitTime = 0x4C4;
    constexpr std::ptrdiff_t LastRenderTime = LastSubmitTime + 0x4;
    constexpr std::ptrdiff_t LastRenderTimeOnScreen = LastSubmitTime + 0x8;
    constexpr std::ptrdiff_t LastSubmitTimeOnScreen = LastRenderTimeOnScreen;
    constexpr std::ptrdiff_t VisibilityBasedAnimTickOption = 0x8F4;
    constexpr std::ptrdiff_t bRecentlyRendered = 0x8F7;

    // ── Bone decrypt (CL-1315578) ────────────────────────────────────────────
    constexpr std::ptrdiff_t Encrypted = 0x7A0;
    // Bone decrypt LOD dword (DecryptBoneArray, CL-1315578) — mesh+0x830, not camera
    constexpr std::ptrdiff_t LodSelect = 0x830;
    constexpr std::ptrdiff_t BoneArrayLodStride = 0xB8;

    // ── Mesh / character ─────────────────────────────────────────────────────
    constexpr std::ptrdiff_t USkeletalMeshComponent = 0x428;
    constexpr std::ptrdiff_t SkeletalMeshAsset = 0xA20;
    constexpr std::ptrdiff_t bNoSkeletonUpdate = 0xBB1;
    constexpr std::ptrdiff_t bForceRefpose = 0xBB2;
    constexpr std::ptrdiff_t CharacterMovement = 0x438;
    constexpr std::ptrdiff_t Velocity = 0x1A8;
    constexpr std::ptrdiff_t PioneerCharacterMovement = 0xb38;
    constexpr std::ptrdiff_t HealthComponent = 0xDC8;
    constexpr std::ptrdiff_t InventoryComponent = 0xCA0; // PioneerPlayerCharacter; 0xCB0 is FieldSalvagingComponent

    constexpr std::ptrdiff_t HealthInfo = 0x530;
    constexpr std::ptrdiff_t PlayerState_Health = HealthInfo;
    constexpr std::ptrdiff_t PlayerState_MaxHealth = 0x538;
    constexpr std::ptrdiff_t PlayerState_Armor = 0x540;
    constexpr std::ptrdiff_t PlayerState_MaxArmor = 0x548; // same slot as PlayerStatus — armor readers use this name (#22)
    constexpr std::ptrdiff_t Health = 0x668;  // HealthComponent::CachedHealth (SDK-confirmed)
    constexpr std::ptrdiff_t MaxHealth = 0x308;
    constexpr std::ptrdiff_t Shield = 0x150;     // ArmorSlot.Health (SDK-confirmed)
    constexpr std::ptrdiff_t ShieldMax = 0x160; // ArmorSlot.CurrentMaxArmor (SDK-confirmed)
    constexpr std::ptrdiff_t MaxDBNO = 0x2E8;
    constexpr std::ptrdiff_t TeamID = 0x812;

    // CurrentItemActors (replicated) — LocalCurrentItemActors@0x4D0 is empty for remotes.
    constexpr std::ptrdiff_t LocalCurrentItemActors = 0x4B0;
    constexpr std::ptrdiff_t EquippedPrimaryItem = 0x520;
    constexpr std::ptrdiff_t WeaponQuality = 0x472;

    // Stowed weapon slots in InventoryComponent (each FStowedWeaponInfo, 0x40 bytes)
    constexpr std::ptrdiff_t StowedWeaponSlot0 = 0x340;
    constexpr std::ptrdiff_t StowedWeaponSlot1 = 0x380;
    // EquippedArmor in InventoryComponent
    constexpr std::ptrdiff_t EquippedArmor = 0x518;

    // LootContainerSingle (help SDK): LootInteraction@0xB58, ItemContainer@0xB60.
    // 0xB68 is MetadataTagComponent — do not use as loot interaction.
    constexpr std::ptrdiff_t LootInteractionComponent = 0xB58;
    constexpr std::ptrdiff_t LootContainer_ItemContainer = 0xB60;
    constexpr std::ptrdiff_t LootInteraction_Container = 0xBB8;
    // SalvageContainerSingle::ChosenMesh / MeshVariants (help SDK).
    constexpr std::ptrdiff_t SalvageContainer_ChosenMesh = 0xC90;
    // LootInteractionComponent::bHasBeenOpened (help SDK) — was wrongly 0x810.
    constexpr std::ptrdiff_t LootInteraction_Searched = 0x870;
    constexpr std::ptrdiff_t SimpleLootActivity_LootInteraction = 0x4E0;
    constexpr std::ptrdiff_t SimpleLootActivity_LootStateMachine = 0x508;
    constexpr std::ptrdiff_t SimpleLootActivity_ItemContainer = 0x4F8;
    constexpr std::ptrdiff_t SalvageContainer_MeshVariants = 0xC58;
    constexpr std::ptrdiff_t ItemDataAsset = 0x8F8;
    constexpr std::ptrdiff_t BP_PickupBase_SpawnItems = 0x540;
    // ItemContainerComponent::OpenTime @0x500; ConstructableItemContainer @0x490.
    constexpr std::ptrdiff_t ItemContainer_OpenTime = 0x500;
    constexpr std::ptrdiff_t ConstructableItemContainer_OpenTime = 0x490;
    constexpr std::ptrdiff_t ItemDataAsset_OverrideItemAssetId = 0x120;
    constexpr std::ptrdiff_t ItemDataAsset_bOverrideItemAssetId = 0x118;
    constexpr std::ptrdiff_t UIHoverData = 0x550;
    constexpr std::ptrdiff_t UIHoverData_Pickup = 0x5F8;

    // Pickup (help)
    constexpr std::ptrdiff_t Pickup_RootCollider = 0x460;
    constexpr std::ptrdiff_t Pickup_Interaction = 0x478;
    constexpr std::ptrdiff_t Pickup_DefaultPickupDataAsset = 0x488;
    /** BP_PickupBase ContainedItem_BB (BBItemContainerItem, 0x30). */
    constexpr std::ptrdiff_t Pickup_ContainedItem_BB = 0x490;
    constexpr std::ptrdiff_t BBItem_DataAssetIndex = 0x0; // uint16
    /** ContainedItem_BB.Amount is ABInt @ +0x4; ABInt.Value @ +0xC within ABInt. */
    constexpr std::ptrdiff_t BBItem_AmountValue = 0x4 + 0xC; // int32 @ ContainedItem+0x10
    /** StandardInteractionComponent::bIsActive @ +0x137 mask 0x8. */
    constexpr std::ptrdiff_t Interaction_bIsActiveByte = 0x137;
    constexpr uint8_t Interaction_bIsActiveMask = 0x8;
    /** BaseInteractionComponent::CurrentInteractionState @ +0x43D (ECurrentInteractionState). */
    constexpr std::ptrdiff_t Interaction_CurrentInteractionState = 0x43d;

    /** Soft-deprecated bot-dead gate (not in SDK). Overlay may still list it; prefer Constructable_bIsDestroyed. */
    constexpr std::ptrdiff_t bIsBreaked = 0x1220;
    constexpr std::ptrdiff_t Constructable_EnemyTypeDataAsset = 0x11A0;
    constexpr std::ptrdiff_t Constructable_AITemplateData = 0x1190;
    /** Primary bot destroyed flag (help/SDK). Use this instead of bIsBreaked@0x1220. */
    constexpr std::ptrdiff_t Constructable_bIsDestroyed = 0x1210;
    constexpr std::ptrdiff_t Constructable_HealthService = 0x1270;
    constexpr std::ptrdiff_t Constructable_HealthGroupService = 0x1298;
    constexpr std::ptrdiff_t HealthGroupService_BaseGroup = 0x1A8;
    constexpr std::ptrdiff_t BotHealthCached = 0x668;
    constexpr std::ptrdiff_t BotHealthMax = 0x308;
    constexpr std::ptrdiff_t LastSubmitTimeAlt = 0x4CC;
    constexpr std::ptrdiff_t CurrentHealth = 0x1D0;
    constexpr std::ptrdiff_t UseDistance = 0x268;

    constexpr std::ptrdiff_t AttachChildren = 0x168;
    constexpr std::ptrdiff_t StaticMesh = 0x710;
    constexpr std::ptrdiff_t StaticMeshLegacy = 0x6A0;
    constexpr std::ptrdiff_t BodySetup = 0x1F0;
    constexpr std::ptrdiff_t AggGeom = 0xb8;
    constexpr std::ptrdiff_t ExtendedBounds = 0x300;
    constexpr std::ptrdiff_t PositiveBoundsExt = 0x2D0;
    constexpr std::ptrdiff_t NegativeBoundsExt = 0x2E8;
    constexpr std::ptrdiff_t AggGeom_SphereElems = 0x00;
    constexpr std::ptrdiff_t AggGeom_BoxElems = 0x10;
    constexpr std::ptrdiff_t AggGeom_SphylElems = 0x20;
    constexpr std::ptrdiff_t AggGeom_ConvexElems = 0x30;
    constexpr std::ptrdiff_t ConvexElem_VertexData = 0x30;
    constexpr std::ptrdiff_t ConvexElem_IndexData = 0x40;
    constexpr std::ptrdiff_t ConvexElem_Stride = 0x110;
    constexpr std::ptrdiff_t BoxElem_Center = 0x30;
    constexpr std::ptrdiff_t BoxElem_Rotation = 0x48;
    constexpr std::ptrdiff_t BoxElem_XExtent = 0x60;
    constexpr std::ptrdiff_t BoxElem_YExtent = 0x64;
    constexpr std::ptrdiff_t BoxElem_ZExtent = 0x68;
    constexpr std::ptrdiff_t BoxElem_Stride = 0x70;
    constexpr std::ptrdiff_t SphereElem_Center = 0x30;
    constexpr std::ptrdiff_t SphereElem_Radius = 0x48;
    constexpr std::ptrdiff_t SphereElem_Stride = 0x50;
    constexpr std::ptrdiff_t SphylElem_Center = 0x30;
    constexpr std::ptrdiff_t SphylElem_Rotation = 0x48;
    constexpr std::ptrdiff_t SphylElem_Radius = 0x60;
    constexpr std::ptrdiff_t SphylElem_Length = 0x64;
    constexpr std::ptrdiff_t SphylElem_Stride = 0x68;

    constexpr std::ptrdiff_t EmbarkMesh = 0x7d8;

    // ── PhysX (help, header-only) ────────────────────────────────────────────
    constexpr std::ptrdiff_t NpScene_ActorArrayData = 0x2618;
    constexpr std::ptrdiff_t NpScene_ActorArrayCount = 0x2620;
    constexpr std::ptrdiff_t NpRigidActor_ShapeManager = 0x30;
    constexpr std::ptrdiff_t NpRigidActor_Pose = 0xA0;
    constexpr std::ptrdiff_t NpShape_LocalPose = 0x90;
    constexpr std::ptrdiff_t NpShape_GeomType = 0xB8;
    constexpr std::ptrdiff_t NpShape_GeomData = 0xBC;

    // ── Lighting (help, header-only) ─────────────────────────────────────────
    constexpr std::ptrdiff_t LightComponentBase_Brightness = 0x3A0;
    constexpr std::ptrdiff_t LightComponent_Temperature = 0x3C8;
    constexpr std::ptrdiff_t DirectionalLight_ShadowCascadeBias = 0x4D0;
    constexpr std::ptrdiff_t SkyLight_bRealTimeCapture = 0x3C8;
    constexpr std::ptrdiff_t SkyAtmosphere_TransformMode = 0x390;
    constexpr std::ptrdiff_t VolumetricCloud_LayerBottomAltitude = 0x390;
    constexpr std::ptrdiff_t HeightFog_FogDensity = 0x390;
    constexpr std::ptrdiff_t PostProcess_Settings = 0x3A0;
    constexpr std::ptrdiff_t PostProcessVolume_Settings = 0x3E0;
}
