# Offset reconciliation

Generated 2026-09-24 21:13 by `tools/reconcile_offsets.py`.

Every constant in `Project/Core/Offsets.h` is scored against every offline source. No row needs a human to pick a winner: a disagreement is decided by the two confidence scores, and `tools/gen_offsets.py --check` fails the build when a source that can see a slot outranks the shipped value.

Sources: the FrostDumper index of the 2026-09-22 build (`tools/sdk_index.json`, 21550 classes/structs), the CL drop `sdk/sdk.txt` (929 constants across 7 namespaces), `Project/Core/SDK.hpp` `game::offsets`.

| bucket | constants |
| --- | --- |
| INAPPLICABLE | 4 |
| BOTH | 1 |
| RETIRED | 1 |
| PROBED | 100 |
| AGREE | 281 |

## Authority ladder

| score | source | what it can see |
| --- | --- | --- |
| 6 | live | pinned against the running target; no static source carries it |
| 5 | dump | reflected UPROPERTY offset + packed-bool mask, and every class size |
| 4 | drop (2026-09-22) | the CL drop's own re-checked entry |
| 4 | header | `SDK.hpp` `game::offsets`, the value the project hand-copied |
| 3 | drop | the CL drop's entry left on its older CL value |
| 2 | derived | arithmetic or alias over a higher-ranked constant |
| 1 | probed | nothing offline can see this slot |

## Scoreboard

| source | pins | wins | loses |
| --- | --- | --- | --- |
| derived | 2 | 0 | 0 |
| drop | 121 | 0 | 4 |
| dump | 126 | 0 | 0 |
| live | 4 | 0 | 0 |
| probed | 28 | 0 | 0 |

## Fix these (a higher-authority source disagrees)

The shipped value is outranked by a source that can see the slot. `--check` fails until the generator carries the source value.

| constant | shipped | source value | provenance | verdict |
| --- | --- | --- | --- | --- |

## Decided without you (the disagreeing source cannot see the slot)

The entry is unconfirmed in the drop's own words, points past the end of the class the dump gives, names a namespace this build does not have, or ships lower authority than a live-pinned value. The shipped value stands.

| constant | shipped | source value | provenance | verdict |
| --- | --- | --- | --- | --- |
| APlayerCameraManager | 0x4D0 | 0x4E0 | live-pinned(6) | drop-unconfirmed says 0x4E0 but the drop marks it unconfirmed (WRONG for CL-1341255: 0x4E0 = CheatClass) - not evidence |
| PlayerState_Armor | 0x560 | 0x10 | derived(2) | drop says 0x10 in namespace ArcOffsets, which this build's dump has no class for - name collision, not this slot |
| PlayerState_Health | 0x550 | 0x0 | derived(2) | drop says 0x0 in namespace ArcOffsets, which this build's dump has no class for - name collision, not this slot |
| PlayerState_MaxArmor | 0x568 | 0x18 | derived(2) | drop says 0x18 in namespace ArcOffsets, which this build's dump has no class for - name collision, not this slot |

## Already resolved in the build (both values ship)

The source's value ships under another constant; a runtime probe decides.

| constant | shipped | source value | provenance | verdict |
| --- | --- | --- | --- | --- |
| ComponentToWorld_Alt | 0x2D0 | 0x310 | probed(1) | drop-fresh says 0x310, shipped value is probed; 0x310 also ships as ComponentToWorld - a runtime probe decides |

## No offline source can see these

Native (non-UPROPERTY) fields, data RVAs and crypto constants with no reflected property and no drop entry. Only the running game can confirm them.

| constant | shipped | source value | provenance | verdict |
| --- | --- | --- | --- | --- |
| AIController_Perception | 0x4A8 | - | drop-file(3) | no offline source can see this slot |
| AISight_PeripheralDeg | 0xC8 | - | drop-file(3) | no offline source can see this slot |
| AcknowledgedPawn_Fallback | 0x3D8 | - | probed(1) | no offline source can see this slot |
| ActorID | 0x18 | - | probed(1) | no offline source can see this slot |
| ActorTypeId | 0xB0 | - | probed(1) | no offline source can see this slot |
| BBItem_AmountValue | 0x10 | - | probed(1) | no offline source can see this slot |
| BBItem_DataAssetIndex | 0x0 | - | probed(1) | no offline source can see this slot |
| CameraFOV | 0x4D0 | - | derived(2) | derived expression |
| CameraLocation | 0x480 | - | derived(2) | derived expression |
| CameraPOV_FOV | 0x4D0 | - | derived(2) | derived expression |
| CameraPOV_Location | 0x480 | - | derived(2) | derived expression |
| CameraPOV_Rotation | 0x4A8 | - | derived(2) | derived expression |
| CameraRotation | 0x4A8 | - | derived(2) | derived expression |
| ClassDefaultObject | 0x70 | - | probed(1) | no offline source can see this slot |
| ClassDefaultObjectAlt | 0x78 | - | probed(1) | no offline source can see this slot |
| ComponentToWorld_Scale3D | 0x350 | - | derived(2) | derived expression |
| ComponentToWorld_Translation | 0x330 | - | derived(2) | derived expression |
| Encrypted | 0x7B0 | - | probed(1) | no offline source can see this slot |
| Encrypted_Legacy | 0x790 | - | probed(1) | no offline source can see this slot |
| FFieldClass_SuperClass | 0x40 | - | probed(1) | no offline source can see this slot |
| FFieldNameKey0Rva | 0xE7B5330 | - | probed(1) | no offline source can see this slot |
| FFieldNameKey1Rva | 0xE6F0554 | - | probed(1) | no offline source can see this slot |
| FNameBlockMaskRva | 0xB523C50 | - | probed(1) | no offline source can see this slot |
| FNameEntry_LenMask | 0x3FF | - | probed(1) | no offline source can see this slot |
| FNameEntry_LenShift | 0x6 | - | probed(1) | no offline source can see this slot |
| FNameEntry_TextOffset | 0x2 | - | probed(1) | no offline source can see this slot |
| FNameHeaderLenShift | 0xD | - | probed(1) | no offline source can see this slot |
| FNameKeyTableRva | 0x1082B26C | - | header-drop(4) | from SDK.hpp game::offsets |
| FNameKeystreamLegacyRva | 0xE2997F4 | - | probed(1) | no offline source can see this slot |
| FNamePool_BlockOffsetBits | 0x10 | - | probed(1) | no offline source can see this slot |
| FNamePool_Blocks | 0x40 | - | probed(1) | no offline source can see this slot |
| FNamePool_EntryStride | 0x2 | - | probed(1) | no offline source can see this slot |
| FStringVerificationRva | 0xD3C29A0 | - | header-drop(4) | from SDK.hpp game::offsets |
| GNamePoolRva | 0x10987D40 | - | header-drop(4) | from SDK.hpp game::offsets |
| GObjPshufbMaskRva | 0xAD97CC0 | - | probed(1) | no offline source can see this slot |
| GObjectsRva | 0x10C56FE0 | - | probed(1) | no offline source can see this slot |
| GObjects_NumElements | 0xC | - | probed(1) | no offline source can see this slot |
| GUObjectArrayChunksRva | 0x10C56FE0 | - | derived(2) | derived expression |
| GameState | 0x338 | - | derived(2) | derived expression |
| HealthInfo | 0x550 | - | derived(2) | no offline source can see this slot |
| Interact_ActiveInstigator | 0x658 | - | drop-file(3) | no offline source can see this slot |
| Interact_DBNOTimerFloats | 0x428 | - | drop-file(3) | no offline source can see this slot |
| Interact_DBNOTimerTicks | 0x430 | - | drop-file(3) | no offline source can see this slot |
| Interact_DefaultInstigator | 0x660 | - | drop-file(3) | no offline source can see this slot |
| Interact_DefibTimerFloats | 0x448 | - | drop-file(3) | no offline source can see this slot |
| Interact_DefibTimerTicks | 0x450 | - | drop-file(3) | no offline source can see this slot |
| Interaction_bIsActiveByte | 0x137 | - | probed(1) | no offline source can see this slot |
| Interaction_bIsActiveMask | 0x8 | - | probed(1) | no offline source can see this slot |
| IsRenderedTime | 0x1F0 | - | probed(1) | no offline source can see this slot |
| ItemBase_Quality | 0x104 | - | drop-file(3) | no offline source can see this slot |
| ItemDataAsset | 0x898 | - | probed(1) | no offline source can see this slot |
| ItemDataAsset_OverrideItemAssetId | 0x120 | - | probed(1) | no offline source can see this slot |
| ItemDataAsset_bOverrideItemAssetId | 0x118 | - | probed(1) | no offline source can see this slot |
| ItemUIHoverData_Size | 0x28 | - | probed(1) | no offline source can see this slot |
| LastSubmitTime | 0x480 | - | derived(2) | derived expression |
| LodSelect | 0x7D0 | - | probed(1) | no offline source can see this slot |
| LodSelect_Legacy | 0x7D0 | - | probed(1) | no offline source can see this slot |
| LootContainer_ItemContainer | 0xBD8 | - | probed(1) | no offline source can see this slot |
| LootInteract_OpenedMask | 0x1 | - | drop-file(3) | no offline source can see this slot |
| LootInteraction_Container | 0xBB8 | - | derived(2) | derived expression |
| LootInteraction_Searched | 0xBD8 | - | probed(1) | no offline source can see this slot |
| Mesh_LastRenderTimeEnc | 0x4BC | - | probed(1) | no offline source can see this slot |
| Mesh_LastRenderTimeKey | 0x5AB299E0 | - | probed(1) | no offline source can see this slot |
| Mesh_LastRenderTimeOnScreenKey | 0xA83E5CBE | - | probed(1) | no offline source can see this slot |
| MiniMap_UnitsPerPixel | 0x478 | - | drop-file(3) | no offline source can see this slot |
| OuterDecrypt_HashSeedOff | 0x10 | - | probed(1) | no offline source can see this slot |
| OuterDecrypt_HashShr | 0x10 | - | probed(1) | no offline source can see this slot |
| OuterDecrypt_SlotMask | 0x3 | - | probed(1) | no offline source can see this slot |
| OuterDecrypt_SlotXor | 0x2 | - | probed(1) | no offline source can see this slot |
| OuterDecrypt_XorMask | 0x9A492C85DDF6F193 | - | drop-file(3) | no offline source can see this slot |
| PHI_BrokenArmorByte | 0x20 | - | drop-file(3) | no offline source can see this slot |
| Pickup_ContainedItem_BB | 0x4A0 | - | probed(1) | no offline source can see this slot |
| PlayerController_bIsLocalPlayerController_Mask | 0x1 | - | probed(1) | no offline source can see this slot |
| PlayerDecrypt_LocalPlayerOffset | 0x4B0 | - | probed(1) | no offline source can see this slot |
| PlayerHealthInfoBase | 0x588 | - | drop-file(3) | no offline source can see this slot |
| PlayerNameOnPawn | 0x438 | - | probed(1) | no offline source can see this slot |
| PlayerState_MaxHealth | 0x558 | - | derived(2) | no offline source can see this slot |
| ProcessEventIndex | 0x4C | - | probed(1) | no offline source can see this slot |
| ProcessEventRva | 0x5AF560 | - | probed(1) | no offline source can see this slot |
| RF_BeginDestroyed | 0x800000 | - | probed(1) | no offline source can see this slot |
| RF_FinishDestroyed | 0x1000000 | - | probed(1) | no offline source can see this slot |
| RepMovement_bRepPhysicsMask | 0x2 | - | probed(1) | no offline source can see this slot |
| ReplicatedRootTransform | 0x1F8 | - | probed(1) | no offline source can see this slot |
| ShieldMax | 0x1D0 | - | derived(2) | derived expression |
| SimpleLootActivity_ItemContainer | 0x498 | - | probed(1) | no offline source can see this slot |
| SimpleLootActivity_LootInteraction | 0x480 | - | probed(1) | no offline source can see this slot |
| SimpleLootActivity_LootStateMachine | 0x4A8 | - | probed(1) | no offline source can see this slot |
| StaticMeshLegacy | 0x718 | - | probed(1) | no offline source can see this slot |
| StowedInfo_Quality | 0x38 | - | drop-file(3) | no offline source can see this slot |
| StowedInfo_Size | 0x40 | - | drop-file(3) | no offline source can see this slot |
| StyleDrivers | 0x610 | - | drop-file(3) | no offline source can see this slot |
| UIHoverData | 0x620 | - | probed(1) | no offline source can see this slot |
| UIHoverData_Pickup | 0x620 | - | derived(2) | derived expression |
| UObject_ClassPrivate | 0x20 | - | probed(1) | no offline source can see this slot |
| UObject_NamePrivate | 0x98 | - | probed(1) | no offline source can see this slot |
| UObject_ObjectFlags | 0x8 | - | probed(1) | no offline source can see this slot |
| UObject_OuterPrivate | 0xA0 | - | probed(1) | no offline source can see this slot |
| UWorld | 0x10839A98 | - | header-drop(4) | from SDK.hpp game::offsets |
| UWorldGlobalIntermediary | 0x0 | - | probed(1) | no offline source can see this slot |
| WorldLocation | 0x330 | - | derived(2) | derived expression |

## Every constant in the CL drop

929 constants, each with the source that decides it: the dump property it should describe (named), the value Offsets.h ships, and the resulting verdict. No row needs a judgement call.

| verdict | constants |
| --- | --- |
| DUMP-WINS | 16 |
| DROP-WINS | 34 |
| UNCONFIRMED | 16 |
| STRUCT-RELATIVE | 2 |
| NO-COMPARABLE | 5 |
| AGREE | 220 |
| NOT-SHIPPED-DUMP | 409 |
| NOT-SHIPPED-DROP | 227 |

| verdict | drop constant | drop | dump source | dump | Offsets.h row | shipped | why |
| --- | --- | --- | --- | --- | --- | --- | --- |
| DUMP-WINS | ArcOffsets::AIStateService::ALERTNESS | 0x1BC | Angelscript.PioneerConstructableAIStateService.Alertness | 0x1EC | AIState_Alertness | 0x1BC | Offsets.h.AIState_Alertness follows the drop's 0x1BC; the dump reflects Angelscript.PioneerConstructableAIStateService.Alertness = 0x1EC |
| DUMP-WINS | ArcOffsets::AIStateService::COMBAT_PHASE | 0x1C9 | Angelscript.PioneerConstructableAIStateService.CombatPhase | 0x1F9 | AIState_CombatPhase | 0x1C9 | Offsets.h.AIState_CombatPhase follows the drop's 0x1C9; the dump reflects Angelscript.PioneerConstructableAIStateService.CombatPhase = 0x1F9 |
| DUMP-WINS | ArcOffsets::AIStateService::SIGHT_HALF_ANGLE | 0x1CA | Angelscript.PioneerConstructableAIStateService.SightHalfAngle | 0x1FA | AIState_SightHalfAngle | 0x1CA | Offsets.h.AIState_SightHalfAngle follows the drop's 0x1CA; the dump reflects Angelscript.PioneerConstructableAIStateService.SightHalfAngle = 0x1FA |
| DUMP-WINS | ArcOffsets::AIStateService::SIGHT_RANGE | 0x1CC | Angelscript.PioneerConstructableAIStateService.SightRange | 0x1FC | AIState_SightRange | 0x1CC | Offsets.h.AIState_SightRange follows the drop's 0x1CC; the dump reflects Angelscript.PioneerConstructableAIStateService.SightRange = 0x1FC |
| DUMP-WINS | ArcOffsets::Actor::B_HIDDEN_MASK | 0x10 | Engine.Actor.bHidden mask | 0x1 | Actor_bHiddenMask | 0x1 | Offsets.h.Actor_bHiddenMask follows Engine.Actor.bHidden mask = 0x1; the drop is stale |
| DUMP-WINS | ArcOffsets::ConstructableStaticMeshStyle::STATIC_MESH | 0x728 | EmbarkConstructable.ConstructableStaticMeshStyleComponent.StaticMesh | 0x6D8 | StaticMesh | 0x6D8 | Offsets.h.StaticMesh follows EmbarkConstructable.ConstructableStaticMeshStyleComponent.StaticMesh = 0x6D8; the drop is stale |
| DUMP-WINS | ArcOffsets::ExtractionPoint::EXTRACTION_INFO | 0xB58 | Angelscript.SalvageExtractionPointBase.ExtractionInfo | 0xBB8 | ExtractionPoint_ExtractionInfo | 0xBB8 | Offsets.h.ExtractionPoint_ExtractionInfo follows Angelscript.SalvageExtractionPointBase.ExtractionInfo = 0xBB8; the drop is stale |
| DUMP-WINS | ArcOffsets::ExtractionPoint::STATE | 0xBD2 | Angelscript.SalvageExtractionPointBase.State | 0xC32 | ExtractionPoint_State | 0xC32 | Offsets.h.ExtractionPoint_State follows Angelscript.SalvageExtractionPointBase.State = 0xC32; the drop is stale |
| DUMP-WINS | ArcOffsets::ExtractionPoint::STATE_CHANGE_TIMESTAMP | 0xBD8 | Angelscript.SalvageExtractionPointBase.StateChangeTimestamp | 0xC38 | ExtractionPoint_StateChangeTimestamp | 0xC38 | Offsets.h.ExtractionPoint_StateChangeTimestamp follows Angelscript.SalvageExtractionPointBase.StateChangeTimestamp = 0xC38; the drop is stale |
| DUMP-WINS | ArcOffsets::ExtractionPoint::TIME_LEFT_AFTER_SHUTDOWN | 0xC38 | Angelscript.SalvageExtractionPoint.TimeLeftAfterShutdown | 0xC98 | ExtractionPoint_TimeLeftAfterShutdown | 0xC98 | Offsets.h.ExtractionPoint_TimeLeftAfterShutdown follows Angelscript.SalvageExtractionPoint.TimeLeftAfterShutdown = 0xC98; the drop is stale |
| DUMP-WINS | ArcOffsets::ExtractionPoint::TIME_LEFT_AFTER_STARTUP | 0xC40 | Angelscript.SalvageExtractionPoint.TimeLeftAfterStartup | 0xCA0 | ExtractionPoint_TimeLeftAfterStartup | 0xCA0 | Offsets.h.ExtractionPoint_TimeLeftAfterStartup follows Angelscript.SalvageExtractionPoint.TimeLeftAfterStartup = 0xCA0; the drop is stale |
| DUMP-WINS | ArcOffsets::HealthComponent::MAX_HEALTH | 0x348 | Angelscript.HealthComponent.MaxHealth | 0x378 | MaxHealth | 0x348 | Offsets.h.MaxHealth follows the drop's 0x348; the dump reflects Angelscript.HealthComponent.MaxHealth = 0x378 |
| DUMP-WINS | ArcOffsets::PlayerState::CURRENT_PAWN | 0x570 | Angelscript.PioneerPlayerState.CurrentPawn | 0x560 | PioneerPlayerState_CurrentPawn | 0x560 | Offsets.h.PioneerPlayerState_CurrentPawn follows Angelscript.PioneerPlayerState.CurrentPawn = 0x560; the drop is stale |
| DUMP-WINS | ArcOffsets::PlayerState::PIONEER_CHARACTER | 0x568 | Angelscript.PioneerPlayerState.PioneerCharacter | 0x558 | PioneerPlayerState_PioneerCharacter | 0x558 | Offsets.h.PioneerPlayerState_PioneerCharacter follows Angelscript.PioneerPlayerState.PioneerCharacter = 0x558; the drop is stale |
| DUMP-WINS | ArcOffsets::PlayerState::PLAYER_STATUS | 0x550 | Angelscript.PioneerPlayerState.PlayerStatus | 0x540 | PlayerState_PlayerStatus | 0x540 | Offsets.h.PlayerState_PlayerStatus follows Angelscript.PioneerPlayerState.PlayerStatus = 0x540; the drop is stale |
| DUMP-WINS | ArcOffsets::SceneComponent::RELATIVE_LOCATION | 0x2A8 | Engine.SceneComponent.RelativeLocation | 0x268 | RelativeLocation_Alt | 0x2A8 | Offsets.h.RelativeLocation_Alt follows the drop's 0x2A8; the dump reflects Engine.SceneComponent.RelativeLocation = 0x268 |
| DROP-WINS | ArcOffsets::AIStateService::EMOTIONAL_STATE | 0x1C8 | Angelscript.PioneerConstructableAIStateService.EmotionalState | 0x1F8 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.EmotionalState = 0x1F8, drop says 0x1C8 |
| DROP-WINS | ArcOffsets::AIStateService::FULL_SIGHT_OVERRIDE_ALERTNESS_STATES | 0x1B8 | Angelscript.PioneerConstructableAIStateService.FullSightOverrideAlertnessStates | 0x1E8 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.FullSightOverrideAlertnessStates = 0x1E8, drop says 0x1B8 |
| DROP-WINS | ArcOffsets::AIStateService::OUT_OF_COMBAT_AIMING_BEHAVIOR | 0x1B1 | Angelscript.PioneerConstructableAIStateService.OutOfCombatAimingBehavior | 0x1E1 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.OutOfCombatAimingBehavior = 0x1E1, drop says 0x1B1 |
| DROP-WINS | ArcOffsets::AIStateService::ROUGH_ALERTNESS_LEVEL | 0x1D0 | Angelscript.PioneerConstructableAIStateService.RoughAlertnessLevel | 0x200 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.RoughAlertnessLevel = 0x200, drop says 0x1D0 |
| DROP-WINS | ArcOffsets::AIStateService::SIGHT_RANGE_STRENGTH_SCALE | 0x1B4 | Angelscript.PioneerConstructableAIStateService.SightRangeStrengthScale | 0x1E4 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.SightRangeStrengthScale = 0x1E4, drop says 0x1B4 |
| DROP-WINS | ArcOffsets::AIStateService::TARGET | 0x1C0 | Angelscript.PioneerConstructableAIStateService.Target | 0x1F0 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.Target = 0x1F0, drop says 0x1C0 |
| DROP-WINS | ArcOffsets::AIStateService::UTILITY_STATE | 0x1D4 | Angelscript.PioneerConstructableAIStateService.UtilityState | 0x204 | - | - | not shipped; dump says Angelscript.PioneerConstructableAIStateService.UtilityState = 0x204, drop says 0x1D4 |
| DROP-WINS | ArcOffsets::Actor::HEALTH_COMPONENT | 0xDC0 | Angelscript.EnemyGymOptionSelector.HealthComponent | 0xBC0 | - | - | not shipped; dump says Angelscript.EnemyGymOptionSelector.HealthComponent = 0xBC0, drop says 0xDC0 |
| DROP-WINS | ArcOffsets::Engine::GAME_INSTANCE | 0x12E0 | EmbarkEngine.EmbarkGameEngine.GameInstance (all 2 classes here) | 0x1590 | - | - | not shipped; dump says EmbarkEngine.EmbarkGameEngine.GameInstance (all 2 classes here) = 0x1590, drop says 0x12E0 |
| DROP-WINS | ArcOffsets::FField::NEXT | 0x68 | CoreUObject.Field.Next | 0x98 | - | - | not shipped; dump says CoreUObject.Field.Next = 0x98, drop says 0x68 |
| DROP-WINS | ArcOffsets::FField::OWNER | 0x58 | Angelscript.WalkerElectrofield.Owner | 0x1D8 | - | - | not shipped; dump says Angelscript.WalkerElectrofield.Owner = 0x1D8, drop says 0x58 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONMipBlend | 0x5FC | Engine.PostProcessSettings.AmbientOcclusionMipBlend | 0x634 | - | - | not shipped; dump says Engine.PostProcessSettings.AmbientOcclusionMipBlend = 0x634, drop says 0x5FC |
| DROP-WINS | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONMipScale | 0x600 | Engine.PostProcessSettings.AmbientOcclusionMipScale | 0x638 | - | - | not shipped; dump says Engine.PostProcessSettings.AmbientOcclusionMipScale = 0x638, drop says 0x600 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONMipThreshold | 0x604 | Engine.PostProcessSettings.AmbientOcclusionMipThreshold | 0x63C | - | - | not shipped; dump says Engine.PostProcessSettings.AmbientOcclusionMipThreshold = 0x63C, drop says 0x604 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONTemporalBlendWeight | 0x608 | Engine.PostProcessSettings.AmbientOcclusionTemporalBlendWeight | 0x640 | - | - | not shipped; dump says Engine.PostProcessSettings.AmbientOcclusionTemporalBlendWeight = 0x640, drop says 0x608 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_RADIUS_IN_WS | 0x5E0 | Engine.PostProcessSettings.AmbientOcclusionRadiusInWS | 0x618 | - | - | not shipped; dump says Engine.PostProcessSettings.AmbientOcclusionRadiusInWS = 0x618, drop says 0x5E0 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::CAMERA_ISO | 0x474 | Engine.PostProcessSettings.CameraISO | 0x484 | - | - | not shipped; dump says Engine.PostProcessSettings.CameraISO = 0x484, drop says 0x474 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::COLOR_GRADING_LUT | 0x620 | Engine.PostProcessSettings.ColorGradingLUT | 0x658 | - | - | not shipped; dump says Engine.PostProcessSettings.ColorGradingLUT = 0x658, drop says 0x620 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::MOTION_BLUR_TARGET_FPS | 0x67C | Engine.PostProcessSettings.MotionBlurTargetFPS | 0x6FC | - | - | not shipped; dump says Engine.PostProcessSettings.MotionBlurTargetFPS = 0x6FC, drop says 0x67C |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_AO | 0x60C | Engine.PostProcessSettings.RayTracingAO | 0x644 | - | - | not shipped; dump says Engine.PostProcessSettings.RayTracingAO = 0x644, drop says 0x60C |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_AO_INTENSITY | 0x614 | Engine.PostProcessSettings.RayTracingAOIntensity | 0x64C | - | - | not shipped; dump says Engine.PostProcessSettings.RayTracingAOIntensity = 0x64C, drop says 0x614 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_AO_RADIUS | 0x618 | Engine.PostProcessSettings.RayTracingAORadius | 0x650 | - | - | not shipped; dump says Engine.PostProcessSettings.RayTracingAORadius = 0x650, drop says 0x618 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_GI_MAX_BOUNCES | 0x41C | Engine.PostProcessSettings.bOverride_RayTracingGIMaxBounces | 0x30 | - | - | not shipped; dump says Engine.PostProcessSettings.bOverride_RayTracingGIMaxBounces = 0x30, drop says 0x41C |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_GI_SAMPLES_PER_PIXEL | 0x420 | Engine.PostProcessSettings.bOverride_RayTracingGISamplesPerPixel | 0x30 | - | - | not shipped; dump says Engine.PostProcessSettings.bOverride_RayTracingGISamplesPerPixel = 0x30, drop says 0x420 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_REFLECTIONS_MAX_BOUNCES | 0x444 | Engine.PostProcessSettings.bOverride_RayTracingReflectionsMaxBounces | 0x20 | - | - | not shipped; dump says Engine.PostProcessSettings.bOverride_RayTracingReflectionsMaxBounces = 0x20, drop says 0x444 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_REFLECTIONS_SAMPLES_PER_PIXEL | 0x448 | Engine.PostProcessSettings.bOverride_RayTracingReflectionsSamplesPerPixel | 0x20 | - | - | not shipped; dump says Engine.PostProcessSettings.bOverride_RayTracingReflectionsSamplesPerPixel = 0x20, drop says 0x448 |
| DROP-WINS | ArcOffsets::FPostProcessSettings::RAY_TRACING_REFLECTIONS_TRANSLUCENCY | 0x44D | Engine.PostProcessSettings.bOverride_RayTracingReflectionsTranslucency | 0x20 | - | - | not shipped; dump says Engine.PostProcessSettings.bOverride_RayTracingReflectionsTranslucency = 0x20, drop says 0x44D |
| DROP-WINS | ArcOffsets::InteractQuestComponent::RELEVANT_PLAYER_IDS_NUM | 0x240 | Angelscript.InteractQuestComponent.RelevantPlayerIds + 0x8 (array count) | 0x2B0 | - | - | not shipped; dump says Angelscript.InteractQuestComponent.RelevantPlayerIds + 0x8 (array count) = 0x2B0, drop says 0x240 |
| DROP-WINS | ArcOffsets::LevelStreaming::LEVEL_TRANSFORM | 0xF0 | Engine.LevelStreaming.LevelTransform | 0x100 | - | - | not shipped; dump says Engine.LevelStreaming.LevelTransform = 0x100, drop says 0xF0 |
| DROP-WINS | ArcOffsets::PlatformIdComponent::PLATFORM_ID | 0x188 | EmbarkGameplay.EmbarkPlatformIdComponent.PlatformId | 0x1B8 | - | - | not shipped; dump says EmbarkGameplay.EmbarkPlatformIdComponent.PlatformId = 0x1B8, drop says 0x188 |
| DROP-WINS | ArcOffsets::PlayerState::ACHIEVEMENT_COMPONENT | 0x5C0 | Angelscript.PioneerLobbyPlayerState.AchievementComponent (all 2 classes here) | 0x5B0 | - | - | not shipped; dump says Angelscript.PioneerLobbyPlayerState.AchievementComponent (all 2 classes here) = 0x5B0, drop says 0x5C0 |
| DROP-WINS | ArcOffsets::PlayerState::B_FINISHED_ROUND | 0x5B8 | Angelscript.PioneerLobbyPlayerState.bFinishedRound (all 2 classes here) | 0x5A8 | - | - | not shipped; dump says Angelscript.PioneerLobbyPlayerState.bFinishedRound (all 2 classes here) = 0x5A8, drop says 0x5B8 |
| DROP-WINS | ArcOffsets::PlayerState::PLATFORM_ID_COMPONENT | 0x4C0 | Angelscript.EmbarkPlayerState.PlatformIdComponent | 0x4B0 | - | - | not shipped; dump says Angelscript.EmbarkPlayerState.PlatformIdComponent = 0x4B0, drop says 0x4C0 |
| DROP-WINS | ArcOffsets::PlayerState::SQUAD | 0x498 | Angelscript.EmbarkPlayerState.Squad | 0x488 | - | - | not shipped; dump says Angelscript.EmbarkPlayerState.Squad = 0x488, drop says 0x498 |
| UNCONFIRMED | ArcOffsets::BodySetup::CHAOS_IMPLICIT | 0xB0 | - | - | - | - | drop only, the drop marks this FImplicitObject* AGG_GEOM-0x08 - recomputed ; no BodySetup.*.CHAOS_IMPLICIT in the dump |
| UNCONFIRMED | ArcOffsets::HealthComponent::HEALTH_DIE | 0x670 | - | - | - | - | drop only, the drop marks this block +0x10 revert (was 0x660) — RE-VERIFY L; no HealthComponent.*.HEALTH_DIE in the dump |
| UNCONFIRMED | ArcOffsets::InventoryComponent::EQUIPPED_PRIMARY_ITEM | 0x500 | - | - | - | - | drop only, the drop marks this estimated 2026-08-11 (was 0x510, -0x10); no InventoryComponent.*.EQUIPPED_PRIMARY_ITEM in the dump |
| UNCONFIRMED | ArcOffsets::Pickup::UI_HOVER_DATA | 0x620 | - | - | - | - | drop only, the drop marks this estimated; no Pickup.*.UI_HOVER_DATA in the dump |
| UNCONFIRMED | ArcOffsets::PlayerState::PLAYER_NAME_FORMATTED | 0x4E0 | - | - | - | - | drop only, the drop marks this estimated 2026-08-09 (was 0x4B0); no PlayerState.*.PLAYER_NAME_FORMATTED in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_FIRED_WEAPON_TIMER | 0x708 | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_FIRED_WEAPON_TIMER in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_FIRE_STATE | 0x6C0 | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_FIRE_STATE in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_FIRE_WEAPON_COUNT | 0x674 | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_FIRE_WEAPON_COUNT in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_HAS_WEAPON_FIRED_SHOT | 0x1340 | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_HAS_WEAPON_FIRED_SHOT in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_IS_FIRING | 0x672 | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_IS_FIRING in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_LAST_FIRED_SHOT_TS | 0x13D0 | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_LAST_FIRED_SHOT_TS in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::ANIM_SHOTS_IN_AUTO_FIRE | 0x132C | - | - | - | - | drop only, the drop marks this AnimInstance internal — needs verification; no SkeletalMeshComponent.*.ANIM_SHOTS_IN_AUTO_FIRE in the dump |
| UNCONFIRMED | ArcOffsets::SkeletalMeshComponent::SKELETAL_MESH_ALT | 0xB10 | - | - | - | - | drop only, the drop marks this estimated 2026-08-11 (was 0xAE0, +0x30); no SkeletalMeshComponent.*.SKELETAL_MESH_ALT in the dump |
| UNCONFIRMED | ArcOffsets::StaticMeshGeometry::SMC_FLAG_BYTE_6B4 | 0x73C | - | - | - | - | drop only, the drop marks this estimated 2026-08-11 (was 0x72C, +0x10); namespace ArcOffsets has no class in the dump |
| UNCONFIRMED | ArcOffsets::UWorld::DEFAULT_QUERY_PARAMS_RVA | 0xE7A3098 | - | - | - | - | drop only, the drop marks this needs re-scan for 2026-08-11; no UWorld.*.DEFAULT_QUERY_PARAMS_RVA in the dump |
| UNCONFIRMED | ArcOffsets::UWorld::LINETRACE_RVA | 0x2F6E930 | - | - | - | - | drop only, the drop marks this needs re-scan for 2026-08-11; no UWorld.*.LINETRACE_RVA in the dump |
| STRUCT-RELATIVE | ArcOffsets::Level::ACTOR_COUNT | 0x118 | Level (struct-relative via Offsets.h.ActorsCount) | - | ActorsCount | 0x118 | the row is Offsets.h.AActors + 0x8 (unsourced); and the drop's 0x118 is that same slot |
| STRUCT-RELATIVE | ArcOffsets::PlayerHealthInfo::HEALTH | 0x0 | PlayerHealthInfo (struct-relative via Offsets.h.PlayerState_Health) | - | PlayerState_Health | 0x550 | the row is Offsets.h.HealthInfo = 0x550; and the drop's 0x0 describes a relative field, with no dump counterpart to compare against |
| NO-COMPARABLE | ArcOffsets::PlayerController::PLAYER_CAMERA_MANAGER | 0x4E0 | - | - | APlayerCameraManager | 0x4D0 | Offsets.h.APlayerCameraManager = 0x4D0 and the drop's 0x4E0 has no counterpart here: no PlayerController.*.PLAYER_CAMERA_MANAGER in the dump |
| NO-COMPARABLE | ArcOffsets::PlayerHealthInfo::ARMOR | 0x10 | - | - | PlayerState_Armor | 0x560 | Offsets.h.PlayerState_Armor = 0x560 and the drop's 0x10 has no counterpart here: namespace ArcOffsets has no class in the dump |
| NO-COMPARABLE | ArcOffsets::PlayerHealthInfo::MAX_ARMOR | 0x18 | - | - | PlayerState_MaxArmor | 0x568 | Offsets.h.PlayerState_MaxArmor = 0x568 and the drop's 0x18 has no counterpart here: namespace ArcOffsets has no class in the dump |
| NO-COMPARABLE | ArcOffsets::PrimitiveComponent::LAST_RENDER_TIME | 0x4C0 | - | - | LastRenderTime | 0x480 | Offsets.h.LastRenderTime = 0x480 and the drop's 0x4C0 has no counterpart here: no PrimitiveComponent.*.LAST_RENDER_TIME in the dump |
| NO-COMPARABLE | ArcOffsets::PrimitiveComponent::LAST_RENDER_TIME_ON_SCREEN | 0x4C4 | - | - | LastRenderTimeOnScreen | 0x484 | Offsets.h.LastRenderTimeOnScreen = 0x484 and the drop's 0x4C4 has no counterpart here: no PrimitiveComponent.*.LAST_RENDER_TIME_ON_SCREEN in the dump |
| AGREE | ArcOffsets::AIController::PERCEPTION_COMPONENT | 0x4A8 | AIModule.AIController.PerceptionComponent | 0x4A8 | AIController_Perception | 0x4A8 | dump confirms AIModule.AIController.PerceptionComponent = 0x4A8 (Offsets.h.AIController_Perception agrees) |
| AGREE | ArcOffsets::AIPerceptionComponent::SENSES_CONFIG | 0x1B8 | AIModule.AIPerceptionComponent.SensesConfig | 0x1B8 | AIPerception_SensesConfig | 0x1B8 | dump confirms AIModule.AIPerceptionComponent.SensesConfig = 0x1B8 (Offsets.h.AIPerception_SensesConfig agrees) |
| AGREE | ArcOffsets::AISenseConfigSight::LOSE_SIGHT_RADIUS | 0xC4 | - | - | AISight_LoseSightRadius | 0xC4 | shipped as Offsets.h.AISight_LoseSightRadius and AISight_LoseSightRadius (no contrary source) |
| AGREE | ArcOffsets::AISenseConfigSight::PERIPHERAL_VISION_DEG | 0xC8 | - | - | AISight_PeripheralDeg | 0xC8 | shipped as Offsets.h.AISight_PeripheralDeg (no contrary source) |
| AGREE | ArcOffsets::AISenseConfigSight::SIGHT_RADIUS | 0xC0 | - | - | AISight_SightRadius | 0xC0 | shipped as Offsets.h.AISight_SightRadius and AISight_SightRadius (no contrary source) |
| AGREE | ArcOffsets::Actor::B_HIDDEN_BYTE | 0xD9 | - | - | Actor_bHiddenByte | 0xD9 | shipped as Offsets.h.Actor_bHiddenByte (no contrary source) |
| AGREE | ArcOffsets::Actor::MESH | 0x4A0 | - | - | USkeletalMeshComponent_Alt | 0x4A0 | shipped as Offsets.h.USkeletalMeshComponent_Alt (no contrary source) |
| AGREE | ArcOffsets::Actor::OWNER | 0x1D8 | Engine.Actor.Owner | 0x1D8 | ActorOwner | 0x1D8 | dump confirms Engine.Actor.Owner = 0x1D8 (Offsets.h.ActorOwner agrees) |
| AGREE | ArcOffsets::Actor::ROOT_COMPONENT | 0x240 | Engine.Actor.RootComponent | 0x240 | RootComponent | 0x240 | dump confirms Engine.Actor.RootComponent = 0x240 (Offsets.h.RootComponent agrees) |
| AGREE | ArcOffsets::BaseInteractionComponent::ACTIVE_INSTIGATOR | 0x658 | - | - | Interact_ActiveInstigator | 0x658 | shipped as Offsets.h.Interact_ActiveInstigator (no contrary source) |
| AGREE | ArcOffsets::BaseInteractionComponent::CURRENT_INTERACTION_STATE | 0x415 | Angelscript.BaseInteractionComponent.CurrentInteractionState | 0x415 | Interaction_CurrentInteractionState | 0x415 | dump confirms Angelscript.BaseInteractionComponent.CurrentInteractionState = 0x415 (Offsets.h.Interaction_CurrentInteractionState agrees) |
| AGREE | ArcOffsets::BaseInteractionComponent::DBNO_TIMER_FLOATS | 0x428 | - | - | Interact_DBNOTimerFloats | 0x428 | shipped as Offsets.h.Interact_DBNOTimerFloats (no contrary source) |
| AGREE | ArcOffsets::BaseInteractionComponent::DBNO_TIMER_TICKS | 0x430 | - | - | Interact_DBNOTimerTicks | 0x430 | shipped as Offsets.h.Interact_DBNOTimerTicks (no contrary source) |
| AGREE | ArcOffsets::BaseInteractionComponent::DEFAULT_INSTIGATOR | 0x660 | - | - | Interact_DefaultInstigator | 0x660 | shipped as Offsets.h.Interact_DefaultInstigator (no contrary source) |
| AGREE | ArcOffsets::BaseInteractionComponent::DEFIB_TIMER_FLOATS | 0x448 | - | - | Interact_DefibTimerFloats | 0x448 | shipped as Offsets.h.Interact_DefibTimerFloats (no contrary source) |
| AGREE | ArcOffsets::BaseInteractionComponent::DEFIB_TIMER_TICKS | 0x450 | - | - | Interact_DefibTimerTicks | 0x450 | shipped as Offsets.h.Interact_DefibTimerTicks (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::ADD_KEY_LO | 0x3FA0DEFFD4B91F00 | - | - | BoneV922AddKeyLo | 0x3FA0DEFFD4B91F00 | shipped as Offsets.h.BoneV922AddKeyLo (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::BONE_STRIDE | 0x60 | - | - | BoneV922Stride | 0x60 | shipped as Offsets.h.BoneV922Stride (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::DESCRIPTOR_BASE | 0x18 | - | - | BoneV922DescriptorBase | 0x18 | shipped as Offsets.h.BoneV922DescriptorBase (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::DESCRIPTOR_STRIDE | 0x10 | - | - | BoneV922DescriptorStride | 0x10 | shipped as Offsets.h.BoneV922DescriptorStride (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::ROL32_AMOUNT | 0x3 | - | - | BoneV922Rol32 | 0x3 | shipped as Offsets.h.BoneV922Rol32 (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::SEED_OFFSET | 0x7B0 | - | - | BoneV922SeedOffset | 0x7B0 | shipped as Offsets.h.BoneV922SeedOffset (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::SELECTOR_MASK | 0x1 | - | - | BoneV922SelectorMask | 0x1 | shipped as Offsets.h.BoneV922SelectorMask (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::SELECTOR_OFFSET | 0x848 | - | - | BoneV922SelectorOffset | 0x848 | shipped as Offsets.h.BoneV922SelectorOffset (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::SELECTOR_SHIFT | 0xF | - | - | BoneV922SelectorShift | 0xF | shipped as Offsets.h.BoneV922SelectorShift (no contrary source) |
| AGREE | ArcOffsets::BoneArrayDecrypt::XOR_KEY_LO | 0xC05F21012B46E100 | - | - | BoneV922XorKeyLo | 0xC05F21012B46E100 | shipped as Offsets.h.BoneV922XorKeyLo (no contrary source) |
| AGREE | ArcOffsets::ConstructableBase::ALL_STYLE_DRIVERS | 0x610 | EmbarkConstructable.ConstructableBase.AllStyleDrivers | 0x610 | StyleDrivers | 0x610 | dump confirms EmbarkConstructable.ConstructableBase.AllStyleDrivers = 0x610 (Offsets.h.StyleDrivers agrees) |
| AGREE | ArcOffsets::ConstructableStaticMeshStyle::DESTROYED_REASON | 0x7FA | - | - | Style_DestroyedReasonByte | 0x7FA | shipped as Offsets.h.Style_DestroyedReasonByte and Style_DestroyedReasonByte (no contrary source) |
| AGREE | ArcOffsets::ConstructableStaticMeshStyle::IS_DESTROYED | 0x7F9 | - | - | Style_IsDestroyedByte | 0x7F9 | shipped as Offsets.h.Style_IsDestroyedByte and Style_IsDestroyedByte (no contrary source) |
| AGREE | ArcOffsets::ConstructableStaticMeshStyle::PART_ID_RANGE | 0x7E0 | - | - | Style_PartIdRange | 0x7E0 | shipped as Offsets.h.Style_PartIdRange and Style_PartIdRange (no contrary source) |
| AGREE | ArcOffsets::Controller::ACKNOWLEDGED_PAWN | 0x418 | - | - | AcknowledgedPawn | 0x418 | shipped as Offsets.h.AcknowledgedPawn and AcknowledgedPawn (no contrary source) |
| AGREE | ArcOffsets::Controller::CHARACTER | 0x400 | Engine.Controller.Character | 0x400 | Controller_Character | 0x400 | dump confirms Engine.Controller.Character = 0x400 (Offsets.h.Controller_Character agrees) |
| AGREE | ArcOffsets::Controller::CONTROL_ROTATION | 0x438 | Engine.Controller.ControlRotation | 0x438 | ControlRotation | 0x438 | dump confirms Engine.Controller.ControlRotation = 0x438 (Offsets.h.ControlRotation agrees) |
| AGREE | ArcOffsets::Controller::PLAYER_STATE | 0x3D0 | Engine.Controller.PlayerState | 0x3D0 | AController_PlayerState | 0x3D0 | dump confirms Engine.Controller.PlayerState = 0x3D0 (Offsets.h.AController_PlayerState agrees) |
| AGREE | ArcOffsets::Crypto::XMM_XOR_VAL | 0xA738DD8241D227C2 | - | - | XmmXorVal | 0xA738DD8241D227C2 | shipped as Offsets.h.XmmXorVal (no contrary source) |
| AGREE | ArcOffsets::EmbarkGameStateBase::ALL_SQUADS | 0x6F0 | EmbarkGameplay.EmbarkGameStateBase.AllSquads | 0x6F0 | EmbarkGS_AllSquads | 0x6F0 | dump confirms EmbarkGameplay.EmbarkGameStateBase.AllSquads = 0x6F0 (Offsets.h.EmbarkGS_AllSquads agrees) |
| AGREE | ArcOffsets::EmbarkGameStateBase::ELAPSED_TIME | 0x4D0 | - | - | EmbarkGS_ElapsedTime | 0x4D0 | shipped as Offsets.h.EmbarkGS_ElapsedTime and EmbarkGS_ElapsedTime (no contrary source) |
| AGREE | ArcOffsets::EmbarkGameStateBase::MATCH_STATE | 0x4C0 | - | - | EmbarkGS_MatchState | 0x4C0 | shipped as Offsets.h.EmbarkGS_MatchState and EmbarkGS_MatchState (no contrary source) |
| AGREE | ArcOffsets::EmbarkGameStateBase::SQUADS_ARRAY | 0x4E8 | - | - | EmbarkGS_SquadsArray | 0x4E8 | shipped as Offsets.h.EmbarkGS_SquadsArray and EmbarkGS_SquadsArray (no contrary source) |
| AGREE | ArcOffsets::FField::CLASS_PRIVATE | 0x60 | - | - | FField_ClassPrivate | 0x60 | shipped as Offsets.h.FField_ClassPrivate (no contrary source) |
| AGREE | ArcOffsets::GameInstance::LOCAL_PLAYERS | 0x130 | Engine.GameInstance.LocalPlayers | 0x130 | LocalPlayers | 0x130 | dump confirms Engine.GameInstance.LocalPlayers = 0x130 (Offsets.h.LocalPlayers agrees) |
| AGREE | ArcOffsets::GameInstanceDecrypt::SHUFFLE_MASK_RVA | 0xB09C350 | - | - | GameInstanceShuffleMaskRva | 0xB09C350 | shipped as Offsets.h.GameInstanceShuffleMaskRva (no contrary source) |
| AGREE | ArcOffsets::GameInstanceDecrypt::WORLD_OFFSET | 0x2F0 | - | - | GameInstance_WorldBackRef | 0x2F0 | shipped as Offsets.h.GameInstance_WorldBackRef (no contrary source) |
| AGREE | ArcOffsets::GameInstanceDecrypt::XOR_KEY_RVA | 0xB06C380 | - | - | GameInstanceXorKey0Rva | 0xB06C380 | shipped as Offsets.h.GameInstanceXorKey0Rva (no contrary source) |
| AGREE | ArcOffsets::GameInstanceDecrypt::XOR_KEY_RVA_2 | 0xB06C390 | - | - | GameInstanceXorKey1Rva | 0xB06C390 | shipped as Offsets.h.GameInstanceXorKey1Rva and GameInstanceXorKey1Rva (no contrary source) |
| AGREE | ArcOffsets::GameState::ENEMY_COUNT | 0xA00 | Angelscript.PioneerGameState.EnemyCount | 0xA00 | GameState_EnemyCount | 0xA00 | dump confirms Angelscript.PioneerGameState.EnemyCount = 0xA00 (Offsets.h.GameState_EnemyCount agrees) |
| AGREE | ArcOffsets::GameState::GAME_PHASE | 0x9D0 | Angelscript.PioneerGameState.GamePhase | 0x9D0 | GameState_GamePhase | 0x9D0 | dump confirms Angelscript.PioneerGameState.GamePhase = 0x9D0 (Offsets.h.GameState_GamePhase agrees) |
| AGREE | ArcOffsets::GameState::GAME_STATE_GLOBAL_RVA | 0xDCA7C88 | - | - | GameStateGlobalRva | 0xDCA7C88 | shipped as Offsets.h.GameStateGlobalRva (no contrary source) |
| AGREE | ArcOffsets::GameState::PICKUP_COUNT | 0xA04 | Angelscript.PioneerGameState.PickupCount | 0xA04 | GameState_PickupCount | 0xA04 | dump confirms Angelscript.PioneerGameState.PickupCount = 0xA04 (Offsets.h.GameState_PickupCount agrees) |
| AGREE | ArcOffsets::GameState::PLAYER_ARRAY | 0x498 | Engine.GameState.PlayerArray | 0x498 | GameState_PlayerArray | 0x498 | dump confirms Engine.GameState.PlayerArray = 0x498 (Offsets.h.GameState_PlayerArray agrees) |
| AGREE | ArcOffsets::GameState::STAGE_INFO | 0x938 | Angelscript.PioneerGameState.StageInfo (all 2 classes here) | 0x938 | GameState_StageInfoRef | 0x938 | dump confirms Angelscript.PioneerGameState.StageInfo (all 2 classes here) = 0x938 (Offsets.h.GameState_StageInfoRef agrees) |
| AGREE | ArcOffsets::GameViewportClient::WORLD | 0x1C8 | Engine.GameViewportClient.World | 0x1C8 | GameViewportClient_World | 0x1C8 | dump confirms Engine.GameViewportClient.World = 0x1C8 (Offsets.h.GameViewportClient_World agrees) |
| AGREE | ArcOffsets::GetObjectIdCrypto::SIMD_MASK_RVA | 0xAD2FC50 | - | - | PlayerNameSimdMaskRva | 0xAD2FC50 | shipped as Offsets.h.PlayerNameSimdMaskRva and GetObjectIdSimdMaskRva, PlayerNameSimdMaskRva (no contrary source) |
| AGREE | ArcOffsets::HealthComponent::MAX_DBNO_HEALTH | 0x380 | Angelscript.HealthComponent.MaxDBNOHealth | 0x380 | HC_MaxDBNOHealth | 0x380 | dump confirms Angelscript.HealthComponent.MaxDBNOHealth = 0x380 (Offsets.h.HC_MaxDBNOHealth agrees) |
| AGREE | ArcOffsets::HealthService::PART_HP_ARRAY | 0x280 | - | - | PartHpArray | 0x280 | shipped as Offsets.h.PartHpArray and PartHpArray (no contrary source) |
| AGREE | ArcOffsets::InventoryComponent::BACKPACK | 0x4A0 | Angelscript.InventoryComponent.Backpack | 0x4A0 | Inventory_Backpack | 0x4A0 | dump confirms Angelscript.InventoryComponent.Backpack = 0x4A0 (Offsets.h.Inventory_Backpack agrees) |
| AGREE | ArcOffsets::InventoryComponent::BELT | 0x4A8 | Angelscript.InventoryComponent.Belt | 0x4A8 | Inventory_Belt | 0x4A8 | dump confirms Angelscript.InventoryComponent.Belt = 0x4A8 (Offsets.h.Inventory_Belt agrees) |
| AGREE | ArcOffsets::InventoryComponent::CURRENT_ITEM_ACTORS | 0x520 | Angelscript.InventoryComponent.CurrentItemActors | 0x520 | CurrentItemActors | 0x520 | dump confirms Angelscript.InventoryComponent.CurrentItemActors = 0x520 (Offsets.h.CurrentItemActors agrees) |
| AGREE | ArcOffsets::InventoryComponent::EQUIPPED_ARMOR | 0x588 | Angelscript.InventoryComponent.EquippedArmor | 0x588 | EquippedArmor | 0x588 | dump confirms Angelscript.InventoryComponent.EquippedArmor = 0x588 (Offsets.h.EquippedArmor agrees) |
| AGREE | ArcOffsets::InventoryComponent::LOADOUT | 0x2C0 | Angelscript.InventoryComponent.Loadout | 0x2C0 | Inventory_Loadout | 0x2C0 | dump confirms Angelscript.InventoryComponent.Loadout = 0x2C0 (Offsets.h.Inventory_Loadout agrees) |
| AGREE | ArcOffsets::InventoryComponent::LOCAL_CURRENT_ITEM_ACTORS | 0x540 | Angelscript.InventoryComponent.LocalCurrentItemActors | 0x540 | LocalCurrentItemActors | 0x540 | dump confirms Angelscript.InventoryComponent.LocalCurrentItemActors = 0x540 (Offsets.h.LocalCurrentItemActors agrees) |
| AGREE | ArcOffsets::InventoryComponent::SAFE_POUCH | 0x4B0 | Angelscript.InventoryComponent.SafePouch | 0x4B0 | Inventory_SafePouch | 0x4B0 | dump confirms Angelscript.InventoryComponent.SafePouch = 0x4B0 (Offsets.h.Inventory_SafePouch agrees) |
| AGREE | ArcOffsets::InventoryComponent::STOWED_TOOL_ACTOR | 0x420 | Angelscript.InventoryComponent.StowedToolActor | 0x420 | Inventory_StowedToolActor | 0x420 | dump confirms Angelscript.InventoryComponent.StowedToolActor = 0x420 (Offsets.h.Inventory_StowedToolActor agrees) |
| AGREE | ArcOffsets::InventoryComponent::STOWED_WEAPON_0 | 0x3A0 | Angelscript.InventoryComponent.StowedWeapon0 | 0x3A0 | StowedWeaponSlot0 | 0x3A0 | dump confirms Angelscript.InventoryComponent.StowedWeapon0 = 0x3A0 (Offsets.h.StowedWeaponSlot0 agrees) |
| AGREE | ArcOffsets::InventoryComponent::STOWED_WEAPON_1 | 0x3E0 | Angelscript.InventoryComponent.StowedWeapon1 | 0x3E0 | StowedWeaponSlot1 | 0x3E0 | dump confirms Angelscript.InventoryComponent.StowedWeapon1 = 0x3E0 (Offsets.h.StowedWeaponSlot1 agrees) |
| AGREE | ArcOffsets::ItemBase::QUALITY_LEVEL | 0x104 | Angelscript.ItemBase.QualityLevel | 0x104 | ItemBase_Quality | 0x104 | dump confirms Angelscript.ItemBase.QualityLevel = 0x104 (Offsets.h.ItemBase_Quality agrees) |
| AGREE | ArcOffsets::ItemUIHoverData::AMOUNT | 0x18 | - | - | ItemUIHoverData_Amount | 0x18 | shipped as Offsets.h.ItemUIHoverData_Amount (no contrary source) |
| AGREE | ArcOffsets::ItemUIHoverData::DATA_ASSET | 0x20 | - | - | ItemUIHoverData_DataAsset | 0x20 | shipped as Offsets.h.ItemUIHoverData_DataAsset (no contrary source) |
| AGREE | ArcOffsets::ItemUIHoverData::DISPLAY_NAME | 0x0 | - | - | ItemUIHoverData_DisplayName | 0x0 | shipped as Offsets.h.ItemUIHoverData_DisplayName (no contrary source) |
| AGREE | ArcOffsets::ItemUIHoverData::MAX_STACK | 0x1C | - | - | ItemUIHoverData_MaxStack | 0x1C | shipped as Offsets.h.ItemUIHoverData_MaxStack (no contrary source) |
| AGREE | ArcOffsets::Level::ACTORS | 0x110 | - | - | AActors | 0x110 | shipped as Offsets.h.AActors (no contrary source) |
| AGREE | ArcOffsets::Level::ACTOR_CLUSTER | 0x158 | Engine.Level.ActorCluster | 0x158 | ActorCluster | 0x158 | dump confirms Engine.Level.ActorCluster = 0x158 (Offsets.h.ActorCluster agrees) |
| AGREE | ArcOffsets::Level::OWNING_WORLD | 0x138 | Engine.Level.OwningWorld | 0x138 | Level_OwningWorld | 0x138 | dump confirms Engine.Level.OwningWorld = 0x138 (Offsets.h.Level_OwningWorld agrees) |
| AGREE | ArcOffsets::LevelActorContainer::ACTORS | 0x98 | Engine.LevelActorContainer.Actors | 0x98 | LevelActorContainer_Actors | 0x98 | dump confirms Engine.LevelActorContainer.Actors = 0x98 (Offsets.h.LevelActorContainer_Actors agrees) |
| AGREE | ArcOffsets::LevelActorContainer::ACTOR_COUNT | 0xA0 | Engine.LevelActorContainer.Actors + 0x8 (array count) | 0xA0 | LevelActorContainer_ActorCount | 0xA0 | dump confirms Engine.LevelActorContainer.Actors + 0x8 (array count) = 0xA0 (Offsets.h.LevelActorContainer_ActorCount agrees) |
| AGREE | ArcOffsets::LevelCollection::GAME_STATE | 0x8 | Engine.LevelCollection.GameState | 0x8 | LevelCollection_GameState | 0x8 | dump confirms Engine.LevelCollection.GameState = 0x8 (Offsets.h.LevelCollection_GameState agrees) |
| AGREE | ArcOffsets::LevelCollection::PERSISTENT_LEVEL | 0x20 | Engine.LevelCollection.PersistentLevel | 0x20 | LevelCollection_PersistentLevel | 0x20 | dump confirms Engine.LevelCollection.PersistentLevel = 0x20 (Offsets.h.LevelCollection_PersistentLevel agrees) |
| AGREE | ArcOffsets::LevelStreaming::LT_ROTATION | 0x0 | - | - | Transform_Rotation | 0x0 | shipped as Offsets.h.Transform_Rotation (no contrary source) |
| AGREE | ArcOffsets::LevelStreaming::LT_SCALE3D | 0x40 | - | - | Transform_Scale3D | 0x40 | shipped as Offsets.h.Transform_Scale3D (no contrary source) |
| AGREE | ArcOffsets::LevelStreaming::LT_TRANSLATION | 0x20 | - | - | Transform_Translation | 0x20 | shipped as Offsets.h.Transform_Translation (no contrary source) |
| AGREE | ArcOffsets::LocalPlayer::CONTROLLER_ID | 0x270 | Engine.LocalPlayer.ControllerId | 0x270 | LocalPlayer_ControllerId | 0x270 | dump confirms Engine.LocalPlayer.ControllerId = 0x270 (Offsets.h.LocalPlayer_ControllerId agrees) |
| AGREE | ArcOffsets::LocalPlayer::PLAYER_CONTROLLER | 0xA0 | - | - | LocalPlayer_PlayerController | 0xA0 | shipped as Offsets.h.LocalPlayer_PlayerController (no contrary source) |
| AGREE | ArcOffsets::LootContainerSingle::LOOT_INTERACTION_COMPONENT | 0xBB8 | Angelscript.LootContainerSingle.LootInteractionComponent | 0xBB8 | LootInteractionComponent | 0xBB8 | dump confirms Angelscript.LootContainerSingle.LootInteractionComponent = 0xBB8 (Offsets.h.LootInteractionComponent agrees) |
| AGREE | ArcOffsets::LootContainerSingle::SOCKET_LOOT_CONTAINER_MESH | 0xBB0 | Angelscript.LootContainerSingle.SocketLootContainerMesh | 0xBB0 | LootContainer_SocketMesh | 0xBB0 | dump confirms Angelscript.LootContainerSingle.SocketLootContainerMesh = 0xBB0 (Offsets.h.LootContainer_SocketMesh agrees) |
| AGREE | ArcOffsets::LootInteractionComponent::DISPENSER_LOCATIONS | 0x810 | Angelscript.LootInteractionComponent.DispenserLocations | 0x810 | LootInteract_DispenserLocations | 0x810 | dump confirms Angelscript.LootInteractionComponent.DispenserLocations = 0x810 (Offsets.h.LootInteract_DispenserLocations agrees) |
| AGREE | ArcOffsets::LootInteractionComponent::LOOT_ACQUISITION_METHOD | 0x80B | Angelscript.LootInteractionComponent.LootAcquisitionMethod | 0x80B | LootInteract_AcquisitionMethod | 0x80B | dump confirms Angelscript.LootInteractionComponent.LootAcquisitionMethod = 0x80B (Offsets.h.LootInteract_AcquisitionMethod agrees) |
| AGREE | ArcOffsets::LootInteractionComponent::LOOT_PING_ICON_OFFSET | 0x860 | Angelscript.LootInteractionComponent.LootPingIconOffset | 0x860 | LootInteract_PingIconOffset | 0x860 | dump confirms Angelscript.LootInteractionComponent.LootPingIconOffset = 0x860 (Offsets.h.LootInteract_PingIconOffset agrees) |
| AGREE | ArcOffsets::LootInteractionComponent::MASK_HAS_BEEN_OPENED | 0x1 | - | - | LootInteract_OpenedMask | 0x1 | shipped as Offsets.h.LootInteract_OpenedMask (no contrary source) |
| AGREE | ArcOffsets::MapWidgetLevelSettings::HAS_UNDERGROUND | 0x70 | - | - | MapLevel_HasUnderground | 0x70 | shipped as Offsets.h.MapLevel_HasUnderground and MapLevel_HasUnderground (no contrary source) |
| AGREE | ArcOffsets::MapWidgetLevelSettings::STRIDE | 0x78 | - | - | LevelCollection_Stride | 0x78 | shipped as Offsets.h.LevelCollection_Stride and MapLevel_Stride, MapLevel_Stride (no contrary source) |
| AGREE | ArcOffsets::MapWidgetLevelSettings::WORLD_POSITION | 0x60 | Angelscript.MapWidgetLevelSettings.WorldPosition | 0x60 | MapLevel_WorldPosition | 0x60 | dump confirms Angelscript.MapWidgetLevelSettings.WorldPosition = 0x60 (Offsets.h.MapLevel_WorldPosition agrees) |
| AGREE | ArcOffsets::MapWidgetLevelSettings::WORLD_SIZE | 0x50 | Angelscript.MapWidgetLevelSettings.WorldSize | 0x50 | MapLevel_WorldSize | 0x50 | dump confirms Angelscript.MapWidgetLevelSettings.WorldSize = 0x50 (Offsets.h.MapLevel_WorldSize agrees) |
| AGREE | ArcOffsets::ObjectXorKey::RVA | 0xD91F885 | - | - | ObjectXorKeyRva | 0xD91F885 | shipped as Offsets.h.ObjectXorKeyRva (no contrary source) |
| AGREE | ArcOffsets::Pawn::CONTROLLER | 0x3F0 | Engine.Pawn.Controller | 0x3F0 | Pawn_Controller | 0x3F0 | dump confirms Engine.Pawn.Controller = 0x3F0 (Offsets.h.Pawn_Controller agrees) |
| AGREE | ArcOffsets::Pawn::PLAYER_STATE | 0x3E0 | Engine.Pawn.PlayerState | 0x3E0 | APlayerState | 0x3E0 | dump confirms Engine.Pawn.PlayerState = 0x3E0 (Offsets.h.APlayerState agrees) |
| AGREE | ArcOffsets::Pickup::DEFAULT_PICKUP_DATA_ASSET | 0x4A8 | Angelscript.Pickup.DefaultPickupDataAsset | 0x4A8 | Pickup_DefaultPickupDataAsset | 0x4A8 | dump confirms Angelscript.Pickup.DefaultPickupDataAsset = 0x4A8 (Offsets.h.Pickup_DefaultPickupDataAsset agrees) |
| AGREE | ArcOffsets::Pickup::INTERACTION | 0x498 | Angelscript.Pickup.Interaction | 0x498 | Pickup_Interaction | 0x498 | dump confirms Angelscript.Pickup.Interaction = 0x498 (Offsets.h.Pickup_Interaction agrees) |
| AGREE | ArcOffsets::Pickup::ROOT_COLLIDER | 0x480 | Angelscript.Pickup.RootCollider | 0x480 | Pickup_RootCollider | 0x480 | dump confirms Angelscript.Pickup.RootCollider = 0x480 (Offsets.h.Pickup_RootCollider agrees) |
| AGREE | ArcOffsets::Pickup::SPAWN_ITEMS | 0x560 | Angelscript.Pickup.SpawnItems | 0x560 | BP_PickupBase_SpawnItems | 0x560 | dump confirms Angelscript.Pickup.SpawnItems = 0x560 (Offsets.h.BP_PickupBase_SpawnItems agrees) |
| AGREE | ArcOffsets::Pickup::VISIBLE_AMOUNT | 0x4D0 | - | - | Pickup_VisibleAmount | 0x4D0 | shipped as Offsets.h.Pickup_VisibleAmount (no contrary source) |
| AGREE | ArcOffsets::PickupDataAsset::RESOLVED_ITEM_CLASS | 0x150 | - | - | PickupDataAsset_ResolvedItemClass | 0x150 | shipped as Offsets.h.PickupDataAsset_ResolvedItemClass and PickupDataAsset_ResolvedItemClass (no contrary source) |
| AGREE | ArcOffsets::PioneerConstructablePawn::AI_STATE_SERVICE | 0x12C0 | - | - | Constructable_AIStateService | 0x12C0 | shipped as Offsets.h.Constructable_AIStateService and Constructable_AIStateService (no contrary source) |
| AGREE | ArcOffsets::PioneerConstructablePawn::AI_TEMPLATE_DATA | 0x11B0 | Angelscript.PioneerConstructablePawn.AITemplateData | 0x11B0 | Constructable_AITemplateData | 0x11B0 | dump confirms Angelscript.PioneerConstructablePawn.AITemplateData = 0x11B0 (Offsets.h.Constructable_AITemplateData agrees) |
| AGREE | ArcOffsets::PioneerConstructablePawn::ENEMY_TYPE_DATA_ASSET | 0x11C0 | Angelscript.PioneerConstructablePawn.EnemyTypeDataAsset | 0x11C0 | Constructable_EnemyTypeDataAsset | 0x11C0 | dump confirms Angelscript.PioneerConstructablePawn.EnemyTypeDataAsset = 0x11C0 (Offsets.h.Constructable_EnemyTypeDataAsset agrees) |
| AGREE | ArcOffsets::PioneerConstructablePawn::HEALTH_SERVICE | 0x1280 | - | - | Constructable_HealthService | 0x1280 | shipped as Offsets.h.Constructable_HealthService and Constructable_HealthService (no contrary source) |
| AGREE | ArcOffsets::PioneerConstructablePawn::bIsDestroyed | 0x1230 | Angelscript.PioneerConstructablePawn.bIsDestroyed | 0x1230 | Constructable_bIsDestroyed | 0x1230 | dump confirms Angelscript.PioneerConstructablePawn.bIsDestroyed = 0x1230 (Offsets.h.Constructable_bIsDestroyed agrees) |
| AGREE | ArcOffsets::PioneerPlayerCharacter::HEALTH_COMPONENT | 0xDC0 | Angelscript.PioneerPlayerCharacter.HealthComponent | 0xDC0 | HealthComponent | 0xDC0 | dump confirms Angelscript.PioneerPlayerCharacter.HealthComponent = 0xDC0 (Offsets.h.HealthComponent agrees) |
| AGREE | ArcOffsets::PioneerPlayerCharacter::INVENTORY_COMPONENT | 0xC80 | Angelscript.PioneerPlayerCharacter.InventoryComponent | 0xC80 | InventoryComponent | 0xC80 | dump confirms Angelscript.PioneerPlayerCharacter.InventoryComponent = 0xC80 (Offsets.h.InventoryComponent agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::DEFAULT_FOV | 0x430 | Engine.PlayerCameraManager.DefaultFOV | 0x430 | DefaultFOV | 0x430 | dump confirms Engine.PlayerCameraManager.DefaultFOV = 0x430 (Offsets.h.DefaultFOV agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::LOCKED_FOV | 0x43C | - | - | LockedFOV | 0x43C | shipped as Offsets.h.LockedFOV (no contrary source) |
| AGREE | ArcOffsets::PlayerCameraManager::PC_OWNER | 0x420 | Engine.PlayerCameraManager.PCOwner | 0x420 | PCOwner | 0x420 | dump confirms Engine.PlayerCameraManager.PCOwner = 0x420 (Offsets.h.PCOwner agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::PENDING_VIEW_TARGET | 0xE10 | Engine.PlayerCameraManager.PendingViewTarget | 0xE10 | PendingViewTarget | 0xE10 | dump confirms Engine.PlayerCameraManager.PendingViewTarget = 0xE10 (Offsets.h.PendingViewTarget agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::VIEW_PITCH_MAX | 0x2090 | Engine.PlayerCameraManager.ViewPitchMax | 0x2090 | PCM_ViewPitchMax | 0x2090 | dump confirms Engine.PlayerCameraManager.ViewPitchMax = 0x2090 (Offsets.h.PCM_ViewPitchMax agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::VIEW_PITCH_MIN | 0x2098 | Engine.PlayerCameraManager.ViewPitchMin | 0x2098 | PCM_ViewPitchMin | 0x2098 | dump confirms Engine.PlayerCameraManager.ViewPitchMin = 0x2098 (Offsets.h.PCM_ViewPitchMin agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::VIEW_TARGET | 0x460 | Engine.PlayerCameraManager.ViewTarget | 0x460 | ViewTarget | 0x460 | dump confirms Engine.PlayerCameraManager.ViewTarget = 0x460 (Offsets.h.ViewTarget agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::VIEW_YAW_MAX | 0x20A0 | Engine.PlayerCameraManager.ViewYawMax | 0x20A0 | PCM_ViewYawMax | 0x20A0 | dump confirms Engine.PlayerCameraManager.ViewYawMax = 0x20A0 (Offsets.h.PCM_ViewYawMax agrees) |
| AGREE | ArcOffsets::PlayerCameraManager::VIEW_YAW_MIN | 0x2094 | Engine.PlayerCameraManager.ViewYawMin | 0x2094 | PCM_ViewYawMin | 0x2094 | dump confirms Engine.PlayerCameraManager.ViewYawMin = 0x2094 (Offsets.h.PCM_ViewYawMin agrees) |
| AGREE | ArcOffsets::PlayerHealthInfo::B_HAS_BROKEN_ARMOR | 0x20 | - | - | PHI_BrokenArmorByte | 0x20 | shipped as Offsets.h.PHI_BrokenArmorByte (no contrary source) |
| AGREE | ArcOffsets::PlayerHealthInfo::B_IS_DBNO | 0x21 | - | - | PHI_DBNOByte | 0x21 | shipped as Offsets.h.PHI_DBNOByte and PHI_DBNOByte (no contrary source) |
| AGREE | ArcOffsets::PlayerHealthInfo::MAX_HEALTH | 0x8 | - | - | PHI_MaxHealth | 0x8 | shipped as Offsets.h.PHI_MaxHealth and PHI_MaxHealth (no contrary source) |
| AGREE | ArcOffsets::PlayerState::B_IS_A_BOT_MASK | 0x8 | Engine.PlayerState.bIsABot mask | 0x8 | PS_BotStateBotMask | 0x8 | dump confirms Engine.PlayerState.bIsABot mask = 0x8 (Offsets.h.PS_BotStateBotMask agrees) |
| AGREE | ArcOffsets::PlayerState::B_IS_SPECTATOR_MASK | 0x2 | Engine.PlayerState.bIsSpectator mask | 0x2 | PS_BotStateSpectatorMask | 0x2 | dump confirms Engine.PlayerState.bIsSpectator mask = 0x2 (Offsets.h.PS_BotStateSpectatorMask agrees) |
| AGREE | ArcOffsets::PlayerState::HEALTH_INFO_BASE | 0x588 | - | - | PlayerHealthInfoBase | 0x588 | shipped as Offsets.h.PlayerHealthInfoBase (no contrary source) |
| AGREE | ArcOffsets::PlayerState::PAWN_PRIVATE | 0x438 | Engine.PlayerState.PawnPrivate | 0x438 | PlayerState_PawnPrivate | 0x438 | dump confirms Engine.PlayerState.PawnPrivate = 0x438 (Offsets.h.PlayerState_PawnPrivate agrees) |
| AGREE | ArcOffsets::PlayerState::PLAYER_NAME_PRIVATE | 0x458 | Engine.PlayerState.PlayerNamePrivate | 0x458 | PlayerNamePrivate | 0x458 | dump confirms Engine.PlayerState.PlayerNamePrivate = 0x458 (Offsets.h.PlayerNamePrivate agrees) |
| AGREE | ArcOffsets::PlayerState::PLAYER_NAME_PRIVATE_2 | 0x480 | - | - | PS_PlayerNamePrivate2 | 0x480 | shipped as Offsets.h.PS_PlayerNamePrivate2 and PS_PlayerNamePrivate2 (no contrary source) |
| AGREE | ArcOffsets::PrimitiveComponent::BOUNDS_SCALE | 0x468 | Engine.PrimitiveComponent.BoundsScale | 0x468 | BoundsScale | 0x468 | dump confirms Engine.PrimitiveComponent.BoundsScale = 0x468 (Offsets.h.BoundsScale agrees) |
| AGREE | ArcOffsets::PrimitiveComponent::WORLD_PRIVATE | 0x140 | - | - | UActorComponent_WorldPrivate | 0x140 | shipped as Offsets.h.UActorComponent_WorldPrivate and UActorComponent_WorldPrivate (no contrary source) |
| AGREE | ArcOffsets::SceneComponent::ATTACH_CHILDREN | 0x1F8 | Engine.SceneComponent.AttachChildren | 0x1F8 | AttachChildren | 0x1F8 | dump confirms Engine.SceneComponent.AttachChildren = 0x1F8 (Offsets.h.AttachChildren agrees) |
| AGREE | ArcOffsets::SceneComponent::B_HIDDEN_IN_GAME_BYTE | 0x2C9 | - | - | Scene_bHiddenInGameByte | 0x2C9 | shipped as Offsets.h.Scene_bHiddenInGameByte (no contrary source) |
| AGREE | ArcOffsets::SceneComponent::B_HIDDEN_IN_GAME_MASK | 0x10 | Engine.SceneComponent.bHiddenInGame mask | 0x10 | Scene_bHiddenInGameMask | 0x10 | dump confirms Engine.SceneComponent.bHiddenInGame mask = 0x10 (Offsets.h.Scene_bHiddenInGameMask agrees) |
| AGREE | ArcOffsets::SceneComponent::B_VISIBLE_BYTE | 0x2C8 | - | - | Scene_bVisibleByte | 0x2C8 | shipped as Offsets.h.Scene_bVisibleByte (no contrary source) |
| AGREE | ArcOffsets::SceneComponent::B_VISIBLE_MASK | 0x20 | Engine.SceneComponent.bVisible mask | 0x20 | Scene_bVisibleMask | 0x20 | dump confirms Engine.SceneComponent.bVisible mask = 0x20 (Offsets.h.Scene_bVisibleMask agrees) |
| AGREE | ArcOffsets::SceneComponent::COMPONENT_TO_WORLD | 0x310 | - | - | ComponentToWorld | 0x310 | shipped as Offsets.h.ComponentToWorld and ComponentToWorld_Alt, ComponentToWorld_Rotation (no contrary source) |
| AGREE | ArcOffsets::SceneComponent::COMPONENT_VELOCITY | 0x2B0 | Engine.SceneComponent.ComponentVelocity | 0x2B0 | ComponentVelocity | 0x2B0 | dump confirms Engine.SceneComponent.ComponentVelocity = 0x2B0 (Offsets.h.ComponentVelocity agrees) |
| AGREE | ArcOffsets::SceneComponent::RELATIVE_ROTATION | 0x280 | Engine.SceneComponent.RelativeRotation | 0x280 | RelativeRotation | 0x280 | dump confirms Engine.SceneComponent.RelativeRotation = 0x280 (Offsets.h.RelativeRotation agrees) |
| AGREE | ArcOffsets::SkeletalMeshComponent::B_FORCE_REFPOSE | 0xCC1 | Engine.SkeletalMeshComponent.bForceRefpose | 0xCC1 | bForceRefpose | 0xCC1 | dump confirms Engine.SkeletalMeshComponent.bForceRefpose = 0xCC1 (Offsets.h.bForceRefpose agrees) |
| AGREE | ArcOffsets::SkeletalMeshComponent::B_FORCE_REFPOSE_MASK | 0x1 | Engine.SkeletalMeshComponent.bForceRefpose mask | 0x1 | bForceRefposeMask | 0x1 | dump confirms Engine.SkeletalMeshComponent.bForceRefpose mask = 0x1 (Offsets.h.bForceRefposeMask agrees) |
| AGREE | ArcOffsets::SkeletalMeshComponent::B_NO_SKELETON_UPDATE | 0xCC0 | Engine.SkeletalMeshComponent.bNoSkeletonUpdate | 0xCC0 | bNoSkeletonUpdate | 0xCC0 | dump confirms Engine.SkeletalMeshComponent.bNoSkeletonUpdate = 0xCC0 (Offsets.h.bNoSkeletonUpdate agrees) |
| AGREE | ArcOffsets::SkeletalMeshComponent::B_NO_SKELETON_UPDATE_MASK | 0x8 | Engine.SkeletalMeshComponent.bNoSkeletonUpdate mask | 0x8 | bNoSkeletonUpdateMask | 0x8 | dump confirms Engine.SkeletalMeshComponent.bNoSkeletonUpdate mask = 0x8 (Offsets.h.bNoSkeletonUpdateMask agrees) |
| AGREE | ArcOffsets::SkeletalMeshComponent::SKELETAL_MESH | 0x740 | - | - | SkeletalMeshAsset_Alt | 0x740 | shipped as Offsets.h.SkeletalMeshAsset_Alt (no contrary source) |
| AGREE | ArcOffsets::SkeletalMeshComponent::VISIBILITY_BASED_ANIM_TICK_OPTION | 0x9EC | Engine.SkeletalMeshComponent.VisibilityBasedAnimTickOption | 0x9EC | VisibilityBasedAnimTickOption | 0x9EC | dump confirms Engine.SkeletalMeshComponent.VisibilityBasedAnimTickOption = 0x9EC (Offsets.h.VisibilityBasedAnimTickOption agrees) |
| AGREE | ArcOffsets::StageInfo::GRACE_TIME | 0x20 | Angelscript.StageInfo.GraceTime | 0x20 | StageInfo_GraceTimeOff | 0x20 | dump confirms Angelscript.StageInfo.GraceTime = 0x20 (Offsets.h.StageInfo_GraceTimeOff agrees) |
| AGREE | ArcOffsets::StageInfo::TIME_LEFT | 0x18 | Angelscript.StageInfo.TimeLeft | 0x18 | StageInfo_TimeLeftOff | 0x18 | dump confirms Angelscript.StageInfo.TimeLeft = 0x18 (Offsets.h.StageInfo_TimeLeftOff agrees) |
| AGREE | ArcOffsets::StowedWeaponLayout::ITEM_DATA_ASSET | 0x18 | - | - | StowedInfo_ItemDataAsset | 0x18 | shipped as Offsets.h.StowedInfo_ItemDataAsset and StowedInfo_ItemDataAsset (no contrary source) |
| AGREE | ArcOffsets::StowedWeaponLayout::STRUCT_SIZE | 0x40 | - | - | StowedInfo_Size | 0x40 | shipped as Offsets.h.StowedInfo_Size (no contrary source) |
| AGREE | ArcOffsets::StowedWeaponLayout::WEAPON_QUALITY | 0x38 | - | - | StowedInfo_Quality | 0x38 | shipped as Offsets.h.StowedInfo_Quality (no contrary source) |
| AGREE | ArcOffsets::UStruct::SUPER_STRUCT | 0xB0 | CoreUObject.Struct.SuperStruct | 0xB0 | UStruct_SuperStruct | 0xB0 | dump confirms CoreUObject.Struct.SuperStruct = 0xB0 (Offsets.h.UStruct_SuperStruct agrees) |
| AGREE | ArcOffsets::UWorld::AUTHORITY_GAME_MODE | 0x310 | Engine.World.AuthorityGameMode | 0x310 | AuthorityGameMode | 0x310 | dump confirms Engine.World.AuthorityGameMode = 0x310 (Offsets.h.AuthorityGameMode agrees) |
| AGREE | ArcOffsets::UWorld::GAME_INSTANCE | 0x478 | - | - | OwningGameInstance | 0x478 | shipped as Offsets.h.OwningGameInstance and OwningGameInstance (no contrary source) |
| AGREE | ArcOffsets::UWorld::LEVELS | 0x2F0 | Engine.World.Levels | 0x2F0 | Levels | 0x2F0 | dump confirms Engine.World.Levels = 0x2F0 (Offsets.h.Levels agrees) |
| AGREE | ArcOffsets::UWorld::LEVEL_COLLECTIONS | 0x338 | Engine.World.LevelCollections | 0x338 | LevelCollections | 0x338 | dump confirms Engine.World.LevelCollections = 0x338 (Offsets.h.LevelCollections agrees) |
| AGREE | ArcOffsets::UWorld::PERSISTENT_LEVEL | 0x110 | Engine.World.PersistentLevel | 0x110 | PersistentLevel | 0x110 | dump confirms Engine.World.PersistentLevel = 0x110 (Offsets.h.PersistentLevel agrees) |
| AGREE | ArcOffsets::UWorld::PHYSICS_FIELD | 0x508 | Engine.World.PhysicsField | 0x508 | PhysicsField | 0x508 | dump confirms Engine.World.PhysicsField = 0x508 (Offsets.h.PhysicsField agrees) |
| AGREE | ArcOffsets::UWorld::STREAMING_LEVELS | 0x170 | Engine.World.StreamingLevels | 0x170 | StreamingLevels | 0x170 | dump confirms Engine.World.StreamingLevels = 0x170 (Offsets.h.StreamingLevels agrees) |
| AGREE | ArcOffsets::UWorld::TIME_SECONDS | 0x240 | - | - | UWorld_TimeSeconds | 0x240 | shipped as Offsets.h.UWorld_TimeSeconds and UWorld_TimeSeconds (no contrary source) |
| AGREE | ArcOffsets::UniqueNetId::PAYLOAD | 0x18 | - | - | NetId_Payload | 0x18 | shipped as Offsets.h.NetId_Payload and NetId_Payload (no contrary source) |
| AGREE | ArcOffsets::UniqueNetIdRepl::NET_ID_OBJECT | 0x8 | - | - | NetIdRepl_Object | 0x8 | shipped as Offsets.h.NetIdRepl_Object and NetIdRepl_Object (no contrary source) |
| AGREE | ArcOffsets::UniqueNetIdRepl::REPL_BYTES_COUNT | 0x28 | - | - | NetIdRepl_ReplBytesCount | 0x28 | shipped as Offsets.h.NetIdRepl_ReplBytesCount and NetIdRepl_ReplBytesCount (no contrary source) |
| AGREE | ArcOffsets::UniqueNetIdRepl::REPL_BYTES_DATA | 0x20 | - | - | NetIdRepl_ReplBytesData | 0x20 | shipped as Offsets.h.NetIdRepl_ReplBytesData and NetIdRepl_ReplBytesData (no contrary source) |
| AGREE | ArcOffsets::UniqueNetIdRepl::TYPE_INDEX | 0x18 | - | - | NetIdRepl_TypeIndex | 0x18 | shipped as Offsets.h.NetIdRepl_TypeIndex and NetIdRepl_TypeIndex (no contrary source) |
| AGREE | ArcOffsets::WeaponActor::CLIP_SIZE | 0x490 | Angelscript.WeaponActor.ClipSize | 0x490 | WeaponActor_ClipSize | 0x490 | dump confirms Angelscript.WeaponActor.ClipSize = 0x490 (Offsets.h.WeaponActor_ClipSize agrees) |
| AGREE | ArcOffsets::WeaponActor::WEAPON_QUALITY | 0x492 | Angelscript.WeaponActor.WeaponQuality | 0x492 | WeaponQuality | 0x492 | dump confirms Angelscript.WeaponActor.WeaponQuality = 0x492 (Offsets.h.WeaponQuality agrees) |
| AGREE | ArcOffsets::WineTeb::SELF_PTR | 0x30 | - | - | TebSelfPtrOff | 0x30 | shipped as Offsets.h.TebSelfPtrOff (no contrary source) |
| AGREE | ArcOffsets::WorldPartitionMiniMap::MINIMAP_TILE_SIZE | 0x488 | Engine.WorldPartitionMiniMap.MiniMapTileSize | 0x488 | MiniMap_TileSize | 0x488 | dump confirms Engine.WorldPartitionMiniMap.MiniMapTileSize = 0x488 (Offsets.h.MiniMap_TileSize agrees) |
| AGREE | ArcOffsets::WorldPartitionMiniMap::MINIMAP_WORLD_BOUNDS | 0x3C0 | Engine.WorldPartitionMiniMap.MiniMapWorldBounds | 0x3C0 | MiniMap_WorldBounds | 0x3C0 | dump confirms Engine.WorldPartitionMiniMap.MiniMapWorldBounds = 0x3C0 (Offsets.h.MiniMap_WorldBounds agrees) |
| AGREE | ArcOffsets::WorldPartitionMiniMap::WORLD_UNITS_PER_PIXEL | 0x478 | Engine.WorldPartitionMiniMap.WorldUnitsPerPixel | 0x478 | MiniMap_UnitsPerPixel | 0x478 | dump confirms Engine.WorldPartitionMiniMap.WorldUnitsPerPixel = 0x478 (Offsets.h.MiniMap_UnitsPerPixel agrees) |
| AGREE | FName::BLOCK_ROL64 | 0x13 | - | - | FNameBlockRol64 | 0x13 | shipped as Offsets.h.FNameBlockRol64 and FNameBlockRol64 (no contrary source) |
| AGREE | FName::BLOCK_XOR | 0xF401C0BE961D3D9A | - | - | FNameBlockXor | 0xF401C0BE961D3D9A | shipped as Offsets.h.FNameBlockXor and FNameBlockXor (no contrary source) |
| AGREE | FName::CHUNKMGR_ROL32 | 0x3 | - | - | ChunksManagerRol32 | 0x3 | shipped as Offsets.h.ChunksManagerRol32 and ChunksManagerRol32 (no contrary source) |
| AGREE | FName::FNV_ADD | 0x6C5FD4827126D389 | - | - | FNameFnvAdd | 0x6C5FD4827126D389 | shipped as Offsets.h.FNameFnvAdd and FNameFnvAdd (no contrary source) |
| AGREE | FName::FNV_ROL1 | 0x28 | - | - | FNameFnvRol1 | 0x28 | shipped as Offsets.h.FNameFnvRol1 and FNameFnvRol1 (no contrary source) |
| AGREE | FName::FNV_ROL2 | 0x32 | - | - | FNameFnvRol2 | 0x32 | shipped as Offsets.h.FNameFnvRol2 and FNameFnvRol2 (no contrary source) |
| AGREE | FName::FUOBJECTITEM_OBJ_OFF | 0x8 | - | - | GObjects_Item_Object | 0x8 | shipped as Offsets.h.GObjects_Item_Object and GObjects_Item_Object (no contrary source) |
| AGREE | FName::FUOBJECTITEM_STRIDE | 0x18 | - | - | GObjects_ItemStride | 0x18 | shipped as Offsets.h.GObjects_ItemStride and GObjects_ItemStride (no contrary source) |
| AGREE | FName::HDR_IS_WIDE_BIT | 0x1 | - | - | FNameEntry_WideBit | 0x1 | shipped as Offsets.h.FNameEntry_WideBit and FNameEntry_WideBit (no contrary source) |
| AGREE | FName::ITEMS_PER_CHUNK | 0x10000 | - | - | GObjects_ElementsPerChunk | 0x10000 | shipped as Offsets.h.GObjects_ElementsPerChunk and GObjects_ElementsPerChunk (no contrary source) |
| AGREE | FName::KEYSTREAM_BASE_IDX | 0xC | - | - | FNameKeystreamBaseIdx | 0xC | shipped as Offsets.h.FNameKeystreamBaseIdx and FNameKeystreamBaseIdx (no contrary source) |
| AGREE | FName::KEYSTREAM_COUNT | 0x100 | - | - | FNameKeystreamCount | 0x100 | shipped as Offsets.h.FNameKeystreamCount and FNameKeystreamCount (no contrary source) |
| AGREE | FName::MGR_CHUNKARRAY_OFF | 0x20 | - | - | ChunksManagerArrayOff | 0x20 | shipped as Offsets.h.ChunksManagerArrayOff and ChunksManagerArrayOff (no contrary source) |
| AGREE | FName::MGR_CHUNKARRAY_XOR | 0x464A80E000000000 | - | - | ChunksManagerArrayXor | 0x464A80E000000000 | shipped as Offsets.h.ChunksManagerArrayXor and ChunksManagerArrayXor (no contrary source) |
| AGREE | FName::MGR_NUMELEMENTS_OFF | 0x2C | - | - | ChunksManagerNumElementsOff | 0x2C | shipped as Offsets.h.ChunksManagerNumElementsOff and ChunksManagerNumElementsOff (no contrary source) |
| AGREE | FName::MGR_NUMELEMENTS_XOR | 0x68D83F2C | - | - | ChunksManagerNumElementsXor | 0x68D83F2C | shipped as Offsets.h.ChunksManagerNumElementsXor and ChunksManagerNumElementsXor (no contrary source) |
| AGREE | FName::NARROW_KEY_SHIFT | 0x3 | - | - | FNameNarrowKeyShift | 0x3 | shipped as Offsets.h.FNameNarrowKeyShift and FNameNarrowKeyShift (no contrary source) |
| AGREE | FName::RVA_CHUNKMGR_ADD | 0xD395270 | - | - | ChunksManagerAddRva | 0xD395270 | shipped as Offsets.h.ChunksManagerAddRva and ChunksManagerAddRva (no contrary source) |
| AGREE | FName::RVA_CHUNKMGR_GLOBAL | 0x10C57060 | - | - | ChunksManagerRva | 0x10C57060 | shipped as Offsets.h.ChunksManagerRva and ChunksManagerRva (no contrary source) |
| AGREE | FName::RVA_CHUNKMGR_XOR | 0xD395260 | - | - | ChunksManagerXorRva | 0xD395260 | shipped as Offsets.h.ChunksManagerXorRva and ChunksManagerXorRva (no contrary source) |
| AGREE | FName::SHARD_BLOCK_BASE_OFF | 0x50 | - | - | FNameShardBlockBase | 0x50 | shipped as Offsets.h.FNameShardBlockBase and FNameShardBlockBase (no contrary source) |
| AGREE | FName::SHARD_BLOCK_STRIDE | 0x20 | - | - | FNameShardBlockStride | 0x20 | shipped as Offsets.h.FNameShardBlockStride and FNameShardBlockStride (no contrary source) |
| AGREE | FName::SHARD_FINAL_SHR | 0x5 | - | - | FNameShardFinalShr | 0x5 | shipped as Offsets.h.FNameShardFinalShr and FNameShardFinalShr (no contrary source) |
| AGREE | FName::SHARD_HASH_ADD | 0xD69AD929 | - | - | FNameShardHashAdd | 0xD69AD929 | shipped as Offsets.h.FNameShardHashAdd and FNameShardHashAdd (no contrary source) |
| AGREE | FName::SHARD_HASH_SEED_OFF | 0x40 | - | - | FNameShardSeedOff | 0x40 | shipped as Offsets.h.FNameShardSeedOff and FNameShardSeedOff (no contrary source) |
| AGREE | FName::SHARD_ROL_A | 0x11 | - | - | FNameShardRolA | 0x11 | shipped as Offsets.h.FNameShardRolA and FNameShardRolA (no contrary source) |
| AGREE | FName::SHARD_ROL_B | 0x1B | - | - | FNameShardRolB | 0x1B | shipped as Offsets.h.FNameShardRolB and FNameShardRolB (no contrary source) |
| AGREE | FName::SLOT_BASE_OFF | 0x20 | - | - | FNameSlotBaseOff | 0x20 | shipped as Offsets.h.FNameSlotBaseOff (no contrary source) |
| AGREE | FName::SLOT_STRIDE | 0x20 | - | - | FNameSlotStride | 0x20 | shipped as Offsets.h.FNameSlotStride (no contrary source) |
| AGREE | FName::UOBJECT_INTERNAL_IDX | 0x90 | - | - | UObject_InternalIndex | 0x90 | shipped as Offsets.h.UObject_InternalIndex and UObject_InternalIndex (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::ADD64 | 0x2A79E93E092D2538 | - | - | GameInstanceStaticAdd | 0x2A79E93E092D2538 | shipped as Offsets.h.GameInstanceStaticAdd and GameInstanceStaticAdd (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::PSHUFB_MASK_RVA | 0xDDFC1D0 | - | - | GameInstanceStaticPshufbMaskRva | 0xDDFC1D0 | shipped as Offsets.h.GameInstanceStaticPshufbMaskRva and GameInstanceStaticPshufbMaskRva (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::RESULT_DEREF | 0x18 | - | - | GameInstanceStaticResultDeref | 0x18 | shipped as Offsets.h.GameInstanceStaticResultDeref and GameInstanceStaticResultDeref (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::ROT64_1 | 0x38 | - | - | GameInstanceStaticRot1 | 0x38 | shipped as Offsets.h.GameInstanceStaticRot1 and GameInstanceStaticRot1 (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::ROT64_2 | 0x21 | - | - | GameInstanceStaticRot2 | 0x21 | shipped as Offsets.h.GameInstanceStaticRot2 and GameInstanceStaticRot2 (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::STAGE_ARRAY_RVA | 0x10F35650 | - | - | GameInstanceStaticStageArrayRva | 0x10F35650 | shipped as Offsets.h.GameInstanceStaticStageArrayRva and GameInstanceStaticStageArrayRva (no contrary source) |
| AGREE | GameInstanceStaticDecrypt::XOR_MASK | 0x291AED004FAE1FAC | - | - | GameInstanceStaticXorMask | 0x291AED004FAE1FAC | shipped as Offsets.h.GameInstanceStaticXorMask and GameInstanceStaticXorMask (no contrary source) |
| AGREE | GameInstanceTlsDecrypt::HASH_PRIME | 0x1000193 | - | - | OuterDecrypt_HashPrime | 0x1000193 | shipped as Offsets.h.OuterDecrypt_HashPrime (no contrary source) |
| AGREE | OuterDecrypt::FINAL_ROT | 0x27 | - | - | OuterDecrypt_FinalRot | 0x27 | shipped as Offsets.h.OuterDecrypt_FinalRot (no contrary source) |
| AGREE | OuterDecrypt::HASH_ADD | 0x20193F54 | - | - | OuterDecrypt_HashAdd | 0x20193F54 | shipped as Offsets.h.OuterDecrypt_HashAdd and OuterDecrypt_HashAdd (no contrary source) |
| AGREE | OuterDecrypt::HASH_PRIME | 0x1000193 | - | - | OuterDecrypt_HashPrime | 0x1000193 | shipped as Offsets.h.OuterDecrypt_HashPrime (no contrary source) |
| AGREE | OuterDecrypt::HASH_ROT_1 | 0x19 | - | - | OuterDecrypt_HashRot1 | 0x19 | shipped as Offsets.h.OuterDecrypt_HashRot1 and OuterDecrypt_HashRot1 (no contrary source) |
| AGREE | OuterDecrypt::HASH_ROT_2 | 0xF | - | - | OuterDecrypt_HashRot2 | 0xF | shipped as Offsets.h.OuterDecrypt_HashRot2 and OuterDecrypt_HashRot2 (no contrary source) |
| AGREE | OuterDecrypt::LANE_ROT | 0xD | - | - | OuterDecrypt_LaneRot | 0xD | shipped as Offsets.h.OuterDecrypt_LaneRot (no contrary source) |
| AGREE | OuterDecrypt::SLOT_BASE_OFF | 0x20 | - | - | OuterDecrypt_SlotBaseOff | 0x20 | shipped as Offsets.h.OuterDecrypt_SlotBaseOff and OuterDecrypt_SlotBaseOff, FNameSlotBaseOff (no contrary source) |
| AGREE | OuterDecrypt::SLOT_STRIDE | 0x20 | - | - | OuterDecrypt_SlotStride | 0x20 | shipped as Offsets.h.OuterDecrypt_SlotStride and OuterDecrypt_SlotStride, FNameSlotStride (no contrary source) |
| AGREE | OuterDecrypt::XOR_MASK | 0x9A492C85DDF6F193 | - | - | OuterDecrypt_XorMask | 0x9A492C85DDF6F193 | shipped as Offsets.h.OuterDecrypt_XorMask (no contrary source) |
| AGREE | PlayerDecrypt::BLEND_XOR_MASK | 0x9A492C85DDF6F193 | - | - | PlayerDecrypt_BlendXorMask | 0x9A492C85DDF6F193 | shipped as Offsets.h.PlayerDecrypt_BlendXorMask and PlayerDecrypt_BlendXorMask (no contrary source) |
| AGREE | PlayerDecrypt::FINAL_ROT | 0x27 | - | - | PlayerDecrypt_FinalRot | 0x27 | shipped as Offsets.h.PlayerDecrypt_FinalRot and PlayerDecrypt_FinalRot, OuterDecrypt_FinalRot (no contrary source) |
| AGREE | PlayerDecrypt::LANE_ROT | 0xD | - | - | PlayerDecrypt_LaneRot | 0xD | shipped as Offsets.h.PlayerDecrypt_LaneRot and PlayerDecrypt_LaneRot, OuterDecrypt_LaneRot (no contrary source) |
| AGREE | TebDecrypt::QWORD_ROR | 0x3 | - | - | TebQwordRor | 0x3 | shipped as Offsets.h.TebQwordRor and TebQwordRor (no contrary source) |
| AGREE | TebDecrypt::TEB_KEY_OFF | 0x1F8 | - | - | TebKeyOff | 0x1F8 | shipped as Offsets.h.TebKeyOff and TebKeyOff (no contrary source) |
| AGREE | TebDecrypt::WORD_ROR | 0x1 | - | - | TebWordRor | 0x1 | shipped as Offsets.h.TebWordRor and TebWordRor (no contrary source) |
| NOT-SHIPPED-DUMP | ArcOffsets::AIController::BLACKBOARD | 0x4B0 | AIModule.AIController.Blackboard | 0x4B0 | - | - | nothing ships this; the dump reflects AIModule.AIController.Blackboard = 0x4B0 |
| NOT-SHIPPED-DUMP | ArcOffsets::AIController::BRAIN_COMPONENT | 0x4A0 | AIModule.AIController.BrainComponent | 0x4A0 | - | - | nothing ships this; the dump reflects AIModule.AIController.BrainComponent = 0x4A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::AIPerceptionComponent::AI_OWNER | 0x1E0 | AIModule.AIPerceptionComponent.AIOwner | 0x1E0 | - | - | nothing ships this; the dump reflects AIModule.AIPerceptionComponent.AIOwner = 0x1E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::AIPerceptionComponent::DOMINANT_SENSE | 0x1C8 | AIModule.AIPerceptionComponent.DominantSense | 0x1C8 | - | - | nothing ships this; the dump reflects AIModule.AIPerceptionComponent.DominantSense = 0x1C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::BodySetup::AGG_GEOM | 0xB8 | Engine.BodySetup.AggGeom | 0xB8 | - | - | nothing ships this; the dump reflects Engine.BodySetup.AggGeom = 0xB8 |
| NOT-SHIPPED-DUMP | ArcOffsets::CapsuleComponent::CAPSULE_HALF_HEIGHT | 0x6A0 | Engine.CapsuleComponent.CapsuleHalfHeight | 0x6A0 | - | - | nothing ships this; the dump reflects Engine.CapsuleComponent.CapsuleHalfHeight = 0x6A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::CapsuleComponent::CAPSULE_RADIUS | 0x6A4 | Engine.CapsuleComponent.CapsuleRadius | 0x6A4 | - | - | nothing ships this; the dump reflects Engine.CapsuleComponent.CapsuleRadius = 0x6A4 |
| NOT-SHIPPED-DUMP | ArcOffsets::Character::B_IS_CROUCHED_MASK | 0x2 | Engine.Character.bIsCrouched mask | 0x2 | - | - | nothing ships this; the dump reflects Engine.Character.bIsCrouched mask = 0x2 |
| NOT-SHIPPED-DUMP | ArcOffsets::Character::REPLICATED_MOVEMENT_MODE | 0x562 | Engine.Character.ReplicatedMovementMode | 0x562 | - | - | nothing ships this; the dump reflects Engine.Character.ReplicatedMovementMode = 0x562 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::ALL_SERVICES | 0x5F0 | EmbarkConstructable.ConstructableBase.AllServices | 0x5F0 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.AllServices = 0x5F0 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::CONSTRUCTION_STREAM | 0x53C | EmbarkConstructable.ConstructableBase.ConstructionStream | 0x53C | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.ConstructionStream = 0x53C |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::INITIAL_ROTATION | 0x560 | EmbarkConstructable.ConstructableBase.InitialRotation | 0x560 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.InitialRotation = 0x560 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::ON_CONSTRUCTABLE_DESTROYED_EVENT | 0x528 | EmbarkConstructable.ConstructableBase.OnConstructableDestroyedEvent | 0x528 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.OnConstructableDestroyedEvent = 0x528 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::ON_CONSTRUCTION_COMPLETE | 0x4F8 | EmbarkConstructable.ConstructableBase.OnConstructionComplete | 0x4F8 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.OnConstructionComplete = 0x4F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::ON_POST_FULLY_CONSTRUCTED | 0x550 | EmbarkConstructable.ConstructableBase.OnPostFullyConstructed | 0x550 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.OnPostFullyConstructed = 0x550 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::PROXY_CONTAINER | 0x620 | EmbarkConstructable.ConstructableBase.ProxyContainer | 0x620 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.ProxyContainer = 0x620 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableBase::bIsFullyConstructed | 0x538 | EmbarkConstructable.ConstructableBase.bIsFullyConstructed | 0x538 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableBase.bIsFullyConstructed = 0x538 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableServiceComponentBase::B_IS_FULLY_CONSTRUCTED | 0x1B9 | EmbarkConstructable.ConstructableServiceComponentBase.bIsFullyConstructed | 0x1B9 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableServiceComponentBase.bIsFullyConstructed = 0x1B9 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableServiceComponentBase::B_NEEDS_AS_SERVICE_PUMP | 0x1BA | EmbarkConstructable.ConstructableServiceComponentBase.bNeedsASServicePump | 0x1BA | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableServiceComponentBase.bNeedsASServicePump = 0x1BA |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableServiceComponentBase::B_ONLY_PUSH_CHANGED_VALUES_CLIENT | 0x1B8 | EmbarkConstructable.ConstructableServiceComponentBase.bOnlyPushChangedValues_Client | 0x1B8 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableServiceComponentBase.bOnlyPushChangedValues_Client = 0x1B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::ConstructableServiceComponentBase::OWNER_CONSTRUCTABLE | 0x1C8 | EmbarkConstructable.ConstructableServiceComponentBase.OwnerConstructable | 0x1C8 | - | - | nothing ships this; the dump reflects EmbarkConstructable.ConstructableServiceComponentBase.OwnerConstructable = 0x1C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::ATMOSPHERE_SUN_DISK_COLOR_SCALE | 0x540 | Engine.DirectionalLightComponent.AtmosphereSunDiskColorScale | 0x540 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.AtmosphereSunDiskColorScale = 0x540 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::ATMOSPHERE_SUN_LIGHT_INDEX | 0x53C | Engine.DirectionalLightComponent.AtmosphereSunLightIndex | 0x53C | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.AtmosphereSunLightIndex = 0x53C |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::B_CAST_MODULATED_SHADOWS | 0x590 | Engine.DirectionalLightComponent.bCastModulatedShadows | 0x590 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.bCastModulatedShadows = 0x590 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::B_ENABLE_LIGHT_SHAFT_OCCLUSION | 0x4D4 | Engine.DirectionalLightComponent.bEnableLightShaftOcclusion | 0x4D4 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.bEnableLightShaftOcclusion = 0x4D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CASCADE_DISTRIBUTION_EXPONENT | 0x508 | Engine.DirectionalLightComponent.CascadeDistributionExponent | 0x508 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CascadeDistributionExponent = 0x508 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CASCADE_TRANSITION_FRACTION | 0x50C | Engine.DirectionalLightComponent.CascadeTransitionFraction | 0x50C | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CascadeTransitionFraction = 0x50C |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SCATTERED_LUMINANCE_SCALE | 0x570 | Engine.DirectionalLightComponent.CloudScatteredLuminanceScale | 0x570 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudScatteredLuminanceScale = 0x570 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_DEPTH_BIAS | 0x560 | Engine.DirectionalLightComponent.CloudShadowDepthBias | 0x560 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowDepthBias = 0x560 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_EXTENT | 0x564 | Engine.DirectionalLightComponent.CloudShadowExtent | 0x564 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowExtent = 0x564 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_MAP_RESOLUTION_SCALE | 0x568 | Engine.DirectionalLightComponent.CloudShadowMapResolutionScale | 0x568 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowMapResolutionScale = 0x568 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_ON_ATMOSPHERE_STRENGTH | 0x558 | Engine.DirectionalLightComponent.CloudShadowOnAtmosphereStrength | 0x558 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowOnAtmosphereStrength = 0x558 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_ON_SURFACE_STRENGTH | 0x55C | Engine.DirectionalLightComponent.CloudShadowOnSurfaceStrength | 0x55C | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowOnSurfaceStrength = 0x55C |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_RAY_SAMPLE_COUNT_SCALE | 0x56C | Engine.DirectionalLightComponent.CloudShadowRaySampleCountScale | 0x56C | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowRaySampleCountScale = 0x56C |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_STRENGTH | 0x554 | Engine.DirectionalLightComponent.CloudShadowStrength | 0x554 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.CloudShadowStrength = 0x554 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::DISTANCE_FIELD_SHADOW_DISTANCE | 0x520 | Engine.DirectionalLightComponent.DistanceFieldShadowDistance | 0x520 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.DistanceFieldShadowDistance = 0x520 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::DYNAMIC_SHADOW_CASCADES | 0x504 | Engine.DirectionalLightComponent.DynamicShadowCascades | 0x504 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.DynamicShadowCascades = 0x504 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::FAR_SHADOW_CASCADE_COUNT | 0x518 | Engine.DirectionalLightComponent.FarShadowCascadeCount | 0x518 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.FarShadowCascadeCount = 0x518 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::FAR_SHADOW_DISTANCE | 0x51C | Engine.DirectionalLightComponent.FarShadowDistance | 0x51C | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.FarShadowDistance = 0x51C |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::FORWARD_SHADING_PRIORITY | 0x524 | Engine.DirectionalLightComponent.ForwardShadingPriority | 0x524 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.ForwardShadingPriority = 0x524 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::LIGHTMASS_SETTINGS | 0x580 | Engine.DirectionalLightComponent.LightmassSettings | 0x580 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.LightmassSettings = 0x580 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::LIGHT_SHAFT_OVERRIDE_DIRECTION | 0x4E0 | Engine.DirectionalLightComponent.LightShaftOverrideDirection | 0x4E0 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.LightShaftOverrideDirection = 0x4E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::LIGHT_SOURCE_ANGLE | 0x528 | Engine.DirectionalLightComponent.LightSourceAngle | 0x528 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.LightSourceAngle = 0x528 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::LIGHT_SOURCE_SOFT_ANGLE | 0x52C | Engine.DirectionalLightComponent.LightSourceSoftAngle | 0x52C | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.LightSourceSoftAngle = 0x52C |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::MODULATED_SHADOW_COLOR | 0x594 | Engine.DirectionalLightComponent.ModulatedShadowColor | 0x594 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.ModulatedShadowColor = 0x594 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::OCCLUSION_DEPTH_RANGE | 0x4DC | Engine.DirectionalLightComponent.OcclusionDepthRange | 0x4DC | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.OcclusionDepthRange = 0x4DC |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::OCCLUSION_MASK_DARKNESS | 0x4D8 | Engine.DirectionalLightComponent.OcclusionMaskDarkness | 0x4D8 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.OcclusionMaskDarkness = 0x4D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::SHADOW_AMOUNT | 0x598 | Engine.DirectionalLightComponent.ShadowAmount | 0x598 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.ShadowAmount = 0x598 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::SHADOW_CASCADE_BIAS_DISTRIBUTION | 0x4D0 | Engine.DirectionalLightComponent.ShadowCascadeBiasDistribution | 0x4D0 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.ShadowCascadeBiasDistribution = 0x4D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::SHADOW_DISTANCE_FADEOUT_FRACTION | 0x510 | Engine.DirectionalLightComponent.ShadowDistanceFadeoutFraction | 0x510 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.ShadowDistanceFadeoutFraction = 0x510 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::SHADOW_SOURCE_ANGLE_FACTOR | 0x530 | Engine.DirectionalLightComponent.ShadowSourceAngleFactor | 0x530 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.ShadowSourceAngleFactor = 0x530 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::TRACE_DISTANCE | 0x534 | Engine.DirectionalLightComponent.TraceDistance | 0x534 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.TraceDistance = 0x534 |
| NOT-SHIPPED-DUMP | ArcOffsets::DirectionalLightComponent::WHOLE_SCENE_DYNAMIC_SHADOW_RADIUS | 0x4F8 | Engine.DirectionalLightComponent.WholeSceneDynamicShadowRadius | 0x4F8 | - | - | nothing ships this; the dump reflects Engine.DirectionalLightComponent.WholeSceneDynamicShadowRadius = 0x4F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::EmbarkSkyActor::LIGHTING_MULTIPLIER | 0x3C8 | Angelscript.EmbarkSkyActor.LightingMultiplier | 0x3C8 | - | - | nothing ships this; the dump reflects Angelscript.EmbarkSkyActor.LightingMultiplier = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::EmbarkSkyActor::MOONLIGHT_INTENSITY | 0x3F8 | Angelscript.EmbarkSkyActor.MoonlightIntensity | 0x3F8 | - | - | nothing ships this; the dump reflects Angelscript.EmbarkSkyActor.MoonlightIntensity = 0x3F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::EmbarkSkyActor::MOON_COLOR | 0x400 | Angelscript.EmbarkSkyActor.MoonColor | 0x400 | - | - | nothing ships this; the dump reflects Angelscript.EmbarkSkyActor.MoonColor = 0x400 |
| NOT-SHIPPED-DUMP | ArcOffsets::EmbarkSkyActor::TIME_OF_DAY | 0x3C0 | Angelscript.EmbarkSkyActor.TimeOfDay | 0x3C0 | - | - | nothing ships this; the dump reflects Angelscript.EmbarkSkyActor.TimeOfDay = 0x3C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFog::B_ENABLED | 0x3C8 | Engine.ExponentialHeightFog.bEnabled | 0x3C8 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFog.bEnabled = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::B_ENABLE_VOLUMETRIC_FOG | 0x450 | Engine.ExponentialHeightFogComponent.bEnableVolumetricFog | 0x450 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.bEnableVolumetricFog = 0x450 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::B_IN_SCATTERING_TEXTURE_AND_COLOR | 0x385 | Engine.ExponentialHeightFogComponent.bInScatteringTextureAndColor | 0x385 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.bInScatteringTextureAndColor = 0x385 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::B_OVERRIDE_LIGHT_COLORS_WITH_FOG_INSCATTERING_COLORS | 0x480 | Engine.ExponentialHeightFogComponent.bOverrideLightColorsWithFogInscatteringColors | 0x480 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.bOverrideLightColorsWithFogInscatteringColors = 0x480 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_COLOR | 0x420 | Engine.ExponentialHeightFogComponent.DirectionalInscatteringColor | 0x420 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.DirectionalInscatteringColor = 0x420 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_EXPONENT | 0x418 | Engine.ExponentialHeightFogComponent.DirectionalInscatteringExponent | 0x418 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.DirectionalInscatteringExponent = 0x418 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_LUMINANCE | 0x430 | Engine.ExponentialHeightFogComponent.DirectionalInscatteringLuminance | 0x430 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.DirectionalInscatteringLuminance = 0x430 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::FOG_CUTOFF_DISTANCE | 0x44C | Engine.ExponentialHeightFogComponent.FogCutoffDistance | 0x44C | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.FogCutoffDistance = 0x44C |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::FOG_DENSITY | 0x370 | Engine.ExponentialHeightFogComponent.FogDensity | 0x370 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.FogDensity = 0x370 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::FOG_HEIGHT_FALLOFF | 0x374 | Engine.ExponentialHeightFogComponent.FogHeightFalloff | 0x374 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.FogHeightFalloff = 0x374 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::FOG_INSCATTERING_COLOR | 0x38C | Engine.ExponentialHeightFogComponent.FogInscatteringColor | 0x38C | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.FogInscatteringColor = 0x38C |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::FOG_INSCATTERING_LUMINANCE | 0x39C | Engine.ExponentialHeightFogComponent.FogInscatteringLuminance | 0x39C | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.FogInscatteringLuminance = 0x39C |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::FOG_MAX_OPACITY | 0x440 | Engine.ExponentialHeightFogComponent.FogMaxOpacity | 0x440 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.FogMaxOpacity = 0x440 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::INSCATTERING_COLOR_CUBEMAP | 0x3C0 | Engine.ExponentialHeightFogComponent.InscatteringColorCubemap | 0x3C0 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.InscatteringColorCubemap = 0x3C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::INSCATTERING_COLOR_CUBEMAP_ANGLE | 0x3C8 | Engine.ExponentialHeightFogComponent.InscatteringColorCubemapAngle | 0x3C8 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.InscatteringColorCubemapAngle = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::INSCATTERING_TEXTURE_TINT | 0x3CC | Engine.ExponentialHeightFogComponent.InscatteringTextureTint | 0x3CC | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.InscatteringTextureTint = 0x3CC |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::INVERT_SECOND_FOG_DIRECTION | 0x384 | Engine.ExponentialHeightFogComponent.InvertSecondFogDirection | 0x384 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.InvertSecondFogDirection = 0x384 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::SECOND_FOG_DATA | 0x378 | Engine.ExponentialHeightFogComponent.SecondFogData | 0x378 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.SecondFogData = 0x378 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::START_DISTANCE | 0x444 | Engine.ExponentialHeightFogComponent.StartDistance | 0x444 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.StartDistance = 0x444 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_ALBEDO | 0x458 | Engine.ExponentialHeightFogComponent.VolumetricFogAlbedo | 0x458 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogAlbedo = 0x458 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_DISTANCE | 0x470 | Engine.ExponentialHeightFogComponent.VolumetricFogDistance | 0x470 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogDistance = 0x470 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_EMISSIVE | 0x45C | Engine.ExponentialHeightFogComponent.VolumetricFogEmissive | 0x45C | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogEmissive = 0x45C |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_EXTINCTION_SCALE | 0x46C | Engine.ExponentialHeightFogComponent.VolumetricFogExtinctionScale | 0x46C | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogExtinctionScale = 0x46C |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_NEAR_FADE_IN_DISTANCE | 0x478 | Engine.ExponentialHeightFogComponent.VolumetricFogNearFadeInDistance | 0x478 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogNearFadeInDistance = 0x478 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_SCATTERING_DISTRIBUTION | 0x454 | Engine.ExponentialHeightFogComponent.VolumetricFogScatteringDistribution | 0x454 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogScatteringDistribution = 0x454 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_START_DISTANCE | 0x474 | Engine.ExponentialHeightFogComponent.VolumetricFogStartDistance | 0x474 | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogStartDistance = 0x474 |
| NOT-SHIPPED-DUMP | ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_STATIC_LIGHTING_SCATTERING_INTENSITY | 0x47C | Engine.ExponentialHeightFogComponent.VolumetricFogStaticLightingScatteringIntensity | 0x47C | - | - | nothing ships this; the dump reflects Engine.ExponentialHeightFogComponent.VolumetricFogStaticLightingScatteringIntensity = 0x47C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_CUBEMAP_INTENSITY | 0x470 | Engine.PostProcessSettings.AmbientCubemapIntensity | 0x470 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientCubemapIntensity = 0x470 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_CUBEMAP_TINT | 0x460 | Engine.PostProcessSettings.AmbientCubemapTint | 0x460 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientCubemapTint = 0x460 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_BIAS | 0x62C | Engine.PostProcessSettings.AmbientOcclusionBias | 0x62C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionBias = 0x62C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_FADE_DISTANCE | 0x61C | Engine.PostProcessSettings.AmbientOcclusionFadeDistance | 0x61C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionFadeDistance = 0x61C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_FADE_RADIUS | 0x620 | Engine.PostProcessSettings.AmbientOcclusionFadeRadius | 0x620 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionFadeRadius = 0x620 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_INTENSITY | 0x60C | Engine.PostProcessSettings.AmbientOcclusionIntensity | 0x60C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionIntensity = 0x60C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_POWER | 0x628 | Engine.PostProcessSettings.AmbientOcclusionPower | 0x628 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionPower = 0x628 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_QUALITY | 0x630 | Engine.PostProcessSettings.AmbientOcclusionQuality | 0x630 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionQuality = 0x630 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_RADIUS | 0x614 | Engine.PostProcessSettings.AmbientOcclusionRadius | 0x614 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionRadius = 0x614 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_STATIC_FRACTION | 0x610 | Engine.PostProcessSettings.AmbientOcclusionStaticFraction | 0x610 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AmbientOcclusionStaticFraction = 0x610 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_BIAS | 0x494 | Engine.PostProcessSettings.AutoExposureBias | 0x494 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureBias = 0x494 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_BIAS_BACKUP | 0x498 | Engine.PostProcessSettings.AutoExposureBiasBackup | 0x498 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureBiasBackup = 0x498 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_CALIBRATION_CONSTANT | 0x4D8 | Engine.PostProcessSettings.AutoExposureCalibrationConstant | 0x4D8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureCalibrationConstant = 0x4D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_HIGH_PERCENT | 0x4BC | Engine.PostProcessSettings.AutoExposureHighPercent | 0x4BC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureHighPercent = 0x4BC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_LOW_PERCENT | 0x4B8 | Engine.PostProcessSettings.AutoExposureLowPercent | 0x4B8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureLowPercent = 0x4B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_MAX_BRIGHTNESS | 0x4C4 | Engine.PostProcessSettings.AutoExposureMaxBrightness | 0x4C4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureMaxBrightness = 0x4C4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_METHOD | 0x36 | Engine.PostProcessSettings.AutoExposureMethod | 0x36 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureMethod = 0x36 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_MIN_BRIGHTNESS | 0x4C0 | Engine.PostProcessSettings.AutoExposureMinBrightness | 0x4C0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureMinBrightness = 0x4C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_SPEED_DOWN | 0x4CC | Engine.PostProcessSettings.AutoExposureSpeedDown | 0x4CC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureSpeedDown = 0x4CC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_SPEED_UP | 0x4C8 | Engine.PostProcessSettings.AutoExposureSpeedUp | 0x4C8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.AutoExposureSpeedUp = 0x4C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::BLOOM_INTENSITY | 0x304 | Engine.PostProcessSettings.BloomIntensity | 0x304 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.BloomIntensity = 0x304 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::BLUE_CORRECTION | 0x2CC | Engine.PostProcessSettings.BlueCorrection | 0x2CC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.BlueCorrection = 0x2CC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::CAMERA_SHUTTER_SPEED | 0x480 | Engine.PostProcessSettings.CameraShutterSpeed | 0x480 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.CameraShutterSpeed = 0x480 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::CHROMATIC_ABERRATION_START_OFFSET | 0x300 | Engine.PostProcessSettings.ChromaticAberrationStartOffset | 0x300 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ChromaticAberrationStartOffset = 0x300 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CONTRAST | 0x60 | Engine.PostProcessSettings.ColorContrast | 0x60 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorContrast = 0x60 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CONTRAST_HIGHLIGHTS | 0x240 | Engine.PostProcessSettings.ColorContrastHighlights | 0x240 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorContrastHighlights = 0x240 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CONTRAST_MIDTONES | 0x1A0 | Engine.PostProcessSettings.ColorContrastMidtones | 0x1A0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorContrastMidtones = 0x1A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CONTRAST_SHADOWS | 0x100 | Engine.PostProcessSettings.ColorContrastShadows | 0x100 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorContrastShadows = 0x100 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CORRECTION_HIGHLIGHTS_MAX | 0x2C4 | Engine.PostProcessSettings.ColorCorrectionHighlightsMax | 0x2C4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorCorrectionHighlightsMax = 0x2C4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CORRECTION_HIGHLIGHTS_MIN | 0x2C0 | Engine.PostProcessSettings.ColorCorrectionHighlightsMin | 0x2C0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorCorrectionHighlightsMin = 0x2C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_CORRECTION_SHADOWS_MAX | 0x2C8 | Engine.PostProcessSettings.ColorCorrectionShadowsMax | 0x2C8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorCorrectionShadowsMax = 0x2C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAIN | 0xA0 | Engine.PostProcessSettings.ColorGain | 0xA0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGain = 0xA0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAIN_HIGHLIGHTS | 0x280 | Engine.PostProcessSettings.ColorGainHighlights | 0x280 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGainHighlights = 0x280 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAIN_MIDTONES | 0x1E0 | Engine.PostProcessSettings.ColorGainMidtones | 0x1E0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGainMidtones = 0x1E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAIN_SHADOWS | 0x140 | Engine.PostProcessSettings.ColorGainShadows | 0x140 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGainShadows = 0x140 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAMMA | 0x80 | Engine.PostProcessSettings.ColorGamma | 0x80 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGamma = 0x80 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAMMA_HIGHLIGHTS | 0x260 | Engine.PostProcessSettings.ColorGammaHighlights | 0x260 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGammaHighlights = 0x260 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAMMA_MIDTONES | 0x1C0 | Engine.PostProcessSettings.ColorGammaMidtones | 0x1C0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGammaMidtones = 0x1C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GAMMA_SHADOWS | 0x120 | Engine.PostProcessSettings.ColorGammaShadows | 0x120 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGammaShadows = 0x120 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_GRADING_INTENSITY | 0x654 | Engine.PostProcessSettings.ColorGradingIntensity | 0x654 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorGradingIntensity = 0x654 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_OFFSET | 0xC0 | Engine.PostProcessSettings.ColorOffset | 0xC0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorOffset = 0xC0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_OFFSET_HIGHLIGHTS | 0x2A0 | Engine.PostProcessSettings.ColorOffsetHighlights | 0x2A0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorOffsetHighlights = 0x2A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_OFFSET_MIDTONES | 0x200 | Engine.PostProcessSettings.ColorOffsetMidtones | 0x200 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorOffsetMidtones = 0x200 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_OFFSET_SHADOWS | 0x160 | Engine.PostProcessSettings.ColorOffsetShadows | 0x160 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorOffsetShadows = 0x160 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_SATURATION | 0x40 | Engine.PostProcessSettings.ColorSaturation | 0x40 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorSaturation = 0x40 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_SATURATION_HIGHLIGHTS | 0x220 | Engine.PostProcessSettings.ColorSaturationHighlights | 0x220 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorSaturationHighlights = 0x220 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_SATURATION_MIDTONES | 0x180 | Engine.PostProcessSettings.ColorSaturationMidtones | 0x180 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorSaturationMidtones = 0x180 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::COLOR_SATURATION_SHADOWS | 0xE0 | Engine.PostProcessSettings.ColorSaturationShadows | 0xE0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ColorSaturationShadows = 0xE0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_BLADE_COUNT | 0x490 | Engine.PostProcessSettings.DepthOfFieldBladeCount | 0x490 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldBladeCount = 0x490 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_DEPTH_BLUR_AMOUNT | 0x680 | Engine.PostProcessSettings.DepthOfFieldDepthBlurAmount | 0x680 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldDepthBlurAmount = 0x680 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FAR_BLUR_SIZE | 0x6E4 | Engine.PostProcessSettings.DepthOfFieldFarBlurSize | 0x6E4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldFarBlurSize = 0x6E4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FAR_TRANSITION_REGION | 0x6D8 | Engine.PostProcessSettings.DepthOfFieldFarTransitionRegion | 0x6D8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldFarTransitionRegion = 0x6D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FOCAL_DISTANCE | 0x67C | Engine.PostProcessSettings.DepthOfFieldFocalDistance | 0x67C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldFocalDistance = 0x67C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FOCAL_REGION | 0x6D0 | Engine.PostProcessSettings.DepthOfFieldFocalRegion | 0x6D0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldFocalRegion = 0x6D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FSTOP | 0x488 | Engine.PostProcessSettings.DepthOfFieldFstop | 0x488 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldFstop = 0x488 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_MIN_FSTOP | 0x48C | Engine.PostProcessSettings.DepthOfFieldMinFstop | 0x48C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldMinFstop = 0x48C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_NEAR_BLUR_SIZE | 0x6E0 | Engine.PostProcessSettings.DepthOfFieldNearBlurSize | 0x6E0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldNearBlurSize = 0x6E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_NEAR_TRANSITION_REGION | 0x6D4 | Engine.PostProcessSettings.DepthOfFieldNearTransitionRegion | 0x6D4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldNearTransitionRegion = 0x6D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_OCCLUSION | 0x6E8 | Engine.PostProcessSettings.DepthOfFieldOcclusion | 0x6E8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldOcclusion = 0x6E8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SCALE | 0x6DC | Engine.PostProcessSettings.DepthOfFieldScale | 0x6DC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldScale = 0x6DC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SENSOR_WIDTH | 0x674 | Engine.PostProcessSettings.DepthOfFieldSensorWidth | 0x674 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldSensorWidth = 0x674 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SKY_FOCUS_DISTANCE | 0x6EC | Engine.PostProcessSettings.DepthOfFieldSkyFocusDistance | 0x6EC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldSkyFocusDistance = 0x6EC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SQUEEZE_FACTOR | 0x678 | Engine.PostProcessSettings.DepthOfFieldSqueezeFactor | 0x678 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldSqueezeFactor = 0x678 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_VIGNETTE_SIZE | 0x6F0 | Engine.PostProcessSettings.DepthOfFieldVignetteSize | 0x6F0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.DepthOfFieldVignetteSize = 0x6F0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::EXPAND_GAMUT | 0x2D0 | Engine.PostProcessSettings.ExpandGamut | 0x2D0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ExpandGamut = 0x2D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_BLACK_CLIP | 0x2E4 | Engine.PostProcessSettings.FilmBlackClip | 0x2E4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmBlackClip = 0x2E4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_HIGHLIGHTS_MAX | 0x5F0 | Engine.PostProcessSettings.FilmGrainHighlightsMax | 0x5F0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainHighlightsMax = 0x5F0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_HIGHLIGHTS_MIN | 0x5EC | Engine.PostProcessSettings.FilmGrainHighlightsMin | 0x5EC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainHighlightsMin = 0x5EC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY | 0x5D8 | Engine.PostProcessSettings.FilmGrainIntensity | 0x5D8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainIntensity = 0x5D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY_HIGHLIGHTS | 0x5E4 | Engine.PostProcessSettings.FilmGrainIntensityHighlights | 0x5E4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainIntensityHighlights = 0x5E4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY_MIDTONES | 0x5E0 | Engine.PostProcessSettings.FilmGrainIntensityMidtones | 0x5E0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainIntensityMidtones = 0x5E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY_SHADOWS | 0x5DC | Engine.PostProcessSettings.FilmGrainIntensityShadows | 0x5DC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainIntensityShadows = 0x5DC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_SHADOWS_MAX | 0x5E8 | Engine.PostProcessSettings.FilmGrainShadowsMax | 0x5E8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainShadowsMax = 0x5E8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_TEXEL_SIZE | 0x5F4 | Engine.PostProcessSettings.FilmGrainTexelSize | 0x5F4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainTexelSize = 0x5F4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_GRAIN_TEXTURE | 0x5F8 | Engine.PostProcessSettings.FilmGrainTexture | 0x5F8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmGrainTexture = 0x5F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_SHOULDER | 0x2E0 | Engine.PostProcessSettings.FilmShoulder | 0x2E0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmShoulder = 0x2E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_SLOPE | 0x2D8 | Engine.PostProcessSettings.FilmSlope | 0x2D8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmSlope = 0x2D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_TOE | 0x2DC | Engine.PostProcessSettings.FilmToe | 0x2DC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmToe = 0x2DC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::FILM_WHITE_CLIP | 0x2E8 | Engine.PostProcessSettings.FilmWhiteClip | 0x2E8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.FilmWhiteClip = 0x2E8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::GRAIN_INTENSITY | 0x5D4 | Engine.PostProcessSettings.GrainIntensity | 0x5D4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.GrainIntensity = 0x5D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::GRAIN_JITTER | 0x5D0 | Engine.PostProcessSettings.GrainJitter | 0x5D0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.GrainJitter = 0x5D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::HISTOGRAM_LOG_MAX | 0x4D4 | Engine.PostProcessSettings.HistogramLogMax | 0x4D4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.HistogramLogMax = 0x4D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::HISTOGRAM_LOG_MIN | 0x4D0 | Engine.PostProcessSettings.HistogramLogMin | 0x4D0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.HistogramLogMin = 0x4D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_BLURRED_LUMINANCE_BLEND | 0x50C | Engine.PostProcessSettings.LocalExposureBlurredLuminanceBlend | 0x50C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureBlurredLuminanceBlend = 0x50C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_BLURRED_LUMINANCE_KERNEL_SIZE_PERCENT | 0x510 | Engine.PostProcessSettings.LocalExposureBlurredLuminanceKernelSizePercent | 0x510 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureBlurredLuminanceKernelSizePercent = 0x510 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_CONTRAST_SCALE | 0x4E0 | Engine.PostProcessSettings.LocalExposureContrastScale | 0x4E0 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureContrastScale = 0x4E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_DETAIL_STRENGTH | 0x508 | Engine.PostProcessSettings.LocalExposureDetailStrength | 0x508 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureDetailStrength = 0x508 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_HIGHLIGHT_CONTRAST_SCALE | 0x4E4 | Engine.PostProcessSettings.LocalExposureHighlightContrastScale | 0x4E4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureHighlightContrastScale = 0x4E4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_MIDDLE_GREY_BIAS | 0x51C | Engine.PostProcessSettings.LocalExposureMiddleGreyBias | 0x51C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureMiddleGreyBias = 0x51C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_SHADOW_CONTRAST_SCALE | 0x4E8 | Engine.PostProcessSettings.LocalExposureShadowContrastScale | 0x4E8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LocalExposureShadowContrastScale = 0x4E8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_DIFFUSE_COLOR_BOOST | 0x418 | Engine.PostProcessSettings.LumenDiffuseColorBoost | 0x418 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenDiffuseColorBoost = 0x418 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_FINAL_GATHER_LIGHTING_UPDATE_SPEED | 0x40C | Engine.PostProcessSettings.LumenFinalGatherLightingUpdateSpeed | 0x40C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenFinalGatherLightingUpdateSpeed = 0x40C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_FINAL_GATHER_QUALITY | 0x408 | Engine.PostProcessSettings.LumenFinalGatherQuality | 0x408 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenFinalGatherQuality = 0x408 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_FULL_SKYLIGHT_LEAKING_DISTANCE | 0x430 | Engine.PostProcessSettings.LumenFullSkylightLeakingDistance | 0x430 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenFullSkylightLeakingDistance = 0x430 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_MAX_REFLECTION_BOUNCES | 0x448 | Engine.PostProcessSettings.LumenMaxReflectionBounces | 0x448 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenMaxReflectionBounces = 0x448 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_MAX_TRACE_DISTANCE | 0x414 | Engine.PostProcessSettings.LumenMaxTraceDistance | 0x414 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenMaxTraceDistance = 0x414 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_RAY_LIGHTING_MODE | 0x3F4 | Engine.PostProcessSettings.LumenRayLightingMode | 0x3F4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenRayLightingMode = 0x3F4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_REFLECTION_QUALITY | 0x43C | Engine.PostProcessSettings.LumenReflectionQuality | 0x43C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenReflectionQuality = 0x43C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_SCENE_DETAIL | 0x3FC | Engine.PostProcessSettings.LumenSceneDetail | 0x3FC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenSceneDetail = 0x3FC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_SCENE_LIGHTING_QUALITY | 0x3F8 | Engine.PostProcessSettings.LumenSceneLightingQuality | 0x3F8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenSceneLightingQuality = 0x3F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_SCENE_LIGHTING_UPDATE_SPEED | 0x404 | Engine.PostProcessSettings.LumenSceneLightingUpdateSpeed | 0x404 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenSceneLightingUpdateSpeed = 0x404 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_SCENE_VIEW_DISTANCE | 0x400 | Engine.PostProcessSettings.LumenSceneViewDistance | 0x400 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenSceneViewDistance = 0x400 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_SKYLIGHT_LEAKING | 0x41C | Engine.PostProcessSettings.LumenSkylightLeaking | 0x41C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenSkylightLeaking = 0x41C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::LUMEN_SURFACE_CACHE_RESOLUTION | 0x434 | Engine.PostProcessSettings.LumenSurfaceCacheResolution | 0x434 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.LumenSurfaceCacheResolution = 0x434 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::MOTION_BLUR_AMOUNT | 0x6F4 | Engine.PostProcessSettings.MotionBlurAmount | 0x6F4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.MotionBlurAmount = 0x6F4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::MOTION_BLUR_MAX | 0x6F8 | Engine.PostProcessSettings.MotionBlurMax | 0x6F8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.MotionBlurMax = 0x6F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::MOTION_BLUR_PER_OBJECT_SIZE | 0x700 | Engine.PostProcessSettings.MotionBlurPerObjectSize | 0x700 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.MotionBlurPerObjectSize = 0x700 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::RAY_TRACING_TRANSLUCENCY_MAX_ROUGHNESS | 0x73C | Engine.PostProcessSettings.RayTracingTranslucencyMaxRoughness | 0x73C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.RayTracingTranslucencyMaxRoughness = 0x73C |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::REFLECTIONS_TYPE | 0x439 | Engine.PostProcessSettings.ReflectionsType | 0x439 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ReflectionsType = 0x439 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::SCENE_COLOR_TINT | 0x2EC | Engine.PostProcessSettings.SceneColorTint | 0x2EC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.SceneColorTint = 0x2EC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::SCENE_FRINGE_INTENSITY | 0x2FC | Engine.PostProcessSettings.SceneFringeIntensity | 0x2FC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.SceneFringeIntensity = 0x2FC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::SCREEN_SPACE_REFLECTION_INTENSITY | 0x450 | Engine.PostProcessSettings.ScreenSpaceReflectionIntensity | 0x450 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ScreenSpaceReflectionIntensity = 0x450 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::SCREEN_SPACE_REFLECTION_MAX_ROUGHNESS | 0x458 | Engine.PostProcessSettings.ScreenSpaceReflectionMaxRoughness | 0x458 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ScreenSpaceReflectionMaxRoughness = 0x458 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::SCREEN_SPACE_REFLECTION_QUALITY | 0x454 | Engine.PostProcessSettings.ScreenSpaceReflectionQuality | 0x454 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ScreenSpaceReflectionQuality = 0x454 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::SHARPEN | 0x5CC | Engine.PostProcessSettings.Sharpen | 0x5CC | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.Sharpen = 0x5CC |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::TONE_CURVE_AMOUNT | 0x2D4 | Engine.PostProcessSettings.ToneCurveAmount | 0x2D4 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.ToneCurveAmount = 0x2D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::VIGNETTE_INTENSITY | 0x5C8 | Engine.PostProcessSettings.VignetteIntensity | 0x5C8 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.VignetteIntensity = 0x5C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::WEIGHTED_BLENDABLES | 0x778 | Engine.PostProcessSettings.WeightedBlendables | 0x778 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.WeightedBlendables = 0x778 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::WHITE_TEMP | 0x38 | Engine.PostProcessSettings.WhiteTemp | 0x38 | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.WhiteTemp = 0x38 |
| NOT-SHIPPED-DUMP | ArcOffsets::FPostProcessSettings::WHITE_TINT | 0x3C | Engine.PostProcessSettings.WhiteTint | 0x3C | - | - | nothing ships this; the dump reflects Engine.PostProcessSettings.WhiteTint = 0x3C |
| NOT-SHIPPED-DUMP | ArcOffsets::GameState::AUTHORITY_GAME_MODE | 0x430 | Engine.GameState.AuthorityGameMode | 0x430 | - | - | nothing ships this; the dump reflects Engine.GameState.AuthorityGameMode = 0x430 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameState::B_REPLICATED_HAS_BEGUN_PLAY | 0x510 | Engine.GameState.bReplicatedHasBegunPlay | 0x510 | - | - | nothing ships this; the dump reflects Engine.GameState.bReplicatedHasBegunPlay = 0x510 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameState::GAME_MODE_CLASS | 0x428 | Engine.GameState.GameModeClass | 0x428 | - | - | nothing ships this; the dump reflects Engine.GameState.GameModeClass = 0x428 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameState::PLAYER_STATES | 0x9C0 | Angelscript.PioneerGameState.PlayerStates (all 2 classes here) | 0x9C0 | - | - | nothing ships this; the dump reflects Angelscript.PioneerGameState.PlayerStates (all 2 classes here) = 0x9C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameState::REPLICATED_WORLD_TIME_SECONDS_DOUBLE | 0x518 | Engine.GameState.ReplicatedWorldTimeSecondsDouble | 0x518 | - | - | nothing ships this; the dump reflects Engine.GameState.ReplicatedWorldTimeSecondsDouble = 0x518 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameState::SPECTATOR_CLASS | 0x438 | Engine.GameState.SpectatorClass | 0x438 | - | - | nothing ships this; the dump reflects Engine.GameState.SpectatorClass = 0x438 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameViewportClient::DEBUG_PROPERTIES | 0xF8 | Engine.GameViewportClient.DebugProperties | 0xF8 | - | - | nothing ships this; the dump reflects Engine.GameViewportClient.DebugProperties = 0xF8 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameViewportClient::GAME_INSTANCE | 0x1F8 | Engine.GameViewportClient.GameInstance | 0x1F8 | - | - | nothing ships this; the dump reflects Engine.GameViewportClient.GameInstance = 0x1F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameViewportClient::MAX_SPLITSCREEN_PLAYERS | 0x120 | Engine.GameViewportClient.MaxSplitscreenPlayers | 0x120 | - | - | nothing ships this; the dump reflects Engine.GameViewportClient.MaxSplitscreenPlayers = 0x120 |
| NOT-SHIPPED-DUMP | ArcOffsets::GameViewportClient::VIEWPORT_CONSOLE | 0x128 | Engine.GameViewportClient.ViewportConsole | 0x128 | - | - | nothing ships this; the dump reflects Engine.GameViewportClient.ViewportConsole = 0x128 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ARMOR | 0x1C0 | Angelscript.HealthComponent.Armor | 0x1C0 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.Armor = 0x1C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ARMORED_ZONE | 0x368 | Angelscript.HealthComponent.ArmoredZone | 0x368 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.ArmoredZone = 0x368 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::B_ALWAYS_DBNO_ON_DEATH | 0x7C1 | Angelscript.HealthComponent.bAlwaysDBNOOnDeath | 0x7C1 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.bAlwaysDBNOOnDeath = 0x7C1 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::B_SELF_DAMAGE | 0x388 | Angelscript.HealthComponent.bSelfDamage | 0x388 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.bSelfDamage = 0x388 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::CACHED_HEALTH | 0x700 | Angelscript.HealthComponent.CachedHealth | 0x700 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.CachedHealth = 0x700 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ON_DAMAGED | 0x3A0 | Angelscript.HealthComponent.OnDamaged | 0x3A0 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.OnDamaged = 0x3A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ON_DBNO | 0x3E0 | Angelscript.HealthComponent.OnDBNO | 0x3E0 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.OnDBNO = 0x3E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ON_DEAD | 0x3D0 | Angelscript.HealthComponent.OnDead | 0x3D0 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.OnDead = 0x3D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ON_HEALTH_CHANGED | 0x390 | Angelscript.HealthComponent.OnHealthChanged | 0x390 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.OnHealthChanged = 0x390 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthComponent::ON_REVIVED | 0x3C0 | Angelscript.HealthComponent.OnRevived | 0x3C0 | - | - | nothing ships this; the dump reflects Angelscript.HealthComponent.OnRevived = 0x3C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthService::ON_PARENT_CONSTRUCTABLE_DESTROYED | 0x4B8 | Angelscript.HealthService.OnParentConstructableDestroyed | 0x4B8 | - | - | nothing ships this; the dump reflects Angelscript.HealthService.OnParentConstructableDestroyed = 0x4B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::HealthService::STUNS_DEFINITION | 0x460 | Angelscript.HealthService.StunsDefinition | 0x460 | - | - | nothing ships this; the dump reflects Angelscript.HealthService.StunsDefinition = 0x460 |
| NOT-SHIPPED-DUMP | ArcOffsets::InteractQuestComponent::RELEVANT_PLAYER_IDS | 0x2A8 | Angelscript.InteractQuestComponent.RelevantPlayerIds | 0x2A8 | - | - | nothing ships this; the dump reflects Angelscript.InteractQuestComponent.RelevantPlayerIds = 0x2A8 |
| NOT-SHIPPED-DUMP | ArcOffsets::InventoryComponent::AUXILIARY | 0x4B8 | Angelscript.InventoryComponent.Auxiliary | 0x4B8 | - | - | nothing ships this; the dump reflects Angelscript.InventoryComponent.Auxiliary = 0x4B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::Landscape::COLLISION_COMPONENTS | 0x5D8 | Landscape.Landscape.CollisionComponents | 0x5D8 | - | - | nothing ships this; the dump reflects Landscape.Landscape.CollisionComponents = 0x5D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::Landscape::COMPONENT_SIZE_QUADS | 0x888 | Landscape.Landscape.ComponentSizeQuads | 0x888 | - | - | nothing ships this; the dump reflects Landscape.Landscape.ComponentSizeQuads = 0x888 |
| NOT-SHIPPED-DUMP | ArcOffsets::Landscape::LANDSCAPE_COMPONENTS | 0x5C8 | Landscape.Landscape.LandscapeComponents | 0x5C8 | - | - | nothing ships this; the dump reflects Landscape.Landscape.LandscapeComponents = 0x5C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::Landscape::NUM_SUBSECTIONS | 0x890 | Landscape.Landscape.NumSubsections | 0x890 | - | - | nothing ships this; the dump reflects Landscape.Landscape.NumSubsections = 0x890 |
| NOT-SHIPPED-DUMP | ArcOffsets::Landscape::SUBSECTION_SIZE_QUADS | 0x88C | Landscape.Landscape.SubsectionSizeQuads | 0x88C | - | - | nothing ships this; the dump reflects Landscape.Landscape.SubsectionSizeQuads = 0x88C |
| NOT-SHIPPED-DUMP | ArcOffsets::LandscapeHeightfieldCollisionComponent::CACHED_LOCAL_BOX | 0x6C8 | Landscape.LandscapeHeightfieldCollisionComponent.CachedLocalBox | 0x6C8 | - | - | nothing ships this; the dump reflects Landscape.LandscapeHeightfieldCollisionComponent.CachedLocalBox = 0x6C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::LandscapeHeightfieldCollisionComponent::COLLISION_SCALE | 0x69C | Landscape.LandscapeHeightfieldCollisionComponent.CollisionScale | 0x69C | - | - | nothing ships this; the dump reflects Landscape.LandscapeHeightfieldCollisionComponent.CollisionScale = 0x69C |
| NOT-SHIPPED-DUMP | ArcOffsets::LandscapeHeightfieldCollisionComponent::COLLISION_SIZE_QUADS | 0x698 | Landscape.LandscapeHeightfieldCollisionComponent.CollisionSizeQuads | 0x698 | - | - | nothing ships this; the dump reflects Landscape.LandscapeHeightfieldCollisionComponent.CollisionSizeQuads = 0x698 |
| NOT-SHIPPED-DUMP | ArcOffsets::LandscapeHeightfieldCollisionComponent::RENDER_COMPONENT_REF | 0x700 | Landscape.LandscapeHeightfieldCollisionComponent.RenderComponentRef | 0x700 | - | - | nothing ships this; the dump reflects Landscape.LandscapeHeightfieldCollisionComponent.RenderComponentRef = 0x700 |
| NOT-SHIPPED-DUMP | ArcOffsets::LandscapeHeightfieldCollisionComponent::SECTION_BASE_X | 0x690 | Landscape.LandscapeHeightfieldCollisionComponent.SectionBaseX | 0x690 | - | - | nothing ships this; the dump reflects Landscape.LandscapeHeightfieldCollisionComponent.SectionBaseX = 0x690 |
| NOT-SHIPPED-DUMP | ArcOffsets::LandscapeHeightfieldCollisionComponent::SECTION_BASE_Y | 0x694 | Landscape.LandscapeHeightfieldCollisionComponent.SectionBaseY | 0x694 | - | - | nothing ships this; the dump reflects Landscape.LandscapeHeightfieldCollisionComponent.SectionBaseY = 0x694 |
| NOT-SHIPPED-DUMP | ArcOffsets::LevelCollection::DEMO_NET_DRIVER | 0x18 | Engine.LevelCollection.DemoNetDriver | 0x18 | - | - | nothing ships this; the dump reflects Engine.LevelCollection.DemoNetDriver = 0x18 |
| NOT-SHIPPED-DUMP | ArcOffsets::LevelCollection::LEVELS | 0x28 | Engine.LevelCollection.Levels | 0x28 | - | - | nothing ships this; the dump reflects Engine.LevelCollection.Levels = 0x28 |
| NOT-SHIPPED-DUMP | ArcOffsets::LevelCollection::NET_DRIVER | 0x10 | Engine.LevelCollection.NetDriver | 0x10 | - | - | nothing ships this; the dump reflects Engine.LevelCollection.NetDriver = 0x10 |
| NOT-SHIPPED-DUMP | ArcOffsets::LevelStreaming::LOADED_LEVEL | 0x200 | Engine.LevelStreaming.LoadedLevel | 0x200 | - | - | nothing ships this; the dump reflects Engine.LevelStreaming.LoadedLevel = 0x200 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::BLOOM_MAX_BRIGHTNESS | 0x454 | Engine.LightComponent.BloomMaxBrightness | 0x454 | - | - | nothing ships this; the dump reflects Engine.LightComponent.BloomMaxBrightness = 0x454 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::BLOOM_SCALE | 0x44C | Engine.LightComponent.BloomScale | 0x44C | - | - | nothing ships this; the dump reflects Engine.LightComponent.BloomScale = 0x44C |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::BLOOM_THRESHOLD | 0x450 | Engine.LightComponent.BloomThreshold | 0x450 | - | - | nothing ships this; the dump reflects Engine.LightComponent.BloomThreshold = 0x450 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::BLOOM_TINT | 0x458 | Engine.LightComponent.BloomTint | 0x458 | - | - | nothing ships this; the dump reflects Engine.LightComponent.BloomTint = 0x458 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::B_ENABLE_LIGHT_SHAFT_BLOOM | 0x448 | Engine.LightComponent.bEnableLightShaftBloom | 0x448 | - | - | nothing ships this; the dump reflects Engine.LightComponent.bEnableLightShaftBloom = 0x448 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::B_USE_IES_BRIGHTNESS | 0x438 | Engine.LightComponent.bUseIESBrightness | 0x438 | - | - | nothing ships this; the dump reflects Engine.LightComponent.bUseIESBrightness = 0x438 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::B_USE_RAYTRACED_DISTANCE_FIELD_SHADOWS | 0x45C | Engine.LightComponent.bUseRayTracedDistanceFieldShadows | 0x45C | - | - | nothing ships this; the dump reflects Engine.LightComponent.bUseRayTracedDistanceFieldShadows = 0x45C |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::B_USE_TEMPERATURE | 0x3C4 | Engine.LightComponent.bUseTemperature | 0x3C4 | - | - | nothing ships this; the dump reflects Engine.LightComponent.bUseTemperature = 0x3C4 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::CONTACT_SHADOW_CASTING_INTENSITY | 0x3F4 | Engine.LightComponent.ContactShadowCastingIntensity | 0x3F4 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ContactShadowCastingIntensity = 0x3F4 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::CONTACT_SHADOW_LENGTH | 0x3EC | Engine.LightComponent.ContactShadowLength | 0x3EC | - | - | nothing ships this; the dump reflects Engine.LightComponent.ContactShadowLength = 0x3EC |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::CONTACT_SHADOW_LENGTH_IN_WS | 0x3F0 | Engine.LightComponent.ContactShadowLengthInWS | 0x3F0 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ContactShadowLengthInWS = 0x3F0 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::CONTACT_SHADOW_NON_CASTING_INTENSITY | 0x3F8 | Engine.LightComponent.ContactShadowNonCastingIntensity | 0x3F8 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ContactShadowNonCastingIntensity = 0x3F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::DISABLED_BRIGHTNESS | 0x444 | Engine.LightComponent.DisabledBrightness | 0x444 | - | - | nothing ships this; the dump reflects Engine.LightComponent.DisabledBrightness = 0x444 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::IES_BRIGHTNESS_SCALE | 0x43C | Engine.LightComponent.IESBrightnessScale | 0x43C | - | - | nothing ships this; the dump reflects Engine.LightComponent.IESBrightnessScale = 0x43C |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::IES_TEXTURE | 0x430 | Engine.LightComponent.IESTexture | 0x430 | - | - | nothing ships this; the dump reflects Engine.LightComponent.IESTexture = 0x430 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::LIGHTING_CHANNELS | 0x40C | Engine.LightComponent.LightingChannels | 0x40C | - | - | nothing ships this; the dump reflects Engine.LightComponent.LightingChannels = 0x40C |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::LIGHT_FUNCTION_FADE_DISTANCE | 0x440 | Engine.LightComponent.LightFunctionFadeDistance | 0x440 | - | - | nothing ships this; the dump reflects Engine.LightComponent.LightFunctionFadeDistance = 0x440 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::LIGHT_FUNCTION_MATERIAL | 0x410 | Engine.LightComponent.LightFunctionMaterial | 0x410 | - | - | nothing ships this; the dump reflects Engine.LightComponent.LightFunctionMaterial = 0x410 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::LIGHT_FUNCTION_SCALE | 0x418 | Engine.LightComponent.LightFunctionScale | 0x418 | - | - | nothing ships this; the dump reflects Engine.LightComponent.LightFunctionScale = 0x418 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::MAX_DISTANCE_FADE_RANGE | 0x3C0 | Engine.LightComponent.MaxDistanceFadeRange | 0x3C0 | - | - | nothing ships this; the dump reflects Engine.LightComponent.MaxDistanceFadeRange = 0x3C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::MAX_DRAW_DISTANCE | 0x3BC | Engine.LightComponent.MaxDrawDistance | 0x3BC | - | - | nothing ships this; the dump reflects Engine.LightComponent.MaxDrawDistance = 0x3BC |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::MIN_ROUGHNESS | 0x3D0 | Engine.LightComponent.MinRoughness | 0x3D0 | - | - | nothing ships this; the dump reflects Engine.LightComponent.MinRoughness = 0x3D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::RAY_START_OFFSET_DEPTH_SCALE | 0x460 | Engine.LightComponent.RayStartOffsetDepthScale | 0x460 | - | - | nothing ships this; the dump reflects Engine.LightComponent.RayStartOffsetDepthScale = 0x460 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::SHADOW_BIAS | 0x3E0 | Engine.LightComponent.ShadowBias | 0x3E0 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ShadowBias = 0x3E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::SHADOW_MAP_CHANNEL | 0x3C8 | Engine.LightComponent.ShadowMapChannel | 0x3C8 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ShadowMapChannel = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::SHADOW_RESOLUTION_SCALE | 0x3DC | Engine.LightComponent.ShadowResolutionScale | 0x3DC | - | - | nothing ships this; the dump reflects Engine.LightComponent.ShadowResolutionScale = 0x3DC |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::SHADOW_SHARPEN | 0x3E8 | Engine.LightComponent.ShadowSharpen | 0x3E8 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ShadowSharpen = 0x3E8 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::SHADOW_SLOPE_BIAS | 0x3E4 | Engine.LightComponent.ShadowSlopeBias | 0x3E4 | - | - | nothing ships this; the dump reflects Engine.LightComponent.ShadowSlopeBias = 0x3E4 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::SPECULAR_SCALE | 0x3D4 | Engine.LightComponent.SpecularScale | 0x3D4 | - | - | nothing ships this; the dump reflects Engine.LightComponent.SpecularScale = 0x3D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponent::TEMPERATURE | 0x3B8 | Engine.LightComponent.Temperature | 0x3B8 | - | - | nothing ships this; the dump reflects Engine.LightComponent.Temperature = 0x3B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::BRIGHTNESS | 0x390 | Engine.LightComponentBase.Brightness | 0x390 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.Brightness = 0x390 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::CAST_RAYTRACED_SHADOW | 0x3A0 | Engine.LightComponentBase.CastRaytracedShadow | 0x3A0 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.CastRaytracedShadow = 0x3A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::DEEP_SHADOW_LAYER_DISTRIBUTION | 0x3A8 | Engine.LightComponentBase.DeepShadowLayerDistribution | 0x3A8 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.DeepShadowLayerDistribution = 0x3A8 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::INDIRECT_LIGHTING_INTENSITY | 0x3AC | Engine.LightComponentBase.IndirectLightingIntensity | 0x3AC | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.IndirectLightingIntensity = 0x3AC |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::INTENSITY | 0x394 | Engine.LightComponentBase.Intensity | 0x394 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.Intensity = 0x394 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::LIGHT_COLOR | 0x398 | Engine.LightComponentBase.LightColor | 0x398 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.LightColor = 0x398 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::SAMPLES_PER_PIXEL | 0x3B4 | Engine.LightComponentBase.SamplesPerPixel | 0x3B4 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.SamplesPerPixel = 0x3B4 |
| NOT-SHIPPED-DUMP | ArcOffsets::LightComponentBase::VOLUMETRIC_SCATTERING_INTENSITY | 0x3B0 | Engine.LightComponentBase.VolumetricScatteringIntensity | 0x3B0 | - | - | nothing ships this; the dump reflects Engine.LightComponentBase.VolumetricScatteringIntensity = 0x3B0 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::ASPECT_RATIO_AXIS_CONSTRAINT | 0x228 | Engine.LocalPlayer.AspectRatioAxisConstraint | 0x228 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.AspectRatioAxisConstraint = 0x228 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::B_FIXED_CULL_FOV | 0x241 | Engine.LocalPlayer.bFixedCullFOV | 0x241 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.bFixedCullFOV = 0x241 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::B_FOREGROUND_STENCIL_ENABLED | 0x240 | Engine.LocalPlayer.bForegroundStencilEnabled | 0x240 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.bForegroundStencilEnabled = 0x240 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::B_SENT_SPLIT_JOIN | 0x238 | Engine.LocalPlayer.bSentSplitJoin | 0x238 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.bSentSplitJoin = 0x238 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_FALLOFF_EXPONENT | 0x254 | Engine.LocalPlayer.FakeCameraLightFalloffExponent | 0x254 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.FakeCameraLightFalloffExponent = 0x254 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_INTENSITY | 0x24C | Engine.LocalPlayer.FakeCameraLightIntensity | 0x24C | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.FakeCameraLightIntensity = 0x24C |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_RADIUS | 0x250 | Engine.LocalPlayer.FakeCameraLightRadius | 0x250 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.FakeCameraLightRadius = 0x250 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_VERTICAL_OFFSET | 0x258 | Engine.LocalPlayer.FakeCameraLightVerticalOffset | 0x258 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.FakeCameraLightVerticalOffset = 0x258 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FIXED_CULL_BASE_FOV | 0x244 | Engine.LocalPlayer.FixedCullBaseFOV | 0x244 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.FixedCullBaseFOV = 0x244 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FIXED_CULL_MAX_FOV | 0x248 | Engine.LocalPlayer.FixedCullMaxFOV | 0x248 | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.FixedCullMaxFOV = 0x248 |
| NOT-SHIPPED-DUMP | ArcOffsets::LocalPlayer::FOREGROUND_FOV | 0x23C | Engine.LocalPlayer.ForegroundFov | 0x23C | - | - | nothing ships this; the dump reflects Engine.LocalPlayer.ForegroundFov = 0x23C |
| NOT-SHIPPED-DUMP | ArcOffsets::LootContainerSingle::ITEM_CONTAINER_COMPONENT | 0xBC0 | Angelscript.LootContainerSingle.ItemContainerComponent | 0xBC0 | - | - | nothing ships this; the dump reflects Angelscript.LootContainerSingle.ItemContainerComponent = 0xBC0 |
| NOT-SHIPPED-DUMP | ArcOffsets::MapWidget::MAP_WIDGET_MAPPINGS | 0x5A8 | Angelscript.MapWidget.MapWidgetMappings | 0x5A8 | - | - | nothing ships this; the dump reflects Angelscript.MapWidget.MapWidgetMappings = 0x5A8 |
| NOT-SHIPPED-DUMP | ArcOffsets::MapWidgetLevelSettings::MAP_MATERIAL | 0x28 | Angelscript.MapWidgetLevelSettings.MapMaterial | 0x28 | - | - | nothing ships this; the dump reflects Angelscript.MapWidgetLevelSettings.MapMaterial = 0x28 |
| NOT-SHIPPED-DUMP | ArcOffsets::MapWidgetLevelSettings::MAP_TEXTURE | 0x0 | Angelscript.MapWidgetLevelSettings.MapTexture | 0x0 | - | - | nothing ships this; the dump reflects Angelscript.MapWidgetLevelSettings.MapTexture = 0x0 |
| NOT-SHIPPED-DUMP | ArcOffsets::MapWidgetMappings::COMPONENT_TEMPLATE | 0xA0 | Angelscript.MapWidgetMappings.ComponentTemplate | 0xA0 | - | - | nothing ships this; the dump reflects Angelscript.MapWidgetMappings.ComponentTemplate = 0xA0 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::ASPECT_RATIO | 0x9C | Engine.MinimalViewInfo.AspectRatio | 0x9C | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.AspectRatio = 0x9C |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::DESIRED_FOV | 0x6C | Engine.MinimalViewInfo.DesiredFOV | 0x6C | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.DesiredFOV = 0x6C |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::FIRST_PERSON_FOV | 0x70 | Engine.MinimalViewInfo.FirstPersonFOV | 0x70 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.FirstPersonFOV = 0x70 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::FIRST_PERSON_SCALE | 0x74 | Engine.MinimalViewInfo.FirstPersonScale | 0x74 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.FirstPersonScale = 0x74 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::FOV | 0x60 | Engine.MinimalViewInfo.FOV | 0x60 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.FOV = 0x60 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::LOCATION | 0x10 | Engine.MinimalViewInfo.Location | 0x10 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.Location = 0x10 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::ORTHO_WIDTH | 0x78 | Engine.MinimalViewInfo.OrthoWidth | 0x78 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.OrthoWidth = 0x78 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::POST_PROCESS_SETTINGS | 0xC0 | Engine.MinimalViewInfo.PostProcessSettings | 0xC0 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.PostProcessSettings = 0xC0 |
| NOT-SHIPPED-DUMP | ArcOffsets::MinimalViewInfo::ROTATION | 0x38 | Engine.MinimalViewInfo.Rotation | 0x38 | - | - | nothing ships this; the dump reflects Engine.MinimalViewInfo.Rotation = 0x38 |
| NOT-SHIPPED-DUMP | ArcOffsets::Pawn::LAST_HIT_BY | 0x3E8 | Engine.Pawn.LastHitBy | 0x3E8 | - | - | nothing ships this; the dump reflects Engine.Pawn.LastHitBy = 0x3E8 |
| NOT-SHIPPED-DUMP | ArcOffsets::Pawn::REMOTE_VIEW_PITCH | 0x3D4 | Engine.Pawn.RemoteViewPitch | 0x3D4 | - | - | nothing ships this; the dump reflects Engine.Pawn.RemoteViewPitch = 0x3D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::PhysicsAsset::SKELETAL_BODY_SETUPS | 0xE8 | Engine.PhysicsAsset.SkeletalBodySetups | 0xE8 | - | - | nothing ships this; the dump reflects Engine.PhysicsAsset.SkeletalBodySetups = 0xE8 |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerConstructablePawn::B_CAN_SELF_DAMAGE | 0x1210 | Angelscript.PioneerConstructablePawn.bCanSelfDamage | 0x1210 | - | - | nothing ships this; the dump reflects Angelscript.PioneerConstructablePawn.bCanSelfDamage = 0x1210 |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerConstructablePawn::SPAWN_COST | 0x1214 | Angelscript.PioneerConstructablePawn.SpawnCost | 0x1214 | - | - | nothing ships this; the dump reflects Angelscript.PioneerConstructablePawn.SpawnCost = 0x1214 |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerConstructablePawn::THREAT_LEVEL | 0x121C | Angelscript.PioneerConstructablePawn.ThreatLevel | 0x121C | - | - | nothing ships this; the dump reflects Angelscript.PioneerConstructablePawn.ThreatLevel = 0x121C |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerPlayerCharacter::FLASHLIGHT_COMPONENT | 0xDE0 | Angelscript.PioneerPlayerCharacter.FlashlightComponent | 0xDE0 | - | - | nothing ships this; the dump reflects Angelscript.PioneerPlayerCharacter.FlashlightComponent = 0xDE0 |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerPlayerCharacter::FOLLOW_CAMERA | 0xC60 | Angelscript.PioneerPlayerCharacter.FollowCamera | 0xC60 | - | - | nothing ships this; the dump reflects Angelscript.PioneerPlayerCharacter.FollowCamera = 0xC60 |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerPlayerCharacter::LAST_RELEVANT_PLAYER_STATE | 0xEC8 | Angelscript.PioneerPlayerCharacter.LastRelevantPlayerState | 0xEC8 | - | - | nothing ships this; the dump reflects Angelscript.PioneerPlayerCharacter.LastRelevantPlayerState = 0xEC8 |
| NOT-SHIPPED-DUMP | ArcOffsets::PioneerPlayerCharacter::PLAYER_STATUS_VAR | 0x12F8 | Angelscript.PioneerPlayerCharacter.PlayerStatusVar | 0x12F8 | - | - | nothing ships this; the dump reflects Angelscript.PioneerPlayerCharacter.PlayerStatusVar = 0x12F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerCameraManager::DEFAULT_ORTHO_WIDTH | 0x440 | Engine.PlayerCameraManager.DefaultOrthoWidth | 0x440 | - | - | nothing ships this; the dump reflects Engine.PlayerCameraManager.DefaultOrthoWidth = 0x440 |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerCameraManager::FREE_CAM_OFFSET | 0x1FC0 | Engine.PlayerCameraManager.FreeCamOffset | 0x1FC0 | - | - | nothing ships this; the dump reflects Engine.PlayerCameraManager.FreeCamOffset = 0x1FC0 |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerCameraManager::MODIFIER_LIST | 0x1FB0 | Engine.PlayerCameraManager.ModifierList | 0x1FB0 | - | - | nothing ships this; the dump reflects Engine.PlayerCameraManager.ModifierList = 0x1FB0 |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerCameraManager::VIEW_ROLL_MAX | 0x209C | Engine.PlayerCameraManager.ViewRollMax | 0x209C | - | - | nothing ships this; the dump reflects Engine.PlayerCameraManager.ViewRollMax = 0x209C |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerCameraManager::VIEW_ROLL_MIN | 0x20A4 | Engine.PlayerCameraManager.ViewRollMin | 0x20A4 | - | - | nothing ships this; the dump reflects Engine.PlayerCameraManager.ViewRollMin = 0x20A4 |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerCameraManager::VIEW_TARGET_OFFSET | 0x1FE0 | Engine.PlayerCameraManager.ViewTargetOffset | 0x1FE0 | - | - | nothing ships this; the dump reflects Engine.PlayerCameraManager.ViewTargetOffset = 0x1FE0 |
| NOT-SHIPPED-DUMP | ArcOffsets::PlayerState::START_TIME | 0x3CC | Engine.PlayerState.StartTime | 0x3CC | - | - | nothing ships this; the dump reflects Engine.PlayerState.StartTime = 0x3CC |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessComponent::BLEND_RADIUS | 0xB14 | Engine.PostProcessComponent.BlendRadius | 0xB14 | - | - | nothing ships this; the dump reflects Engine.PostProcessComponent.BlendRadius = 0xB14 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessComponent::BLEND_WEIGHT | 0xB18 | Engine.PostProcessComponent.BlendWeight | 0xB18 | - | - | nothing ships this; the dump reflects Engine.PostProcessComponent.BlendWeight = 0xB18 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessComponent::B_ENABLED | 0xB1C | Engine.PostProcessComponent.bEnabled | 0xB1C | - | - | nothing ships this; the dump reflects Engine.PostProcessComponent.bEnabled = 0xB1C |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessComponent::PRIORITY | 0xB10 | Engine.PostProcessComponent.Priority | 0xB10 | - | - | nothing ships this; the dump reflects Engine.PostProcessComponent.Priority = 0xB10 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessComponent::SETTINGS | 0x380 | Engine.PostProcessComponent.Settings | 0x380 | - | - | nothing ships this; the dump reflects Engine.PostProcessComponent.Settings = 0x380 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessVolume::BLEND_RADIUS | 0xB94 | Engine.PostProcessVolume.BlendRadius | 0xB94 | - | - | nothing ships this; the dump reflects Engine.PostProcessVolume.BlendRadius = 0xB94 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessVolume::BLEND_WEIGHT | 0xB98 | Engine.PostProcessVolume.BlendWeight | 0xB98 | - | - | nothing ships this; the dump reflects Engine.PostProcessVolume.BlendWeight = 0xB98 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessVolume::B_ENABLED | 0xB9C | Engine.PostProcessVolume.bEnabled | 0xB9C | - | - | nothing ships this; the dump reflects Engine.PostProcessVolume.bEnabled = 0xB9C |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessVolume::PRIORITY | 0xB90 | Engine.PostProcessVolume.Priority | 0xB90 | - | - | nothing ships this; the dump reflects Engine.PostProcessVolume.Priority = 0xB90 |
| NOT-SHIPPED-DUMP | ArcOffsets::PostProcessVolume::SETTINGS | 0x400 | Engine.PostProcessVolume.Settings | 0x400 | - | - | nothing ships this; the dump reflects Engine.PostProcessVolume.Settings = 0x400 |
| NOT-SHIPPED-DUMP | ArcOffsets::PrimitiveComponent::BODY_INSTANCE | 0x4D0 | Engine.PrimitiveComponent.BodyInstance | 0x4D0 | - | - | nothing ships this; the dump reflects Engine.PrimitiveComponent.BodyInstance = 0x4D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SceneComponent::RELATIVE_SCALE3D | 0x298 | Engine.SceneComponent.RelativeScale3D | 0x298 | - | - | nothing ships this; the dump reflects Engine.SceneComponent.RelativeScale3D = 0x298 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMesh::PHYSICS_ASSET | 0x308 | Engine.SkeletalMesh.PhysicsAsset | 0x308 | - | - | nothing ships this; the dump reflects Engine.SkeletalMesh.PhysicsAsset = 0x308 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMeshComponent::ANIM_SCRIPT_INSTANCE | 0xB38 | Engine.SkeletalMeshComponent.AnimScriptInstance | 0xB38 | - | - | nothing ships this; the dump reflects Engine.SkeletalMeshComponent.AnimScriptInstance = 0xB38 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMeshComponent::B_PAUSE_ANIMS | 0xCC0 | Engine.SkeletalMeshComponent.bPauseAnims | 0xCC0 | - | - | nothing ships this; the dump reflects Engine.SkeletalMeshComponent.bPauseAnims = 0xCC0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMeshComponent::B_PAUSE_ANIMS_MASK | 0x10 | Engine.SkeletalMeshComponent.bPauseAnims mask | 0x10 | - | - | nothing ships this; the dump reflects Engine.SkeletalMeshComponent.bPauseAnims mask = 0x10 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMeshComponent::CACHED_BONE_SPACE_TRANSFORMS | 0xBE8 | Engine.SkeletalMeshComponent.CachedBoneSpaceTransforms | 0xBE8 | - | - | nothing ships this; the dump reflects Engine.SkeletalMeshComponent.CachedBoneSpaceTransforms = 0xBE8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMeshComponent::CACHED_COMPONENT_SPACE_TRANSFORMS | 0xBF8 | Engine.SkeletalMeshComponent.CachedComponentSpaceTransforms | 0xBF8 | - | - | nothing ships this; the dump reflects Engine.SkeletalMeshComponent.CachedComponentSpaceTransforms = 0xBF8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkeletalMeshComponent::LAST_POSE_TICK_FRAME | 0x1148 | Engine.SkeletalMeshComponent.LastPoseTickFrame | 0x1148 | - | - | nothing ships this; the dump reflects Engine.SkeletalMeshComponent.LastPoseTickFrame = 0x1148 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::AERIAL_PERSPECTIVE_START_DEPTH | 0x41C | Engine.SkyAtmosphereComponent.AerialPerspectiveStartDepth | 0x41C | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.AerialPerspectiveStartDepth = 0x41C |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::ATMOSPHERE_HEIGHT | 0x37C | Engine.SkyAtmosphereComponent.AtmosphereHeight | 0x37C | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.AtmosphereHeight = 0x37C |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::BOTTOM_RADIUS | 0x374 | Engine.SkyAtmosphereComponent.BottomRadius | 0x374 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.BottomRadius = 0x374 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::GROUND_ALBEDO | 0x378 | Engine.SkyAtmosphereComponent.GroundAlbedo | 0x378 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.GroundAlbedo = 0x378 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::HEIGHT_FOG_CONTRIBUTION | 0x414 | Engine.SkyAtmosphereComponent.HeightFogContribution | 0x414 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.HeightFogContribution = 0x414 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MIE_ABSORPTION | 0x3B8 | Engine.SkyAtmosphereComponent.MieAbsorption | 0x3B8 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MieAbsorption = 0x3B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MIE_ABSORPTION_SCALE | 0x3B4 | Engine.SkyAtmosphereComponent.MieAbsorptionScale | 0x3B4 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MieAbsorptionScale = 0x3B4 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MIE_ANISOTROPY | 0x3C8 | Engine.SkyAtmosphereComponent.MieAnisotropy | 0x3C8 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MieAnisotropy = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MIE_EXPONENTIAL_DISTRIBUTION | 0x3CC | Engine.SkyAtmosphereComponent.MieExponentialDistribution | 0x3CC | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MieExponentialDistribution = 0x3CC |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MIE_SCATTERING | 0x3A4 | Engine.SkyAtmosphereComponent.MieScattering | 0x3A4 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MieScattering = 0x3A4 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MIE_SCATTERING_SCALE | 0x3A0 | Engine.SkyAtmosphereComponent.MieScatteringScale | 0x3A0 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MieScatteringScale = 0x3A0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::MULTI_SCATTERING_FACTOR | 0x380 | Engine.SkyAtmosphereComponent.MultiScatteringFactor | 0x380 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.MultiScatteringFactor = 0x380 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::OTHER_ABSORPTION | 0x3D4 | Engine.SkyAtmosphereComponent.OtherAbsorption | 0x3D4 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.OtherAbsorption = 0x3D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::OTHER_ABSORPTION_SCALE | 0x3D0 | Engine.SkyAtmosphereComponent.OtherAbsorptionScale | 0x3D0 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.OtherAbsorptionScale = 0x3D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::RAYLEIGH_EXPONENTIAL_DISTRIBUTION | 0x39C | Engine.SkyAtmosphereComponent.RayleighExponentialDistribution | 0x39C | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.RayleighExponentialDistribution = 0x39C |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::RAYLEIGH_SCATTERING | 0x38C | Engine.SkyAtmosphereComponent.RayleighScattering | 0x38C | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.RayleighScattering = 0x38C |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::RAYLEIGH_SCATTERING_SCALE | 0x388 | Engine.SkyAtmosphereComponent.RayleighScatteringScale | 0x388 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.RayleighScatteringScale = 0x388 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::SKY_LUMINANCE_FACTOR | 0x3F0 | Engine.SkyAtmosphereComponent.SkyLuminanceFactor | 0x3F0 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.SkyLuminanceFactor = 0x3F0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::TRACE_SAMPLE_COUNT_SCALE | 0x384 | Engine.SkyAtmosphereComponent.TraceSampleCountScale | 0x384 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.TraceSampleCountScale = 0x384 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::TRANSFORM_MODE | 0x370 | Engine.SkyAtmosphereComponent.TransformMode | 0x370 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.TransformMode = 0x370 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyAtmosphereComponent::TRANSMITTANCE_MIN_LIGHT_ELEVATION_ANGLE | 0x418 | Engine.SkyAtmosphereComponent.TransmittanceMinLightElevationAngle | 0x418 | - | - | nothing ships this; the dump reflects Engine.SkyAtmosphereComponent.TransmittanceMinLightElevationAngle = 0x418 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::BLEND_DESTINATION_CUBEMAP | 0x4C8 | Engine.SkyLightComponent.BlendDestinationCubemap | 0x4C8 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.BlendDestinationCubemap = 0x4C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::B_CAPTURE_EMISSIVE_ONLY | 0x3DC | Engine.SkyLightComponent.bCaptureEmissiveOnly | 0x3DC | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.bCaptureEmissiveOnly = 0x3DC |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::B_CLOUD_AMBIENT_OCCLUSION | 0x404 | Engine.SkyLightComponent.bCloudAmbientOcclusion | 0x404 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.bCloudAmbientOcclusion = 0x404 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::B_LOWER_HEMISPHERE_IS_BLACK | 0x3DD | Engine.SkyLightComponent.bLowerHemisphereIsBlack | 0x3DD | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.bLowerHemisphereIsBlack = 0x3DD |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::B_REAL_TIME_CAPTURE | 0x3B8 | Engine.SkyLightComponent.bRealTimeCapture | 0x3B8 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.bRealTimeCapture = 0x3B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_APERTURE_SCALE | 0x414 | Engine.SkyLightComponent.CloudAmbientOcclusionApertureScale | 0x414 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.CloudAmbientOcclusionApertureScale = 0x414 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_EXTENT | 0x40C | Engine.SkyLightComponent.CloudAmbientOcclusionExtent | 0x40C | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.CloudAmbientOcclusionExtent = 0x40C |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_MAP_RESOLUTION_SCALE | 0x410 | Engine.SkyLightComponent.CloudAmbientOcclusionMapResolutionScale | 0x410 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.CloudAmbientOcclusionMapResolutionScale = 0x410 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_STRENGTH | 0x408 | Engine.SkyLightComponent.CloudAmbientOcclusionStrength | 0x408 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.CloudAmbientOcclusionStrength = 0x408 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CONTRAST | 0x3F4 | Engine.SkyLightComponent.Contrast | 0x3F4 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.Contrast = 0x3F4 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CUBEMAP | 0x3C0 | Engine.SkyLightComponent.Cubemap | 0x3C0 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.Cubemap = 0x3C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::CUBEMAP_RESOLUTION | 0x3CC | Engine.SkyLightComponent.CubemapResolution | 0x3CC | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.CubemapResolution = 0x3CC |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::LOWER_HEMISPHERE_COLOR | 0x3E0 | Engine.SkyLightComponent.LowerHemisphereColor | 0x3E0 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.LowerHemisphereColor = 0x3E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::MIN_OCCLUSION | 0x3FC | Engine.SkyLightComponent.MinOcclusion | 0x3FC | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.MinOcclusion = 0x3FC |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::OCCLUSION_COMBINE_MODE | 0x418 | Engine.SkyLightComponent.OcclusionCombineMode | 0x418 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.OcclusionCombineMode = 0x418 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::OCCLUSION_EXPONENT | 0x3F8 | Engine.SkyLightComponent.OcclusionExponent | 0x3F8 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.OcclusionExponent = 0x3F8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::OCCLUSION_MAX_DISTANCE | 0x3F0 | Engine.SkyLightComponent.OcclusionMaxDistance | 0x3F0 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.OcclusionMaxDistance = 0x3F0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::OCCLUSION_TINT | 0x400 | Engine.SkyLightComponent.OcclusionTint | 0x400 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.OcclusionTint = 0x400 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::REFLECTION_CAPTURE_SKY_DISTANCE_THRESHOLD | 0x3D8 | Engine.SkyLightComponent.ReflectionCaptureSkyDistanceThreshold | 0x3D8 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.ReflectionCaptureSkyDistanceThreshold = 0x3D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::SKY_DISTANCE_THRESHOLD | 0x3D0 | Engine.SkyLightComponent.SkyDistanceThreshold | 0x3D0 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.SkyDistanceThreshold = 0x3D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::SOURCE_CUBEMAP_ANGLE | 0x3C8 | Engine.SkyLightComponent.SourceCubemapAngle | 0x3C8 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.SourceCubemapAngle = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::SkyLightComponent::SOURCE_TYPE | 0x3B9 | Engine.SkyLightComponent.SourceType | 0x3B9 | - | - | nothing ships this; the dump reflects Engine.SkyLightComponent.SourceType = 0x3B9 |
| NOT-SHIPPED-DUMP | ArcOffsets::TViewTarget::POV | 0x10 | Engine.TViewTarget.POV | 0x10 | - | - | nothing ships this; the dump reflects Engine.TViewTarget.POV = 0x10 |
| NOT-SHIPPED-DUMP | ArcOffsets::TViewTarget::TARGET | 0x0 | Engine.TViewTarget.Target | 0x0 | - | - | nothing ships this; the dump reflects Engine.TViewTarget.Target = 0x0 |
| NOT-SHIPPED-DUMP | ArcOffsets::UStruct::CHILDREN | 0xB8 | CoreUObject.Struct.Children | 0xB8 | - | - | nothing ships this; the dump reflects CoreUObject.Struct.Children = 0xB8 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::B_USE_PER_SAMPLE_ATMOSPHERIC_LIGHT_TRANSMITTANCE | 0x3B8 | Engine.VolumetricCloudComponent.bUsePerSampleAtmosphericLightTransmittance | 0x3B8 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.bUsePerSampleAtmosphericLightTransmittance = 0x3B8 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::GROUND_ALBEDO | 0x38C | Engine.VolumetricCloudComponent.GroundAlbedo | 0x38C | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.GroundAlbedo = 0x38C |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::LAYER_BOTTOM_ALTITUDE | 0x370 | Engine.VolumetricCloudComponent.LayerBottomAltitude | 0x370 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.LayerBottomAltitude = 0x370 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::LAYER_HEIGHT | 0x374 | Engine.VolumetricCloudComponent.LayerHeight | 0x374 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.LayerHeight = 0x374 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::MATERIAL | 0x390 | Engine.VolumetricCloudComponent.Material | 0x390 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.Material = 0x390 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::PLANET_RADIUS | 0x388 | Engine.VolumetricCloudComponent.PlanetRadius | 0x388 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.PlanetRadius = 0x388 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::REFLECTION_SAMPLE_COUNT_SCALE | 0x3CC | Engine.VolumetricCloudComponent.ReflectionSampleCountScale | 0x3CC | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ReflectionSampleCountScale = 0x3CC |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::REFLECTION_VIEW_SAMPLE_COUNT_SCALE | 0x3C8 | Engine.VolumetricCloudComponent.ReflectionViewSampleCountScale | 0x3C8 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ReflectionViewSampleCountScale = 0x3C8 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::REFLECTION_VIEW_SAMPLE_COUNT_SCALE_VALUE | 0x3C4 | Engine.VolumetricCloudComponent.ReflectionViewSampleCountScaleValue | 0x3C4 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ReflectionViewSampleCountScaleValue = 0x3C4 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::SHADOW_REFLECTION_SAMPLE_COUNT_SCALE | 0x3DC | Engine.VolumetricCloudComponent.ShadowReflectionSampleCountScale | 0x3DC | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ShadowReflectionSampleCountScale = 0x3DC |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::SHADOW_REFLECTION_VIEW_SAMPLE_COUNT_SCALE | 0x3D8 | Engine.VolumetricCloudComponent.ShadowReflectionViewSampleCountScale | 0x3D8 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ShadowReflectionViewSampleCountScale = 0x3D8 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::SHADOW_REFLECTION_VIEW_SAMPLE_COUNT_SCALE_VALUE | 0x3D4 | Engine.VolumetricCloudComponent.ShadowReflectionViewSampleCountScaleValue | 0x3D4 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ShadowReflectionViewSampleCountScaleValue = 0x3D4 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::SHADOW_TRACING_DISTANCE | 0x3E0 | Engine.VolumetricCloudComponent.ShadowTracingDistance | 0x3E0 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ShadowTracingDistance = 0x3E0 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::SHADOW_VIEW_SAMPLE_COUNT_SCALE | 0x3D0 | Engine.VolumetricCloudComponent.ShadowViewSampleCountScale | 0x3D0 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ShadowViewSampleCountScale = 0x3D0 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::SKY_LIGHT_CLOUD_BOTTOM_OCCLUSION | 0x3BC | Engine.VolumetricCloudComponent.SkyLightCloudBottomOcclusion | 0x3BC | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.SkyLightCloudBottomOcclusion = 0x3BC |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::STOP_TRACING_TRANSMITTANCE_THRESHOLD | 0x3E4 | Engine.VolumetricCloudComponent.StopTracingTransmittanceThreshold | 0x3E4 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.StopTracingTransmittanceThreshold = 0x3E4 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::TRACING_MAX_DISTANCE | 0x384 | Engine.VolumetricCloudComponent.TracingMaxDistance | 0x384 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.TracingMaxDistance = 0x384 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::TRACING_MAX_DISTANCE_MODE | 0x380 | Engine.VolumetricCloudComponent.TracingMaxDistanceMode | 0x380 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.TracingMaxDistanceMode = 0x380 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::TRACING_START_MAX_DISTANCE | 0x378 | Engine.VolumetricCloudComponent.TracingStartMaxDistance | 0x378 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.TracingStartMaxDistance = 0x378 |
| NOT-SHIPPED-DUMP | ArcOffsets::VolumetricCloudComponent::VIEW_SAMPLE_COUNT_SCALE | 0x3C0 | Engine.VolumetricCloudComponent.ViewSampleCountScale | 0x3C0 | - | - | nothing ships this; the dump reflects Engine.VolumetricCloudComponent.ViewSampleCountScale = 0x3C0 |
| NOT-SHIPPED-DUMP | ArcOffsets::WorldPartitionMiniMap::MINIMAP_TEXTURE | 0x420 | Engine.WorldPartitionMiniMap.MiniMapTexture | 0x420 | - | - | nothing ships this; the dump reflects Engine.WorldPartitionMiniMap.MiniMapTexture = 0x420 |
| NOT-SHIPPED-DROP | ArcOffsets::AIController::ACTIONS_COMP | 0x4B0 | - | - | - | - | drop only (drop); no AIController.*.ACTIONS_COMP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::AIController::PATH_FOLLOWING | 0x498 | - | - | - | - | drop only (drop); no AIController.*.PATH_FOLLOWING in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::AISenseConfigSight::AUTO_SUCCESS_RANGE_FROM_LAST_SEEN | 0xD0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::AISenseConfigSight::DETECTION_BY_AFFILIATION | 0xCC | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::AISenseConfigSight::MINIMUM_SIGHT_RADIUS | 0xD0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::AISenseConfigSight::NEAR_CLIPPING_RADIUS | 0xD8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::AISenseConfigSight::POINT_OF_VIEW_BACKWARD_OFFSET | 0xD4 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Actor::LAST_RENDER_ON_SCREEN_FLOAT | 0x1FC | - | - | - | - | drop only (drop); no Actor.*.LAST_RENDER_ON_SCREEN_FLOAT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Actor::LAST_RENDER_TIME_HINT | 0x1E0 | - | - | - | - | drop only (drop); no Actor.*.LAST_RENDER_TIME_HINT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Actor::LAST_SUBMIT_ON_SCREEN | 0x1F8 | - | - | - | - | drop only (drop); no Actor.*.LAST_SUBMIT_ON_SCREEN in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::BASE | 0x140000000 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::BodySetup::BONE_NAME | 0x98 | - | - | - | - | drop only (drop); no BodySetup.*.BONE_NAME in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Character::B_IS_CROUCHED_BYTE | 0x570 | - | - | - | - | drop only (drop); no Character.*.B_IS_CROUCHED_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ConstructableBase::STATE_INTERPOLATOR | 0x540 | - | - | - | - | drop only (drop); no ConstructableBase.*.STATE_INTERPOLATOR in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ConstructableStaticMeshStyle::PART_TAGS | 0x7C0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ConstructableStaticMeshStyle::RESISTANCE_GROUP | 0x7F8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::DirectionalLightComponent::ATMOSPHERE_FLAGS_BYTE | 0x578 | - | - | - | - | drop only (drop); no DirectionalLightComponent.*.ATMOSPHERE_FLAGS_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::DirectionalLightComponent::B_USE_INSET_SHADOWS_FOR_MOVABLE | 0x554 | - | - | - | - | drop only (drop); no DirectionalLightComponent.*.B_USE_INSET_SHADOWS_FOR_MOVABLE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::DirectionalLightComponent::CLOUD_FLAGS_BYTE | 0x590 | - | - | - | - | drop only (drop); no DirectionalLightComponent.*.CLOUD_FLAGS_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::DirectionalLightComponent::DYNAMIC_SHADOW_DISTANCE_MOVABLE | 0x53C | - | - | - | - | drop only (drop); no DirectionalLightComponent.*.DYNAMIC_SHADOW_DISTANCE_MOVABLE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::DirectionalLightComponent::DYNAMIC_SHADOW_DISTANCE_STATIONARY | 0x540 | - | - | - | - | drop only (drop); no DirectionalLightComponent.*.DYNAMIC_SHADOW_DISTANCE_STATIONARY in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::EmbarkGameStateBase::REPLICATED_REALTIME_WORLD_TIME | 0x590 | - | - | - | - | drop only (drop); no EmbarkGameStateBase.*.REPLICATED_REALTIME_WORLD_TIME in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::EmbarkGameStateBase::REPLICATED_WORLD_TIME_SECONDS_DOUBLE | 0x498 | - | - | - | - | drop only (drop); no EmbarkGameStateBase.*.REPLICATED_WORLD_TIME_SECONDS_DOUBLE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_START_DIST | 0x47C | - | - | - | - | drop only (drop); no ExponentialHeightFogComponent.*.DIRECTIONAL_INSCATTERING_START_DIST in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExponentialHeightFogComponent::FOG_INSCATTERING_COLOR_FADE_MULT | 0x3E8 | - | - | - | - | drop only (drop); no ExponentialHeightFogComponent.*.FOG_INSCATTERING_COLOR_FADE_MULT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExponentialHeightFogComponent::FULLY_DIRECTIONAL_INSCATTERING_COLOR_DIST | 0x43C | - | - | - | - | drop only (drop); no ExponentialHeightFogComponent.*.FULLY_DIRECTIONAL_INSCATTERING_COLOR_DIST in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExponentialHeightFogComponent::NON_DIRECTIONAL_INSCATTERING_COLOR_DIST | 0x440 | - | - | - | - | drop only (drop); no ExponentialHeightFogComponent.*.NON_DIRECTIONAL_INSCATTERING_COLOR_DIST in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExponentialHeightFogComponent::SKY_ATMOSPHERE_AMBIENT_CONTRIB_COLOR_SCALE | 0x40C | - | - | - | - | drop only (drop); no ExponentialHeightFogComponent.*.SKY_ATMOSPHERE_AMBIENT_CONTRIB_COLOR_SCALE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExtractionPoint::EXTRACTION_STARTED_TS | 0xB58 | - | - | - | - | drop only (drop); no ExtractionPoint.*.EXTRACTION_STARTED_TS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExtractionPoint::EXTRACTION_TIME | 0xB60 | - | - | - | - | drop only (drop); no ExtractionPoint.*.EXTRACTION_TIME in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::ExtractionPoint::IS_ENABLED | 0xBD0 | - | - | - | - | drop only (drop); no ExtractionPoint.*.IS_ENABLED in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FBodyInstance::ACTOR_HANDLE_ACTOR | 0x158 | - | - | - | - | drop only (drop); no FBodyInstance.*.ACTOR_HANDLE_ACTOR in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::ARRAY_DIM | 0xA8 | - | - | - | - | drop only (drop-fresh); no FField.*.ARRAY_DIM in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::BOOL_BYTE_MASK | 0x11A | - | - | - | - | drop only (drop-fresh); no FField.*.BOOL_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::BOOL_BYTE_OFFSET | 0x119 | - | - | - | - | drop only (drop-fresh); no FField.*.BOOL_BYTE_OFFSET in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::BOOL_FIELD_MASK | 0x11B | - | - | - | - | drop only (drop-fresh); no FField.*.BOOL_FIELD in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::BOOL_FIELD_SIZE | 0x118 | - | - | - | - | drop only (drop-fresh); no FField.*.BOOL_FIELD_SIZE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::ELEMENT_SIZE | 0xA4 | - | - | - | - | drop only (drop-fresh); no FField.*.ELEMENT_SIZE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::FNAME_ENC | 0x90 | - | - | - | - | drop only (drop-fresh); no FField.*.FNAME_ENC in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::MAP_KEY_PROP | 0x118 | - | - | - | - | drop only (drop-fresh); no FField.*.MAP_KEY_PROP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::MAP_VALUE_PROP | 0x120 | - | - | - | - | drop only (drop-fresh); no FField.*.MAP_VALUE_PROP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::OBJ_PROP_CLASS | 0x118 | - | - | - | - | drop only (drop-fresh); no FField.*.OBJ_PROP_CLASS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::OFFSET_ENC | 0xBC | - | - | - | - | drop only (drop-fresh); no FField.*.OFFSET_ENC in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FField::PROPERTY_FLAGS | 0xB0 | - | - | - | - | drop only (drop-fresh); no FField.*.PROPERTY_FLAGS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FFieldCrypto::FFIELD_ADD2_CONST | 0xBB34B0C01E5AA000 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FFieldCrypto::FFIELD_ADD_CONST | 0x44CB31F912F95FFF | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FFieldCrypto::OFFSET_XOR_KEY | 0x8DE128DE | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FHeightfieldRef::RB_HEIGHTFIELD | 0x30 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FHeightfieldRef::RB_HEIGHTFIELD_SIMPLE | 0x38 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FHeightfieldRef::REFCOUNT | 0x8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FPostProcessSettings::OVERRIDE_FLAGS_BEGIN | 0x0 | - | - | - | - | drop only (drop); no FPostProcessSettings.*.OVERRIDE_FLAGS_BEGIN in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FPostProcessSettings::OVERRIDE_FLAGS_END | 0x34 | - | - | - | - | drop only (drop); no FPostProcessSettings.*.OVERRIDE_FLAGS_END in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::FPostProcessSettings::RAY_TRACING_GI_TYPE | 0x418 | - | - | - | - | drop only (drop); no FPostProcessSettings.*.RAY_TRACING_GI_TYPE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GameInstanceDecrypt::MAX_VALID_POINTER | 0x7FFFFFFFFFFF | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GameInstanceDecrypt::MIN_VALID_POINTER | 0x1000 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GameState::SERVER_WORLD_TIME_DELTA_SECONDS | 0x520 | - | - | - | - | drop only (drop-fresh); no GameState.*.SERVER_WORLD_TIME_DELTA_SECONDS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GameState::SERVER_WORLD_TIME_SECONDS_UPDATE_FREQ | 0x524 | - | - | - | - | drop only (drop-fresh); no GameState.*.SERVER_WORLD_TIME_SECONDS_UPDATE_FREQ in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GameViewportClient::SIZE | 0x6E0 | - | - | - | - | drop only (drop); no GameViewportClient.*.SIZE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GeometryCollection::GCA_GC_COMPONENT | 0x3B0 | - | - | - | - | drop only (drop); no GeometryCollection.*.GCA_GC_COMPONENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GeometryCollection::GCC_DUMMY_BODY_SETUP | 0xD10 | - | - | - | - | drop only (drop); no GeometryCollection.*.GCC_DUMMY_BODY_SETUP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GeometryCollection::GCC_REST_COLLECTION | 0x810 | - | - | - | - | drop only (drop); no GeometryCollection.*.GCC_REST_COLLECTION in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GeometryCollection::GCC_REST_TRANSFORMS | 0x918 | - | - | - | - | drop only (drop); no GeometryCollection.*.GCC_REST_TRANSFORMS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GeometryCollection::GCC_SIMULATING | 0x828 | - | - | - | - | drop only (drop); no GeometryCollection.*.GCC_SIMULATING in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Global::UWORLD_BASE_RVA | 0x10839A98 | - | - | - | - | drop only (drop-fresh); no Global.*.UWORLD_BASE_RVA in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::AABB | 0x38 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::FLAGS | 0x5C | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::FLAG_16BIT_INDICES | 0x2 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::NB_TRIANGLES | 0x24 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::NB_VERTICES | 0x20 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::TRIANGLES | 0x30 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::GuTriangleMesh::VERTICES | 0x28 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::HealthComponent::CURRENT_ARMOR | 0x190 | - | - | - | - | drop only (drop); no HealthComponent.*.CURRENT_ARMOR in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::HealthComponent::CURRENT_HEALTH | 0x6D0 | - | - | - | - | drop only (drop); no HealthComponent.*.CURRENT_HEALTH in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::HealthComponent::MAX_ARMOR | 0x198 | - | - | - | - | drop only (drop); no HealthComponent.*.MAX_ARMOR in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::HealthService::ABILITY_ON_DAMAGE_DEFS | 0x3C0 | - | - | - | - | drop only (drop); no HealthService.*.ABILITY_ON_DAMAGE_DEFS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::HealthService::CHAIN_DESTRUCTION_DEFS | 0x3B0 | - | - | - | - | drop only (drop); no HealthService.*.CHAIN_DESTRUCTION_DEFS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::HealthService::EFFECT_ON_DAMAGE_DEFS | 0x3D0 | - | - | - | - | drop only (drop); no HealthService.*.EFFECT_ON_DAMAGE_DEFS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::InteractQuestComponent::PLAYERS_WITH_INTERACT_ACCESS | 0x248 | - | - | - | - | drop only (drop); no InteractQuestComponent.*.PLAYERS_WITH_INTERACT_ACCESS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LandscapeHeightfieldCollisionComponent::HEIGHTFIELD_REF | 0x788 | - | - | - | - | drop only (drop); no LandscapeHeightfieldCollisionComponent.*.HEIGHTFIELD_REF in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LandscapeHeightfieldCollisionComponent::HEIGHT_DATA_ARRAY | 0x760 | - | - | - | - | drop only (drop); no LandscapeHeightfieldCollisionComponent.*.HEIGHT_DATA_ARRAY in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Level::ACTOR_MAX | 0x11C | - | - | - | - | drop only (drop); no Level.*.ACTOR_MAX in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LevelCollection::STRUCT_SIZE | 0x78 | - | - | - | - | drop only (drop); no LevelCollection.*.STRUCT_SIZE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LightComponent::SHADOW_FLAGS_BYTE | 0x448 | - | - | - | - | drop only (drop); no LightComponent.*.SHADOW_FLAGS_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LightComponentBase::B_AFFECT_REFLECTION_BYTE | 0x3E4 | - | - | - | - | drop only (drop); no LightComponentBase.*.B_AFFECT_REFLECTION_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LightComponentBase::B_FLAGS_BYTE | 0x3EC | - | - | - | - | drop only (drop); no LightComponentBase.*.B_FLAGS_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LightComponentBase::B_FLAGS_BYTE_2 | 0x3ED | - | - | - | - | drop only (drop); no LightComponentBase.*.B_FLAGS_BYTE_2 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LocalPlayer::PENDING_LEVEL_PC_CLASS | 0x230 | - | - | - | - | drop only (drop); no LocalPlayer.*.PENDING_LEVEL_PC_CLASS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LocalPlayer::SIZE | 0x440 | - | - | - | - | drop only (drop); no LocalPlayer.*.SIZE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::LootInteractionComponent::BYTE_HAS_BEEN_OPENED | 0x8D8 | - | - | - | - | drop only (drop); no LootInteractionComponent.*.BYTE_HAS_BEEN_OPENED in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MapWidgetMappings::FALLBACK_WIDGET | 0xF0 | - | - | - | - | drop only (drop); no MapWidgetMappings.*.FALLBACK_WIDGET in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MeshBoneInfo::NAME | 0x0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MeshBoneInfo::PARENT_INDEX | 0x8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MeshBoneInfo::STRIDE | 0x18 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MinimalViewInfo::ORTHO_FAR_CLIP | 0x8C | - | - | - | - | drop only (drop); no MinimalViewInfo.*.ORTHO_FAR_CLIP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MinimalViewInfo::ORTHO_NEAR_CLIP | 0x88 | - | - | - | - | drop only (drop); no MinimalViewInfo.*.ORTHO_NEAR_CLIP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::MinimalViewInfo::PERSPECTIVE_NEAR_CLIP | 0x90 | - | - | - | - | drop only (drop); no MinimalViewInfo.*.PERSPECTIVE_NEAR_CLIP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pawn::HEALTH_COMPONENT | 0xDC0 | - | - | - | - | drop only (drop-fresh); no Pawn.*.HEALTH_COMPONENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pawn::MESH | 0x4A0 | - | - | - | - | drop only (drop-fresh); no Pawn.*.MESH in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::CAPABILITY_SERVICE | 0x1298 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.CAPABILITY_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::COLLISION_SERVICE | 0x1290 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.COLLISION_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::DEBRIS_SERVICE | 0x12B8 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.DEBRIS_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::HEALTH_GROUP_SERVICE | 0x12A8 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.HEALTH_GROUP_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::INVESTIGATION_COMP | 0x12F8 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.INVESTIGATION_COMP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::ITEM_CONTAINER_COMP | 0x1300 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.ITEM_CONTAINER_COMP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::PERCEPTION_SERVICE | 0x12A0 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.PERCEPTION_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::PIONEER_PING_INFO_COMP | 0x12D8 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.PIONEER_PING_INFO_COMP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::REPLICATION_SETTINGS | 0x12F0 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.REPLICATION_SETTINGS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::SKELETAL_REPLICATION_SERVICE | 0x12B0 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.SKELETAL_REPLICATION_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::SPAWN_SIZE_COMPONENT | 0x12E8 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.SPAWN_SIZE_COMPONENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::STYLE | 0x12D0 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.STYLE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::TARGETING_SERVICE | 0x1288 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.TARGETING_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerConstructablePawn::TIMER_SERVICE | 0x12C8 | - | - | - | - | drop only (drop); no PioneerConstructablePawn.*.TIMER_SERVICE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerPC::CONTEXTS_TO_DISABLE_ON_MENU_OPENED | 0x1258 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerPlayerCharacter::DBNO_COMPONENT | 0xCF0 | - | - | - | - | drop only (drop); no PioneerPlayerCharacter.*.DBNO_COMPONENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerPlayerCharacter::KILL_ASSIST_NOTIFIER_COMP | 0xE40 | - | - | - | - | drop only (drop); no PioneerPlayerCharacter.*.KILL_ASSIST_NOTIFIER_COMP in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PioneerPlayerCharacter::TEAM_COMPONENT | 0x450 | - | - | - | - | drop only (drop); no PioneerPlayerCharacter.*.TEAM_COMPONENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::CONTRAST | 0x558 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::EXPOSURE | 0x458 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::EXPOSURE_MAX | 0x748 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::EXPOSURE_MIN | 0x740 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::MOON_LIGHT_COMPONENT | 0x728 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::MOON_LIGHT_COMPONENT_DUP | 0x718 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::SATURATION | 0x4E0 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::SKY_LIGHT_COMPONENT | 0x730 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::SKY_LIGHT_COMPONENT_DUP | 0x720 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::SUN_LIGHT_COMPONENT | 0x720 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::SUN_LIGHT_COMPONENT_DUP | 0x710 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::SUN_LIGHT_INTENSITY | 0x670 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::Pioneer_Sky_C::USE_EXPOSURE_RANGE | 0x738 | - | - | - | - | drop only (drop-fresh); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerCameraManager::CAMERA_CACHE_POST_PROCESS | 0x17A0 | - | - | - | - | drop only (drop); no PlayerCameraManager.*.CAMERA_CACHE_POST_PROCESS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerCameraManager::LOCKED_FOV_ALT1 | 0x438 | - | - | - | - | drop only (drop); no PlayerCameraManager.*.LOCKED_FOV_ALT1 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerCameraManager::LOCKED_FOV_ALT2 | 0x434 | - | - | - | - | drop only (drop); no PlayerCameraManager.*.LOCKED_FOV_ALT2 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerController::ACKNOWLEDGED_PAWN | 0x418 | - | - | - | - | drop only (drop); no PlayerController.*.ACKNOWLEDGED_PAWN in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerController::CHARACTER | 0x408 | - | - | - | - | drop only (drop); no PlayerController.*.CHARACTER in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerController::CONTROL_ROTATION | 0x450 | - | - | - | - | drop only (drop); no PlayerController.*.CONTROL_ROTATION in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerController::PLAYER_STATE | 0x408 | - | - | - | - | drop only (drop); no PlayerController.*.PLAYER_STATE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerState::B_IS_A_BOT_BYTE | 0x3DA | - | - | - | - | drop only (drop); no PlayerState.*.B_IS_A_BOT_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerState::IDENTITY_HASH_1 | 0x3A0 | - | - | - | - | drop only (drop); no PlayerState.*.IDENTITY_HASH_1 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerState::IDENTITY_HASH_2 | 0x3B0 | - | - | - | - | drop only (drop); no PlayerState.*.IDENTITY_HASH_2 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerState::IDENTITY_REF_1 | 0x3B8 | - | - | - | - | drop only (drop); no PlayerState.*.IDENTITY_REF_1 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PlayerState::IDENTITY_REF_2 | 0x3C0 | - | - | - | - | drop only (drop); no PlayerState.*.IDENTITY_REF_2 in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PrimitiveComponent::BLUEPRINT_CREATED_COMPONENTS | 0x380 | - | - | - | - | drop only (drop); no PrimitiveComponent.*.BLUEPRINT_CREATED_COMPONENTS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PxHeightField::NB_COLUMNS | 0x3C | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PxHeightField::NB_ROWS | 0x38 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PxHeightField::SAMPLES_PTR | 0x50 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::PxHeightField::SAMPLE_STRIDE | 0x4 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::QuestInteractable::COMP_VALIDATE_FLOAT | 0x460 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::QuestInteractable::INTERACTION_COMP_A | 0x3D0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::QuestInteractable::INTERACTION_COMP_B | 0x4D8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SceneComponent::MOBILITY_BYTE | 0x2CC | - | - | - | - | drop only (drop-fresh); no SceneComponent.*.MOBILITY_BYTE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SceneComponent::MOBILITY_MOVABLE | 0x2 | - | - | - | - | drop only (drop); no SceneComponent.*.MOBILITY_MOVABLE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkeletalMesh::REF_SKELETON_PROBE_HI | 0x300 | - | - | - | - | drop only (drop); no SkeletalMesh.*.REF_SKELETON_PROBE_HI in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkeletalMesh::REF_SKELETON_PROBE_LO | 0x140 | - | - | - | - | drop only (drop); no SkeletalMesh.*.REF_SKELETON_PROBE_LO in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkeletalMeshComponent::BOUNDS_SCALE | 0x4B8 | - | - | - | - | drop only (drop); no SkeletalMeshComponent.*.BOUNDS_SCALE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkeletalMeshComponent::LEADER_POSE_COMPONENT | 0x750 | - | - | - | - | drop only (drop); no SkeletalMeshComponent.*.LEADER_POSE_COMPONENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkeletalMeshComponent::SKINNED_ASSET | 0x748 | - | - | - | - | drop only (drop); no SkeletalMeshComponent.*.SKINNED_ASSET in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkyAtmosphereComponent::AERIAL_PERSPECTIVE_VIEW_DISTANCE_SCALE | 0x460 | - | - | - | - | drop only (drop); no SkyAtmosphereComponent.*.AERIAL_PERSPECTIVE_VIEW_DISTANCE_SCALE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkyLightComponent::B_SHOW_ILLUMINANCE_METER | 0x46C | - | - | - | - | drop only (drop); no SkyLightComponent.*.B_SHOW_ILLUMINANCE_METER in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::SkyLightComponent::B_USE_SEPARATE_SKY_DISTANCE_FOR_REFLECTIONS | 0x424 | - | - | - | - | drop only (drop); no SkyLightComponent.*.B_USE_SEPARATE_SKY_DISTANCE_FOR_REFLECTIONS in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::ATTACH_CHILDREN | 0x1E8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::IBUFFER_BYTE_COUNT | 0x8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::IBUFFER_DATA_PTR | 0x0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::IBUFFER_IS_16BIT | 0x10 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::LOD_DEPTH_ONLY_INDEX_BUFFER | 0x88 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::LOD_INDEX_BUFFER | 0x58 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::LOD_VERTEX_BUFFERS | 0x0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::POSBUFFER_DATA | 0x8 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::POSBUFFER_NUM_VERTICES | 0x10 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::RENDER_DATA_LOD_RESOURCES | 0x0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::SMC_NAV_OBSTACLE_MASK | 0x10 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::SMC_STATIC_MESH | 0x728 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::SM_BODY_SETUP | 0x1F0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::SM_FLAG_BYTE_278 | 0x278 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::SM_SUPPORT_RAYTRACE_MASK | 0x20 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::STATIC_MESH_RENDER_DATA | 0x150 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::VBUFFERS_POSITION_BUFFER | 0x58 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StaticMeshGeometry::VBUFFER_DATA_PTR | 0x0 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StowedWeaponLayout::QUALITY_VISUAL | 0x20 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::StowedWeaponLayout::STOWED_ACTOR | 0x10 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UClass::DEFAULT_OBJECT | 0x130 | - | - | - | - | drop only (drop); no UClass.*.DEFAULT_OBJECT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UStruct::CHILD_PROPERTIES | 0xE0 | - | - | - | - | drop only (drop-fresh); no UStruct.*.CHILD_PROPERTIES in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UStruct::MIN_ALIGNMENT | 0x94 | - | - | - | - | drop only (drop); no UStruct.*.MIN_ALIGNMENT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UStruct::PROPERTIES_SIZE | 0xD8 | - | - | - | - | drop only (drop-fresh); no UStruct.*.PROPERTIES_SIZE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UWorld::GAMESTATE | 0x0 | - | - | - | - | drop only (drop); no UWorld.*.GAMESTATE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UWorld::INSTANCE_TIME | 0x240 | - | - | - | - | drop only (drop); no UWorld.*.INSTANCE_TIME in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UWorld::INSTANCE_TIME_DOUBLE_HINT | 0x240 | - | - | - | - | drop only (drop); no UWorld.*.INSTANCE_TIME_DOUBLE_HINT in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::UWorld::PHYS_SCENE | 0x5C0 | - | - | - | - | drop only (drop); no UWorld.*.PHYS_SCENE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_MIE_SCATTERING_FADE_DISTANCE | 0x434 | - | - | - | - | drop only (drop); no VolumetricCloudComponent.*.AERIAL_PERSPECTIVE_MIE_SCATTERING_FADE_DISTANCE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_MIE_SCATTERING_START_DISTANCE | 0x430 | - | - | - | - | drop only (drop); no VolumetricCloudComponent.*.AERIAL_PERSPECTIVE_MIE_SCATTERING_START_DISTANCE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_RAYLEIGH_SCATTERING_FADE_DISTANCE | 0x42C | - | - | - | - | drop only (drop); no VolumetricCloudComponent.*.AERIAL_PERSPECTIVE_RAYLEIGH_SCATTERING_FADE_DISTANCE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_RAYLEIGH_SCATTERING_START_DISTANCE | 0x428 | - | - | - | - | drop only (drop); no VolumetricCloudComponent.*.AERIAL_PERSPECTIVE_RAYLEIGH_SCATTERING_START_DISTANCE in the dump |
| NOT-SHIPPED-DROP | ArcOffsets::WineTeb::THREAD_TLS | 0x58 | - | - | - | - | drop only (drop); namespace ArcOffsets has no class in the dump |
| NOT-SHIPPED-DROP | FName::CHUNK_INDEX_MASK | 0xFFFF | - | - | - | - | drop only (drop); no FName.*.CHUNK_INDEX in the dump |
| NOT-SHIPPED-DROP | FName::CHUNK_INDEX_SHIFT | 0x10 | - | - | - | - | drop only (drop); no FName.*.CHUNK_INDEX_SHIFT in the dump |
| NOT-SHIPPED-DROP | FName::FFIELD_ADD2_CONST | 0xBB34B0C01E5AA000 | - | - | - | - | drop only (drop); no FName.*.FFIELD_ADD2_CONST in the dump |
| NOT-SHIPPED-DROP | FName::FFIELD_ADD_CONST | 0x44CB31F912F95FFF | - | - | - | - | drop only (drop); no FName.*.FFIELD_ADD_CONST in the dump |
| NOT-SHIPPED-DROP | FName::FFIELD_NAME_OFF | 0x90 | - | - | - | - | drop only (drop); no FName.*.FFIELD_NAME_OFF in the dump |
| NOT-SHIPPED-DROP | FName::FNV_PRIME_COMMON | 0x100000001B3 | - | - | - | - | drop only (drop); no FName.*.FNV_PRIME_COMMON in the dump |
| NOT-SHIPPED-DROP | FName::HASH_PRIME | 0x1000193 | - | - | - | - | drop only (drop); no FName.*.HASH_PRIME in the dump |
| NOT-SHIPPED-DROP | FName::RVA_GNAMEPOOL | 0x10987D40 | - | - | - | - | drop only (drop); no FName.*.RVA_GNAMEPOOL in the dump |
| NOT-SHIPPED-DROP | FName::RVA_KEYSTREAM | 0x1082B26C | - | - | - | - | drop only (drop); no FName.*.RVA_KEYSTREAM in the dump |
| NOT-SHIPPED-DROP | FName::SLOT_CLMUL_K1 | 0x8E9484400ADF26C1 | - | - | - | - | drop only (drop); no FName.*.SLOT_CLMUL_K1 in the dump |
| NOT-SHIPPED-DROP | FName::SLOT_CLMUL_K2 | 0x1ADA36649C975181 | - | - | - | - | drop only (drop); no FName.*.SLOT_CLMUL_K2 in the dump |
| NOT-SHIPPED-DROP | FName::UOBJ_NAME_ROL64 | 0x20 | - | - | - | - | drop only (drop); no FName.*.UOBJ_NAME_ROL64 in the dump |
| NOT-SHIPPED-DROP | FName::UOBJ_SLOT_CLASS_ADJ | 0x0 | - | - | - | - | drop only (drop); no FName.*.UOBJ_SLOT_CLASS_ADJ in the dump |
| NOT-SHIPPED-DROP | FName::UOBJ_SLOT_HASH_ADD | 0x2306CC41 | - | - | - | - | drop only (drop); no FName.*.UOBJ_SLOT_HASH_ADD in the dump |
| NOT-SHIPPED-DROP | FName::UOBJ_SLOT_NAME_XOR | 0x2 | - | - | - | - | drop only (drop); no FName.*.UOBJ_SLOT_NAME_XOR in the dump |
| NOT-SHIPPED-DROP | FName::UOBJ_SLOT_OUTER_ADJ | 0x1 | - | - | - | - | drop only (drop); no FName.*.UOBJ_SLOT_OUTER_ADJ in the dump |
| NOT-SHIPPED-DROP | GameInstanceStaticDecrypt::FNV32 | 0x1000193 | - | - | - | - | drop only (drop); namespace GameInstanceStaticDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceStaticDecrypt::FNV64 | 0x100000001B3 | - | - | - | - | drop only (drop); namespace GameInstanceStaticDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceStaticDecrypt::K1 | 0x742217C8 | - | - | - | - | drop only (drop); namespace GameInstanceStaticDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceStaticDecrypt::K2 | 0x5E838 | - | - | - | - | drop only (drop); namespace GameInstanceStaticDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceStaticDecrypt::NEG109 | 0xFFFFFF93 | - | - | - | - | drop only (drop); namespace GameInstanceStaticDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::HASH_ADD | 0x957E395C | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::KEY_FNV_ADD | 0xB92AB41238A38FDC | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::KEY_FNV_PRIME | 0x100000001B3 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::KEY_FNV_ROT_1 | 0x26 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::KEY_FNV_ROT_2 | 0x2B | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TIB_TLS_SLOTS_OFF | 0x58 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_INDEX_RVA | 0xEAF5D78 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_INIT_FLAG_OFF | 0xED0 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_KEY_TABLE_OFF | 0xEE0 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_MAX_SLOTS | 0x440 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_TABLE_STRIDE | 0x90 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_VT_FN_INDEX_OFF | 0x48 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | GameInstanceTlsDecrypt::TLS_XMM_INPUT_OFF | 0x80 | - | - | - | - | drop only (drop); namespace GameInstanceTlsDecrypt has no class in the dump |
| NOT-SHIPPED-DROP | TebDecrypt::TEB_SELF_PTR_OFF | 0x30 | - | - | - | - | drop only (drop); namespace TebDecrypt has no class in the dump |

## Drop constants not adopted by the project

727 of 929 constants in the drop are not referenced by `Offsets.h`.

| drop path | value | note |
| --- | --- | --- |
| ArcOffsets::AIController::ACTIONS_COMP | 0x4B0 | SDK CL-1341255 (was 0x488) |
| ArcOffsets::AIController::BLACKBOARD | 0x4B0 | SDK CL-1341255 (was 0x490); updated 2026-09-08 was 0x4B8; up |
| ArcOffsets::AIController::BRAIN_COMPONENT | 0x4A0 | SDK CL-1341255 (was 0x478); updated 2026-09-08 was 0x4A0; up |
| ArcOffsets::AIController::PATH_FOLLOWING | 0x498 | SDK CL-1341255 (was 0x470) |
| ArcOffsets::AIController::PERCEPTION_COMPONENT | 0x4A8 | SDK CL-1341255 (was 0x480); updated 2026-09-08 was 0x4A8; up |
| ArcOffsets::AIPerceptionComponent::AI_OWNER | 0x1E0 | SDK CL-1341255 (was 0x178); updated 2026-09-08 was 0x170; up |
| ArcOffsets::AIPerceptionComponent::DOMINANT_SENSE | 0x1C8 | SDK CL-1341255 (was 0x160); updated 2026-09-22 was 0x158 |
| ArcOffsets::AISenseConfigSight::AUTO_SUCCESS_RANGE_FROM_LAST_SEEN | 0xD0 | SDK CL-1341255 (was 0x0C8) |
| ArcOffsets::AISenseConfigSight::DETECTION_BY_AFFILIATION | 0xCC | SDK CL-1341255 (was 0x0C4) |
| ArcOffsets::AISenseConfigSight::MINIMUM_SIGHT_RADIUS | 0xD0 |  |
| ArcOffsets::AISenseConfigSight::NEAR_CLIPPING_RADIUS | 0xD8 | SDK CL-1341255 (was 0x0D0) |
| ArcOffsets::AISenseConfigSight::PERIPHERAL_VISION_DEG | 0xC8 | SDK CL-1341255 (was 0x0C0) |
| ArcOffsets::AISenseConfigSight::POINT_OF_VIEW_BACKWARD_OFFSET | 0xD4 | SDK CL-1341255 (was 0x0CC) |
| ArcOffsets::AIStateService::EMOTIONAL_STATE | 0x1C8 | uint8_t (calm/scared/angry) |
| ArcOffsets::AIStateService::FULL_SIGHT_OVERRIDE_ALERTNESS_STATES | 0x1B8 | int32 bitmask |
| ArcOffsets::AIStateService::OUT_OF_COMBAT_AIMING_BEHAVIOR | 0x1B1 | uint8_t |
| ArcOffsets::AIStateService::ROUGH_ALERTNESS_LEVEL | 0x1D0 | uint8_t |
| ArcOffsets::AIStateService::SIGHT_RANGE_STRENGTH_SCALE | 0x1B4 | float, e.g. 0.5 |
| ArcOffsets::AIStateService::TARGET | 0x1C0 | AITargetComponent* (0 = no target) |
| ArcOffsets::AIStateService::UTILITY_STATE | 0x1D4 | FGameplayTag (u64 tag hash) |
| ArcOffsets::Actor::HEALTH_COMPONENT | 0xDC0 | SDK PioneerPlayerCharacter 2026-09-22 (was 0xDD8) |
| ArcOffsets::Actor::LAST_RENDER_ON_SCREEN_FLOAT | 0x1FC |  |
| ArcOffsets::Actor::LAST_RENDER_TIME_HINT | 0x1E0 |  |
| ArcOffsets::Actor::LAST_SUBMIT_ON_SCREEN | 0x1F8 |  |
| ArcOffsets::Actor::OWNER | 0x1D8 | SDK CL-1341255 (was 0x1B8); updated 2026-09-08 was 0x1c8; up |
| ArcOffsets::Actor::ROOT_COMPONENT | 0x240 | SDK+live-verified 2026-09-22 (was 0x250) |
| ArcOffsets::BASE | 0x140000000 |  |
| ArcOffsets::BaseInteractionComponent::ACTIVE_INSTIGATOR | 0x658 | estimated +0x10 from v809 |
| ArcOffsets::BaseInteractionComponent::DBNO_TIMER_FLOATS | 0x428 | estimated +0x10 from v809 |
| ArcOffsets::BaseInteractionComponent::DBNO_TIMER_TICKS | 0x430 | estimated +0x10 from v809 |
| ArcOffsets::BaseInteractionComponent::DEFAULT_INSTIGATOR | 0x660 | estimated +0x10 from v809 |
| ArcOffsets::BaseInteractionComponent::DEFIB_TIMER_FLOATS | 0x448 | estimated +0x10 from v809 |
| ArcOffsets::BaseInteractionComponent::DEFIB_TIMER_TICKS | 0x450 | estimated +0x10 from v809 |
| ArcOffsets::BodySetup::AGG_GEOM | 0xB8 | SDK CL-1341255 (was 0xB0) |
| ArcOffsets::BodySetup::BONE_NAME | 0x98 | native, not reflected in new SDK — kept from prior live tag  |
| ArcOffsets::BodySetup::CHAOS_IMPLICIT | 0xB0 | FImplicitObject* AGG_GEOM-0x08 - recomputed (was 0xA8) — RE- |
| ArcOffsets::CapsuleComponent::CAPSULE_HALF_HEIGHT | 0x6A0 | SDK CL-1341255 (was 0x6F0, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::CapsuleComponent::CAPSULE_RADIUS | 0x6A4 | SDK CL-1341255 (was 0x6F4, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::Character::B_IS_CROUCHED_BYTE | 0x570 |  |
| ArcOffsets::Character::B_IS_CROUCHED_MASK | 0x2 | SDK 2026-09-08 (was 0x01; 0x01=bInBaseReplication) |
| ArcOffsets::Character::REPLICATED_MOVEMENT_MODE | 0x562 | uint8, EMovementMode: 0=None,1=Walking,3=Falling,4=Swim,5=Fl |
| ArcOffsets::ConstructableBase::ALL_SERVICES | 0x5F0 | SDK CL-1341255 (was 0x5D0); updated 2026-09-08 was 0x5e8; up |
| ArcOffsets::ConstructableBase::ALL_STYLE_DRIVERS | 0x610 | SDK CL-1341255 (was 0x5F0); updated 2026-09-22 was 0x608 |
| ArcOffsets::ConstructableBase::CONSTRUCTION_STREAM | 0x53C | SDK CL-1341255 (was 0x51C); updated 2026-09-08 was 0x534; up |
| ArcOffsets::ConstructableBase::INITIAL_ROTATION | 0x560 | SDK CL-1341255 (was 0x540); updated 2026-09-08 was 0x558; up |
| ArcOffsets::ConstructableBase::ON_CONSTRUCTABLE_DESTROYED_EVENT | 0x528 | SDK CL-1341255 (was 0x508); updated 2026-09-08 was 0x520; up |
| ArcOffsets::ConstructableBase::ON_CONSTRUCTION_COMPLETE | 0x4F8 | SDK CL-1341255 (was 0x4D8); updated 2026-09-08 was 0x4f0; up |
| ArcOffsets::ConstructableBase::ON_POST_FULLY_CONSTRUCTED | 0x550 | SDK CL-1341255 (was 0x530); updated 2026-09-08 was 0x548; up |
| ArcOffsets::ConstructableBase::PROXY_CONTAINER | 0x620 | SDK CL-1341255 (was 0x600); updated 2026-09-22 was 0x618 |
| ArcOffsets::ConstructableBase::STATE_INTERPOLATOR | 0x540 | SDK CL-1341255 (was 0x528) |
| ArcOffsets::ConstructableBase::bIsFullyConstructed | 0x538 | SDK CL-1341255 (was 0x518); updated 2026-09-08 was 0x530; up |
| ArcOffsets::ConstructableServiceComponentBase::B_IS_FULLY_CONSTRUCTED | 0x1B9 | SDK CL-1341255 (was 0x151); updated 2026-09-08 was 0x149; up |
| ArcOffsets::ConstructableServiceComponentBase::B_NEEDS_AS_SERVICE_PUMP | 0x1BA | SDK CL-1341255 (was 0x152); updated 2026-09-08 was 0x14a; up |
| ArcOffsets::ConstructableServiceComponentBase::B_ONLY_PUSH_CHANGED_VALUES_CLIENT | 0x1B8 | SDK CL-1341255 (was 0x150); updated 2026-09-08 was 0x148; up |
| ArcOffsets::ConstructableServiceComponentBase::OWNER_CONSTRUCTABLE | 0x1C8 | SDK CL-1341255 (was 0x160); updated 2026-09-08 was 0x158; up |
| ArcOffsets::ConstructableStaticMeshStyle::PART_TAGS | 0x7C0 | SDK CL-1341255 (was 0x7B0, +0x10) |
| ArcOffsets::ConstructableStaticMeshStyle::RESISTANCE_GROUP | 0x7F8 | SDK CL-1341255 (was 0x7E8, +0x10) |
| ArcOffsets::DirectionalLightComponent::ATMOSPHERE_FLAGS_BYTE | 0x578 |  |
| ArcOffsets::DirectionalLightComponent::ATMOSPHERE_SUN_DISK_COLOR_SCALE | 0x540 | updated 2026-09-22 was 0x580 |
| ArcOffsets::DirectionalLightComponent::ATMOSPHERE_SUN_LIGHT_INDEX | 0x53C | updated 2026-09-22 was 0x57C |
| ArcOffsets::DirectionalLightComponent::B_CAST_MODULATED_SHADOWS | 0x590 | updated 2026-09-22 was 0x5D0 |
| ArcOffsets::DirectionalLightComponent::B_ENABLE_LIGHT_SHAFT_OCCLUSION | 0x4D4 | updated 2026-09-22 was 0x514 |
| ArcOffsets::DirectionalLightComponent::B_USE_INSET_SHADOWS_FOR_MOVABLE | 0x554 |  |
| ArcOffsets::DirectionalLightComponent::CASCADE_DISTRIBUTION_EXPONENT | 0x508 | updated 2026-09-22 was 0x548 |
| ArcOffsets::DirectionalLightComponent::CASCADE_TRANSITION_FRACTION | 0x50C | updated 2026-09-22 was 0x54C |
| ArcOffsets::DirectionalLightComponent::CLOUD_FLAGS_BYTE | 0x590 |  |
| ArcOffsets::DirectionalLightComponent::CLOUD_SCATTERED_LUMINANCE_SCALE | 0x570 | updated 2026-09-22 was 0x5B0 |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_DEPTH_BIAS | 0x560 | updated 2026-09-22 was 0x5A0 |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_EXTENT | 0x564 | updated 2026-09-22 was 0x5A4 |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_MAP_RESOLUTION_SCALE | 0x568 | updated 2026-09-22 was 0x5A8 |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_ON_ATMOSPHERE_STRENGTH | 0x558 | updated 2026-09-22 was 0x598 |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_ON_SURFACE_STRENGTH | 0x55C | updated 2026-09-22 was 0x59C |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_RAY_SAMPLE_COUNT_SCALE | 0x56C | updated 2026-09-22 was 0x5AC |
| ArcOffsets::DirectionalLightComponent::CLOUD_SHADOW_STRENGTH | 0x554 | updated 2026-09-22 was 0x594 |
| ArcOffsets::DirectionalLightComponent::DISTANCE_FIELD_SHADOW_DISTANCE | 0x520 | updated 2026-09-22 was 0x560 |
| ArcOffsets::DirectionalLightComponent::DYNAMIC_SHADOW_CASCADES | 0x504 | updated 2026-09-22 was 0x544 |
| ArcOffsets::DirectionalLightComponent::DYNAMIC_SHADOW_DISTANCE_MOVABLE | 0x53C |  |
| ArcOffsets::DirectionalLightComponent::DYNAMIC_SHADOW_DISTANCE_STATIONARY | 0x540 |  |
| ArcOffsets::DirectionalLightComponent::FAR_SHADOW_CASCADE_COUNT | 0x518 | updated 2026-09-22 was 0x558 |
| ArcOffsets::DirectionalLightComponent::FAR_SHADOW_DISTANCE | 0x51C | updated 2026-09-22 was 0x55C |
| ArcOffsets::DirectionalLightComponent::FORWARD_SHADING_PRIORITY | 0x524 | updated 2026-09-22 was 0x564 |
| ArcOffsets::DirectionalLightComponent::LIGHTMASS_SETTINGS | 0x580 | updated 2026-09-22 was 0x5C0 |
| ArcOffsets::DirectionalLightComponent::LIGHT_SHAFT_OVERRIDE_DIRECTION | 0x4E0 | updated 2026-09-22 was 0x520 |
| ArcOffsets::DirectionalLightComponent::LIGHT_SOURCE_ANGLE | 0x528 | updated 2026-09-22 was 0x568 |
| ArcOffsets::DirectionalLightComponent::LIGHT_SOURCE_SOFT_ANGLE | 0x52C | updated 2026-09-22 was 0x56C |
| ArcOffsets::DirectionalLightComponent::MODULATED_SHADOW_COLOR | 0x594 | updated 2026-09-22 was 0x5D4 |
| ArcOffsets::DirectionalLightComponent::OCCLUSION_DEPTH_RANGE | 0x4DC | updated 2026-09-22 was 0x51C |
| ArcOffsets::DirectionalLightComponent::OCCLUSION_MASK_DARKNESS | 0x4D8 | updated 2026-09-22 was 0x518 |
| ArcOffsets::DirectionalLightComponent::SHADOW_AMOUNT | 0x598 | updated 2026-09-22 was 0x5D8 |
| ArcOffsets::DirectionalLightComponent::SHADOW_CASCADE_BIAS_DISTRIBUTION | 0x4D0 | SDK CL-1341255 (was 0x500, +0x10); updated 2026-09-22 was 0x |
| ArcOffsets::DirectionalLightComponent::SHADOW_DISTANCE_FADEOUT_FRACTION | 0x510 | updated 2026-09-22 was 0x550 |
| ArcOffsets::DirectionalLightComponent::SHADOW_SOURCE_ANGLE_FACTOR | 0x530 | updated 2026-09-22 was 0x570 |
| ArcOffsets::DirectionalLightComponent::TRACE_DISTANCE | 0x534 | updated 2026-09-22 was 0x574 |
| ArcOffsets::DirectionalLightComponent::WHOLE_SCENE_DYNAMIC_SHADOW_RADIUS | 0x4F8 | updated 2026-09-22 was 0x538 |
| ArcOffsets::EmbarkGameStateBase::REPLICATED_REALTIME_WORLD_TIME | 0x590 | float wall-clock seconds (non-dilated) |
| ArcOffsets::EmbarkGameStateBase::REPLICATED_WORLD_TIME_SECONDS_DOUBLE | 0x498 | authoritative match clock, seconds since start |
| ArcOffsets::EmbarkSkyActor::LIGHTING_MULTIPLIER | 0x3C8 | SDK CL-1341255 (was 0x3A8); updated 2026-09-08 was 0x3B8; up |
| ArcOffsets::EmbarkSkyActor::MOONLIGHT_INTENSITY | 0x3F8 | SDK CL-1341255 (was 0x3D8); updated 2026-09-08 was 0x3E8; up |
| ArcOffsets::EmbarkSkyActor::MOON_COLOR | 0x400 | SDK CL-1341255 (was 0x3E0); updated 2026-09-08 was 0x3F0; up |
| ArcOffsets::EmbarkSkyActor::TIME_OF_DAY | 0x3C0 | SDK CL-1341255 (was 0x3A0); updated 2026-09-08 was 0x3B0; up |
| ArcOffsets::Engine::GAME_INSTANCE | 0x12E0 | UEngine::GameInstance |
| ArcOffsets::ExponentialHeightFog::B_ENABLED | 0x3C8 | SDK CL-1341255 (was 0x3A0); updated 2026-09-08 was 0x3b8; up |
| ArcOffsets::ExponentialHeightFogComponent::B_ENABLE_VOLUMETRIC_FOG | 0x450 | updated 2026-09-08 was 0x4AC; updated 2026-09-22 was 0x490 |
| ArcOffsets::ExponentialHeightFogComponent::B_IN_SCATTERING_TEXTURE_AND_COLOR | 0x385 | updated 2026-09-08 was 0x3E5; updated 2026-09-22 was 0x3C5 |
| ArcOffsets::ExponentialHeightFogComponent::B_OVERRIDE_LIGHT_COLORS_WITH_FOG_INSCATTERING_COLORS | 0x480 | updated 2026-09-08 was 0x4DC; updated 2026-09-22 was 0x4C0 |
| ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_COLOR | 0x420 | updated 2026-09-08 was 0x480; updated 2026-09-22 was 0x460 |
| ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_EXPONENT | 0x418 | updated 2026-09-08 was 0x478; updated 2026-09-22 was 0x458 |
| ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_LUMINANCE | 0x430 | updated 2026-09-08 was 0x490; updated 2026-09-22 was 0x470 |
| ArcOffsets::ExponentialHeightFogComponent::DIRECTIONAL_INSCATTERING_START_DIST | 0x47C |  |
| ArcOffsets::ExponentialHeightFogComponent::FOG_CUTOFF_DISTANCE | 0x44C | updated 2026-09-08 was 0x4A8; updated 2026-09-22 was 0x48C |
| ArcOffsets::ExponentialHeightFogComponent::FOG_DENSITY | 0x370 | SDK CL-1341255 (was 0x3C0, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::ExponentialHeightFogComponent::FOG_HEIGHT_FALLOFF | 0x374 | updated 2026-09-08 was 0x3D4; updated 2026-09-22 was 0x3B4 |
| ArcOffsets::ExponentialHeightFogComponent::FOG_INSCATTERING_COLOR | 0x38C | updated 2026-09-08 was 0x3EC; updated 2026-09-22 was 0x3CC |
| ArcOffsets::ExponentialHeightFogComponent::FOG_INSCATTERING_COLOR_FADE_MULT | 0x3E8 |  |
| ArcOffsets::ExponentialHeightFogComponent::FOG_INSCATTERING_LUMINANCE | 0x39C | updated 2026-09-08 was 0x3FC; updated 2026-09-22 was 0x3DC |
| ArcOffsets::ExponentialHeightFogComponent::FOG_MAX_OPACITY | 0x440 | updated 2026-09-08 was 0x4A0; updated 2026-09-22 was 0x480 |
| ArcOffsets::ExponentialHeightFogComponent::FULLY_DIRECTIONAL_INSCATTERING_COLOR_DIST | 0x43C |  |
| ArcOffsets::ExponentialHeightFogComponent::INSCATTERING_COLOR_CUBEMAP | 0x3C0 | updated 2026-09-08 was 0x420; updated 2026-09-22 was 0x400 |
| ArcOffsets::ExponentialHeightFogComponent::INSCATTERING_COLOR_CUBEMAP_ANGLE | 0x3C8 | updated 2026-09-08 was 0x428; updated 2026-09-22 was 0x408 |
| ArcOffsets::ExponentialHeightFogComponent::INSCATTERING_TEXTURE_TINT | 0x3CC | updated 2026-09-08 was 0x42C; updated 2026-09-22 was 0x40C |
| ArcOffsets::ExponentialHeightFogComponent::INVERT_SECOND_FOG_DIRECTION | 0x384 | updated 2026-09-08 was 0x3E4; updated 2026-09-22 was 0x3C4 |
| ArcOffsets::ExponentialHeightFogComponent::NON_DIRECTIONAL_INSCATTERING_COLOR_DIST | 0x440 |  |
| ArcOffsets::ExponentialHeightFogComponent::SECOND_FOG_DATA | 0x378 | updated 2026-09-08 was 0x3D8; updated 2026-09-22 was 0x3B8 |
| ArcOffsets::ExponentialHeightFogComponent::SKY_ATMOSPHERE_AMBIENT_CONTRIB_COLOR_SCALE | 0x40C |  |
| ArcOffsets::ExponentialHeightFogComponent::START_DISTANCE | 0x444 | updated 2026-09-08 was 0x4A4; updated 2026-09-22 was 0x484 |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_ALBEDO | 0x458 | updated 2026-09-08 was 0x4B4; updated 2026-09-22 was 0x498 |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_DISTANCE | 0x470 | updated 2026-09-08 was 0x4CC; updated 2026-09-22 was 0x4B0 |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_EMISSIVE | 0x45C | updated 2026-09-08 was 0x4B8; updated 2026-09-22 was 0x49C |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_EXTINCTION_SCALE | 0x46C | updated 2026-09-08 was 0x4C8; updated 2026-09-22 was 0x4AC |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_NEAR_FADE_IN_DISTANCE | 0x478 | updated 2026-09-08 was 0x4D4; updated 2026-09-22 was 0x4B8 |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_SCATTERING_DISTRIBUTION | 0x454 | updated 2026-09-08 was 0x4B0; updated 2026-09-22 was 0x494 |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_START_DISTANCE | 0x474 | updated 2026-09-08 was 0x4D0; updated 2026-09-22 was 0x4B4 |
| ArcOffsets::ExponentialHeightFogComponent::VOLUMETRIC_FOG_STATIC_LIGHTING_SCATTERING_INTENSITY | 0x47C | updated 2026-09-08 was 0x4D8; updated 2026-09-22 was 0x4BC |
| ArcOffsets::ExtractionPoint::EXTRACTION_STARTED_TS | 0xB58 | double (WorldTime when extract began) |
| ArcOffsets::ExtractionPoint::EXTRACTION_TIME | 0xB60 | double (duration in seconds) |
| ArcOffsets::ExtractionPoint::IS_ENABLED | 0xBD0 | bool |
| ArcOffsets::FBodyInstance::ACTOR_HANDLE_ACTOR | 0x158 |  |
| ArcOffsets::FField::ARRAY_DIM | 0xA8 | v20260922 dumper (was 0xF0) |
| ArcOffsets::FField::BOOL_BYTE_MASK | 0x11A | v20260922 dumper (was 0x122) |
| ArcOffsets::FField::BOOL_BYTE_OFFSET | 0x119 | v20260922 dumper (was 0x121) |
| ArcOffsets::FField::BOOL_FIELD_MASK | 0x11B | v20260922 dumper (was 0x123) |
| ArcOffsets::FField::BOOL_FIELD_SIZE | 0x118 | v20260922 dumper (was 0x120) |
| ArcOffsets::FField::ELEMENT_SIZE | 0xA4 | v20260922 dumper (was 0xF8) |
| ArcOffsets::FField::FNAME_ENC | 0x90 | v20260922 dumper (was 0x70) |
| ArcOffsets::FField::MAP_KEY_PROP | 0x118 | v20260922 dumper |
| ArcOffsets::FField::MAP_VALUE_PROP | 0x120 | v20260922 dumper |
| ArcOffsets::FField::NEXT | 0x68 | v20260922 dumper (was 0x98) |
| ArcOffsets::FField::OBJ_PROP_CLASS | 0x118 | v20260922 dumper (was 0x138) |
| ArcOffsets::FField::OFFSET_ENC | 0xBC | v20260922 dumper (was 0xC4) |
| ArcOffsets::FField::OWNER | 0x58 | v20260922 dumper (was 0xA0) |
| ArcOffsets::FField::PROPERTY_FLAGS | 0xB0 | v20260922 dumper (was 0xB8) |
| ArcOffsets::FFieldCrypto::FFIELD_ADD2_CONST | 0xBB34B0C01E5AA000 | v20260922 dumper |
| ArcOffsets::FFieldCrypto::FFIELD_ADD_CONST | 0x44CB31F912F95FFF | v20260922 dumper — ADD-based, not PSHUFLW |
| ArcOffsets::FFieldCrypto::OFFSET_XOR_KEY | 0x8DE128DE | v20260922 dumper (was 0xEE0CA1CB) |
| ArcOffsets::FHeightfieldRef::RB_HEIGHTFIELD | 0x30 |  |
| ArcOffsets::FHeightfieldRef::RB_HEIGHTFIELD_SIMPLE | 0x38 |  |
| ArcOffsets::FHeightfieldRef::REFCOUNT | 0x8 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_CUBEMAP_INTENSITY | 0x470 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_CUBEMAP_TINT | 0x460 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONMipBlend | 0x5FC |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONMipScale | 0x600 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONMipThreshold | 0x604 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSIONTemporalBlendWeight | 0x608 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_BIAS | 0x62C |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_FADE_DISTANCE | 0x61C |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_FADE_RADIUS | 0x620 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_INTENSITY | 0x60C |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_POWER | 0x628 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_QUALITY | 0x630 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_RADIUS | 0x614 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_RADIUS_IN_WS | 0x5E0 |  |
| ArcOffsets::FPostProcessSettings::AMBIENT_OCCLUSION_STATIC_FRACTION | 0x610 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_BIAS | 0x494 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_BIAS_BACKUP | 0x498 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_CALIBRATION_CONSTANT | 0x4D8 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_HIGH_PERCENT | 0x4BC |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_LOW_PERCENT | 0x4B8 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_MAX_BRIGHTNESS | 0x4C4 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_METHOD | 0x36 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_MIN_BRIGHTNESS | 0x4C0 |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_SPEED_DOWN | 0x4CC |  |
| ArcOffsets::FPostProcessSettings::AUTO_EXPOSURE_SPEED_UP | 0x4C8 |  |
| ArcOffsets::FPostProcessSettings::BLOOM_INTENSITY | 0x304 |  |
| ArcOffsets::FPostProcessSettings::BLUE_CORRECTION | 0x2CC |  |
| ArcOffsets::FPostProcessSettings::CAMERA_ISO | 0x474 |  |
| ArcOffsets::FPostProcessSettings::CAMERA_SHUTTER_SPEED | 0x480 |  |
| ArcOffsets::FPostProcessSettings::CHROMATIC_ABERRATION_START_OFFSET | 0x300 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CONTRAST | 0x60 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CONTRAST_HIGHLIGHTS | 0x240 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CONTRAST_MIDTONES | 0x1A0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CONTRAST_SHADOWS | 0x100 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CORRECTION_HIGHLIGHTS_MAX | 0x2C4 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CORRECTION_HIGHLIGHTS_MIN | 0x2C0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_CORRECTION_SHADOWS_MAX | 0x2C8 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAIN | 0xA0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAIN_HIGHLIGHTS | 0x280 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAIN_MIDTONES | 0x1E0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAIN_SHADOWS | 0x140 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAMMA | 0x80 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAMMA_HIGHLIGHTS | 0x260 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAMMA_MIDTONES | 0x1C0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GAMMA_SHADOWS | 0x120 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GRADING_INTENSITY | 0x654 |  |
| ArcOffsets::FPostProcessSettings::COLOR_GRADING_LUT | 0x620 |  |
| ArcOffsets::FPostProcessSettings::COLOR_OFFSET | 0xC0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_OFFSET_HIGHLIGHTS | 0x2A0 |  |
| ArcOffsets::FPostProcessSettings::COLOR_OFFSET_MIDTONES | 0x200 |  |
| ArcOffsets::FPostProcessSettings::COLOR_OFFSET_SHADOWS | 0x160 |  |
| ArcOffsets::FPostProcessSettings::COLOR_SATURATION | 0x40 |  |
| ArcOffsets::FPostProcessSettings::COLOR_SATURATION_HIGHLIGHTS | 0x220 |  |
| ArcOffsets::FPostProcessSettings::COLOR_SATURATION_MIDTONES | 0x180 |  |
| ArcOffsets::FPostProcessSettings::COLOR_SATURATION_SHADOWS | 0xE0 |  |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_BLADE_COUNT | 0x490 |  |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_DEPTH_BLUR_AMOUNT | 0x680 | SDK CL-1341255 (was 0x634) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FAR_BLUR_SIZE | 0x6E4 | SDK CL-1341255 (was 0x650) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FAR_TRANSITION_REGION | 0x6D8 | SDK CL-1341255 (was 0x644) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FOCAL_DISTANCE | 0x67C | SDK CL-1341255 (was 0x630) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FOCAL_REGION | 0x6D0 | SDK CL-1341255 (was 0x63C) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_FSTOP | 0x488 |  |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_MIN_FSTOP | 0x48C |  |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_NEAR_BLUR_SIZE | 0x6E0 | SDK CL-1341255 (was 0x64C) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_NEAR_TRANSITION_REGION | 0x6D4 | SDK CL-1341255 (was 0x640) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_OCCLUSION | 0x6E8 | SDK CL-1341255 (was 0x654) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SCALE | 0x6DC | SDK CL-1341255 (was 0x648) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SENSOR_WIDTH | 0x674 | SDK CL-1341255 (was 0x628) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SKY_FOCUS_DISTANCE | 0x6EC | SDK CL-1341255 (was 0x658) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_SQUEEZE_FACTOR | 0x678 | SDK CL-1341255 (was 0x62C) |
| ArcOffsets::FPostProcessSettings::DEPTH_OF_FIELD_VIGNETTE_SIZE | 0x6F0 | SDK CL-1341255 (was 0x65C) |
| ArcOffsets::FPostProcessSettings::EXPAND_GAMUT | 0x2D0 |  |
| ArcOffsets::FPostProcessSettings::FILM_BLACK_CLIP | 0x2E4 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_HIGHLIGHTS_MAX | 0x5F0 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_HIGHLIGHTS_MIN | 0x5EC |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY | 0x5D8 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY_HIGHLIGHTS | 0x5E4 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY_MIDTONES | 0x5E0 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_INTENSITY_SHADOWS | 0x5DC |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_SHADOWS_MAX | 0x5E8 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_TEXEL_SIZE | 0x5F4 |  |
| ArcOffsets::FPostProcessSettings::FILM_GRAIN_TEXTURE | 0x5F8 |  |
| ArcOffsets::FPostProcessSettings::FILM_SHOULDER | 0x2E0 |  |
| ArcOffsets::FPostProcessSettings::FILM_SLOPE | 0x2D8 |  |
| ArcOffsets::FPostProcessSettings::FILM_TOE | 0x2DC |  |
| ArcOffsets::FPostProcessSettings::FILM_WHITE_CLIP | 0x2E8 |  |
| ArcOffsets::FPostProcessSettings::GRAIN_INTENSITY | 0x5D4 |  |
| ArcOffsets::FPostProcessSettings::GRAIN_JITTER | 0x5D0 |  |
| ArcOffsets::FPostProcessSettings::HISTOGRAM_LOG_MAX | 0x4D4 |  |
| ArcOffsets::FPostProcessSettings::HISTOGRAM_LOG_MIN | 0x4D0 |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_BLURRED_LUMINANCE_BLEND | 0x50C |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_BLURRED_LUMINANCE_KERNEL_SIZE_PERCENT | 0x510 |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_CONTRAST_SCALE | 0x4E0 |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_DETAIL_STRENGTH | 0x508 |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_HIGHLIGHT_CONTRAST_SCALE | 0x4E4 |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_MIDDLE_GREY_BIAS | 0x51C |  |
| ArcOffsets::FPostProcessSettings::LOCAL_EXPOSURE_SHADOW_CONTRAST_SCALE | 0x4E8 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_DIFFUSE_COLOR_BOOST | 0x418 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_FINAL_GATHER_LIGHTING_UPDATE_SPEED | 0x40C |  |
| ArcOffsets::FPostProcessSettings::LUMEN_FINAL_GATHER_QUALITY | 0x408 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_FULL_SKYLIGHT_LEAKING_DISTANCE | 0x430 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_MAX_REFLECTION_BOUNCES | 0x448 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_MAX_TRACE_DISTANCE | 0x414 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_RAY_LIGHTING_MODE | 0x3F4 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_REFLECTION_QUALITY | 0x43C |  |
| ArcOffsets::FPostProcessSettings::LUMEN_SCENE_DETAIL | 0x3FC |  |
| ArcOffsets::FPostProcessSettings::LUMEN_SCENE_LIGHTING_QUALITY | 0x3F8 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_SCENE_LIGHTING_UPDATE_SPEED | 0x404 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_SCENE_VIEW_DISTANCE | 0x400 |  |
| ArcOffsets::FPostProcessSettings::LUMEN_SKYLIGHT_LEAKING | 0x41C |  |
| ArcOffsets::FPostProcessSettings::LUMEN_SURFACE_CACHE_RESOLUTION | 0x434 |  |
| ArcOffsets::FPostProcessSettings::MOTION_BLUR_AMOUNT | 0x6F4 | SDK CL-1341255 (was 0x660) |
| ArcOffsets::FPostProcessSettings::MOTION_BLUR_MAX | 0x6F8 | SDK CL-1341255 (was 0x664) |
| ArcOffsets::FPostProcessSettings::MOTION_BLUR_PER_OBJECT_SIZE | 0x700 | SDK CL-1341255 (was 0x66C) |
| ArcOffsets::FPostProcessSettings::MOTION_BLUR_TARGET_FPS | 0x67C | SDK CL-1341255 (was 0x668) |
| ArcOffsets::FPostProcessSettings::OVERRIDE_FLAGS_BEGIN | 0x0 |  |
| ArcOffsets::FPostProcessSettings::OVERRIDE_FLAGS_END | 0x34 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_AO | 0x60C |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_AO_INTENSITY | 0x614 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_AO_RADIUS | 0x618 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_GI_MAX_BOUNCES | 0x41C |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_GI_SAMPLES_PER_PIXEL | 0x420 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_GI_TYPE | 0x418 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_REFLECTIONS_MAX_BOUNCES | 0x444 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_REFLECTIONS_SAMPLES_PER_PIXEL | 0x448 |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_REFLECTIONS_TRANSLUCENCY | 0x44D |  |
| ArcOffsets::FPostProcessSettings::RAY_TRACING_TRANSLUCENCY_MAX_ROUGHNESS | 0x73C | SDK CL-1341255 (was 0x6A8) |
| ArcOffsets::FPostProcessSettings::REFLECTIONS_TYPE | 0x439 |  |
| ArcOffsets::FPostProcessSettings::SCENE_COLOR_TINT | 0x2EC |  |
| ArcOffsets::FPostProcessSettings::SCENE_FRINGE_INTENSITY | 0x2FC |  |
| ArcOffsets::FPostProcessSettings::SCREEN_SPACE_REFLECTION_INTENSITY | 0x450 |  |
| ArcOffsets::FPostProcessSettings::SCREEN_SPACE_REFLECTION_MAX_ROUGHNESS | 0x458 |  |
| ArcOffsets::FPostProcessSettings::SCREEN_SPACE_REFLECTION_QUALITY | 0x454 |  |
| ArcOffsets::FPostProcessSettings::SHARPEN | 0x5CC |  |
| ArcOffsets::FPostProcessSettings::TONE_CURVE_AMOUNT | 0x2D4 |  |
| ArcOffsets::FPostProcessSettings::VIGNETTE_INTENSITY | 0x5C8 |  |
| ArcOffsets::FPostProcessSettings::WEIGHTED_BLENDABLES | 0x778 | SDK CL-1341255 (was 0x6D8) |
| ArcOffsets::FPostProcessSettings::WHITE_TEMP | 0x38 |  |
| ArcOffsets::FPostProcessSettings::WHITE_TINT | 0x3C |  |
| ArcOffsets::GameInstanceDecrypt::MAX_VALID_POINTER | 0x7FFFFFFFFFFF |  |
| ArcOffsets::GameInstanceDecrypt::MIN_VALID_POINTER | 0x1000 |  |
| ArcOffsets::GameState::AUTHORITY_GAME_MODE | 0x430 | SDK 2026-09-22 (was 0x458) |
| ArcOffsets::GameState::B_REPLICATED_HAS_BEGUN_PLAY | 0x510 | SDK 2026-09-22 (was 0x450) |
| ArcOffsets::GameState::GAME_MODE_CLASS | 0x428 | SDK 2026-09-22 (was 0x450) |
| ArcOffsets::GameState::PLAYER_STATES | 0x9C0 | SDK 2026-09-22 (was 0x898) |
| ArcOffsets::GameState::REPLICATED_WORLD_TIME_SECONDS_DOUBLE | 0x518 | SDK 2026-09-22 (was 0x4E0) |
| ArcOffsets::GameState::SERVER_WORLD_TIME_DELTA_SECONDS | 0x520 | SDK 2026-09-22 (was 0x4E8) |
| ArcOffsets::GameState::SERVER_WORLD_TIME_SECONDS_UPDATE_FREQ | 0x524 | SDK 2026-09-22 (was 0x4EC) |
| ArcOffsets::GameState::SPECTATOR_CLASS | 0x438 | SDK 2026-09-22 (was 0x460) |
| ArcOffsets::GameViewportClient::DEBUG_PROPERTIES | 0xF8 | SDK CL-1341255 (was 0x0E0); updated 2026-09-08 was 0x108; up |
| ArcOffsets::GameViewportClient::GAME_INSTANCE | 0x1F8 | SDK CL-1341255 (was 0x158); updated 2026-09-08 was 0x198; up |
| ArcOffsets::GameViewportClient::MAX_SPLITSCREEN_PLAYERS | 0x120 | SDK CL-1341255 (was 0x0F8); updated 2026-09-08 was 0x11C; up |
| ArcOffsets::GameViewportClient::SIZE | 0x6E0 | SDK CL-1341255 (was 0x700) |
| ArcOffsets::GameViewportClient::VIEWPORT_CONSOLE | 0x128 | SDK CL-1341255 (was 0x0F0); updated 2026-09-08 was 0x100; up |
| ArcOffsets::GeometryCollection::GCA_GC_COMPONENT | 0x3B0 | SDK CL-1341255 (was 0x398) |
| ArcOffsets::GeometryCollection::GCC_DUMMY_BODY_SETUP | 0xD10 | SDK CL-1341255 (was 0xD00, +0x10) |
| ArcOffsets::GeometryCollection::GCC_REST_COLLECTION | 0x810 | SDK CL-1341255 (was 0x800, +0x10) |
| ArcOffsets::GeometryCollection::GCC_REST_TRANSFORMS | 0x918 | SDK CL-1341255 (was 0x908, +0x10) |
| ArcOffsets::GeometryCollection::GCC_SIMULATING | 0x828 | SDK CL-1341255 (was 0x818, +0x10) |
| ArcOffsets::Global::UWORLD_BASE_RVA | 0x10839A98 | v20260922 dumper (was 0x10967B98) — double-deref: [RVA]→inte |
| ArcOffsets::GuTriangleMesh::AABB | 0x38 |  |
| ArcOffsets::GuTriangleMesh::FLAGS | 0x5C |  |
| ArcOffsets::GuTriangleMesh::FLAG_16BIT_INDICES | 0x2 |  |
| ArcOffsets::GuTriangleMesh::NB_TRIANGLES | 0x24 |  |
| ArcOffsets::GuTriangleMesh::NB_VERTICES | 0x20 |  |
| ArcOffsets::GuTriangleMesh::TRIANGLES | 0x30 |  |
| ArcOffsets::GuTriangleMesh::VERTICES | 0x28 |  |
| ArcOffsets::HealthComponent::ARMOR | 0x1C0 | SDK-visible struct base (Armor block, size 0x28); updated 20 |
| ArcOffsets::HealthComponent::ARMORED_ZONE | 0x368 | CL-1341255 SDK (was 0x2E8, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::B_ALWAYS_DBNO_ON_DEATH | 0x7C1 | CL-1341255 SDK (was 0x719, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::B_SELF_DAMAGE | 0x388 | CL-1341255 SDK (was 0x308, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::CACHED_HEALTH | 0x700 | SDK CL-1341255 (same as CURRENT_HEALTH); updated 2026-09-22  |
| ArcOffsets::HealthComponent::CURRENT_ARMOR | 0x190 | live-verified 2026-09-09 (was 0x150) — double at Armor+0x00 |
| ArcOffsets::HealthComponent::CURRENT_HEALTH | 0x6D0 | live-verified 2026-09-09 (was 0x678) — CachedHealth (RepNoti |
| ArcOffsets::HealthComponent::HEALTH_DIE | 0x670 | block +0x10 revert (was 0x660) — RE-VERIFY LIVE |
| ArcOffsets::HealthComponent::MAX_ARMOR | 0x198 | live-verified 2026-09-09 (was 0x158) — double at Armor+0x08 |
| ArcOffsets::HealthComponent::MAX_HEALTH | 0x348 | live-verified 2026-09-10 — sits in SDK's Pad_0340 hole, righ |
| ArcOffsets::HealthComponent::ON_DAMAGED | 0x3A0 | CL-1341255 SDK (was 0x320, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::ON_DBNO | 0x3E0 | CL-1341255 SDK (was 0x360, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::ON_DEAD | 0x3D0 | CL-1341255 SDK (was 0x350, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::ON_HEALTH_CHANGED | 0x390 | CL-1341255 SDK (was 0x310, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthComponent::ON_REVIVED | 0x3C0 | CL-1341255 SDK (was 0x340, prior live tag stale — RE-VERIFY  |
| ArcOffsets::HealthService::ABILITY_ON_DAMAGE_DEFS | 0x3C0 | SDK CL-1341255 (was 0x3D0) |
| ArcOffsets::HealthService::CHAIN_DESTRUCTION_DEFS | 0x3B0 | SDK CL-1341255 (was 0x3C0) |
| ArcOffsets::HealthService::EFFECT_ON_DAMAGE_DEFS | 0x3D0 | SDK CL-1341255 (was 0x3E0) |
| ArcOffsets::HealthService::ON_PARENT_CONSTRUCTABLE_DESTROYED | 0x4B8 | SDK CL-1315578; updated 2026-09-08 was 0x448; updated 2026-0 |
| ArcOffsets::HealthService::STUNS_DEFINITION | 0x460 | SDK CL-1315578; updated 2026-09-22 was 0x3F0 |
| ArcOffsets::InteractQuestComponent::PLAYERS_WITH_INTERACT_ACCESS | 0x248 | SDK CL-1341255 (was 0x238) |
| ArcOffsets::InteractQuestComponent::RELEVANT_PLAYER_IDS | 0x2A8 | SDK CL-1341255 (was 0x228); updated 2026-09-08 was 0x238; up |
| ArcOffsets::InteractQuestComponent::RELEVANT_PLAYER_IDS_NUM | 0x240 | SDK CL-1341255 (was 0x230) |
| ArcOffsets::InventoryComponent::AUXILIARY | 0x4B8 | SDK CL-1341255 (was 0x438); updated 2026-09-08 was 0x448; up |
| ArcOffsets::InventoryComponent::CURRENT_ITEM_ACTORS | 0x520 | SDK CL-1341255 (was 0x4A0); updated 2026-09-08 was 0x4b0; up |
| ArcOffsets::InventoryComponent::EQUIPPED_ARMOR | 0x588 | SDK CL-1341255 (was 0x508); updated 2026-09-22 was 0x518 |
| ArcOffsets::InventoryComponent::EQUIPPED_PRIMARY_ITEM | 0x500 | estimated 2026-08-11 (was 0x510, -0x10) |
| ArcOffsets::InventoryComponent::LOCAL_CURRENT_ITEM_ACTORS | 0x540 | SDK CL-1341255 (was 0x4C0); updated 2026-09-22 was 0x4d0 |
| ArcOffsets::InventoryComponent::STOWED_WEAPON_0 | 0x3A0 | SDK CL-1341255 (was 0x340); updated 2026-09-08 was 0x330; up |
| ArcOffsets::InventoryComponent::STOWED_WEAPON_1 | 0x3E0 | SDK CL-1341255 (was 0x380); updated 2026-09-08 was 0x370; up |
| ArcOffsets::ItemBase::QUALITY_LEVEL | 0x104 | EItemRarity on CDO: 0=common,1=rare,2=epic,3=legendary |
| ArcOffsets::Landscape::COLLISION_COMPONENTS | 0x5D8 | SDK CL-1341255 (was 0x478); updated 2026-09-22 was 0x490 |
| ArcOffsets::Landscape::COMPONENT_SIZE_QUADS | 0x888 | SDK CL-1341255 (was 0x70C); updated 2026-09-08 was 0x724; up |
| ArcOffsets::Landscape::LANDSCAPE_COMPONENTS | 0x5C8 | SDK CL-1341255 (was 0x468); updated 2026-09-08 was 0x480; up |
| ArcOffsets::Landscape::NUM_SUBSECTIONS | 0x890 | SDK CL-1341255 (was 0x714); updated 2026-09-08 was 0x72c; up |
| ArcOffsets::Landscape::SUBSECTION_SIZE_QUADS | 0x88C | SDK CL-1341255 (was 0x710); updated 2026-09-08 was 0x728; up |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::CACHED_LOCAL_BOX | 0x6C8 | SDK CL-1341255 (was 0x718, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::COLLISION_SCALE | 0x69C | SDK CL-1341255 (was 0x6EC, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::COLLISION_SIZE_QUADS | 0x698 | SDK CL-1341255 (was 0x6E8, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::HEIGHTFIELD_REF | 0x788 | live verified 2026-08-13 |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::HEIGHT_DATA_ARRAY | 0x760 | unused; SDK says this is RenderComponentRef |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::RENDER_COMPONENT_REF | 0x700 | SDK CL-1341255 (was 0x750, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::SECTION_BASE_X | 0x690 | SDK CL-1341255 (was 0x6E0, +0x10); updated 2026-09-22 was 0x |
| ArcOffsets::LandscapeHeightfieldCollisionComponent::SECTION_BASE_Y | 0x694 | SDK CL-1341255 (was 0x6E4, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::Level::ACTOR_MAX | 0x11C | CL-1372005 live-verified (was 0x114) |
| ArcOffsets::LevelCollection::DEMO_NET_DRIVER | 0x18 |  |
| ArcOffsets::LevelCollection::LEVELS | 0x28 |  |
| ArcOffsets::LevelCollection::NET_DRIVER | 0x10 |  |
| ArcOffsets::LevelCollection::STRUCT_SIZE | 0x78 |  |
| ArcOffsets::LevelStreaming::LEVEL_TRANSFORM | 0xF0 |  |
| ArcOffsets::LevelStreaming::LOADED_LEVEL | 0x200 |  |
| ArcOffsets::LevelStreaming::LT_ROTATION | 0x0 |  |
| ArcOffsets::LevelStreaming::LT_SCALE3D | 0x40 |  |
| ArcOffsets::LevelStreaming::LT_TRANSLATION | 0x20 |  |
| ArcOffsets::LightComponent::BLOOM_MAX_BRIGHTNESS | 0x454 | updated 2026-09-22 was 0x494 |
| ArcOffsets::LightComponent::BLOOM_SCALE | 0x44C | updated 2026-09-22 was 0x48C |
| ArcOffsets::LightComponent::BLOOM_THRESHOLD | 0x450 | updated 2026-09-22 was 0x490 |
| ArcOffsets::LightComponent::BLOOM_TINT | 0x458 | updated 2026-09-22 was 0x498 |
| ArcOffsets::LightComponent::B_ENABLE_LIGHT_SHAFT_BLOOM | 0x448 | updated 2026-09-22 was 0x488 |
| ArcOffsets::LightComponent::B_USE_IES_BRIGHTNESS | 0x438 | updated 2026-09-22 was 0x478 |
| ArcOffsets::LightComponent::B_USE_RAYTRACED_DISTANCE_FIELD_SHADOWS | 0x45C | updated 2026-09-22 was 0x49C |
| ArcOffsets::LightComponent::B_USE_TEMPERATURE | 0x3C4 | updated 2026-09-08 was 0x414; updated 2026-09-22 was 0x404 |
| ArcOffsets::LightComponent::CONTACT_SHADOW_CASTING_INTENSITY | 0x3F4 | updated 2026-09-08 was 0x440; updated 2026-09-22 was 0x434 |
| ArcOffsets::LightComponent::CONTACT_SHADOW_LENGTH | 0x3EC | updated 2026-09-08 was 0x438; updated 2026-09-22 was 0x42C |
| ArcOffsets::LightComponent::CONTACT_SHADOW_LENGTH_IN_WS | 0x3F0 | updated 2026-09-08 was 0x43C; updated 2026-09-22 was 0x430 |
| ArcOffsets::LightComponent::CONTACT_SHADOW_NON_CASTING_INTENSITY | 0x3F8 | updated 2026-09-08 was 0x444; updated 2026-09-22 was 0x438 |
| ArcOffsets::LightComponent::DISABLED_BRIGHTNESS | 0x444 | updated 2026-09-22 was 0x484 |
| ArcOffsets::LightComponent::IES_BRIGHTNESS_SCALE | 0x43C | updated 2026-09-22 was 0x47C |
| ArcOffsets::LightComponent::IES_TEXTURE | 0x430 | updated 2026-09-22 was 0x470 |
| ArcOffsets::LightComponent::LIGHTING_CHANNELS | 0x40C | updated 2026-09-22 was 0x44C |
| ArcOffsets::LightComponent::LIGHT_FUNCTION_FADE_DISTANCE | 0x440 | updated 2026-09-22 was 0x480 |
| ArcOffsets::LightComponent::LIGHT_FUNCTION_MATERIAL | 0x410 | updated 2026-09-22 was 0x450 |
| ArcOffsets::LightComponent::LIGHT_FUNCTION_SCALE | 0x418 | updated 2026-09-22 was 0x458 |
| ArcOffsets::LightComponent::MAX_DISTANCE_FADE_RANGE | 0x3C0 | updated 2026-09-08 was 0x410; updated 2026-09-22 was 0x400 |
| ArcOffsets::LightComponent::MAX_DRAW_DISTANCE | 0x3BC | updated 2026-09-08 was 0x40C; updated 2026-09-22 was 0x3FC |
| ArcOffsets::LightComponent::MIN_ROUGHNESS | 0x3D0 | updated 2026-09-08 was 0x420; updated 2026-09-22 was 0x410 |
| ArcOffsets::LightComponent::RAY_START_OFFSET_DEPTH_SCALE | 0x460 | updated 2026-09-22 was 0x4A0 |
| ArcOffsets::LightComponent::SHADOW_BIAS | 0x3E0 | updated 2026-09-08 was 0x42C; updated 2026-09-22 was 0x420 |
| ArcOffsets::LightComponent::SHADOW_FLAGS_BYTE | 0x448 |  |
| ArcOffsets::LightComponent::SHADOW_MAP_CHANNEL | 0x3C8 | updated 2026-09-08 was 0x418; updated 2026-09-22 was 0x408 |
| ArcOffsets::LightComponent::SHADOW_RESOLUTION_SCALE | 0x3DC | updated 2026-09-08 was 0x428; updated 2026-09-22 was 0x41C |
| ArcOffsets::LightComponent::SHADOW_SHARPEN | 0x3E8 | updated 2026-09-08 was 0x434; updated 2026-09-22 was 0x428 |
| ArcOffsets::LightComponent::SHADOW_SLOPE_BIAS | 0x3E4 | updated 2026-09-08 was 0x430; updated 2026-09-22 was 0x424 |
| ArcOffsets::LightComponent::SPECULAR_SCALE | 0x3D4 | updated 2026-09-08 was 0x424; updated 2026-09-22 was 0x414 |
| ArcOffsets::LightComponent::TEMPERATURE | 0x3B8 | SDK CL-1341255 (was 0x3F8, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::LightComponentBase::BRIGHTNESS | 0x390 | SDK CL-1341255 (was 0x3D0, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::LightComponentBase::B_AFFECT_REFLECTION_BYTE | 0x3E4 |  |
| ArcOffsets::LightComponentBase::B_FLAGS_BYTE | 0x3EC |  |
| ArcOffsets::LightComponentBase::B_FLAGS_BYTE_2 | 0x3ED |  |
| ArcOffsets::LightComponentBase::CAST_RAYTRACED_SHADOW | 0x3A0 | updated 2026-09-08 was 0x3F0; updated 2026-09-09 was 0x3DD;  |
| ArcOffsets::LightComponentBase::DEEP_SHADOW_LAYER_DISTRIBUTION | 0x3A8 | updated 2026-09-08 was 0x3F8; updated 2026-09-22 was 0x3E8 |
| ArcOffsets::LightComponentBase::INDIRECT_LIGHTING_INTENSITY | 0x3AC | updated 2026-09-08 was 0x3FC; updated 2026-09-22 was 0x3EC |
| ArcOffsets::LightComponentBase::INTENSITY | 0x394 | updated 2026-09-08 was 0x3E4; updated 2026-09-22 was 0x3D4 |
| ArcOffsets::LightComponentBase::LIGHT_COLOR | 0x398 | updated 2026-09-08 was 0x3E8; updated 2026-09-22 was 0x3D8 |
| ArcOffsets::LightComponentBase::SAMPLES_PER_PIXEL | 0x3B4 | updated 2026-09-08 was 0x404; updated 2026-09-22 was 0x3F4 |
| ArcOffsets::LightComponentBase::VOLUMETRIC_SCATTERING_INTENSITY | 0x3B0 | updated 2026-09-08 was 0x400; updated 2026-09-22 was 0x3F0 |
| ArcOffsets::LocalPlayer::ASPECT_RATIO_AXIS_CONSTRAINT | 0x228 | SDK CL-1341255 (was 0x0218) |
| ArcOffsets::LocalPlayer::B_FIXED_CULL_FOV | 0x241 | SDK CL-1341255 (was 0x0231) |
| ArcOffsets::LocalPlayer::B_FOREGROUND_STENCIL_ENABLED | 0x240 | SDK CL-1341255 (was 0x0230) |
| ArcOffsets::LocalPlayer::B_SENT_SPLIT_JOIN | 0x238 | SDK CL-1341255 (was 0x0228) |
| ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_FALLOFF_EXPONENT | 0x254 | SDK CL-1341255 (was 0x0244) |
| ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_INTENSITY | 0x24C | SDK CL-1341255 (was 0x023C) |
| ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_RADIUS | 0x250 | SDK CL-1341255 (was 0x0240) |
| ArcOffsets::LocalPlayer::FAKE_CAMERA_LIGHT_VERTICAL_OFFSET | 0x258 | SDK CL-1341255 (was 0x0248) |
| ArcOffsets::LocalPlayer::FIXED_CULL_BASE_FOV | 0x244 | SDK CL-1341255 (was 0x0234) |
| ArcOffsets::LocalPlayer::FIXED_CULL_MAX_FOV | 0x248 | SDK CL-1341255 (was 0x0238) |
| ArcOffsets::LocalPlayer::FOREGROUND_FOV | 0x23C | SDK CL-1341255 (was 0x022C) |
| ArcOffsets::LocalPlayer::PENDING_LEVEL_PC_CLASS | 0x230 | SDK CL-1341255 (was 0x0220) |
| ArcOffsets::LocalPlayer::SIZE | 0x440 | SDK CL-1341255 (was 0x0430) |
| ArcOffsets::LootContainerSingle::ITEM_CONTAINER_COMPONENT | 0xBC0 | SDK CL-1341255 (was 0xBC0); updated 2026-09-08 was 0xb60; up |
| ArcOffsets::LootInteractionComponent::BYTE_HAS_BEEN_OPENED | 0x8D8 | SDK CL-1341255 (was 0x8B0) |
| ArcOffsets::LootInteractionComponent::MASK_HAS_BEEN_OPENED | 0x1 |  |
| ArcOffsets::MapWidget::MAP_WIDGET_MAPPINGS | 0x5A8 | SDK CL-1341255 (was 0x458); updated 2026-09-08 was 0x518 |
| ArcOffsets::MapWidgetLevelSettings::MAP_MATERIAL | 0x28 |  |
| ArcOffsets::MapWidgetLevelSettings::MAP_TEXTURE | 0x0 |  |
| ArcOffsets::MapWidgetMappings::COMPONENT_TEMPLATE | 0xA0 |  |
| ArcOffsets::MapWidgetMappings::FALLBACK_WIDGET | 0xF0 |  |
| ArcOffsets::MeshBoneInfo::NAME | 0x0 |  |
| ArcOffsets::MeshBoneInfo::PARENT_INDEX | 0x8 |  |
| ArcOffsets::MeshBoneInfo::STRIDE | 0x18 |  |
| ArcOffsets::MinimalViewInfo::ASPECT_RATIO | 0x9C | float |
| ArcOffsets::MinimalViewInfo::DESIRED_FOV | 0x6C | float |
| ArcOffsets::MinimalViewInfo::FIRST_PERSON_FOV | 0x70 | float |
| ArcOffsets::MinimalViewInfo::FIRST_PERSON_SCALE | 0x74 | float |
| ArcOffsets::MinimalViewInfo::FOV | 0x60 | float |
| ArcOffsets::MinimalViewInfo::LOCATION | 0x10 | FVector (3 doubles) |
| ArcOffsets::MinimalViewInfo::ORTHO_FAR_CLIP | 0x8C | float |
| ArcOffsets::MinimalViewInfo::ORTHO_NEAR_CLIP | 0x88 | float |
| ArcOffsets::MinimalViewInfo::ORTHO_WIDTH | 0x78 | float |
| ArcOffsets::MinimalViewInfo::PERSPECTIVE_NEAR_CLIP | 0x90 | float |
| ArcOffsets::MinimalViewInfo::POST_PROCESS_SETTINGS | 0xC0 | PostProcessSettings (size=0x790) |
| ArcOffsets::MinimalViewInfo::ROTATION | 0x38 | FRotator (3 doubles) |
| ArcOffsets::Pawn::HEALTH_COMPONENT | 0xDC0 | SDK PioneerPlayerCharacter 2026-09-22 (was 0xDD8) |
| ArcOffsets::Pawn::LAST_HIT_BY | 0x3E8 | SDK CL-1341255 (was 0x3C8); updated 2026-09-08 was 0x3E0; up |
| ArcOffsets::Pawn::REMOTE_VIEW_PITCH | 0x3D4 | uint8_t network-compressed pitch (raw * 360/255); updated 20 |
| ArcOffsets::PhysicsAsset::SKELETAL_BODY_SETUPS | 0xE8 | SDK CL-1341255 (was 0xD8); updated 2026-09-22 was 0xE0 |
| ArcOffsets::Pickup::UI_HOVER_DATA | 0x620 | estimated |
| ArcOffsets::PioneerConstructablePawn::B_CAN_SELF_DAMAGE | 0x1210 | SDK CL-1341255 (was 0x11F0); updated 2026-09-09 was 0x1200;  |
| ArcOffsets::PioneerConstructablePawn::CAPABILITY_SERVICE | 0x1298 | SDK CL-1341255 (was 0x1288) |
| ArcOffsets::PioneerConstructablePawn::COLLISION_SERVICE | 0x1290 | SDK CL-1341255 (was 0x1280) |
| ArcOffsets::PioneerConstructablePawn::DEBRIS_SERVICE | 0x12B8 | SDK CL-1341255 (was 0x12A8) |
| ArcOffsets::PioneerConstructablePawn::HEALTH_GROUP_SERVICE | 0x12A8 | SDK CL-1341255 (was 0x1298) |
| ArcOffsets::PioneerConstructablePawn::INVESTIGATION_COMP | 0x12F8 | SDK CL-1341255 (was 0x12E8) |
| ArcOffsets::PioneerConstructablePawn::ITEM_CONTAINER_COMP | 0x1300 | SDK CL-1341255 (was 0x12F0) |
| ArcOffsets::PioneerConstructablePawn::PERCEPTION_SERVICE | 0x12A0 | SDK CL-1341255 (was 0x1290) |
| ArcOffsets::PioneerConstructablePawn::PIONEER_PING_INFO_COMP | 0x12D8 | SDK CL-1341255 (was 0x12C8) |
| ArcOffsets::PioneerConstructablePawn::REPLICATION_SETTINGS | 0x12F0 | SDK CL-1341255 (was 0x12E0) |
| ArcOffsets::PioneerConstructablePawn::SKELETAL_REPLICATION_SERVICE | 0x12B0 | SDK CL-1341255 (was 0x12A0) |
| ArcOffsets::PioneerConstructablePawn::SPAWN_COST | 0x1214 | SDK CL-1341255 (was 0x11F4); updated 2026-09-09 was 0x1204;  |
| ArcOffsets::PioneerConstructablePawn::SPAWN_SIZE_COMPONENT | 0x12E8 | SDK CL-1341255 (was 0x12D8) |
| ArcOffsets::PioneerConstructablePawn::STYLE | 0x12D0 | SDK CL-1341255 (was 0x12C0) |
| ArcOffsets::PioneerConstructablePawn::TARGETING_SERVICE | 0x1288 | SDK CL-1341255 (was 0x1278) |
| ArcOffsets::PioneerConstructablePawn::THREAT_LEVEL | 0x121C | SDK CL-1341255 (was 0x11FC); updated 2026-09-09 was 0x120c;  |
| ArcOffsets::PioneerConstructablePawn::TIMER_SERVICE | 0x12C8 | SDK CL-1341255 (was 0x12B8) |
| ArcOffsets::PioneerPC::CONTEXTS_TO_DISABLE_ON_MENU_OPENED | 0x1258 | TArray Num used as any-UI-open detector |
| ArcOffsets::PioneerPlayerCharacter::DBNO_COMPONENT | 0xCF0 | SDK CL-1315578 |
| ArcOffsets::PioneerPlayerCharacter::FLASHLIGHT_COMPONENT | 0xDE0 | SDK CL-1341255 (was 0xDE8); updated 2026-09-08 was 0xdf8; up |
| ArcOffsets::PioneerPlayerCharacter::FOLLOW_CAMERA | 0xC60 | SDK CL-1341255 (was 0xC80); updated 2026-09-22 was 0xc90 |
| ArcOffsets::PioneerPlayerCharacter::INVENTORY_COMPONENT | 0xC80 | SDK CL-1341255 (was 0xCA0); updated 2026-09-08 was 0xcb0; up |
| ArcOffsets::PioneerPlayerCharacter::KILL_ASSIST_NOTIFIER_COMP | 0xE40 | SDK CL-1341255 (was 0xE30) |
| ArcOffsets::PioneerPlayerCharacter::LAST_RELEVANT_PLAYER_STATE | 0xEC8 | SDK CL-1341255 (was 0xED0); updated 2026-09-08 was 0xee0; up |
| ArcOffsets::PioneerPlayerCharacter::PLAYER_STATUS_VAR | 0x12F8 | SDK CL-1341255 (was 0x12F8); updated 2026-09-22 was 0x1308 |
| ArcOffsets::PioneerPlayerCharacter::TEAM_COMPONENT | 0x450 |  |
| ArcOffsets::Pioneer_Sky_C::CONTRAST | 0x558 | SDK CL-1341255 (was 0x538); updated 2026-09-09 was 0x548; up |
| ArcOffsets::Pioneer_Sky_C::EXPOSURE | 0x458 | SDK CL-1341255 (was 0x438); updated 2026-09-09 was 0x448; up |
| ArcOffsets::Pioneer_Sky_C::EXPOSURE_MAX | 0x748 | SDK CL-1341255 (was 0x728); updated 2026-09-09 was 0x738; up |
| ArcOffsets::Pioneer_Sky_C::EXPOSURE_MIN | 0x740 | SDK CL-1341255 (was 0x720); updated 2026-09-09 was 0x730; up |
| ArcOffsets::Pioneer_Sky_C::MOON_LIGHT_COMPONENT | 0x728 | SDK CL-1341255 (was 0x440); updated 2026-09-09 was 0x450; up |
| ArcOffsets::Pioneer_Sky_C::MOON_LIGHT_COMPONENT_DUP | 0x718 | SDK CL-1341255 (was 0x708) |
| ArcOffsets::Pioneer_Sky_C::SATURATION | 0x4E0 | SDK CL-1341255 (was 0x4C0); updated 2026-09-09 was 0x4D0; up |
| ArcOffsets::Pioneer_Sky_C::SKY_LIGHT_COMPONENT | 0x730 | SDK CL-1341255 (was 0x430); updated 2026-09-09 was 0x440; up |
| ArcOffsets::Pioneer_Sky_C::SKY_LIGHT_COMPONENT_DUP | 0x720 | SDK CL-1341255 (was 0x710) |
| ArcOffsets::Pioneer_Sky_C::SUN_LIGHT_COMPONENT | 0x720 | SDK CL-1341255 (was 0x448); updated 2026-09-09 was 0x458; up |
| ArcOffsets::Pioneer_Sky_C::SUN_LIGHT_COMPONENT_DUP | 0x710 | SDK CL-1341255 (was 0x700) |
| ArcOffsets::Pioneer_Sky_C::SUN_LIGHT_INTENSITY | 0x670 | SDK CL-1341255 (was 0x650); updated 2026-09-09 was 0x660; up |
| ArcOffsets::Pioneer_Sky_C::USE_EXPOSURE_RANGE | 0x738 | SDK CL-1341255 (was 0x718); updated 2026-09-09 was 0x728; up |
| ArcOffsets::PlatformIdComponent::PLATFORM_ID | 0x188 | FUniqueNetIdRepl (0x30 bytes) |
| ArcOffsets::PlayerCameraManager::CAMERA_CACHE_POST_PROCESS | 0x17A0 | SDK CameraCachePostProcessSettings |
| ArcOffsets::PlayerCameraManager::DEFAULT_FOV | 0x430 | SDK DefaultFOV (float); updated 2026-09-22 was 0x3F0 |
| ArcOffsets::PlayerCameraManager::DEFAULT_ORTHO_WIDTH | 0x440 | SDK DefaultOrthoWidth; updated 2026-09-22 was 0x400 |
| ArcOffsets::PlayerCameraManager::FREE_CAM_OFFSET | 0x1FC0 | SDK FreeCamOffset; updated 2026-09-22 was 0x1F90 |
| ArcOffsets::PlayerCameraManager::LOCKED_FOV_ALT1 | 0x438 | adjusted -4 from LOCKED_FOV (was 0x3F8) |
| ArcOffsets::PlayerCameraManager::LOCKED_FOV_ALT2 | 0x434 | adjusted -8 from LOCKED_FOV (was 0x3F4) |
| ArcOffsets::PlayerCameraManager::MODIFIER_LIST | 0x1FB0 | SDK ModifierList; updated 2026-09-22 was 0x1F60 |
| ArcOffsets::PlayerCameraManager::PC_OWNER | 0x420 | SDK PCOwner; updated 2026-09-22 was 0x3E0 |
| ArcOffsets::PlayerCameraManager::PENDING_VIEW_TARGET | 0xE10 | SDK PendingViewTarget (FTViewTarget, 0x940); updated 2026-09 |
| ArcOffsets::PlayerCameraManager::VIEW_ROLL_MAX | 0x209C | SDK ViewRollMax; updated 2026-09-22 was 0x2040 |
| ArcOffsets::PlayerCameraManager::VIEW_ROLL_MIN | 0x20A4 | SDK ViewRollMin; updated 2026-09-22 was 0x2044 |
| ArcOffsets::PlayerCameraManager::VIEW_TARGET | 0x460 | SDK ViewTarget (FTViewTarget, 0x940); updated 2026-09-22 was |
| ArcOffsets::PlayerCameraManager::VIEW_TARGET_OFFSET | 0x1FE0 | SDK ViewTargetOffset; updated 2026-09-22 was 0x1FB0 |
| ArcOffsets::PlayerController::ACKNOWLEDGED_PAWN | 0x418 | SDK CL-1341255 (was 0x3E0) |
| ArcOffsets::PlayerController::CHARACTER | 0x408 | SDK CL-1341255 (was 0x3E8) |
| ArcOffsets::PlayerController::CONTROL_ROTATION | 0x450 | SDK CL-1372005 AController::ControlRotation (was 0x438) |
| ArcOffsets::PlayerController::PLAYER_STATE | 0x408 | SDK CL-1341255 (was 0x3A0) |
| ArcOffsets::PlayerHealthInfo::B_HAS_BROKEN_ARMOR | 0x20 |  |
| ArcOffsets::PlayerState::ACHIEVEMENT_COMPONENT | 0x5C0 | SDK 2026-09-10 (was 0x5D8) |
| ArcOffsets::PlayerState::B_FINISHED_ROUND | 0x5B8 | SDK 2026-09-10 (was 0x5D0) |
| ArcOffsets::PlayerState::B_IS_A_BOT_BYTE | 0x3DA | byte holding bIsABot (bit 0x08) + bIsSpectator (bit 0x02) |
| ArcOffsets::PlayerState::HEALTH_INFO_BASE | 0x588 | estimated — needs multiplayer verification |
| ArcOffsets::PlayerState::IDENTITY_HASH_1 | 0x3A0 | unchanged |
| ArcOffsets::PlayerState::IDENTITY_HASH_2 | 0x3B0 | unchanged |
| ArcOffsets::PlayerState::IDENTITY_REF_1 | 0x3B8 | unchanged |
| ArcOffsets::PlayerState::IDENTITY_REF_2 | 0x3C0 | unchanged |
| ArcOffsets::PlayerState::PLATFORM_ID_COMPONENT | 0x4C0 | SDK CL-1341255 |
| ArcOffsets::PlayerState::PLAYER_NAME_FORMATTED | 0x4E0 | estimated 2026-08-09 (was 0x4B0) |
| ArcOffsets::PlayerState::SQUAD | 0x498 | SDK CL-1341255 (was 0x460) |
| ArcOffsets::PlayerState::START_TIME | 0x3CC | int32 — game time when this PS joined; updated 2026-09-08 wa |
| ArcOffsets::PostProcessComponent::BLEND_RADIUS | 0xB14 | SDK CL-1341255 (was 0xAD4); updated 2026-09-08 was 0xae4; up |
| ArcOffsets::PostProcessComponent::BLEND_WEIGHT | 0xB18 | SDK CL-1341255 (was 0xAD8); updated 2026-09-08 was 0xae8; up |
| ArcOffsets::PostProcessComponent::B_ENABLED | 0xB1C | SDK CL-1341255 (was 0xADC); updated 2026-09-08 was 0xaec; up |
| ArcOffsets::PostProcessComponent::PRIORITY | 0xB10 | SDK CL-1341255 (was 0xAD0); updated 2026-09-08 was 0xae0; up |
| ArcOffsets::PostProcessComponent::SETTINGS | 0x380 | SDK CL-1341255 (was 0x3D0, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::PostProcessVolume::BLEND_RADIUS | 0xB94 | SDK CL-1341255 (was 0xAD4); updated 2026-09-08 was 0xaf4; up |
| ArcOffsets::PostProcessVolume::BLEND_WEIGHT | 0xB98 | SDK CL-1341255 (was 0xAD8); updated 2026-09-08 was 0xaf8; up |
| ArcOffsets::PostProcessVolume::B_ENABLED | 0xB9C | SDK CL-1341255 (was 0xADC); updated 2026-09-08 was 0xafc; up |
| ArcOffsets::PostProcessVolume::PRIORITY | 0xB90 | SDK CL-1341255 (was 0xAD0); updated 2026-09-08 was 0xaf0; up |
| ArcOffsets::PostProcessVolume::SETTINGS | 0x400 | SDK CL-1341255 (was 0x3E0); updated 2026-09-08 was 0x3f0; up |
| ArcOffsets::PrimitiveComponent::BLUEPRINT_CREATED_COMPONENTS | 0x380 | 2026-08-19 user-provided |
| ArcOffsets::PrimitiveComponent::BODY_INSTANCE | 0x4D0 | updated 2026-09-22 was 0x510 |
| ArcOffsets::PxHeightField::NB_COLUMNS | 0x3C |  |
| ArcOffsets::PxHeightField::NB_ROWS | 0x38 |  |
| ArcOffsets::PxHeightField::SAMPLES_PTR | 0x50 |  |
| ArcOffsets::PxHeightField::SAMPLE_STRIDE | 0x4 |  |
| ArcOffsets::QuestInteractable::COMP_VALIDATE_FLOAT | 0x460 |  |
| ArcOffsets::QuestInteractable::INTERACTION_COMP_A | 0x3D0 |  |
| ArcOffsets::QuestInteractable::INTERACTION_COMP_B | 0x4D8 |  |
| ArcOffsets::SceneComponent::MOBILITY_BYTE | 0x2CC | SDK CL-1341255 (was 0x2AB); updated 2026-09-08 was 0x2C3; up |
| ArcOffsets::SceneComponent::MOBILITY_MOVABLE | 0x2 |  |
| ArcOffsets::SceneComponent::RELATIVE_ROTATION | 0x280 | SDK CL-1341255 (was 0x260); updated 2026-09-08 was 0x278; up |
| ArcOffsets::SceneComponent::RELATIVE_SCALE3D | 0x298 | SDK CL-1341255 (was 0x278); updated 2026-09-08 was 0x290; up |
| ArcOffsets::SkeletalMesh::PHYSICS_ASSET | 0x308 | SDK CL-1341255 (was 0x2E8); updated 2026-09-08 was 0x2F0 |
| ArcOffsets::SkeletalMesh::REF_SKELETON_PROBE_HI | 0x300 |  |
| ArcOffsets::SkeletalMesh::REF_SKELETON_PROBE_LO | 0x140 |  |
| ArcOffsets::SkeletalMeshComponent::ANIM_FIRED_WEAPON_TIMER | 0x708 | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::ANIM_FIRE_STATE | 0x6C0 | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::ANIM_FIRE_WEAPON_COUNT | 0x674 | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::ANIM_HAS_WEAPON_FIRED_SHOT | 0x1340 | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::ANIM_IS_FIRING | 0x672 | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::ANIM_LAST_FIRED_SHOT_TS | 0x13D0 | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::ANIM_SCRIPT_INSTANCE | 0xB38 | SDK CL-1341255 (was 0xAF8); updated 2026-09-08 was 0xAD8; up |
| ArcOffsets::SkeletalMeshComponent::ANIM_SHOTS_IN_AUTO_FIRE | 0x132C | AnimInstance internal — needs verification |
| ArcOffsets::SkeletalMeshComponent::BOUNDS_SCALE | 0x4B8 | SDK CL-1341255 (was 0x4A8, +0x10) — from PrimitiveComponent |
| ArcOffsets::SkeletalMeshComponent::B_FORCE_REFPOSE | 0xCC1 | SDK CL-1341255 (was 0xC72); updated 2026-09-08 was 0xC52; up |
| ArcOffsets::SkeletalMeshComponent::B_FORCE_REFPOSE_MASK | 0x1 |  |
| ArcOffsets::SkeletalMeshComponent::B_NO_SKELETON_UPDATE | 0xCC0 | SDK CL-1372005 (bNoSkeletonUpdate @ +0xD10 mask=0x08 per SDK |
| ArcOffsets::SkeletalMeshComponent::B_NO_SKELETON_UPDATE_MASK | 0x8 | SDK-verified 2026-09-10 (was 0x10 which is bPauseAnims) |
| ArcOffsets::SkeletalMeshComponent::B_PAUSE_ANIMS | 0xCC0 | same byte, tested two lines up; updated 2026-09-08 was 0xC51 |
| ArcOffsets::SkeletalMeshComponent::B_PAUSE_ANIMS_MASK | 0x10 | SDK 2026-09-08 (was 0x20) |
| ArcOffsets::SkeletalMeshComponent::CACHED_BONE_SPACE_TRANSFORMS | 0xBE8 | SDK CL-1341255 (was 0xBA0); updated 2026-09-22 was 0xB80 |
| ArcOffsets::SkeletalMeshComponent::CACHED_COMPONENT_SPACE_TRANSFORMS | 0xBF8 | SDK CL-1341255 (was 0xBB0); updated 2026-09-22 was 0xB90 |
| ArcOffsets::SkeletalMeshComponent::LAST_POSE_TICK_FRAME | 0x1148 | SDK CL-1341255 (was 0xDE8); updated 2026-09-08 was 0xDC8; up |
| ArcOffsets::SkeletalMeshComponent::LEADER_POSE_COMPONENT | 0x750 | SDK CL-1341255 (was 0x738) |
| ArcOffsets::SkeletalMeshComponent::SKELETAL_MESH_ALT | 0xB10 | estimated 2026-08-11 (was 0xAE0, +0x30) |
| ArcOffsets::SkeletalMeshComponent::SKINNED_ASSET | 0x748 | SDK CL-1341255 (was 0x730) |
| ArcOffsets::SkeletalMeshComponent::VISIBILITY_BASED_ANIM_TICK_OPTION | 0x9EC | SDK CL-1341255 (was 0x09B4); updated 2026-09-22 was 0x994 |
| ArcOffsets::SkyAtmosphereComponent::AERIAL_PERSPECTIVE_START_DEPTH | 0x41C | updated 2026-09-08 was 0x46C; updated 2026-09-22 was 0x45C |
| ArcOffsets::SkyAtmosphereComponent::AERIAL_PERSPECTIVE_VIEW_DISTANCE_SCALE | 0x460 |  |
| ArcOffsets::SkyAtmosphereComponent::ATMOSPHERE_HEIGHT | 0x37C | updated 2026-09-08 was 0x3DC; updated 2026-09-22 was 0x3BC |
| ArcOffsets::SkyAtmosphereComponent::BOTTOM_RADIUS | 0x374 | updated 2026-09-08 was 0x3D4; updated 2026-09-22 was 0x3B4 |
| ArcOffsets::SkyAtmosphereComponent::GROUND_ALBEDO | 0x378 | updated 2026-09-08 was 0x3D8; updated 2026-09-22 was 0x3B8 |
| ArcOffsets::SkyAtmosphereComponent::HEIGHT_FOG_CONTRIBUTION | 0x414 | updated 2026-09-08 was 0x464; updated 2026-09-22 was 0x454 |
| ArcOffsets::SkyAtmosphereComponent::MIE_ABSORPTION | 0x3B8 | updated 2026-09-08 was 0x418; updated 2026-09-22 was 0x3F8 |
| ArcOffsets::SkyAtmosphereComponent::MIE_ABSORPTION_SCALE | 0x3B4 | updated 2026-09-08 was 0x414; updated 2026-09-22 was 0x3F4 |
| ArcOffsets::SkyAtmosphereComponent::MIE_ANISOTROPY | 0x3C8 | updated 2026-09-08 was 0x428; updated 2026-09-22 was 0x408 |
| ArcOffsets::SkyAtmosphereComponent::MIE_EXPONENTIAL_DISTRIBUTION | 0x3CC | updated 2026-09-08 was 0x42C; updated 2026-09-22 was 0x40C |
| ArcOffsets::SkyAtmosphereComponent::MIE_SCATTERING | 0x3A4 | updated 2026-09-08 was 0x404; updated 2026-09-22 was 0x3E4 |
| ArcOffsets::SkyAtmosphereComponent::MIE_SCATTERING_SCALE | 0x3A0 | updated 2026-09-08 was 0x400; updated 2026-09-22 was 0x3E0 |
| ArcOffsets::SkyAtmosphereComponent::MULTI_SCATTERING_FACTOR | 0x380 | updated 2026-09-08 was 0x3E0; updated 2026-09-22 was 0x3C0 |
| ArcOffsets::SkyAtmosphereComponent::OTHER_ABSORPTION | 0x3D4 | updated 2026-09-08 was 0x434; updated 2026-09-22 was 0x414 |
| ArcOffsets::SkyAtmosphereComponent::OTHER_ABSORPTION_SCALE | 0x3D0 | updated 2026-09-08 was 0x430; updated 2026-09-22 was 0x410 |
| ArcOffsets::SkyAtmosphereComponent::RAYLEIGH_EXPONENTIAL_DISTRIBUTION | 0x39C | updated 2026-09-08 was 0x3FC; updated 2026-09-22 was 0x3DC |
| ArcOffsets::SkyAtmosphereComponent::RAYLEIGH_SCATTERING | 0x38C | updated 2026-09-08 was 0x3EC; updated 2026-09-22 was 0x3CC |
| ArcOffsets::SkyAtmosphereComponent::RAYLEIGH_SCATTERING_SCALE | 0x388 | updated 2026-09-08 was 0x3E8; updated 2026-09-22 was 0x3C8 |
| ArcOffsets::SkyAtmosphereComponent::SKY_LUMINANCE_FACTOR | 0x3F0 | updated 2026-09-08 was 0x450; updated 2026-09-22 was 0x430 |
| ArcOffsets::SkyAtmosphereComponent::TRACE_SAMPLE_COUNT_SCALE | 0x384 | updated 2026-09-08 was 0x3E4; updated 2026-09-22 was 0x3C4 |
| ArcOffsets::SkyAtmosphereComponent::TRANSFORM_MODE | 0x370 | SDK CL-1341255 (was 0x3C0, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::SkyAtmosphereComponent::TRANSMITTANCE_MIN_LIGHT_ELEVATION_ANGLE | 0x418 | updated 2026-09-08 was 0x468; updated 2026-09-22 was 0x458 |
| ArcOffsets::SkyLightComponent::BLEND_DESTINATION_CUBEMAP | 0x4C8 | updated 2026-09-08 was 0x518; updated 2026-09-22 was 0x508 |
| ArcOffsets::SkyLightComponent::B_CAPTURE_EMISSIVE_ONLY | 0x3DC | updated 2026-09-08 was 0x42C; updated 2026-09-22 was 0x41C |
| ArcOffsets::SkyLightComponent::B_CLOUD_AMBIENT_OCCLUSION | 0x404 | updated 2026-09-08 was 0x454; updated 2026-09-22 was 0x444 |
| ArcOffsets::SkyLightComponent::B_LOWER_HEMISPHERE_IS_BLACK | 0x3DD | updated 2026-09-08 was 0x42D; updated 2026-09-22 was 0x41D |
| ArcOffsets::SkyLightComponent::B_REAL_TIME_CAPTURE | 0x3B8 | SDK CL-1341255 (was 0x3F8, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::SkyLightComponent::B_SHOW_ILLUMINANCE_METER | 0x46C |  |
| ArcOffsets::SkyLightComponent::B_USE_SEPARATE_SKY_DISTANCE_FOR_REFLECTIONS | 0x424 |  |
| ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_APERTURE_SCALE | 0x414 | updated 2026-09-08 was 0x464; updated 2026-09-22 was 0x454 |
| ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_EXTENT | 0x40C | updated 2026-09-08 was 0x45C; updated 2026-09-22 was 0x44C |
| ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_MAP_RESOLUTION_SCALE | 0x410 | updated 2026-09-08 was 0x460; updated 2026-09-22 was 0x450 |
| ArcOffsets::SkyLightComponent::CLOUD_AMBIENT_OCCLUSION_STRENGTH | 0x408 | updated 2026-09-08 was 0x458; updated 2026-09-22 was 0x448 |
| ArcOffsets::SkyLightComponent::CONTRAST | 0x3F4 | updated 2026-09-08 was 0x444; updated 2026-09-22 was 0x434 |
| ArcOffsets::SkyLightComponent::CUBEMAP | 0x3C0 | updated 2026-09-08 was 0x410; updated 2026-09-22 was 0x400 |
| ArcOffsets::SkyLightComponent::CUBEMAP_RESOLUTION | 0x3CC | updated 2026-09-08 was 0x41C; updated 2026-09-22 was 0x40C |
| ArcOffsets::SkyLightComponent::LOWER_HEMISPHERE_COLOR | 0x3E0 | updated 2026-09-08 was 0x430; updated 2026-09-22 was 0x420 |
| ArcOffsets::SkyLightComponent::MIN_OCCLUSION | 0x3FC | updated 2026-09-08 was 0x44C; updated 2026-09-22 was 0x43C |
| ArcOffsets::SkyLightComponent::OCCLUSION_COMBINE_MODE | 0x418 | updated 2026-09-08 was 0x468; updated 2026-09-22 was 0x458 |
| ArcOffsets::SkyLightComponent::OCCLUSION_EXPONENT | 0x3F8 | updated 2026-09-08 was 0x448; updated 2026-09-22 was 0x438 |
| ArcOffsets::SkyLightComponent::OCCLUSION_MAX_DISTANCE | 0x3F0 | updated 2026-09-08 was 0x440; updated 2026-09-22 was 0x430 |
| ArcOffsets::SkyLightComponent::OCCLUSION_TINT | 0x400 | updated 2026-09-08 was 0x450; updated 2026-09-22 was 0x440 |
| ArcOffsets::SkyLightComponent::REFLECTION_CAPTURE_SKY_DISTANCE_THRESHOLD | 0x3D8 | updated 2026-09-08 was 0x428; updated 2026-09-22 was 0x418 |
| ArcOffsets::SkyLightComponent::SKY_DISTANCE_THRESHOLD | 0x3D0 | updated 2026-09-08 was 0x420; updated 2026-09-22 was 0x410 |
| ArcOffsets::SkyLightComponent::SOURCE_CUBEMAP_ANGLE | 0x3C8 | updated 2026-09-08 was 0x418; updated 2026-09-22 was 0x408 |
| ArcOffsets::SkyLightComponent::SOURCE_TYPE | 0x3B9 | updated 2026-09-08 was 0x409; updated 2026-09-22 was 0x3F9 |
| ArcOffsets::StaticMeshGeometry::ATTACH_CHILDREN | 0x1E8 | SDK CL-1341255 |
| ArcOffsets::StaticMeshGeometry::IBUFFER_BYTE_COUNT | 0x8 |  |
| ArcOffsets::StaticMeshGeometry::IBUFFER_DATA_PTR | 0x0 |  |
| ArcOffsets::StaticMeshGeometry::IBUFFER_IS_16BIT | 0x10 |  |
| ArcOffsets::StaticMeshGeometry::LOD_DEPTH_ONLY_INDEX_BUFFER | 0x88 |  |
| ArcOffsets::StaticMeshGeometry::LOD_INDEX_BUFFER | 0x58 |  |
| ArcOffsets::StaticMeshGeometry::LOD_VERTEX_BUFFERS | 0x0 |  |
| ArcOffsets::StaticMeshGeometry::POSBUFFER_DATA | 0x8 |  |
| ArcOffsets::StaticMeshGeometry::POSBUFFER_NUM_VERTICES | 0x10 |  |
| ArcOffsets::StaticMeshGeometry::RENDER_DATA_LOD_RESOURCES | 0x0 |  |
| ArcOffsets::StaticMeshGeometry::SMC_FLAG_BYTE_6B4 | 0x73C | estimated 2026-08-11 (was 0x72C, +0x10) |
| ArcOffsets::StaticMeshGeometry::SMC_NAV_OBSTACLE_MASK | 0x10 |  |
| ArcOffsets::StaticMeshGeometry::SMC_STATIC_MESH | 0x728 | SDK CL-1341255 (was 0x718, +0x10) |
| ArcOffsets::StaticMeshGeometry::SM_BODY_SETUP | 0x1F0 | SDK CL-1341255 (was 0x1E8) |
| ArcOffsets::StaticMeshGeometry::SM_FLAG_BYTE_278 | 0x278 | UStaticMesh — not in SDK, keeping old |
| ArcOffsets::StaticMeshGeometry::SM_SUPPORT_RAYTRACE_MASK | 0x20 |  |
| ArcOffsets::StaticMeshGeometry::STATIC_MESH_RENDER_DATA | 0x150 |  |
| ArcOffsets::StaticMeshGeometry::VBUFFERS_POSITION_BUFFER | 0x58 |  |
| ArcOffsets::StaticMeshGeometry::VBUFFER_DATA_PTR | 0x0 |  |
| ArcOffsets::StowedWeaponLayout::QUALITY_VISUAL | 0x20 |  |
| ArcOffsets::StowedWeaponLayout::STOWED_ACTOR | 0x10 | AStowedWeaponActor |
| ArcOffsets::StowedWeaponLayout::STRUCT_SIZE | 0x40 |  |
| ArcOffsets::StowedWeaponLayout::WEAPON_QUALITY | 0x38 | int32_t |
| ArcOffsets::TViewTarget::POV | 0x10 | FMinimalViewInfo (size=0x930) |
| ArcOffsets::TViewTarget::TARGET | 0x0 | AActor* |
| ArcOffsets::UClass::DEFAULT_OBJECT | 0x130 | SDK CL-1341255 (was 0x138) |
| ArcOffsets::UStruct::CHILDREN | 0xB8 | SDK 2026-09-22 |
| ArcOffsets::UStruct::CHILD_PROPERTIES | 0xE0 | v20260922 dumper (was 0x100) |
| ArcOffsets::UStruct::MIN_ALIGNMENT | 0x94 | unchanged |
| ArcOffsets::UStruct::PROPERTIES_SIZE | 0xD8 | v20260922 dumper (was 0x110) |
| ArcOffsets::UWorld::DEFAULT_QUERY_PARAMS_RVA | 0xE7A3098 | needs re-scan for 2026-08-11 |
| ArcOffsets::UWorld::GAMESTATE | 0x0 | DEPRECATED: use LevelCollections[0]+0x08 instead |
| ArcOffsets::UWorld::INSTANCE_TIME | 0x240 | UWorld::TimeSeconds (double, NOT float); CL-1372005 user-ver |
| ArcOffsets::UWorld::INSTANCE_TIME_DOUBLE_HINT | 0x240 |  |
| ArcOffsets::UWorld::LEVEL_COLLECTIONS | 0x338 | SDK CL-1341255 (was 0x370); updated 2026-09-22 was 0x3A8 |
| ArcOffsets::UWorld::LINETRACE_RVA | 0x2F6E930 | needs re-scan for 2026-08-11 |
| ArcOffsets::UWorld::PHYSICS_FIELD | 0x508 | SDK CL-1341255: PhysicsField (was 0x570); updated 2026-09-08 |
| ArcOffsets::UWorld::PHYS_SCENE | 0x5C0 | SDK CL-1341255: PhysicsField (was 0x570) |
| ArcOffsets::UWorld::STREAMING_LEVELS | 0x170 | SDK CL-1341255 (was 0x170); updated 2026-09-22 was 0x158 |
| ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_MIE_SCATTERING_FADE_DISTANCE | 0x434 |  |
| ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_MIE_SCATTERING_START_DISTANCE | 0x430 |  |
| ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_RAYLEIGH_SCATTERING_FADE_DISTANCE | 0x42C |  |
| ArcOffsets::VolumetricCloudComponent::AERIAL_PERSPECTIVE_RAYLEIGH_SCATTERING_START_DISTANCE | 0x428 |  |
| ArcOffsets::VolumetricCloudComponent::B_USE_PER_SAMPLE_ATMOSPHERIC_LIGHT_TRANSMITTANCE | 0x3B8 | updated 2026-09-22 was 0x3F8 |
| ArcOffsets::VolumetricCloudComponent::GROUND_ALBEDO | 0x38C | updated 2026-09-08 was 0x3E8; updated 2026-09-22 was 0x3CC |
| ArcOffsets::VolumetricCloudComponent::LAYER_BOTTOM_ALTITUDE | 0x370 | SDK CL-1341255 (was 0x3C0, +0x10); updated 2026-09-08 was 0x |
| ArcOffsets::VolumetricCloudComponent::LAYER_HEIGHT | 0x374 | updated 2026-09-08 was 0x3D4; updated 2026-09-22 was 0x3B4 |
| ArcOffsets::VolumetricCloudComponent::MATERIAL | 0x390 | updated 2026-09-08 was 0x3F0; updated 2026-09-22 was 0x3D0 |
| ArcOffsets::VolumetricCloudComponent::PLANET_RADIUS | 0x388 | updated 2026-09-08 was 0x3E4; updated 2026-09-22 was 0x3C8 |
| ArcOffsets::VolumetricCloudComponent::REFLECTION_SAMPLE_COUNT_SCALE | 0x3CC | updated 2026-09-22 was 0x40C |
| ArcOffsets::VolumetricCloudComponent::REFLECTION_VIEW_SAMPLE_COUNT_SCALE | 0x3C8 | updated 2026-09-22 was 0x408 |
| ArcOffsets::VolumetricCloudComponent::REFLECTION_VIEW_SAMPLE_COUNT_SCALE_VALUE | 0x3C4 | updated 2026-09-22 was 0x404 |
| ArcOffsets::VolumetricCloudComponent::SHADOW_REFLECTION_SAMPLE_COUNT_SCALE | 0x3DC | updated 2026-09-22 was 0x41C |
| ArcOffsets::VolumetricCloudComponent::SHADOW_REFLECTION_VIEW_SAMPLE_COUNT_SCALE | 0x3D8 | updated 2026-09-22 was 0x418 |
| ArcOffsets::VolumetricCloudComponent::SHADOW_REFLECTION_VIEW_SAMPLE_COUNT_SCALE_VALUE | 0x3D4 | updated 2026-09-22 was 0x414 |
| ArcOffsets::VolumetricCloudComponent::SHADOW_TRACING_DISTANCE | 0x3E0 | updated 2026-09-22 was 0x420 |
| ArcOffsets::VolumetricCloudComponent::SHADOW_VIEW_SAMPLE_COUNT_SCALE | 0x3D0 | updated 2026-09-22 was 0x410 |
| ArcOffsets::VolumetricCloudComponent::SKY_LIGHT_CLOUD_BOTTOM_OCCLUSION | 0x3BC | updated 2026-09-22 was 0x3FC |
| ArcOffsets::VolumetricCloudComponent::STOP_TRACING_TRANSMITTANCE_THRESHOLD | 0x3E4 | updated 2026-09-22 was 0x424 |
| ArcOffsets::VolumetricCloudComponent::TRACING_MAX_DISTANCE | 0x384 | updated 2026-09-08 was 0x3E0; updated 2026-09-22 was 0x3C4 |
| ArcOffsets::VolumetricCloudComponent::TRACING_MAX_DISTANCE_MODE | 0x380 | updated 2026-09-08 was 0x3DC; updated 2026-09-22 was 0x3C0 |
| ArcOffsets::VolumetricCloudComponent::TRACING_START_MAX_DISTANCE | 0x378 | updated 2026-09-08 was 0x3D8; updated 2026-09-22 was 0x3B8 |
| ArcOffsets::VolumetricCloudComponent::VIEW_SAMPLE_COUNT_SCALE | 0x3C0 | updated 2026-09-22 was 0x400 |
| ArcOffsets::WineTeb::THREAD_TLS | 0x58 | TEB->ThreadLocalStoragePointer at gs_base+0x58 |
| ArcOffsets::WorldPartitionMiniMap::MINIMAP_TEXTURE | 0x420 | SDK CL-1341255 (was 0x3F8); updated 2026-09-08 was 0x410; up |
| ArcOffsets::WorldPartitionMiniMap::WORLD_UNITS_PER_PIXEL | 0x478 | SDK CL-1341255 (was 0x450); updated 2026-09-09 was 0x468; up |
| FName::CHUNK_INDEX_MASK | 0xFFFF |  |
| FName::CHUNK_INDEX_SHIFT | 0x10 |  |
| FName::FFIELD_ADD2_CONST | 0xBB34B0C01E5AA000 |  |
| FName::FFIELD_ADD_CONST | 0x44CB31F912F95FFF |  |
| FName::FFIELD_NAME_OFF | 0x90 |  |
| FName::FNV_PRIME_COMMON | 0x100000001B3 |  |
| FName::HASH_PRIME | 0x1000193 |  |
| FName::RVA_GNAMEPOOL | 0x10987D40 |  |
| FName::RVA_KEYSTREAM | 0x1082B26C |  |
| FName::SLOT_CLMUL_K1 | 0x8E9484400ADF26C1 |  |
| FName::SLOT_CLMUL_K2 | 0x1ADA36649C975181 |  |
| FName::UOBJ_NAME_ROL64 | 0x20 |  |
| FName::UOBJ_SLOT_CLASS_ADJ | 0x0 |  |
| FName::UOBJ_SLOT_HASH_ADD | 0x2306CC41 |  |
| FName::UOBJ_SLOT_NAME_XOR | 0x2 |  |
| FName::UOBJ_SLOT_OUTER_ADJ | 0x1 |  |
| GameInstanceStaticDecrypt::FNV32 | 0x1000193 |  |
| GameInstanceStaticDecrypt::FNV64 | 0x100000001B3 |  |
| GameInstanceStaticDecrypt::K1 | 0x742217C8 | subtracted each stage |
| GameInstanceStaticDecrypt::K2 | 0x5E838 |  |
| GameInstanceStaticDecrypt::NEG109 | 0xFFFFFF93 |  |
| GameInstanceTlsDecrypt::HASH_ADD | 0x957E395C |  |
| GameInstanceTlsDecrypt::KEY_FNV_ADD | 0xB92AB41238A38FDC |  |
| GameInstanceTlsDecrypt::KEY_FNV_PRIME | 0x100000001B3 |  |
| GameInstanceTlsDecrypt::KEY_FNV_ROT_1 | 0x26 |  |
| GameInstanceTlsDecrypt::KEY_FNV_ROT_2 | 0x2B |  |
| GameInstanceTlsDecrypt::TIB_TLS_SLOTS_OFF | 0x58 |  |
| GameInstanceTlsDecrypt::TLS_INDEX_RVA | 0xEAF5D78 | stale — scan_tls now iterates all slots |
| GameInstanceTlsDecrypt::TLS_INIT_FLAG_OFF | 0xED0 | user-provided 2026-09-09 (was 0x11F0) |
| GameInstanceTlsDecrypt::TLS_KEY_TABLE_OFF | 0xEE0 | StageABase — 8 blocks start here (was 0x1200) |
| GameInstanceTlsDecrypt::TLS_MAX_SLOTS | 0x440 | Windows PE TLS array cap |
| GameInstanceTlsDecrypt::TLS_TABLE_STRIDE | 0x90 | BlockStride (was 0x80) |
| GameInstanceTlsDecrypt::TLS_VT_FN_INDEX_OFF | 0x48 | FuncSlot at vtable+0x48 (was 0x38) |
| GameInstanceTlsDecrypt::TLS_XMM_INPUT_OFF | 0x80 | XmmOffset — 16-byte input at block+0x80 |
| OuterDecrypt::XOR_MASK | 0x9A492C85DDF6F193 |  |
| TebDecrypt::TEB_SELF_PTR_OFF | 0x30 |  |

## Full table

| constant | shipped | state | source | provenance | verdict |
| --- | --- | --- | --- | --- | --- |
| APlayerCameraManager | 0x4D0 | INAPPLICABLE | ArcOffsets::PlayerController::PLAYER_CAMERA_MANAGER | live-pinned(6) | drop-unconfirmed says 0x4E0 but the drop marks it unconfirmed (WRONG for CL-1341255: 0x4E0 = CheatClass) - not evidence |
| PlayerState_Armor | 0x560 | INAPPLICABLE | ArcOffsets::PlayerHealthInfo::ARMOR | derived(2) | drop says 0x10 in namespace ArcOffsets, which this build's dump has no class for - name collision, not this slot |
| PlayerState_Health | 0x550 | INAPPLICABLE | ArcOffsets::PlayerHealthInfo::HEALTH | derived(2) | drop says 0x0 in namespace ArcOffsets, which this build's dump has no class for - name collision, not this slot |
| PlayerState_MaxArmor | 0x568 | INAPPLICABLE | ArcOffsets::PlayerHealthInfo::MAX_ARMOR | derived(2) | drop says 0x18 in namespace ArcOffsets, which this build's dump has no class for - name collision, not this slot |
| ComponentToWorld_Alt | 0x2D0 | BOTH | ArcOffsets::SceneComponent::COMPONENT_TO_WORLD | probed(1) | drop-fresh says 0x310, shipped value is probed; 0x310 also ships as ComponentToWorld - a runtime probe decides |
| Mesh_LastRenderTimeOnScreenEnc | 0x4C4 | RETIRED | lit:0x4C4 | probed(1) | the drop retired this: 0x4C4 is its plain LAST_RENDER_TIME_ON_SCREEN, and the drop says live-verified 2026-08-20 (was 0x148); found UWorld ptr at CharacterMesh0+0x140 on player W |
| AIController_Perception | 0x4A8 | PROBED | drop:AIController::PERCEPTION_COMPONENT | drop-file(3) | no offline source can see this slot |
| AISight_PeripheralDeg | 0xC8 | PROBED | drop:AISenseConfigSight::PERIPHERAL_VISION_DEG | drop-file(3) | no offline source can see this slot |
| AcknowledgedPawn_Fallback | 0x3D8 | PROBED | lit:0x3D8 | probed(1) | no offline source can see this slot |
| ActorID | 0x18 | PROBED | lit:0x18 | probed(1) | no offline source can see this slot |
| ActorTypeId | 0xB0 | PROBED | lit:0xB0 | probed(1) | no offline source can see this slot |
| BBItem_AmountValue | 0x10 | PROBED | lit:0x4 + 0xC | probed(1) | no offline source can see this slot |
| BBItem_DataAssetIndex | 0x0 | PROBED | lit:0x0 | probed(1) | no offline source can see this slot |
| CameraFOV | 0x4D0 | PROBED | expr:CameraPOV_FOV | derived(2) | derived expression |
| CameraLocation | 0x480 | PROBED | expr:CameraPOV_Location | derived(2) | derived expression |
| CameraPOV_FOV | 0x4D0 | PROBED | expr:ViewTarget + 0x10 + 0x60 | derived(2) | derived expression |
| CameraPOV_Location | 0x480 | PROBED | expr:ViewTarget + 0x10 + 0x10 | derived(2) | derived expression |
| CameraPOV_Rotation | 0x4A8 | PROBED | expr:ViewTarget + 0x10 + 0x38 | derived(2) | derived expression |
| CameraRotation | 0x4A8 | PROBED | expr:CameraPOV_Rotation | derived(2) | derived expression |
| ClassDefaultObject | 0x70 | PROBED | lit:0x70 | probed(1) | no offline source can see this slot |
| ClassDefaultObjectAlt | 0x78 | PROBED | lit:0x78 | probed(1) | no offline source can see this slot |
| ComponentToWorld_Scale3D | 0x350 | PROBED | expr:ComponentToWorld + Transform_Scale3D | derived(2) | derived expression |
| ComponentToWorld_Translation | 0x330 | PROBED | expr:ComponentToWorld + Transform_Translation | derived(2) | derived expression |
| Encrypted | 0x7B0 | PROBED | lit:0x7B0 | probed(1) | no offline source can see this slot |
| Encrypted_Legacy | 0x790 | PROBED | lit:0x790 | probed(1) | no offline source can see this slot |
| FFieldClass_SuperClass | 0x40 | PROBED | lit:0x40 | probed(1) | no offline source can see this slot |
| FFieldNameKey0Rva | 0xE7B5330 | PROBED | lit:0xE7B5330 | probed(1) | no offline source can see this slot |
| FFieldNameKey1Rva | 0xE6F0554 | PROBED | lit:0xE6F0554 | probed(1) | no offline source can see this slot |
| FNameBlockMaskRva | 0xB523C50 | PROBED | lit:0xB523C50 | probed(1) | no offline source can see this slot |
| FNameEntry_LenMask | 0x3FF | PROBED | lit:0x03FF | probed(1) | no offline source can see this slot |
| FNameEntry_LenShift | 0x6 | PROBED | lit:0x0006 | probed(1) | no offline source can see this slot |
| FNameEntry_TextOffset | 0x2 | PROBED | lit:0x02 | probed(1) | no offline source can see this slot |
| FNameHeaderLenShift | 0xD | PROBED | lit:13 | probed(1) | no offline source can see this slot |
| FNameKeyTableRva | 0x1082B26C | PROBED | sdk:KEYTABLE | header-drop(4) | from SDK.hpp game::offsets |
| FNameKeystreamLegacyRva | 0xE2997F4 | PROBED | lit:0xE2997F4 | probed(1) | no offline source can see this slot |
| FNamePool_BlockOffsetBits | 0x10 | PROBED | lit:0x10 | probed(1) | no offline source can see this slot |
| FNamePool_Blocks | 0x40 | PROBED | lit:0x40 | probed(1) | no offline source can see this slot |
| FNamePool_EntryStride | 0x2 | PROBED | lit:0x02 | probed(1) | no offline source can see this slot |
| FStringVerificationRva | 0xD3C29A0 | PROBED | sdk:FString_Verification_Offset | header-drop(4) | from SDK.hpp game::offsets |
| GNamePoolRva | 0x10987D40 | PROBED | sdk:GNAMES | header-drop(4) | from SDK.hpp game::offsets |
| GObjPshufbMaskRva | 0xAD97CC0 | PROBED | lit:0xAD97CC0 | probed(1) | no offline source can see this slot |
| GObjectsRva | 0x10C56FE0 | PROBED | lit:0x10C56FE0 | probed(1) | no offline source can see this slot |
| GObjects_NumElements | 0xC | PROBED | lit:0x0C | probed(1) | no offline source can see this slot |
| GUObjectArrayChunksRva | 0x10C56FE0 | PROBED | expr:GObjectsRva | derived(2) | derived expression |
| GameState | 0x338 | PROBED | expr:LevelCollections | derived(2) | derived expression |
| HealthInfo | 0x550 | PROBED | lit:0x550 | derived(2) | no offline source can see this slot |
| Interact_ActiveInstigator | 0x658 | PROBED | drop:BaseInteractionComponent::ACTIVE_INSTIGATOR | drop-file(3) | no offline source can see this slot |
| Interact_DBNOTimerFloats | 0x428 | PROBED | drop:BaseInteractionComponent::DBNO_TIMER_FLOATS | drop-file(3) | no offline source can see this slot |
| Interact_DBNOTimerTicks | 0x430 | PROBED | drop:BaseInteractionComponent::DBNO_TIMER_TICKS | drop-file(3) | no offline source can see this slot |
| Interact_DefaultInstigator | 0x660 | PROBED | drop:BaseInteractionComponent::DEFAULT_INSTIGATOR | drop-file(3) | no offline source can see this slot |
| Interact_DefibTimerFloats | 0x448 | PROBED | drop:BaseInteractionComponent::DEFIB_TIMER_FLOATS | drop-file(3) | no offline source can see this slot |
| Interact_DefibTimerTicks | 0x450 | PROBED | drop:BaseInteractionComponent::DEFIB_TIMER_TICKS | drop-file(3) | no offline source can see this slot |
| Interaction_bIsActiveByte | 0x137 | PROBED | lit:0x137 | probed(1) | no offline source can see this slot |
| Interaction_bIsActiveMask | 0x8 | PROBED | lit:0x8 | probed(1) | no offline source can see this slot |
| IsRenderedTime | 0x1F0 | PROBED | lit:0x1F0 | probed(1) | no offline source can see this slot |
| ItemBase_Quality | 0x104 | PROBED | drop:ItemBase::QUALITY_LEVEL | drop-file(3) | no offline source can see this slot |
| ItemDataAsset | 0x898 | PROBED | lit:0x898 | probed(1) | no offline source can see this slot |
| ItemDataAsset_OverrideItemAssetId | 0x120 | PROBED | lit:0x120 | probed(1) | no offline source can see this slot |
| ItemDataAsset_bOverrideItemAssetId | 0x118 | PROBED | lit:0x118 | probed(1) | no offline source can see this slot |
| ItemUIHoverData_Size | 0x28 | PROBED | lit:0x28 | probed(1) | no offline source can see this slot |
| LastSubmitTime | 0x480 | PROBED | expr:LastRenderTime | derived(2) | derived expression |
| LodSelect | 0x7D0 | PROBED | lit:0x7D0 | probed(1) | no offline source can see this slot |
| LodSelect_Legacy | 0x7D0 | PROBED | lit:0x7D0 | probed(1) | no offline source can see this slot |
| LootContainer_ItemContainer | 0xBD8 | PROBED | lit:0xBD8 | probed(1) | no offline source can see this slot |
| LootInteract_OpenedMask | 0x1 | PROBED | drop:LootInteractionComponent::MASK_HAS_BEEN_OPENED | drop-file(3) | no offline source can see this slot |
| LootInteraction_Container | 0xBB8 | PROBED | expr:LootInteractionComponent | derived(2) | derived expression |
| LootInteraction_Searched | 0xBD8 | PROBED | lit:0xBD8 | probed(1) | no offline source can see this slot |
| Mesh_LastRenderTimeEnc | 0x4BC | PROBED | lit:0x4BC | probed(1) | no offline source can see this slot |
| Mesh_LastRenderTimeKey | 0x5AB299E0 | PROBED | lit:0x5AB299E0 | probed(1) | no offline source can see this slot |
| Mesh_LastRenderTimeOnScreenKey | 0xA83E5CBE | PROBED | lit:0xA83E5CBE | probed(1) | no offline source can see this slot |
| MiniMap_UnitsPerPixel | 0x478 | PROBED | drop:WorldPartitionMiniMap::WORLD_UNITS_PER_PIXEL | drop-file(3) | no offline source can see this slot |
| OuterDecrypt_HashSeedOff | 0x10 | PROBED | lit:0x10 | probed(1) | no offline source can see this slot |
| OuterDecrypt_HashShr | 0x10 | PROBED | lit:0x10 | probed(1) | no offline source can see this slot |
| OuterDecrypt_SlotMask | 0x3 | PROBED | lit:0x3 | probed(1) | no offline source can see this slot |
| OuterDecrypt_SlotXor | 0x2 | PROBED | lit:0x2 | probed(1) | no offline source can see this slot |
| OuterDecrypt_XorMask | 0x9A492C85DDF6F193 | PROBED | drop:OuterDecrypt::XOR_MASK | drop-file(3) | no offline source can see this slot |
| PHI_BrokenArmorByte | 0x20 | PROBED | drop:PlayerHealthInfo::B_HAS_BROKEN_ARMOR | drop-file(3) | no offline source can see this slot |
| Pickup_ContainedItem_BB | 0x4A0 | PROBED | lit:0x4A0 | probed(1) | no offline source can see this slot |
| PlayerController_bIsLocalPlayerController_Mask | 0x1 | PROBED | lit:0x1 | probed(1) | no offline source can see this slot |
| PlayerDecrypt_LocalPlayerOffset | 0x4B0 | PROBED | lit:0x4B0 | probed(1) | no offline source can see this slot |
| PlayerHealthInfoBase | 0x588 | PROBED | drop:PlayerState::HEALTH_INFO_BASE | drop-file(3) | no offline source can see this slot |
| PlayerNameOnPawn | 0x438 | PROBED | lit:0x438 | probed(1) | no offline source can see this slot |
| PlayerState_MaxHealth | 0x558 | PROBED | lit:0x558 | derived(2) | no offline source can see this slot |
| ProcessEventIndex | 0x4C | PROBED | lit:0x4C | probed(1) | no offline source can see this slot |
| ProcessEventRva | 0x5AF560 | PROBED | lit:0x5AF560 | probed(1) | no offline source can see this slot |
| RF_BeginDestroyed | 0x800000 | PROBED | lit:0x00800000 | probed(1) | no offline source can see this slot |
| RF_FinishDestroyed | 0x1000000 | PROBED | lit:0x01000000 | probed(1) | no offline source can see this slot |
| RepMovement_bRepPhysicsMask | 0x2 | PROBED | lit:0x02 | probed(1) | no offline source can see this slot |
| ReplicatedRootTransform | 0x1F8 | PROBED | lit:0x1F8 | probed(1) | no offline source can see this slot |
| ShieldMax | 0x1D0 | PROBED | expr:Shield + 0x10 | derived(2) | derived expression |
| SimpleLootActivity_ItemContainer | 0x498 | PROBED | lit:0x498 | probed(1) | no offline source can see this slot |
| SimpleLootActivity_LootInteraction | 0x480 | PROBED | lit:0x480 | probed(1) | no offline source can see this slot |
| SimpleLootActivity_LootStateMachine | 0x4A8 | PROBED | lit:0x4A8 | probed(1) | no offline source can see this slot |
| StaticMeshLegacy | 0x718 | PROBED | lit:0x718 | probed(1) | no offline source can see this slot |
| StowedInfo_Quality | 0x38 | PROBED | drop:StowedWeaponLayout::WEAPON_QUALITY | drop-file(3) | no offline source can see this slot |
| StowedInfo_Size | 0x40 | PROBED | drop:StowedWeaponLayout::STRUCT_SIZE | drop-file(3) | no offline source can see this slot |
| StyleDrivers | 0x610 | PROBED | drop:ConstructableBase::ALL_STYLE_DRIVERS | drop-file(3) | no offline source can see this slot |
| UIHoverData | 0x620 | PROBED | lit:0x620 | probed(1) | no offline source can see this slot |
| UIHoverData_Pickup | 0x620 | PROBED | expr:UIHoverData | derived(2) | derived expression |
| UObject_ClassPrivate | 0x20 | PROBED | lit:0x20 | probed(1) | no offline source can see this slot |
| UObject_NamePrivate | 0x98 | PROBED | lit:0x98 | probed(1) | no offline source can see this slot |
| UObject_ObjectFlags | 0x8 | PROBED | lit:0x08 | probed(1) | no offline source can see this slot |
| UObject_OuterPrivate | 0xA0 | PROBED | lit:0xA0 | probed(1) | no offline source can see this slot |
| UWorld | 0x10839A98 | PROBED | sdk:GWORLD | header-drop(4) | from SDK.hpp game::offsets |
| UWorldGlobalIntermediary | 0x0 | PROBED | lit:0x0 | probed(1) | no offline source can see this slot |
| WorldLocation | 0x330 | PROBED | expr:ComponentToWorld_Translation | derived(2) | derived expression |
| AActors | 0x110 | AGREE | ArcOffsets::Level::ACTORS | probed(1) | confirmed by drop (ArcOffsets::Level::ACTORS) |
| AController_PlayerState | 0x3D0 | AGREE | ArcOffsets::Controller::PLAYER_STATE | drop-file(3) | confirmed by drop-fresh (ArcOffsets::Controller::PLAYER_STATE) |
| AIPerception_SensesConfig | 0x1B8 | AGREE | ArcOffsets::AIPerceptionComponent::SENSES_CONFIG | drop-file(3) | confirmed by drop-fresh (ArcOffsets::AIPerceptionComponent::SENSES_CONFIG) |
| AISight_LoseSightRadius | 0xC4 | AGREE | ArcOffsets::AISenseConfigSight::LOSE_SIGHT_RADIUS | drop-file(3) | confirmed by drop (ArcOffsets::AISenseConfigSight::LOSE_SIGHT_RADIUS) |
| AISight_SightRadius | 0xC0 | AGREE | ArcOffsets::AISenseConfigSight::SIGHT_RADIUS | drop-file(3) | confirmed by drop (ArcOffsets::AISenseConfigSight::SIGHT_RADIUS) |
| AIState_Alertness | 0x1BC | AGREE | ArcOffsets::AIStateService::ALERTNESS | drop-file(3) | confirmed by drop (ArcOffsets::AIStateService::ALERTNESS) |
| AIState_CombatPhase | 0x1C9 | AGREE | ArcOffsets::AIStateService::COMBAT_PHASE | drop-file(3) | confirmed by drop (ArcOffsets::AIStateService::COMBAT_PHASE) |
| AIState_SightHalfAngle | 0x1CA | AGREE | ArcOffsets::AIStateService::SIGHT_HALF_ANGLE | drop-file(3) | confirmed by drop (ArcOffsets::AIStateService::SIGHT_HALF_ANGLE) |
| AIState_SightRange | 0x1CC | AGREE | ArcOffsets::AIStateService::SIGHT_RANGE | drop-file(3) | confirmed by drop (ArcOffsets::AIStateService::SIGHT_RANGE) |
| APlayerState | 0x3E0 | AGREE | ArcOffsets::Pawn::PLAYER_STATE | drop-file(3) | confirmed by drop-fresh (ArcOffsets::Pawn::PLAYER_STATE) |
| AcknowledgedPawn | 0x418 | AGREE | ArcOffsets::Controller::ACKNOWLEDGED_PAWN | drop-file(3) | confirmed by drop (ArcOffsets::Controller::ACKNOWLEDGED_PAWN) |
| ActorCluster | 0x158 | AGREE | ArcOffsets::Level::ACTOR_CLUSTER | dump(5) | dump: reflected property Engine.Level.ActorCluster = 0x158 |
| ActorInstigator | 0x220 | AGREE | prop:Engine.Actor.Instigator | dump(5) | dump: reflected property Engine.Actor.Instigator = 0x220 |
| ActorOwner | 0x1D8 | AGREE | prop:Engine.Actor.Owner | dump(5) | dump: reflected property Engine.Actor.Owner = 0x1D8 |
| Actor_FlagsDd | 0xDD | AGREE | prop:Engine.Actor.bActorEnableCollision | dump(5) | dump: reflected property Engine.Actor.bActorEnableCollision = 0xDD |
| Actor_InstanceComponents | 0x398 | AGREE | prop:Engine.Actor.InstanceComponents | dump(5) | dump: reflected property Engine.Actor.InstanceComponents = 0x398 |
| Actor_bActorEnableCollisionMask | 0x2 | AGREE | maskOf:Engine.Actor.bActorEnableCollision | dump(5) | dump: Engine.Actor.bActorEnableCollision mask = 0x2 |
| Actor_bActorIsBeingDestroyedMask | 0x4 | AGREE | maskOf:Engine.Actor.bActorIsBeingDestroyed | dump(5) | dump: Engine.Actor.bActorIsBeingDestroyed mask = 0x4 |
| Actor_bHiddenByte | 0xD9 | AGREE | ArcOffsets::Actor::B_HIDDEN_BYTE | dump(5) | dump: reflected property Engine.Actor.bHidden = 0xD9 |
| Actor_bHiddenMask | 0x1 | AGREE | ArcOffsets::Actor::B_HIDDEN_MASK | dump(5) | dump: Engine.Actor.bHidden mask = 0x1 |
| ActorsCount | 0x118 | AGREE | ArcOffsets::Level::ACTOR_COUNT | derived(2) | confirmed by drop (ArcOffsets::Level::ACTOR_COUNT) |
| AttachChildren | 0x1F8 | AGREE | ArcOffsets::SceneComponent::ATTACH_CHILDREN | dump(5) | dump: reflected property Engine.SceneComponent.AttachChildren = 0x1F8 |
| AuthorityGameMode | 0x310 | AGREE | ArcOffsets::UWorld::AUTHORITY_GAME_MODE | dump(5) | dump: reflected property Engine.World.AuthorityGameMode = 0x310 |
| BP_PickupBase_SpawnItems | 0x560 | AGREE | ArcOffsets::Pickup::SPAWN_ITEMS | dump(5) | dump: reflected property Angelscript.Pickup.SpawnItems = 0x560 |
| BoneV922AddKeyLo | 0x3FA0DEFFD4B91F00 | AGREE | ArcOffsets::BoneArrayDecrypt::ADD_KEY_LO | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::ADD_KEY_LO) |
| BoneV922DescriptorBase | 0x18 | AGREE | ArcOffsets::BoneArrayDecrypt::DESCRIPTOR_BASE | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::DESCRIPTOR_BASE) |
| BoneV922DescriptorStride | 0x10 | AGREE | ArcOffsets::BoneArrayDecrypt::DESCRIPTOR_STRIDE | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::DESCRIPTOR_STRIDE) |
| BoneV922Rol32 | 0x3 | AGREE | ArcOffsets::BoneArrayDecrypt::ROL32_AMOUNT | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::ROL32_AMOUNT) |
| BoneV922SeedOffset | 0x7B0 | AGREE | ArcOffsets::BoneArrayDecrypt::SEED_OFFSET | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::SEED_OFFSET) |
| BoneV922SelectorMask | 0x1 | AGREE | ArcOffsets::BoneArrayDecrypt::SELECTOR_MASK | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::SELECTOR_MASK) |
| BoneV922SelectorOffset | 0x848 | AGREE | ArcOffsets::BoneArrayDecrypt::SELECTOR_OFFSET | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::SELECTOR_OFFSET) |
| BoneV922SelectorShift | 0xF | AGREE | ArcOffsets::BoneArrayDecrypt::SELECTOR_SHIFT | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::SELECTOR_SHIFT) |
| BoneV922Stride | 0x60 | AGREE | ArcOffsets::BoneArrayDecrypt::BONE_STRIDE | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::BONE_STRIDE) |
| BoneV922XorKeyLo | 0xC05F21012B46E100 | AGREE | ArcOffsets::BoneArrayDecrypt::XOR_KEY_LO | probed(1) | confirmed by drop (ArcOffsets::BoneArrayDecrypt::XOR_KEY_LO) |
| BoundsScale | 0x468 | AGREE | ArcOffsets::PrimitiveComponent::BOUNDS_SCALE | dump(5) | dump: reflected property Engine.PrimitiveComponent.BoundsScale = 0x468 |
| CMC_LastUpdateLocation | 0x460 | AGREE | prop:Engine.CharacterMovementComponent.LastUpdateLocation | dump(5) | dump: reflected property Engine.CharacterMovementComponent.LastUpdateLocation = 0x460 |
| CharacterMovement | 0x448 | AGREE | prop:Engine.Character.CharacterMovement | dump(5) | dump: reflected property Engine.Character.CharacterMovement = 0x448 |
| Character_CapsuleComponent | 0x450 | AGREE | prop:Engine.Character.CapsuleComponent | dump(5) | dump: reflected property Engine.Character.CapsuleComponent = 0x450 |
| ChunksManagerAddRva | 0xD395270 | AGREE | FName::RVA_CHUNKMGR_ADD | drop-file(3) | confirmed by drop (FName::RVA_CHUNKMGR_ADD) |
| ChunksManagerArrayOff | 0x20 | AGREE | FName::MGR_CHUNKARRAY_OFF | drop-file(3) | confirmed by drop (FName::MGR_CHUNKARRAY_OFF) |
| ChunksManagerArrayXor | 0x464A80E000000000 | AGREE | FName::MGR_CHUNKARRAY_XOR | drop-file(3) | confirmed by drop (FName::MGR_CHUNKARRAY_XOR) |
| ChunksManagerNumElementsOff | 0x2C | AGREE | FName::MGR_NUMELEMENTS_OFF | drop-file(3) | confirmed by drop (FName::MGR_NUMELEMENTS_OFF) |
| ChunksManagerNumElementsXor | 0x68D83F2C | AGREE | FName::MGR_NUMELEMENTS_XOR | drop-file(3) | confirmed by drop (FName::MGR_NUMELEMENTS_XOR) |
| ChunksManagerRol32 | 0x3 | AGREE | FName::CHUNKMGR_ROL32 | drop-file(3) | confirmed by drop (FName::CHUNKMGR_ROL32) |
| ChunksManagerRva | 0x10C57060 | AGREE | FName::RVA_CHUNKMGR_GLOBAL | drop-file(3) | confirmed by drop (FName::RVA_CHUNKMGR_GLOBAL) |
| ChunksManagerXorRva | 0xD395260 | AGREE | FName::RVA_CHUNKMGR_XOR | drop-file(3) | confirmed by drop (FName::RVA_CHUNKMGR_XOR) |
| ComponentToWorld | 0x310 | AGREE | ArcOffsets::SceneComponent::COMPONENT_TO_WORLD | probed(1) | ArcOffsets::SceneComponent::COMPONENT_TO_WORLD = 0x310 (v20260922 dumper + user); translation at +0x20 => 0x330 |
| ComponentToWorld_Rotation | 0x310 | AGREE | ArcOffsets::SceneComponent::COMPONENT_TO_WORLD | drop-value(4) | drop carries this value as ArcOffsets::SceneComponent::COMPONENT_TO_WORLD (renamed slot) |
| ComponentVelocity | 0x2B0 | AGREE | ArcOffsets::SceneComponent::COMPONENT_VELOCITY | dump(5) | dump: reflected property Engine.SceneComponent.ComponentVelocity = 0x2B0 |
| ConstructableItemContainer_OpenTime | 0x470 | AGREE | prop:Angelscript.ConstructableItemContainerComponent.OpenTime | dump(5) | dump: reflected property Angelscript.ConstructableItemContainerComponent.OpenTime = 0x470 |
| Constructable_AIStateService | 0x12C0 | AGREE | ArcOffsets::PioneerConstructablePawn::AI_STATE_SERVICE | drop-file(3) | confirmed by drop (ArcOffsets::PioneerConstructablePawn::AI_STATE_SERVICE) |
| Constructable_AITemplateData | 0x11B0 | AGREE | ArcOffsets::PioneerConstructablePawn::AI_TEMPLATE_DATA | dump(5) | dump: reflected property Angelscript.PioneerConstructablePawn.AITemplateData = 0x11B0 |
| Constructable_EnemyTypeDataAsset | 0x11C0 | AGREE | ArcOffsets::PioneerConstructablePawn::ENEMY_TYPE_DATA_ASSET | dump(5) | dump: reflected property Angelscript.PioneerConstructablePawn.EnemyTypeDataAsset = 0x11C0 |
| Constructable_HealthService | 0x1280 | AGREE | ArcOffsets::PioneerConstructablePawn::HEALTH_SERVICE | drop-file(3) | confirmed by drop (ArcOffsets::PioneerConstructablePawn::HEALTH_SERVICE) |
| Constructable_bIsDestroyed | 0x1230 | AGREE | ArcOffsets::PioneerConstructablePawn::bIsDestroyed | dump(5) | dump: reflected property Angelscript.PioneerConstructablePawn.bIsDestroyed = 0x1230 |
| ControlRotation | 0x438 | AGREE | ArcOffsets::Controller::CONTROL_ROTATION | drop-file(3) | confirmed by drop-fresh (ArcOffsets::Controller::CONTROL_ROTATION) |
| Controller_Character | 0x400 | AGREE | ArcOffsets::Controller::CHARACTER | dump(5) | dump: reflected property Engine.Controller.Character = 0x400 |
| CurrentItemActors | 0x520 | AGREE | prop:Angelscript.InventoryComponent.CurrentItemActors | dump(5) | dump: reflected property Angelscript.InventoryComponent.CurrentItemActors = 0x520 |
| DefaultFOV | 0x430 | AGREE | prop:Engine.PlayerCameraManager.DefaultFOV | dump(5) | dump: reflected property Engine.PlayerCameraManager.DefaultFOV = 0x430 |
| EmbarkGS_AllSquads | 0x6F0 | AGREE | ArcOffsets::EmbarkGameStateBase::ALL_SQUADS | drop-file(3) | confirmed by drop-fresh (ArcOffsets::EmbarkGameStateBase::ALL_SQUADS) |
| EmbarkGS_ElapsedTime | 0x4D0 | AGREE | ArcOffsets::EmbarkGameStateBase::ELAPSED_TIME | drop-file(3) | confirmed by drop (ArcOffsets::EmbarkGameStateBase::ELAPSED_TIME) |
| EmbarkGS_MatchState | 0x4C0 | AGREE | ArcOffsets::EmbarkGameStateBase::MATCH_STATE | drop-file(3) | confirmed by drop (ArcOffsets::EmbarkGameStateBase::MATCH_STATE) |
| EmbarkGS_SquadsArray | 0x4E8 | AGREE | ArcOffsets::EmbarkGameStateBase::SQUADS_ARRAY | drop-file(3) | confirmed by drop (ArcOffsets::EmbarkGameStateBase::SQUADS_ARRAY) |
| EmbarkMesh | 0x7B8 | AGREE | prop:EmbarkCharacter.EmbarkCharacterBase.EmbarkMesh | dump(5) | dump: reflected property EmbarkCharacter.EmbarkCharacterBase.EmbarkMesh = 0x7B8 |
| EquippedArmor | 0x588 | AGREE | prop:Angelscript.InventoryComponent.EquippedArmor | dump(5) | dump: reflected property Angelscript.InventoryComponent.EquippedArmor = 0x588 |
| EquippedPrimaryItem | 0x588 | AGREE | prop:Angelscript.InventoryComponent.EquippedArmor | dump(5) | dump: reflected property Angelscript.InventoryComponent.EquippedArmor = 0x588 |
| ExtractionPoint_ExtractionInfo | 0xBB8 | AGREE | ArcOffsets::ExtractionPoint::EXTRACTION_INFO | dump(5) | dump: reflected property Angelscript.SalvageExtractionPointBase.ExtractionInfo = 0xBB8 |
| ExtractionPoint_State | 0xC32 | AGREE | ArcOffsets::ExtractionPoint::STATE | dump(5) | dump: reflected property Angelscript.SalvageExtractionPointBase.State = 0xC32 |
| ExtractionPoint_StateChangeTimestamp | 0xC38 | AGREE | ArcOffsets::ExtractionPoint::STATE_CHANGE_TIMESTAMP | dump(5) | dump: reflected property Angelscript.SalvageExtractionPointBase.StateChangeTimestamp = 0xC38 |
| ExtractionPoint_TimeLeftAfterShutdown | 0xC98 | AGREE | ArcOffsets::ExtractionPoint::TIME_LEFT_AFTER_SHUTDOWN | dump(5) | dump: reflected property Angelscript.SalvageExtractionPoint.TimeLeftAfterShutdown = 0xC98 |
| ExtractionPoint_TimeLeftAfterStartup | 0xCA0 | AGREE | ArcOffsets::ExtractionPoint::TIME_LEFT_AFTER_STARTUP | dump(5) | dump: reflected property Angelscript.SalvageExtractionPoint.TimeLeftAfterStartup = 0xCA0 |
| ExtractionPoint_bIsEnabled | 0xC30 | AGREE | prop:Angelscript.SalvageExtractionPointBase.bIsEnabled | dump(5) | dump: reflected property Angelscript.SalvageExtractionPointBase.bIsEnabled = 0xC30 |
| FField_ClassPrivate | 0x60 | AGREE | ArcOffsets::FField::CLASS_PRIVATE | probed(1) | confirmed by drop-fresh (ArcOffsets::FField::CLASS_PRIVATE) |
| FNameBlockRol64 | 0x13 | AGREE | FName::BLOCK_ROL64 | drop-value(3) | drop carries this value as FName::BLOCK_ROL64 (renamed slot) |
| FNameBlockXor | 0xF401C0BE961D3D9A | AGREE | FName::BLOCK_XOR | drop-value(3) | drop carries this value as FName::BLOCK_XOR (renamed slot) |
| FNameEntry_WideBit | 0x1 | AGREE | FName::HDR_IS_WIDE_BIT | drop-file(3) | confirmed by drop (FName::HDR_IS_WIDE_BIT) |
| FNameFnvAdd | 0x6C5FD4827126D389 | AGREE | FName::FNV_ADD | drop-value(3) | drop carries this value as FName::FNV_ADD (renamed slot) |
| FNameFnvRol1 | 0x28 | AGREE | FName::FNV_ROL1 | drop-value(3) | drop carries this value as FName::FNV_ROL1 (renamed slot) |
| FNameFnvRol2 | 0x32 | AGREE | FName::FNV_ROL2 | drop-value(3) | drop carries this value as FName::FNV_ROL2 (renamed slot) |
| FNameKeystreamBaseIdx | 0xC | AGREE | FName::KEYSTREAM_BASE_IDX | drop-value(3) | drop carries this value as FName::KEYSTREAM_BASE_IDX (renamed slot) |
| FNameKeystreamCount | 0x100 | AGREE | FName::KEYSTREAM_COUNT | drop-value(3) | drop carries this value as FName::KEYSTREAM_COUNT (renamed slot) |
| FNameNarrowKeyShift | 0x3 | AGREE | FName::NARROW_KEY_SHIFT | drop-value(3) | drop carries this value as FName::NARROW_KEY_SHIFT (renamed slot) |
| FNameShardBlockBase | 0x50 | AGREE | FName::SHARD_BLOCK_BASE_OFF | drop-file(3) | confirmed by drop (FName::SHARD_BLOCK_BASE_OFF) |
| FNameShardBlockStride | 0x20 | AGREE | FName::SHARD_BLOCK_STRIDE | drop-value(3) | drop carries this value as FName::SHARD_BLOCK_STRIDE (renamed slot) |
| FNameShardFinalShr | 0x5 | AGREE | FName::SHARD_FINAL_SHR | drop-value(3) | drop carries this value as FName::SHARD_FINAL_SHR (renamed slot) |
| FNameShardHashAdd | 0xD69AD929 | AGREE | FName::SHARD_HASH_ADD | drop-value(3) | drop carries this value as FName::SHARD_HASH_ADD (renamed slot) |
| FNameShardRolA | 0x11 | AGREE | FName::SHARD_ROL_A | drop-value(3) | drop carries this value as FName::SHARD_ROL_A (renamed slot) |
| FNameShardRolB | 0x1B | AGREE | FName::SHARD_ROL_B | drop-value(3) | drop carries this value as FName::SHARD_ROL_B (renamed slot) |
| FNameShardSeedOff | 0x40 | AGREE | FName::SHARD_HASH_SEED_OFF | drop-file(3) | confirmed by drop (FName::SHARD_HASH_SEED_OFF) |
| FNameSlotBaseOff | 0x20 | AGREE | OuterDecrypt::SLOT_BASE_OFF | drop-value(3) | drop carries this value as OuterDecrypt::SLOT_BASE_OFF, FName::SLOT_BASE_OFF (renamed slot) |
| FNameSlotStride | 0x20 | AGREE | OuterDecrypt::SLOT_STRIDE | drop-value(3) | drop carries this value as OuterDecrypt::SLOT_STRIDE, FName::SLOT_STRIDE (renamed slot) |
| FObjectPropertyBase_PropertyClass | 0xE0 | AGREE | prop:CoreUObject.ObjectPropertyBase.PropertyClass | dump(5) | dump: reflected property CoreUObject.ObjectPropertyBase.PropertyClass = 0xE0 |
| FProperty_PropertyLinkNext | 0x98 | AGREE | prop:CoreUObject.Field.Next | dump(5) | dump: reflected property CoreUObject.Field.Next = 0x98 |
| GObjects_ElementsPerChunk | 0x10000 | AGREE | FName::ITEMS_PER_CHUNK | drop-file(3) | confirmed by drop (FName::ITEMS_PER_CHUNK) |
| GObjects_ItemStride | 0x18 | AGREE | FName::FUOBJECTITEM_STRIDE | drop-file(3) | confirmed by drop (FName::FUOBJECTITEM_STRIDE) |
| GObjects_Item_Object | 0x8 | AGREE | FName::FUOBJECTITEM_OBJ_OFF | drop-file(3) | confirmed by drop (FName::FUOBJECTITEM_OBJ_OFF) |
| GameInstanceShuffleMaskRva | 0xB09C350 | AGREE | ArcOffsets::GameInstanceDecrypt::SHUFFLE_MASK_RVA | probed(1) | confirmed by drop (ArcOffsets::GameInstanceDecrypt::SHUFFLE_MASK_RVA) |
| GameInstanceStaticAdd | 0x2A79E93E092D2538 | AGREE | GameInstanceStaticDecrypt::ADD64 | drop-file(3) | confirmed by drop (GameInstanceStaticDecrypt::ADD64) |
| GameInstanceStaticPshufbMaskRva | 0xDDFC1D0 | AGREE | GameInstanceStaticDecrypt::PSHUFB_MASK_RVA | drop-value(3) | drop carries this value as GameInstanceStaticDecrypt::PSHUFB_MASK_RVA (renamed slot) |
| GameInstanceStaticResultDeref | 0x18 | AGREE | GameInstanceStaticDecrypt::RESULT_DEREF | drop-value(3) | drop carries this value as GameInstanceStaticDecrypt::RESULT_DEREF (renamed slot) |
| GameInstanceStaticRot1 | 0x38 | AGREE | GameInstanceStaticDecrypt::ROT64_1 | drop-file(3) | confirmed by drop (GameInstanceStaticDecrypt::ROT64_1) |
| GameInstanceStaticRot2 | 0x21 | AGREE | GameInstanceStaticDecrypt::ROT64_2 | drop-file(3) | confirmed by drop (GameInstanceStaticDecrypt::ROT64_2) |
| GameInstanceStaticStageArrayRva | 0x10F35650 | AGREE | GameInstanceStaticDecrypt::STAGE_ARRAY_RVA | drop-value(3) | drop carries this value as GameInstanceStaticDecrypt::STAGE_ARRAY_RVA (renamed slot) |
| GameInstanceStaticXorMask | 0x291AED004FAE1FAC | AGREE | GameInstanceStaticDecrypt::XOR_MASK | drop-file(3) | confirmed by drop (GameInstanceStaticDecrypt::XOR_MASK) |
| GameInstanceXorKey0Rva | 0xB06C380 | AGREE | ArcOffsets::GameInstanceDecrypt::XOR_KEY_RVA | probed(1) | confirmed by drop (ArcOffsets::GameInstanceDecrypt::XOR_KEY_RVA) |
| GameInstanceXorKey1Rva | 0xB06C390 | AGREE | ArcOffsets::GameInstanceDecrypt::XOR_KEY_RVA_2 | drop-file(3) | confirmed by drop (ArcOffsets::GameInstanceDecrypt::XOR_KEY_RVA_2) |
| GameInstance_WorldBackRef | 0x2F0 | AGREE | ArcOffsets::GameInstanceDecrypt::WORLD_OFFSET | probed(1) | confirmed by drop-unconfirmed (ArcOffsets::GameInstanceDecrypt::WORLD_OFFSET) |
| GameStateGlobalRva | 0xDCA7C88 | AGREE | ArcOffsets::GameState::GAME_STATE_GLOBAL_RVA | probed(1) | confirmed by drop (ArcOffsets::GameState::GAME_STATE_GLOBAL_RVA) |
| GameState_EnemyCount | 0xA00 | AGREE | ArcOffsets::GameState::ENEMY_COUNT | dump(5) | dump: reflected property Angelscript.PioneerGameState.EnemyCount = 0xA00 |
| GameState_GamePhase | 0x9D0 | AGREE | ArcOffsets::GameState::GAME_PHASE | dump(5) | dump: reflected property Angelscript.PioneerGameState.GamePhase = 0x9D0 |
| GameState_PickupCount | 0xA04 | AGREE | ArcOffsets::GameState::PICKUP_COUNT | dump(5) | dump: reflected property Angelscript.PioneerGameState.PickupCount = 0xA04 |
| GameState_PlayerArray | 0x498 | AGREE | ArcOffsets::GameState::PLAYER_ARRAY | dump(5) | dump: reflected property Engine.GameStateBase.PlayerArray = 0x498 |
| GameState_StageInfoRef | 0x938 | AGREE | ArcOffsets::GameState::STAGE_INFO | drop-value(4) | drop carries this value as ArcOffsets::GameState::STAGE_INFO (renamed slot) |
| GameViewportClient_World | 0x1C8 | AGREE | ArcOffsets::GameViewportClient::WORLD | dump(5) | dump: reflected property Engine.GameViewportClient.World = 0x1C8 |
| GetObjectIdSimdMaskRva | 0xAD2FC50 | AGREE | ArcOffsets::GetObjectIdCrypto::SIMD_MASK_RVA | probed(1) | confirmed by drop (ArcOffsets::GetObjectIdCrypto::SIMD_MASK_RVA) |
| HC_MaxDBNOHealth | 0x380 | AGREE | ArcOffsets::HealthComponent::MAX_DBNO_HEALTH | drop-file(3) | confirmed by drop-fresh (ArcOffsets::HealthComponent::MAX_DBNO_HEALTH) |
| Health | 0x6D0 | AGREE | ArcOffsets::PlayerHealthInfo::HEALTH | live-pinned(6) | ArcOffsets::HealthComponent::CURRENT_HEALTH = 0x6D0 (the 0x700 dump field is an FName shadow) |
| HealthComponent | 0xDC0 | AGREE | ArcOffsets::PioneerPlayerCharacter::HEALTH_COMPONENT | dump(5) | dump: reflected property Angelscript.PioneerPlayerCharacter.HealthComponent = 0xDC0 |
| HealthNameShadow | 0x700 | AGREE | prop:Angelscript.HealthComponent.CachedHealth | dump(5) | dump: reflected property Angelscript.HealthComponent.CachedHealth = 0x700 |
| Interaction_CurrentInteractionState | 0x415 | AGREE | ArcOffsets::BaseInteractionComponent::CURRENT_INTERACTION_STATE | dump(5) | dump: reflected property Angelscript.BaseInteractionComponent.CurrentInteractionState = 0x415 |
| InventoryComponent | 0xC80 | AGREE | prop:Angelscript.PioneerPlayerCharacter.InventoryComponent | dump(5) | dump: reflected property Angelscript.PioneerPlayerCharacter.InventoryComponent = 0xC80 |
| Inventory_Backpack | 0x4A0 | AGREE | ArcOffsets::InventoryComponent::BACKPACK | drop-file(3) | confirmed by drop-fresh (ArcOffsets::InventoryComponent::BACKPACK) |
| Inventory_Belt | 0x4A8 | AGREE | ArcOffsets::InventoryComponent::BELT | drop-file(3) | confirmed by drop-fresh (ArcOffsets::InventoryComponent::BELT) |
| Inventory_Loadout | 0x2C0 | AGREE | ArcOffsets::InventoryComponent::LOADOUT | drop-file(3) | confirmed by drop-fresh (ArcOffsets::InventoryComponent::LOADOUT) |
| Inventory_SafePouch | 0x4B0 | AGREE | ArcOffsets::InventoryComponent::SAFE_POUCH | drop-file(3) | confirmed by drop-fresh (ArcOffsets::InventoryComponent::SAFE_POUCH) |
| Inventory_StowedToolActor | 0x420 | AGREE | ArcOffsets::InventoryComponent::STOWED_TOOL_ACTOR | drop-file(3) | confirmed by drop-fresh (ArcOffsets::InventoryComponent::STOWED_TOOL_ACTOR) |
| ItemContainer_ItemLimit | 0x4D8 | AGREE | prop:Angelscript.ItemContainerComponent.ItemLimit | dump(5) | dump: reflected property Angelscript.ItemContainerComponent.ItemLimit = 0x4D8 |
| ItemContainer_OpenTime | 0x4E0 | AGREE | prop:Angelscript.ItemContainerComponent.OpenTime | dump(5) | dump: reflected property Angelscript.ItemContainerComponent.OpenTime = 0x4E0 |
| ItemUIHoverData_Amount | 0x18 | AGREE | ArcOffsets::ItemUIHoverData::AMOUNT | probed(1) | confirmed by drop (ArcOffsets::ItemUIHoverData::AMOUNT) |
| ItemUIHoverData_DataAsset | 0x20 | AGREE | ArcOffsets::ItemUIHoverData::DATA_ASSET | probed(1) | confirmed by drop (ArcOffsets::ItemUIHoverData::DATA_ASSET) |
| ItemUIHoverData_DisplayName | 0x0 | AGREE | ArcOffsets::ItemUIHoverData::DISPLAY_NAME | probed(1) | confirmed by drop (ArcOffsets::ItemUIHoverData::DISPLAY_NAME) |
| ItemUIHoverData_MaxStack | 0x1C | AGREE | ArcOffsets::ItemUIHoverData::MAX_STACK | probed(1) | confirmed by drop (ArcOffsets::ItemUIHoverData::MAX_STACK) |
| LastRenderTime | 0x480 | AGREE | ArcOffsets::PrimitiveComponent::LAST_RENDER_TIME | live-pinned(6) | vis check user-provided 2026-09-23: PrimitiveComponent::LastRenderTime = 0x480 (plain float; drop's 0x4C0 reads zero now) |
| LastRenderTimeOnScreen | 0x484 | AGREE | ArcOffsets::PrimitiveComponent::LAST_RENDER_TIME_ON_SCREEN | live-pinned(6) | vis check user-provided 2026-09-23: LastRenderTimeOnScreen = 0x484 (plain float; was 0x4C4) |
| LevelActorContainer_ActorCount | 0xA0 | AGREE | ArcOffsets::LevelActorContainer::ACTOR_COUNT | derived(2) | confirmed by drop (ArcOffsets::LevelActorContainer::ACTOR_COUNT) |
| LevelActorContainer_Actors | 0x98 | AGREE | ArcOffsets::LevelActorContainer::ACTORS | dump(5) | dump: reflected property Engine.ActorContainer.Actors = 0x98 |
| LevelCollection_GameState | 0x8 | AGREE | ArcOffsets::LevelCollection::GAME_STATE | dump(5) | dump: reflected property Engine.LevelCollection.GameState = 0x8 |
| LevelCollection_PersistentLevel | 0x20 | AGREE | ArcOffsets::LevelCollection::PERSISTENT_LEVEL | dump(5) | dump: reflected property Engine.LevelCollection.PersistentLevel = 0x20 |
| LevelCollection_Stride | 0x78 | AGREE | ArcOffsets::MapWidgetLevelSettings::STRIDE | dump(5) | dump: sizeof(Engine.LevelCollection) = 0x78 |
| LevelCollections | 0x338 | AGREE | prop:Engine.World.LevelCollections | dump(5) | dump: reflected property Engine.World.LevelCollections = 0x338 |
| Level_OwningWorld | 0x138 | AGREE | ArcOffsets::Level::OWNING_WORLD | dump(5) | dump: reflected property Engine.Level.OwningWorld = 0x138 |
| Levels | 0x2F0 | AGREE | ArcOffsets::UWorld::LEVELS | dump(5) | dump: reflected property Engine.World.Levels = 0x2F0 |
| LocalCurrentItemActors | 0x540 | AGREE | prop:Angelscript.InventoryComponent.LocalCurrentItemActors | dump(5) | dump: reflected property Angelscript.InventoryComponent.LocalCurrentItemActors = 0x540 |
| LocalPlayer_ControllerId | 0x270 | AGREE | ArcOffsets::LocalPlayer::CONTROLLER_ID | dump(5) | dump: reflected property Engine.LocalPlayer.ControllerId = 0x270 |
| LocalPlayer_PlayerController | 0xA0 | AGREE | ArcOffsets::LocalPlayer::PLAYER_CONTROLLER | dump(5) | dump: reflected property Engine.Player.PlayerController = 0xA0 |
| LocalPlayers | 0x130 | AGREE | ArcOffsets::GameInstance::LOCAL_PLAYERS | drop-value(4) | drop carries this value as ArcOffsets::GameInstance::LOCAL_PLAYERS (renamed slot) |
| LockedFOV | 0x43C | AGREE | ArcOffsets::PlayerCameraManager::LOCKED_FOV | probed(1) | confirmed by drop-fresh (ArcOffsets::PlayerCameraManager::LOCKED_FOV) |
| LootContainer_SocketMesh | 0xBB0 | AGREE | ArcOffsets::LootContainerSingle::SOCKET_LOOT_CONTAINER_MESH | drop-value(4) | drop carries this value as ArcOffsets::LootContainerSingle::SOCKET_LOOT_CONTAINER_MESH (renamed slot) |
| LootInteract_AcquisitionMethod | 0x80B | AGREE | ArcOffsets::LootInteractionComponent::LOOT_ACQUISITION_METHOD | drop-value(4) | drop carries this value as ArcOffsets::LootInteractionComponent::LOOT_ACQUISITION_METHOD (renamed slot) |
| LootInteract_DispenserLocations | 0x810 | AGREE | ArcOffsets::LootInteractionComponent::DISPENSER_LOCATIONS | drop-file(3) | confirmed by drop-fresh (ArcOffsets::LootInteractionComponent::DISPENSER_LOCATIONS) |
| LootInteract_OpenedByte | 0x858 | AGREE | prop:Angelscript.LootInteractionComponent.bHasBeenOpened | dump(5) | dump: reflected property Angelscript.LootInteractionComponent.bHasBeenOpened = 0x858 |
| LootInteract_PingIconOffset | 0x860 | AGREE | ArcOffsets::LootInteractionComponent::LOOT_PING_ICON_OFFSET | drop-value(4) | drop carries this value as ArcOffsets::LootInteractionComponent::LOOT_PING_ICON_OFFSET (renamed slot) |
| LootInteractionComponent | 0xBB8 | AGREE | ArcOffsets::LootContainerSingle::LOOT_INTERACTION_COMPONENT | dump(5) | dump: reflected property Angelscript.LootContainerSingle.LootInteractionComponent = 0xBB8 |
| MapLevel_HasUnderground | 0x70 | AGREE | ArcOffsets::MapWidgetLevelSettings::HAS_UNDERGROUND | drop-file(3) | confirmed by drop (ArcOffsets::MapWidgetLevelSettings::HAS_UNDERGROUND) |
| MapLevel_Stride | 0x78 | AGREE | ArcOffsets::MapWidgetLevelSettings::STRIDE | drop-file(3) | confirmed by drop (ArcOffsets::MapWidgetLevelSettings::STRIDE) |
| MapLevel_WorldPosition | 0x60 | AGREE | ArcOffsets::MapWidgetLevelSettings::WORLD_POSITION | drop-file(3) | confirmed by drop (ArcOffsets::MapWidgetLevelSettings::WORLD_POSITION) |
| MapLevel_WorldSize | 0x50 | AGREE | ArcOffsets::MapWidgetLevelSettings::WORLD_SIZE | drop-file(3) | confirmed by drop (ArcOffsets::MapWidgetLevelSettings::WORLD_SIZE) |
| MaxDBNO | 0x380 | AGREE | prop:Angelscript.HealthComponent.MaxDBNOHealth | dump(5) | dump: reflected property Angelscript.HealthComponent.MaxDBNOHealth = 0x380 |
| MaxHealth | 0x348 | AGREE | lit:0x348 | probed(1) | ArcOffsets::HealthComponent::MAX_HEALTH = 0x348 (native, in the dump's Pad_0340 hole) |
| MiniMap_TileSize | 0x488 | AGREE | ArcOffsets::WorldPartitionMiniMap::MINIMAP_TILE_SIZE | drop-file(3) | confirmed by drop-fresh (ArcOffsets::WorldPartitionMiniMap::MINIMAP_TILE_SIZE) |
| MiniMap_WorldBounds | 0x3C0 | AGREE | ArcOffsets::WorldPartitionMiniMap::MINIMAP_WORLD_BOUNDS | drop-file(3) | confirmed by drop-fresh (ArcOffsets::WorldPartitionMiniMap::MINIMAP_WORLD_BOUNDS) |
| NetIdRepl_Object | 0x8 | AGREE | ArcOffsets::UniqueNetIdRepl::NET_ID_OBJECT | drop-value(3) | drop carries this value as ArcOffsets::UniqueNetIdRepl::NET_ID_OBJECT (renamed slot) |
| NetIdRepl_ReplBytesCount | 0x28 | AGREE | ArcOffsets::UniqueNetIdRepl::REPL_BYTES_COUNT | drop-file(3) | confirmed by drop (ArcOffsets::UniqueNetIdRepl::REPL_BYTES_COUNT) |
| NetIdRepl_ReplBytesData | 0x20 | AGREE | ArcOffsets::UniqueNetIdRepl::REPL_BYTES_DATA | drop-file(3) | confirmed by drop (ArcOffsets::UniqueNetIdRepl::REPL_BYTES_DATA) |
| NetIdRepl_TypeIndex | 0x18 | AGREE | ArcOffsets::UniqueNetIdRepl::TYPE_INDEX | drop-file(3) | confirmed by drop (ArcOffsets::UniqueNetIdRepl::TYPE_INDEX) |
| NetId_Payload | 0x18 | AGREE | ArcOffsets::UniqueNetId::PAYLOAD | drop-file(3) | confirmed by drop (ArcOffsets::UniqueNetId::PAYLOAD) |
| ObjectXorKeyRva | 0xD91F885 | AGREE | ArcOffsets::ObjectXorKey::RVA | probed(1) | confirmed by drop (ArcOffsets::ObjectXorKey::RVA) |
| OuterDecrypt_FinalRot | 0x27 | AGREE | PlayerDecrypt::FINAL_ROT | drop-value(3) | drop carries this value as PlayerDecrypt::FINAL_ROT, OuterDecrypt::FINAL_ROT (renamed slot) |
| OuterDecrypt_HashAdd | 0x20193F54 | AGREE | OuterDecrypt::HASH_ADD | drop-value(3) | drop carries this value as OuterDecrypt::HASH_ADD (renamed slot) |
| OuterDecrypt_HashPrime | 0x1000193 | AGREE | GameInstanceTlsDecrypt::HASH_PRIME | drop-value(3) | drop carries this value as GameInstanceTlsDecrypt::HASH_PRIME, OuterDecrypt::HASH_PRIME (renamed slot) |
| OuterDecrypt_HashRot1 | 0x19 | AGREE | OuterDecrypt::HASH_ROT_1 | drop-file(3) | confirmed by drop (OuterDecrypt::HASH_ROT_1) |
| OuterDecrypt_HashRot2 | 0xF | AGREE | OuterDecrypt::HASH_ROT_2 | drop-file(3) | confirmed by drop (OuterDecrypt::HASH_ROT_2) |
| OuterDecrypt_LaneRot | 0xD | AGREE | PlayerDecrypt::LANE_ROT | drop-value(3) | drop carries this value as PlayerDecrypt::LANE_ROT, OuterDecrypt::LANE_ROT (renamed slot) |
| OuterDecrypt_SlotBaseOff | 0x20 | AGREE | OuterDecrypt::SLOT_BASE_OFF | drop-file(3) | confirmed by drop (OuterDecrypt::SLOT_BASE_OFF) |
| OuterDecrypt_SlotStride | 0x20 | AGREE | OuterDecrypt::SLOT_STRIDE | drop-file(3) | confirmed by drop (OuterDecrypt::SLOT_STRIDE) |
| OwningGameInstance | 0x478 | AGREE | ArcOffsets::UWorld::GAME_INSTANCE | drop-file(3) | confirmed by drop (ArcOffsets::UWorld::GAME_INSTANCE) |
| PCM_ViewPitchMax | 0x2090 | AGREE | ArcOffsets::PlayerCameraManager::VIEW_PITCH_MAX | drop-value(4) | drop carries this value as ArcOffsets::PlayerCameraManager::VIEW_PITCH_MAX (renamed slot) |
| PCM_ViewPitchMin | 0x2098 | AGREE | ArcOffsets::PlayerCameraManager::VIEW_PITCH_MIN | drop-value(4) | drop carries this value as ArcOffsets::PlayerCameraManager::VIEW_PITCH_MIN (renamed slot) |
| PCM_ViewYawMax | 0x20A0 | AGREE | ArcOffsets::PlayerCameraManager::VIEW_YAW_MAX | drop-value(4) | drop carries this value as ArcOffsets::PlayerCameraManager::VIEW_YAW_MAX (renamed slot) |
| PCM_ViewYawMin | 0x2094 | AGREE | ArcOffsets::PlayerCameraManager::VIEW_YAW_MIN | drop-value(4) | drop carries this value as ArcOffsets::PlayerCameraManager::VIEW_YAW_MIN (renamed slot) |
| PCOwner | 0x420 | AGREE | prop:Engine.PlayerCameraManager.PCOwner | dump(5) | dump: reflected property Engine.PlayerCameraManager.PCOwner = 0x420 |
| PHI_DBNOByte | 0x21 | AGREE | ArcOffsets::PlayerHealthInfo::B_IS_DBNO | drop-value(3) | drop carries this value as ArcOffsets::PlayerHealthInfo::B_IS_DBNO (renamed slot) |
| PHI_Health | 0x0 | AGREE | ArcOffsets::PlayerHealthInfo::HEALTH | drop-file(3) | confirmed by drop (ArcOffsets::PlayerHealthInfo::HEALTH) |
| PHI_MaxHealth | 0x8 | AGREE | ArcOffsets::PlayerHealthInfo::MAX_HEALTH | drop-value(3) | drop carries this value as ArcOffsets::PlayerHealthInfo::MAX_HEALTH (renamed slot) |
| PS_AchievementComponent | 0x5B0 | AGREE | prop:Angelscript.PioneerPlayerState.AchievementComponent | dump(5) | dump: reflected property Angelscript.PioneerPlayerState.AchievementComponent = 0x5B0 |
| PS_BotStateBotMask | 0x8 | AGREE | ArcOffsets::PlayerState::B_IS_A_BOT_MASK | drop-value(3) | drop carries this value as ArcOffsets::PlayerState::B_IS_A_BOT_MASK (renamed slot) |
| PS_BotStateByte | 0x3CA | AGREE | prop:Engine.PlayerState.bIsABot | dump(5) | dump: reflected property Engine.PlayerState.bIsABot = 0x3CA |
| PS_BotStateSpectatorMask | 0x2 | AGREE | ArcOffsets::PlayerState::B_IS_SPECTATOR_MASK | drop-value(3) | drop carries this value as ArcOffsets::PlayerState::B_IS_SPECTATOR_MASK (renamed slot) |
| PS_FinishedRoundByte | 0x5A8 | AGREE | prop:Angelscript.PioneerPlayerState.bFinishedRound | dump(5) | dump: reflected property Angelscript.PioneerPlayerState.bFinishedRound = 0x5A8 |
| PS_FinishedRoundMask | 0x1 | AGREE | maskOf:Angelscript.PioneerPlayerState.bFinishedRound | dump(5) | dump: Angelscript.PioneerPlayerState.bFinishedRound mask = 0x1 |
| PS_PlatformIdComponent | 0x4B0 | AGREE | prop:EmbarkGameplay.EmbarkPlayerStateBase.PlatformIdComponent | dump(5) | dump: reflected property EmbarkGameplay.EmbarkPlayerStateBase.PlatformIdComponent = 0x4B0 |
| PS_PlayerNamePrivate2 | 0x480 | AGREE | ArcOffsets::PlayerState::PLAYER_NAME_PRIVATE_2 | drop-file(3) | confirmed by drop (ArcOffsets::PlayerState::PLAYER_NAME_PRIVATE_2) |
| PS_Squad | 0x488 | AGREE | prop:EmbarkGameplay.EmbarkPlayerStateBase.Squad | dump(5) | dump: reflected property EmbarkGameplay.EmbarkPlayerStateBase.Squad = 0x488 |
| PartHpArray | 0x280 | AGREE | ArcOffsets::HealthService::PART_HP_ARRAY | drop-value(3) | drop carries this value as ArcOffsets::HealthService::PART_HP_ARRAY (renamed slot) |
| Pawn_Controller | 0x3F0 | AGREE | ArcOffsets::Pawn::CONTROLLER | drop-file(3) | confirmed by drop-fresh (ArcOffsets::Pawn::CONTROLLER) |
| PendingViewTarget | 0xE10 | AGREE | prop:Engine.PlayerCameraManager.PendingViewTarget | dump(5) | dump: reflected property Engine.PlayerCameraManager.PendingViewTarget = 0xE10 |
| PersistentLevel | 0x110 | AGREE | ArcOffsets::UWorld::PERSISTENT_LEVEL | dump(5) | dump: reflected property Engine.World.PersistentLevel = 0x110 |
| PhysicsField | 0x508 | AGREE | prop:Engine.World.PhysicsField | dump(5) | dump: reflected property Engine.World.PhysicsField = 0x508 |
| PickupDataAsset_ResolvedItemClass | 0x150 | AGREE | ArcOffsets::PickupDataAsset::RESOLVED_ITEM_CLASS | drop-file(3) | confirmed by drop (ArcOffsets::PickupDataAsset::RESOLVED_ITEM_CLASS) |
| Pickup_DefaultDataAsset | 0x4A8 | AGREE | ArcOffsets::Pickup::DEFAULT_PICKUP_DATA_ASSET | drop-value(4) | drop carries this value as ArcOffsets::Pickup::DEFAULT_PICKUP_DATA_ASSET (renamed slot) |
| Pickup_DefaultPickupDataAsset | 0x4A8 | AGREE | ArcOffsets::Pickup::DEFAULT_PICKUP_DATA_ASSET | dump(5) | dump: reflected property Angelscript.Pickup.DefaultPickupDataAsset = 0x4A8 |
| Pickup_Interaction | 0x498 | AGREE | ArcOffsets::Pickup::INTERACTION | dump(5) | dump: reflected property Angelscript.Pickup.Interaction = 0x498 |
| Pickup_RootCollider | 0x480 | AGREE | ArcOffsets::Pickup::ROOT_COLLIDER | dump(5) | dump: reflected property Angelscript.Pickup.RootCollider = 0x480 |
| Pickup_VisibleAmount | 0x4D0 | AGREE | ArcOffsets::Pickup::VISIBLE_AMOUNT | probed(1) | confirmed by drop-unconfirmed (ArcOffsets::Pickup::VISIBLE_AMOUNT) |
| PioneerCharacterMovement | 0xB18 | AGREE | prop:PioneerGameplay.PioneerCharacterBase.PioneerCharacterMovement | dump(5) | dump: reflected property PioneerGameplay.PioneerCharacterBase.PioneerCharacterMovement = 0xB18 |
| PioneerPlayerState_CurrentPawn | 0x560 | AGREE | ArcOffsets::PlayerState::CURRENT_PAWN | dump(5) | dump: reflected property Angelscript.PioneerPlayerState.CurrentPawn = 0x560 |
| PioneerPlayerState_PioneerCharacter | 0x558 | AGREE | ArcOffsets::PlayerState::PIONEER_CHARACTER | dump(5) | dump: reflected property Angelscript.PioneerPlayerState.PioneerCharacter = 0x558 |
| PlatformId_Repl | 0x1B8 | AGREE | prop:EmbarkGameplay.EmbarkPlatformIdComponent.PlatformId | dump(5) | dump: reflected property EmbarkGameplay.EmbarkPlatformIdComponent.PlatformId = 0x1B8 |
| PlayerController_bIsLocalPlayerController | 0xD64 | AGREE | prop:Engine.PlayerController.bIsLocalPlayerController | dump(5) | dump: reflected property Engine.PlayerController.bIsLocalPlayerController = 0xD64 |
| PlayerDecrypt_BlendXorMask | 0x9A492C85DDF6F193 | AGREE | PlayerDecrypt::BLEND_XOR_MASK | drop-file(3) | confirmed by drop (PlayerDecrypt::BLEND_XOR_MASK) |
| PlayerDecrypt_FinalRot | 0x27 | AGREE | PlayerDecrypt::FINAL_ROT | drop-value(3) | drop carries this value as PlayerDecrypt::FINAL_ROT, OuterDecrypt::FINAL_ROT (renamed slot) |
| PlayerDecrypt_LaneRot | 0xD | AGREE | PlayerDecrypt::LANE_ROT | drop-value(3) | drop carries this value as PlayerDecrypt::LANE_ROT, OuterDecrypt::LANE_ROT (renamed slot) |
| PlayerNamePrivate | 0x458 | AGREE | ArcOffsets::PlayerState::PLAYER_NAME_PRIVATE | dump(5) | dump: reflected property Engine.PlayerState.PlayerNamePrivate = 0x458 |
| PlayerNameSimdMaskRva | 0xAD2FC50 | AGREE | ArcOffsets::GetObjectIdCrypto::SIMD_MASK_RVA | drop-value(3) | drop carries this value as ArcOffsets::GetObjectIdCrypto::SIMD_MASK_RVA (renamed slot) |
| PlayerState_PawnPrivate | 0x438 | AGREE | ArcOffsets::PlayerState::PAWN_PRIVATE | dump(5) | dump: reflected property Engine.PlayerState.PawnPrivate = 0x438 |
| PlayerState_PlayerStatus | 0x540 | AGREE | ArcOffsets::PlayerState::PLAYER_STATUS | dump(5) | dump: reflected property Angelscript.PioneerPlayerState.PlayerStatus = 0x540 |
| RelativeLocation | 0x268 | AGREE | prop:Engine.SceneComponent.RelativeLocation | dump(5) | dump: reflected property Engine.SceneComponent.RelativeLocation = 0x268 |
| RelativeLocation_Alt | 0x2A8 | AGREE | ArcOffsets::SceneComponent::RELATIVE_LOCATION | drop-value(3) | drop carries this value as ArcOffsets::SceneComponent::RELATIVE_LOCATION (renamed slot) |
| RelativeRotation | 0x280 | AGREE | prop:Engine.SceneComponent.RelativeRotation | dump(5) | dump: reflected property Engine.SceneComponent.RelativeRotation = 0x280 |
| RepMov_LinearVelocity | 0x0 | AGREE | prop:Engine.RepMovement.LinearVelocity | dump(5) | dump: reflected property Engine.RepMovement.LinearVelocity = 0x0 |
| RepMovement_AngularVelocity | 0x18 | AGREE | prop:Engine.RepMovement.AngularVelocity | dump(5) | dump: reflected property Engine.RepMovement.AngularVelocity = 0x18 |
| RepMovement_Location | 0x30 | AGREE | prop:Engine.RepMovement.Location | dump(5) | dump: reflected property Engine.RepMovement.Location = 0x30 |
| RepMovement_Rotation | 0x48 | AGREE | prop:Engine.RepMovement.Rotation | dump(5) | dump: reflected property Engine.RepMovement.Rotation = 0x48 |
| RepMovement_ServerFrame | 0x7C | AGREE | prop:Engine.RepMovement.ServerFrame | dump(5) | dump: reflected property Engine.RepMovement.ServerFrame = 0x7C |
| RepMovement_bRepPhysicsByte | 0x78 | AGREE | prop:Engine.RepMovement.bRepPhysics | dump(5) | dump: reflected property Engine.RepMovement.bRepPhysics = 0x78 |
| ReplicatedMovement | 0x150 | AGREE | prop:Engine.Actor.ReplicatedMovement | dump(5) | dump: reflected property Engine.Actor.ReplicatedMovement = 0x150 |
| RootComponent | 0x240 | AGREE | prop:Engine.Actor.RootComponent | dump(5) | dump: reflected property Engine.Actor.RootComponent = 0x240 |
| SalvageContainer_ChosenMesh | 0xCF0 | AGREE | prop:Angelscript.SalvageContainerSingle.ChosenMesh | dump(5) | dump: reflected property Angelscript.SalvageContainerSingle.ChosenMesh = 0xCF0 |
| SalvageContainer_MeshVariants | 0xCB8 | AGREE | prop:Angelscript.SalvageContainerSingle.MeshVariants | dump(5) | dump: reflected property Angelscript.SalvageContainerSingle.MeshVariants = 0xCB8 |
| Scene_bHiddenInGameByte | 0x2C9 | AGREE | ArcOffsets::SceneComponent::B_HIDDEN_IN_GAME_BYTE | dump(5) | dump: reflected property Engine.SceneComponent.bHiddenInGame = 0x2C9 |
| Scene_bHiddenInGameMask | 0x10 | AGREE | ArcOffsets::SceneComponent::B_HIDDEN_IN_GAME_MASK | dump(5) | dump: Engine.SceneComponent.bHiddenInGame mask = 0x10 |
| Scene_bVisibleByte | 0x2C8 | AGREE | ArcOffsets::SceneComponent::B_VISIBLE_BYTE | dump(5) | dump: reflected property Engine.SceneComponent.bVisible = 0x2C8 |
| Scene_bVisibleMask | 0x20 | AGREE | ArcOffsets::SceneComponent::B_VISIBLE_MASK | dump(5) | dump: Engine.SceneComponent.bVisible mask = 0x20 |
| Shield | 0x1C0 | AGREE | prop:Angelscript.HealthComponent.Armor | dump(5) | dump: reflected property Angelscript.HealthComponent.Armor = 0x1C0 |
| SkeletalMeshAsset | 0x720 | AGREE | prop:Engine.SkinnedMeshComponent.SkeletalMesh | dump(5) | dump: reflected property Engine.SkinnedMeshComponent.SkeletalMesh = 0x720 |
| SkeletalMeshAsset_Alt | 0x740 | AGREE | ArcOffsets::SkeletalMeshComponent::SKELETAL_MESH | drop-value(3) | drop carries this value as ArcOffsets::SkeletalMeshComponent::SKELETAL_MESH (renamed slot) |
| StageInfo_GraceTimeOff | 0x20 | AGREE | ArcOffsets::StageInfo::GRACE_TIME | drop-value(3) | drop carries this value as ArcOffsets::StageInfo::GRACE_TIME (renamed slot) |
| StageInfo_TimeLeftOff | 0x18 | AGREE | ArcOffsets::StageInfo::TIME_LEFT | drop-value(3) | drop carries this value as ArcOffsets::StageInfo::TIME_LEFT (renamed slot) |
| StateInterpolator | 0x7A0 | AGREE | prop:EmbarkCharacter.EmbarkCharacterBase.StateInterpolatorComponent | dump(5) | dump: reflected property EmbarkCharacter.EmbarkCharacterBase.StateInterpolatorComponent = 0x7A0 |
| StaticMesh | 0x6D8 | AGREE | ArcOffsets::ConstructableStaticMeshStyle::STATIC_MESH | dump(5) | dump: reflected property Engine.StaticMeshComponent.StaticMesh = 0x6D8 |
| StaticMesh_ExtendedBounds | 0x368 | AGREE | prop:Engine.StaticMesh.ExtendedBounds | dump(5) | dump: reflected property Engine.StaticMesh.ExtendedBounds = 0x368 |
| StowedInfo_ItemDataAsset | 0x18 | AGREE | ArcOffsets::StowedWeaponLayout::ITEM_DATA_ASSET | drop-file(3) | confirmed by drop (ArcOffsets::StowedWeaponLayout::ITEM_DATA_ASSET) |
| StowedWeaponSlot0 | 0x3A0 | AGREE | prop:Angelscript.InventoryComponent.StowedWeapon0 | dump(5) | dump: reflected property Angelscript.InventoryComponent.StowedWeapon0 = 0x3A0 |
| StowedWeaponSlot1 | 0x3E0 | AGREE | prop:Angelscript.InventoryComponent.StowedWeapon1 | dump(5) | dump: reflected property Angelscript.InventoryComponent.StowedWeapon1 = 0x3E0 |
| StreamingLevels | 0x170 | AGREE | prop:Engine.World.StreamingLevels | dump(5) | dump: reflected property Engine.World.StreamingLevels = 0x170 |
| Style_DestroyedReasonByte | 0x7FA | AGREE | ArcOffsets::ConstructableStaticMeshStyle::DESTROYED_REASON | drop-value(3) | drop carries this value as ArcOffsets::ConstructableStaticMeshStyle::DESTROYED_REASON (renamed slot) |
| Style_IsDestroyedByte | 0x7F9 | AGREE | ArcOffsets::ConstructableStaticMeshStyle::IS_DESTROYED | drop-value(3) | drop carries this value as ArcOffsets::ConstructableStaticMeshStyle::IS_DESTROYED (renamed slot) |
| Style_PartIdRange | 0x7E0 | AGREE | ArcOffsets::ConstructableStaticMeshStyle::PART_ID_RANGE | drop-file(3) | confirmed by drop (ArcOffsets::ConstructableStaticMeshStyle::PART_ID_RANGE) |
| TeamID | 0x7F2 | AGREE | prop:EmbarkCharacter.EmbarkCharacterBase.TeamId | dump(5) | dump: reflected property EmbarkCharacter.EmbarkCharacterBase.TeamId = 0x7F2 |
| TebKeyOff | 0x1F8 | AGREE | TebDecrypt::TEB_KEY_OFF | drop-file(3) | confirmed by drop (TebDecrypt::TEB_KEY_OFF) |
| TebQwordRor | 0x3 | AGREE | TebDecrypt::QWORD_ROR | drop-value(3) | drop carries this value as TebDecrypt::QWORD_ROR (renamed slot) |
| TebSelfPtrOff | 0x30 | AGREE | ArcOffsets::WineTeb::SELF_PTR | probed(1) | confirmed by drop (ArcOffsets::WineTeb::SELF_PTR) |
| TebWordRor | 0x1 | AGREE | TebDecrypt::WORD_ROR | drop-value(3) | drop carries this value as TebDecrypt::WORD_ROR (renamed slot) |
| Transform_Rotation | 0x0 | AGREE | prop:CoreUObject.Transform.Rotation | dump(5) | dump: reflected property CoreUObject.Transform.Rotation = 0x0 |
| Transform_Scale3D | 0x40 | AGREE | prop:CoreUObject.Transform.Scale3D | dump(5) | dump: reflected property CoreUObject.Transform.Scale3D = 0x40 |
| Transform_Size | 0x60 | AGREE | sizeof:CoreUObject.Transform | dump(5) | dump: sizeof(CoreUObject.Transform) = 0x60 |
| Transform_Translation | 0x20 | AGREE | prop:CoreUObject.Transform.Translation | dump(5) | dump: reflected property CoreUObject.Transform.Translation = 0x20 |
| UActorComponent_WorldPrivate | 0x140 | AGREE | ArcOffsets::PrimitiveComponent::WORLD_PRIVATE | live-pinned(6) | confirmed by drop (ArcOffsets::PrimitiveComponent::WORLD_PRIVATE) |
| UClass_DefaultObjectSlot | 0x158 | AGREE | prop:CoreUObject.Class.ClassDefaultObject | dump(5) | dump: reflected property CoreUObject.Class.ClassDefaultObject = 0x158 |
| UObject_InternalIndex | 0x90 | AGREE | FName::UOBJECT_INTERNAL_IDX | drop-file(3) | confirmed by drop (FName::UOBJECT_INTERNAL_IDX) |
| UObject_Size | 0xA0 | AGREE | sizeof:CoreUObject.Object | dump(5) | dump: sizeof(CoreUObject.Object) = 0xA0 |
| USkeletalMeshComponent | 0x440 | AGREE | prop:Engine.Character.Mesh | dump(5) | dump: reflected property Engine.Character.Mesh = 0x440 |
| USkeletalMeshComponent_Alt | 0x4A0 | AGREE | ArcOffsets::Actor::MESH | drop-value(4) | drop carries this value as ArcOffsets::Actor::MESH, ArcOffsets::Pawn::MESH (renamed slot) |
| UStruct_PropertyLink | 0xB8 | AGREE | prop:CoreUObject.Struct.Children | dump(5) | dump: reflected property CoreUObject.Struct.Children = 0xB8 |
| UStruct_SuperStruct | 0xB0 | AGREE | ArcOffsets::UStruct::SUPER_STRUCT | dump(5) | dump: reflected property CoreUObject.Struct.SuperStruct = 0xB0 |
| UWorld_TimeSeconds | 0x240 | AGREE | ArcOffsets::UWorld::TIME_SECONDS | drop-file(3) | confirmed by drop (ArcOffsets::UWorld::TIME_SECONDS) |
| Velocity | 0x1D0 | AGREE | prop:Engine.MovementComponent.Velocity | dump(5) | dump: reflected property Engine.MovementComponent.Velocity = 0x1D0 |
| ViewTarget | 0x460 | AGREE | prop:Engine.PlayerCameraManager.ViewTarget | dump(5) | dump: reflected property Engine.PlayerCameraManager.ViewTarget = 0x460 |
| VisibilityBasedAnimTickOption | 0x9EC | AGREE | prop:Engine.SkinnedMeshComponent.VisibilityBasedAnimTickOption | dump(5) | dump: reflected property Engine.SkinnedMeshComponent.VisibilityBasedAnimTickOption = 0x9EC |
| WeaponActor_ClipSize | 0x490 | AGREE | ArcOffsets::WeaponActor::CLIP_SIZE | drop-file(3) | confirmed by drop-fresh (ArcOffsets::WeaponActor::CLIP_SIZE) |
| WeaponActor_Quality | 0x492 | AGREE | ArcOffsets::WeaponActor::WEAPON_QUALITY | drop-value(4) | drop carries this value as ArcOffsets::WeaponActor::WEAPON_QUALITY (renamed slot) |
| WeaponClip | 0x490 | AGREE | prop:Angelscript.BeamFirearmActor.ClipSize | dump(5) | dump: reflected property Angelscript.BeamFirearmActor.ClipSize = 0x490 |
| WeaponQuality | 0x492 | AGREE | ArcOffsets::WeaponActor::WEAPON_QUALITY | dump(5) | dump: reflected property Angelscript.BeamFirearmActor.WeaponQuality = 0x492 |
| XmmXorVal | 0xA738DD8241D227C2 | AGREE | ArcOffsets::Crypto::XMM_XOR_VAL | probed(1) | confirmed by drop (ArcOffsets::Crypto::XMM_XOR_VAL) |
| bForceRefpose | 0xCC1 | AGREE | prop:Engine.SkeletalMeshComponent.bForceRefpose | dump(5) | dump: reflected property Engine.SkeletalMeshComponent.bForceRefpose = 0xCC1 |
| bForceRefposeMask | 0x1 | AGREE | maskOf:Engine.SkeletalMeshComponent.bForceRefpose | dump(5) | dump: Engine.SkeletalMeshComponent.bForceRefpose mask = 0x1 |
| bIsBreaked | 0x1230 | AGREE | prop:Angelscript.PioneerConstructablePawn.bIsDestroyed | dump(5) | dump: reflected property Angelscript.PioneerConstructablePawn.bIsDestroyed = 0x1230 |
| bNoSkeletonUpdate | 0xCC0 | AGREE | prop:Engine.SkeletalMeshComponent.bNoSkeletonUpdate | dump(5) | dump: reflected property Engine.SkeletalMeshComponent.bNoSkeletonUpdate = 0xCC0 |
| bNoSkeletonUpdateMask | 0x8 | AGREE | maskOf:Engine.SkeletalMeshComponent.bNoSkeletonUpdate | dump(5) | dump: Engine.SkeletalMeshComponent.bNoSkeletonUpdate mask = 0x8 |
| bRecentlyRendered | 0x9F0 | AGREE | prop:Engine.SkinnedMeshComponent.bRecentlyRendered | dump(5) | dump: reflected property Engine.SkinnedMeshComponent.bRecentlyRendered = 0x9F0 |
| bRecentlyRenderedMask | 0x40 | AGREE | maskOf:Engine.SkinnedMeshComponent.bRecentlyRendered | dump(5) | dump: Engine.SkinnedMeshComponent.bRecentlyRendered mask = 0x40 |
