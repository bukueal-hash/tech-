#pragma once
#include <cstddef>

namespace Offsets {
	constexpr std::ptrdiff_t UWorld = 0xDD77B78;
	constexpr std::ptrdiff_t GameInstance = 0x300;
	constexpr std::ptrdiff_t PersistentLevel = 0x130;
	constexpr std::ptrdiff_t AActors = 0x100;
	constexpr std::ptrdiff_t LocalPlayers = 0xF0;
	constexpr std::ptrdiff_t APlayerController = 0x98;
	constexpr std::ptrdiff_t APlayerCameraManager = 0xC30;
	constexpr std::ptrdiff_t CameraCachePrivate = 0x4A0;
	constexpr std::ptrdiff_t ViewTargetTarget = 0xC40;
	constexpr std::ptrdiff_t AcknowledgedPawn = 0x3E0;
	constexpr std::ptrdiff_t PlayerNamePrivate = 0x438;
	constexpr std::ptrdiff_t APlayerState = 0x3C0;
	constexpr std::ptrdiff_t RootComponent = 0x220;
	constexpr std::ptrdiff_t RelativeLocation = 0x290;
	constexpr std::ptrdiff_t USkeletalMeshComponent = 0x428;
	constexpr std::ptrdiff_t BoundsScale = 0x4E8;
	constexpr std::ptrdiff_t LastSubmitTime = 0x4EC;
	constexpr std::ptrdiff_t LastRenderTimeOnScreen = 0x4F0;
	constexpr std::ptrdiff_t ComponentToWorld = 0x3A0;
	constexpr std::ptrdiff_t ActorID = 0x18;
	constexpr std::ptrdiff_t Super = 0xB0;
	constexpr std::ptrdiff_t HealthInfo = 0x530;
	constexpr std::ptrdiff_t HealthComponent = 0xD20;
	constexpr std::ptrdiff_t Health = 0x6B8;
	constexpr std::ptrdiff_t MaxHealth = 0x308;
	constexpr std::ptrdiff_t Shield = 0x1A0;
	constexpr std::ptrdiff_t TeamID = 0x812;
	constexpr std::ptrdiff_t InventoryComponent = 0xC58;
	constexpr std::ptrdiff_t LocalCurrentItemActors = 0x540;
	constexpr std::ptrdiff_t WeaponQuality = 0x540;

	constexpr std::ptrdiff_t ComponentVelocity = 0x248;
	constexpr std::ptrdiff_t bIsBreaked = 0x1220;
	constexpr std::ptrdiff_t ControlRotation = 0x1220;

}