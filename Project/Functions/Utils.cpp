#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include <cmath>
#include <immintrin.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <stdint.h>
#include <stdlib.h>

#include <Windows.h>
#include <vector>


#include "../Core/AssetNames.h"
#include "../Functions/RobotList.h"
#include "WorldScanCommon.h"
#include "../Core/Cache.hpp"
#include "../Core/Memory.h"
#include "../Core/SteamDecrypt.hpp"
#include "../Core/WorldItemCategory.h"

#include "../Interface/OverlayHost.h"

#include <atomic>

namespace {

std::atomic<float> g_projViewportW{ 1920.f };
std::atomic<float> g_projViewportH{ 1080.f };

bool IsPlausibleWorldPosD(const Engine::FVector3d& p)
{
	return ::IsPlausibleWorldPos(Vector3{
		static_cast<float>(p.x),
		static_cast<float>(p.y),
		static_cast<float>(p.z)
	});
}



/** PCM validation — DefaultFOV alone is the strongest signal.
 *  POV/Location checks are too sensitive to offset errors; a real PCM
 *  always has DefaultFOV in (1,179). */
bool PcmLivePovLooksSane(uintptr_t pcm)
{
	if (!pcm || !Memory::IsValidPtrFast2(pcm))
		return false;

	const float defaultFov =
		Memory::read_nocache<float>(pcm + Offsets::DefaultFOV);
	return (defaultFov > 1.f && defaultFov < 179.f);
}

bool IsPlausibleRotation(const Engine::FVector3d& rot)
{
	if (!std::isfinite(rot.x) || !std::isfinite(rot.y) || !std::isfinite(rot.z))
		return false;
	const double mag =
		rot.x * rot.x + rot.y * rot.y + rot.z * rot.z;
	if (mag < 0.25 || mag > 360.0 * 360.0)
		return false;
	return std::fabs(rot.x) <= 89.5 && std::fabs(rot.y) <= 360.0 && std::fabs(rot.z) <= 180.0;
}

bool IsPlausiblePitch(double pitchDeg)
{
	return std::isfinite(pitchDeg) && std::fabs(pitchDeg) <= 89.5;
}

bool IsPlausibleYaw(double yawDeg)
{
	return std::isfinite(yawDeg) && std::fabs(yawDeg) <= 360.0;
}

Engine::FVector3d MergeCameraRotation(
	const Engine::FVector3d& povRot,
	const Vector3& ctrlRot,
	bool hasPc)
{
	// Reject all-zero POV rotation — it means the offset read garbage,
	// not that the camera is looking straight ahead.
	const double povMag = povRot.x * povRot.x + povRot.y * povRot.y + povRot.z * povRot.z;
	const bool povUseful = povMag >= 0.01;
	Engine::FVector3d merged{};
	merged.x = (povUseful && IsPlausiblePitch(povRot.x))
		? povRot.x
		: (hasPc ? static_cast<double>(ctrlRot.x) : 0.0);
	merged.y = (povUseful && IsPlausibleYaw(povRot.y))
		? povRot.y
		: (hasPc ? static_cast<double>(ctrlRot.y) : 0.0);
	merged.z = povUseful && std::isfinite(povRot.z) && std::fabs(povRot.z) <= 180.0
		? povRot.z
		: (hasPc ? static_cast<double>(ctrlRot.z) : 0.0);
	return merged;
}

} // namespace

void Engine::DbgStoreCameraProbe(const CameraProbeSnapshot& probe)
{
    (void)probe;
}

bool Engine::BuildCameraCacheFromPovReads(
    bool pcmOk,
    float povFov,
    float defFov,
    float jsonFov,
    const FVector3d& povLoc,
    const FVector3d& povRot,
    const Vector3& pawnPos,
    bool pawnOk,
    const Vector3& ctrlRot,
    bool hasPc,
    CameraCache& outCamera,
    CameraProbeSnapshot* outProbe) const
{
    const bool povLocPlausible = pcmOk && IsPlausibleWorldPosD(povLoc);
    const bool povRotOk = IsPlausibleRotation(povRot);

    float fov = 90.0f;
    if (povFov > 1.0f && povFov < 179.0f)
        fov = povFov;
    else if (defFov > 1.0f && defFov < 179.0f)
        fov = defFov;
    else if (jsonFov > 1.0f && jsonFov < 179.0f)
        fov = jsonFov;

    FVector3d loc{};
    FVector3d rot{};
    const char* src = "none";

    // Third-person look (right stick pitch) moves the real camera away from the
    // pawn. Never fall back to pawn-eye location when POV world location is valid.
    if (povLocPlausible) {
        loc = povLoc;
        src = "pov_loc";
    } else if (pawnOk) {
        loc = FVector3d{ pawnPos.x, pawnPos.y, pawnPos.z + 160.0 };
        src = "pawn_eye";
    }

    if (povRotOk) {
        rot = povRot;
        if (std::strcmp(src, "pov_loc") == 0)
            src = "pov_full";
        else if (std::strcmp(src, "pawn_eye") == 0)
            src = "pawn_eye_pov_rot";
    } else {
        rot = MergeCameraRotation(povRot, ctrlRot, hasPc);
        const bool mergedOk = IsPlausibleRotation(rot)
            || (IsPlausiblePitch(rot.x) && IsPlausibleYaw(rot.y));
        if (mergedOk) {
            if (std::strcmp(src, "pov_loc") == 0)
                src = "pov_loc_merged_rot";
            else if (std::strcmp(src, "pawn_eye") == 0)
                src = "pawn_eye_merged_rot";
            else
                src = "merged_rot";
        } else if (hasPc) {
            const double ctrlMag =
                static_cast<double>(ctrlRot.x) * ctrlRot.x +
                static_cast<double>(ctrlRot.y) * ctrlRot.y +
                static_cast<double>(ctrlRot.z) * ctrlRot.z;
            if (ctrlMag >= 1.0) {
                rot = FVector3d{ ctrlRot.x, ctrlRot.y, ctrlRot.z };
                if (std::strcmp(src, "pawn_eye") == 0)
                    src = "pawn_eye_pc_rot";
                else if (std::strcmp(src, "pov_loc") == 0)
                    src = "pov_loc_pc_rot";
                else if (std::strcmp(src, "none") == 0)
                    src = "pc_rot_only";
            }
        }
    }

    const bool ok =
        fov > 1.0f && fov < 179.0f && ::IsPlausibleWorldPos(ToVector3(loc));

    if (outProbe) {
        snprintf(outProbe->src, sizeof(outProbe->src), "%s", src);
        outProbe->ok = ok;
        outProbe->povLocOk = povLocPlausible;
        outProbe->povRotOk = povRotOk;
        outProbe->pawnOk = pawnOk;
        outProbe->povRotX = static_cast<float>(povRot.x);
        outProbe->povRotY = static_cast<float>(povRot.y);
        outProbe->povRotZ = static_cast<float>(povRot.z);
        outProbe->ctrlRotX = static_cast<float>(ctrlRot.x);
        outProbe->ctrlRotY = static_cast<float>(ctrlRot.y);
        outProbe->ctrlRotZ = static_cast<float>(ctrlRot.z);
        outProbe->pubRotX = static_cast<float>(rot.x);
        outProbe->pubRotY = static_cast<float>(rot.y);
        outProbe->pubRotZ = static_cast<float>(rot.z);
        outProbe->pubLocX = static_cast<float>(loc.x);
        outProbe->pubLocY = static_cast<float>(loc.y);
        outProbe->pubLocZ = static_cast<float>(loc.z);
        outProbe->povLocX = static_cast<float>(povLoc.x);
        outProbe->povLocY = static_cast<float>(povLoc.y);
        outProbe->povLocZ = static_cast<float>(povLoc.z);
        outProbe->pawnX = static_cast<float>(pawnPos.x);
        outProbe->pawnY = static_cast<float>(pawnPos.y);
        outProbe->pawnZ = static_cast<float>(pawnPos.z);
        outProbe->fov = fov;
    }

    if (!ok)
        return false;

    outCamera.Location = ToVector3(loc);
    outCamera.Rotation = ToVector3(rot);
    outCamera.FOV = fov;
    return true;
}

bool Engine::ResolvePlayerCameraManagerLadder(
	uintptr_t& pc,
	uintptr_t& pawn,
	uintptr_t& pcm,
	uintptr_t level,
	uintptr_t actors,
	bool nocacheFov)
{
	auto IsPcmValid = [&](uintptr_t p) {
		if (!p || !IsValidPointer(p))
			return false;
		const float f = nocacheFov
			? Memory::read_nocache<float>(p + Offsets::DefaultFOV)
			: Memory::read<float>(p + Offsets::DefaultFOV);
		if (!(f > 1.0f && f < 179.0f))
			return false;
		if (!PcmLivePovLooksSane(p))
			return false;
		const uintptr_t owner = Memory::read_nocache<uintptr_t>(p + Offsets::PCOwner);
		return !pc || !owner || owner == pc;
	};

	// LP may be 0 — still recover PCM via FName / PCOwner scan.
	if (pc && !IsPcmValid(pcm))
		pcm = Memory::read_nocache<uintptr_t>(pc + Offsets::APlayerCameraManager);
	if (!IsPcmValid(pcm))
		pcm = GetCameraManagerFromActors();
	if (!IsPcmValid(pcm) && level && actors) {
		uintptr_t pcmPc = pc;
		uintptr_t pcmPawn = pawn;
		uintptr_t foundPcm = pcm;
		if (ResolvePcFromLevelCameraManager(level, actors, pcmPc, pcmPawn, foundPcm)) {
			pcm = foundPcm;
			pc = pcmPc;
			pawn = pcmPawn;
		}
	}
	return IsPcmValid(pcm);
}

bool Engine::RefreshCameraFromViewTarget()
{
	uintptr_t pc = 0;
	uintptr_t pawn = 0;
	uintptr_t root = 0;
	uintptr_t pcm = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		pc = PlayerController;
		pawn = AcknowledgedPawn;
		root = RootComponent;
		pcm = PlayerCameraManager;
	}

	const uintptr_t pcmStored = pcm;
	const uintptr_t pcBeforeLadder = pc;
	uintptr_t level = 0;
	uintptr_t actors = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		level = PersistentLevel;
		actors = Actors;
	}
	const bool pcmResolved = ResolvePlayerCameraManagerLadder(
		pc, pawn, pcm, level, actors, /*nocacheFov=*/true);
	if (pc && (!pcBeforeLadder || pc != pcBeforeLadder)) {
		std::unique_lock<std::shared_mutex> lock(m_stateMutex);
		PlayerController = pc;
		AcknowledgedPawn = pawn;
	}

	const bool pcmOk = pcmResolved && pcm && IsValidPointer(pcm);

	float povFov = 0.f;
	float defFov = 0.f;
	float jsonFov = 0.f;
	FVector3d povLoc{};
	FVector3d povRot{};

	if (pcmOk) {
		const uintptr_t povBase = pcm;
        povLoc = Memory::read_nocache<FVector3d>(povBase + Offsets::CameraPOV_Location);
        povRot = Memory::read_nocache<FVector3d>(povBase + Offsets::CameraPOV_Rotation);
        povFov = Memory::read_nocache<float>(povBase + Offsets::CameraPOV_FOV);
        defFov = Memory::read_nocache<float>(pcm + Offsets::DefaultFOV);
		jsonFov = 0.f;
	}

	Vector3 pawnPos{};
	uintptr_t rootComp = root;
	if (pawn && IsValidPointer(pawn)) {
		const uintptr_t pawnRoot =
            Memory::read_nocache<uintptr_t>(pawn + Offsets::RootComponent);
		if (pawnRoot && IsValidPointer(pawnRoot))
			rootComp = pawnRoot;
	}
	if (rootComp && IsValidPointer(rootComp)) {
		// ComponentToWorld is non-UPROPERTY: probe the working 0x2D0 profile
		// against the 0x310 alternate instead of trusting one fixed slot.
		const std::ptrdiff_t ctwOff = Engine::ProbeComponentToWorldOffset(rootComp);
        const FVector3d pawnWorld = Memory::read_nocache<FVector3d>(
			rootComp + ctwOff + Offsets::Transform_Translation);
		pawnPos = ToVector3(pawnWorld);
		if (!::IsPlausibleWorldPos(pawnPos))
            pawnPos = Memory::read_nocache<Vector3>(rootComp + Offsets::RelativeLocation);
		// Reflection-verified fallback: AActor::ReplicatedMovement -> Location.
		if (!::IsPlausibleWorldPos(pawnPos))
            pawnPos = Engine::ReadActorReplicatedLocation(pawn);
	}

	const bool pawnOk = ::IsPlausibleWorldPos(pawnPos);

	Vector3 ctrlRot{};
	if (pc && IsValidPointer(pc))
        ctrlRot = Memory::read_nocache<Vector3>(pc + Offsets::ControlRotation);

	CameraProbeSnapshot probe{};
	CameraCache published{};
	const bool ok = BuildCameraCacheFromPovReads(
		pcmOk,
		povFov,
		defFov,
		jsonFov,
		povLoc,
		povRot,
		pawnPos,
		pawnOk,
		ctrlRot,
		pc && IsValidPointer(pc),
		published,
		&probe);

	if (pcm && pcm != pcmStored) {
		std::unique_lock<std::shared_mutex> lock(m_stateMutex);
		PlayerCameraManager = pcm;
	}

	{
		DbgStoreCameraProbe(probe);
	}

	if (!ok)
		return false;

	{
		std::unique_lock<std::shared_mutex> lock(m_cameraMutex);
		g_Camera = published;
	}

	return true;
}

bool Engine::TryBuildCameraFromPcmPov(uintptr_t pcm, CameraCache& outCamera) const
{
	if (!pcm || !IsValidPointer(pcm) || !PcmLivePovLooksSane(pcm))
		return false;

	const uintptr_t povBase = pcm;
	const FVector3d povLoc =
		Memory::read_nocache<FVector3d>(povBase + Offsets::CameraPOV_Location);
	const FVector3d povRot =
		Memory::read_nocache<FVector3d>(povBase + Offsets::CameraPOV_Rotation);
	const float povFov =
		Memory::read_nocache<float>(povBase + Offsets::CameraPOV_FOV);
	const float defFov =
		Memory::read_nocache<float>(pcm + Offsets::DefaultFOV);

	Vector3 unusedPawn{};
	Vector3 unusedCtrl{};
	return BuildCameraCacheFromPovReads(
		true,
		povFov,
		defFov,
		0.f,
		povLoc,
		povRot,
		unusedPawn,
		false,
		unusedCtrl,
		false,
		outCamera,
		nullptr);
}

// Worker camera refresh (Update thread). RenderEsp also reads POV per-paint via
// TryBuildCameraFromPcmPov inside ResolveLiveRenderCamera — keep this as fallback.
void Engine::UpdateCamera()
{
	RefreshCameraFromViewTarget();
}

void Engine::SetProjectionViewport(float width, float height)
{
	if (width > 8.f && height > 8.f) {
		g_projViewportW.store(width, std::memory_order_relaxed);
		g_projViewportH.store(height, std::memory_order_relaxed);
	}
}

Vector3 Engine::GetProjectionScreenCenter() const
{
	const float w = g_projViewportW.load(std::memory_order_relaxed);
	const float h = g_projViewportH.load(std::memory_order_relaxed);
	return Vector3{
		static_cast<double>(w) * 0.5,
		static_cast<double>(h) * 0.5,
		0.0};
}

bool Engine::ProjectWorldLocationToScreen(
	Vector3 WorldLocation,
	Vector3& screen,
	const CameraCache& CameraInfo
)
{
	const double overlayW = static_cast<double>(
		g_projViewportW.load(std::memory_order_relaxed));
	const double overlayH = static_cast<double>(
		g_projViewportH.load(std::memory_order_relaxed));
	return EngineProjection::ProjectWorldLocationToScreen(
		WorldLocation,
		screen,
		CameraInfo.Location,
		CameraInfo.Rotation,
		CameraInfo.FOV,
		overlayW,
		overlayH);
}

bool Engine::ProjectWorldLocationToScreen(
	Vector3 WorldLocation,
	Vector3& screen
)
{
	CameraCache cameraInfo{};
	{
		std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
		cameraInfo = g_Camera;
	}
	return ProjectWorldLocationToScreen(WorldLocation, screen, cameraInfo);
}

static bool PawnHasWorldPosition(uint64_t pawn);

struct GITArrayHdr {
    uintptr_t Data = 0;
    int32_t Num = 0;
    int32_t Max = 0;
};

static GITArrayHdr ReadGIArray(uintptr_t gi, std::ptrdiff_t off)
{
    GITArrayHdr t{};
    if (!gi || !Memory::IsValidPtrFast2(gi))
        return t;
    t.Data = Memory::read<uintptr_t>(gi + off);
    t.Num = Memory::read<int32_t>(gi + off + 8);
    t.Max = Memory::read<int32_t>(gi + off + 12);
    return t;
}

static bool IsUsableObjectPtr(uintptr_t p)
{
    return Engine::IsUsableObjectPtr(p);
}

static bool ValidatePlayerController(uintptr_t pc, uintptr_t& outPawn, bool requirePawnWorld)
{
    if (!IsUsableObjectPtr(pc))
        return false;

    const uintptr_t pawn = Engine::ReadAcknowledgedPawn(pc);
    const uintptr_t pcm =
        Memory::read_nocache<uintptr_t>(pc + Offsets::APlayerCameraManager);

    bool pcmOk = false;
    if (IsUsableObjectPtr(pcm)) {
        const uintptr_t owner =
            Memory::read_nocache<uintptr_t>(pcm + Offsets::PCOwner);
        pcmOk = owner == pc && PcmLivePovLooksSane(pcm);
    }

    if (requirePawnWorld) {
        if (!IsUsableObjectPtr(pawn) || !PawnHasWorldPosition(pawn))
            return false;
    } else if (!pcmOk) {
        if (!IsUsableObjectPtr(pawn) || !Engine::ResolveActorRoot(pawn))
            return false;
    }

    outPawn = pawn;
    return true;
}

static bool TryPCFromLocalPlayer(uintptr_t lp, uintptr_t& outPc, uintptr_t& outPawn,
    bool requirePawnWorld = false)
{
    if (!IsUsableObjectPtr(lp))
        return false;

    const uintptr_t pc = Memory::read<uintptr_t>(lp + Offsets::LocalPlayer_PlayerController);
    if (ValidatePlayerController(pc, outPawn, requirePawnWorld)) {
        outPc = pc;
        return true;
    }
    return false;
}

static bool TryLocalPlayersSlots(uintptr_t data, int slotLimit, uintptr_t& outLp,
    uintptr_t& outPc, uintptr_t& outPawn, bool requirePawnWorld = false)
{
    if (!IsUsableObjectPtr(data) || slotLimit <= 0)
        return false;

    const int limit = (slotLimit > 16) ? 16 : slotLimit;
    for (int i = 0; i < limit; ++i) {
        const uintptr_t slot =
            Memory::read<uintptr_t>(data + static_cast<size_t>(i) * sizeof(uintptr_t));
        if (!IsUsableObjectPtr(slot))
            continue;

        uintptr_t pc = 0;
        uintptr_t pawn = 0;
        if (TryPCFromLocalPlayer(slot, pc, pawn, requirePawnWorld)) {
            outLp = slot;
            outPc = pc;
            outPawn = pawn;
            return true;
        }
    }
    return false;
}

static bool TryLocalPlayerChain(uintptr_t gi, std::ptrdiff_t localPlayersOff,
    uintptr_t& outLocalPlayer, uintptr_t& outPlayerController)
{
    outLocalPlayer = 0;
    outPlayerController = 0;

    if (!gi || !Memory::IsValidPtrFast2(gi))
        return false;

    uintptr_t pawn = 0;
    const GITArrayHdr arr = ReadGIArray(gi, localPlayersOff);
    if (IsUsableObjectPtr(arr.Data)) {
        const int limit = (arr.Num > 0 && arr.Num <= 16) ? arr.Num : 8;
        if (TryLocalPlayersSlots(arr.Data, limit, outLocalPlayer, outPlayerController, pawn, false))
            return true;
    }

    return false;
}

/** True for addresses inside the game image (CDOs, .rdata) — never a live object. */
static bool IsModuleSpace(uintptr_t p)
{
    const uint64_t base = Memory::getBaseAddress();
    if (!base || !p || p < base)
        return false;
    const uint64_t size = Memory::GetModuleSize();
    return size ? (p < base + size) : false;
}

static bool ValidateGameInstance(uintptr_t gi, uintptr_t* outLocalPlayer = nullptr,
    uintptr_t* outPlayerController = nullptr, uintptr_t world = 0)
{
    if (!IsUsableObjectPtr(gi) || Engine::LooksLikeUtf16Garbage(gi) || IsModuleSpace(gi))
        return false;

    uintptr_t lp = 0;
    uintptr_t pc = 0;

    // Help LocalPlayers 0x120; prior CL used 0x130 — try both (do not edit Offsets.h).
    static const std::ptrdiff_t kLpArrOffs[] = {
        Offsets::LocalPlayers,
        static_cast<std::ptrdiff_t>(0x130),
    };
    for (std::ptrdiff_t off : kLpArrOffs) {
        if (TryLocalPlayerChain(gi, off, lp, pc)) {
            if (outLocalPlayer)
                *outLocalPlayer = lp;
            if (outPlayerController)
                *outPlayerController = pc;
            return true;
        }
    }

    // The SDK drop does not define a GI->World back-reference field. Do not
    // validate through the old guessed +0x2F0 slot; the LocalPlayers array and
    // its LP->PC identity check are the available structural proof.
    (void)world;
    for (std::ptrdiff_t off : kLpArrOffs) {
        const GITArrayHdr arr = ReadGIArray(gi, off);
        if (!IsUsableObjectPtr(arr.Data) || IsModuleSpace(arr.Data))
            continue;
        if (arr.Num < 0 || arr.Num > 16)
            continue;
        if (arr.Max < arr.Num || arr.Max > 64)
            continue;
        const uintptr_t slot0 = Memory::read<uintptr_t>(arr.Data);
        if (!IsUsableObjectPtr(slot0) || IsModuleSpace(slot0) || Engine::LooksLikeUtf16Garbage(slot0))
            continue;
        if (outLocalPlayer)
            *outLocalPlayer = slot0;
        if (outPlayerController)
            *outPlayerController = 0;
        return true;
    }

    return false;
}

bool Engine::LooksLikeActor(uintptr_t obj)
{
    // A component's Outer is its actor, and an actor's RootComponent points back
    // at it the same way — see OuterLink::OwnerIsActor. The object itself is
    // still filtered first, because that rule says nothing about UTF-16 garbage.
    if (!IsPlausibleObjPtr(obj))
        return false;

    return OuterLink::OwnerIsActor(
        &OuterLink::NoCacheReader, obj,
        static_cast<uint64_t>(Offsets::RootComponent));
}

uintptr_t Engine::ResolveOwningActor(uintptr_t obj, int maxHops)
{
    if (!IsPlausibleObjPtr(obj))
        return 0;

    // A hop has to land on an object the tool can actually inspect — its class
    // pointer resolves — or the first rung would shadow the others, because a
    // wrong rung's mix is still a 64-bit number in range. The class pointer also
    // fails on an unmapped address, which is what that mix usually is.
    return OuterLink::ResolveOwner(
        obj,
        &OuterLink::NoCacheReader,
        [](uintptr_t p) {
            return Engine::IsPlausibleObjPtr(p) &&
                steam_decrypt::GetActorClassPtr(p) != 0;
        },
        [](uintptr_t p) { return Engine::LooksLikeActor(p); },
        maxHops);
}

// The OuterLink rung that reached a ULocalPlayer's GameInstance, spelled in the
// ladder's vocabulary so the panel and the trace can name it.
static PlayerChain::Rung MapOuterRung(OuterLink::Rung rung)
{
    switch (rung) {
    case OuterLink::Rung::SdkSlot:    return PlayerChain::Rung::OuterSdkSlot;
    case OuterLink::Rung::SdkSlotRol: return PlayerChain::Rung::OuterSdkSlotRol;
    case OuterLink::Rung::DropCipher: return PlayerChain::Rung::OuterDrop;
    case OuterLink::Rung::PlainOuter: return PlayerChain::Rung::OuterPlainA0;
    case OuterLink::Rung::PlainSlot:  return PlayerChain::Rung::OuterPlain20;
    default:                          return PlayerChain::Rung::None;
    }
}

/**
 * GameInstance rung: a ULocalPlayer's Outer *is* its GameInstance, so the
 * decrypt turns this into one exact hop instead of a slot scan. `which` names
 * the OuterLink rung that answered (the ladder reports it, nothing else does).
 */
static uintptr_t GiFromOuterRung(uintptr_t localPlayer, PlayerChain::Rung& which)
{
    which = PlayerChain::Rung::None;
    if (!IsUsableObjectPtr(localPlayer))
        return 0;

    OuterLink::Rung rung = OuterLink::Rung::None;
    const uintptr_t outer = OuterLink::GetOuterChecked(
        localPlayer, [](uintptr_t p) { return ValidateGameInstance(p); }, &rung);
    if (outer) {
        which = MapOuterRung(rung);
        return outer;
    }

    // Slot scan for whichever CL moved the outer field out of the schemes
    // OuterLink knows about.
    static const std::ptrdiff_t kOuterCands[] = {
        0x20, 0x28, 0x18, 0x30, 0x10, 0x38, 0x40, 0x48, 0x50,
        0x58, 0x60, 0x68, 0x70, 0x78, 0x80,
    };
    for (std::ptrdiff_t off : kOuterCands) {
        const uintptr_t cand = Memory::read<uintptr_t>(localPlayer + off);
        if (!ValidateGameInstance(cand))
            continue;
        which = PlayerChain::Rung::Scan;
        return cand;
    }
    return 0;
}

// CL-1341255: UWorld+0x478 (OwningGameInstance) is SIMD-encrypted — a plain
// read returns 0. Decrypt: 16B blob @ world+0x478, PSHUFB with the 16-byte
// mask @ base+0xB09C350, XOR qword lanes with keys @ base+0xB06C380/0xB06C390.
// Validate via the GI's SDK LocalPlayers chain; the drop does not define a
// GI->World back-reference field.
static uintptr_t DecryptGameInstancePointer(uintptr_t world)
{
    const uint64_t base = Memory::getBaseAddress();
    if (!base || !world)
        return 0;

    alignas(16) uint8_t enc[16];
    alignas(16) uint8_t mask[16];
    if (!Memory::ReadRaw(world + Offsets::OwningGameInstance, enc, sizeof(enc)))
        return 0;
    if (!Memory::ReadRaw(base + Offsets::GameInstanceShuffleMaskRva, mask, sizeof(mask)))
        return 0;

    bool maskAllZero = true;
    for (int i = 0; i < 16; ++i)
        if (mask[i]) { maskAllZero = false; break; }
    if (maskAllZero)
        return 0;

    const uint64_t k0 = Memory::read<uint64_t>(base + Offsets::GameInstanceXorKey0Rva);
    const uint64_t k1 = Memory::read<uint64_t>(base + Offsets::GameInstanceXorKey1Rva);
    if (!k0 && !k1)
        return 0;

    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(enc));
    v = _mm_shuffle_epi8(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask)));
    v = _mm_xor_si128(v, _mm_set_epi64x(
        static_cast<long long>(k1), static_cast<long long>(k0)));

    const uintptr_t gi = static_cast<uintptr_t>(_mm_cvtsi128_si64(v));
    if (!IsUsableObjectPtr(gi))
        return 0;

    // The SDK's static decrypt has no GI->World back-reference read. Validate
    // the result through the reflected LocalPlayers chain instead of the old
    // guessed +0x2F0 field.
    return ValidateGameInstance(gi, nullptr, nullptr, world) ? gi : 0;
}

// ── GameInstance rungs ───────────────────────────────────────────────────────
// One function per rung of the ladder's GameInstance hop, each carrying its own
// proof. Split out of the old single GetGameInstance() so the ladder can order
// and report them; nothing else in the tool can publish a GameInstance.

/** SDK UWorld::OwningGameInstance slot; `backRef` means the GI chain proved it. */
static uintptr_t GiOwningSlotRung(uintptr_t world, bool& backRef)
{
    backRef = false;
    const uintptr_t plain = Memory::read<uintptr_t>(world + Offsets::OwningGameInstance);
    if (!IsUsableObjectPtr(plain))
        return 0;
    // +0x478 is the SDK's UWorld::OwningGameInstance slot. The drop does not
    // define a GI->World field at +0x2F0, so identity comes from LocalPlayers,
    // not from that stale back-reference guess.
    if (ValidateGameInstance(plain, nullptr, nullptr, world)) {
        backRef = true;
        return plain;
    }
    return 0;
}

/** Legacy CL-1341255 SIMD-obfuscated slot (data RVAs from the old build). */
static uintptr_t GiFromDecryptSlot(uintptr_t world)
{
    // The 2026-09-22 drop's static decrypt is the primary source for this hop.
    // It does not read an invented GI field or a legacy slot: it derives the
    // result from GameInstanceStaticDecrypt::STAGE_ARRAY_RVA and returns
    // *(seed_ptr + RESULT_DEREF). Keep the older world-slot decrypt only as a
    // compatibility fallback for pre-drop targets.
    const uint64_t base = Memory::getBaseAddress();
    if (const uintptr_t gi = static_cast<uintptr_t>(GameInstanceLink::DecryptStatic(base));
        gi && ValidateGameInstance(gi, nullptr, nullptr, world))
        return gi;
    return DecryptGameInstancePointer(world);
}

/** The previous build's slot (0x3B0). */
static uintptr_t GiFromLegacySlotRung(uintptr_t world)
{
    const uintptr_t legacy = Memory::read<uintptr_t>(
        world + static_cast<std::ptrdiff_t>(0x3B0));
    if (!legacy || !ValidateGameInstance(legacy, nullptr, nullptr, world))
        return 0;
    return legacy;
}

/** Last resort: scan UWorld for a slot whose GI chain validates. */
static uintptr_t GiFromScanRung(uintptr_t world)
{
    uintptr_t loose = 0;
    for (std::ptrdiff_t off = 0x80; off <= 0x700; off += 0x8) {
        if (off == Offsets::OwningGameInstance || off == 0x3B0)
            continue;
        const uintptr_t gi = Memory::read<uintptr_t>(world + off);
        if (!IsUsableObjectPtr(gi))
            continue;
        if (!loose && ValidateGameInstance(gi, nullptr, nullptr, world))
            loose = gi;
    }
    return loose;
}

uintptr_t Engine::ResolveGameStateFromWorld(uintptr_t uworldAddr)
{
    if (!uworldAddr || !Memory::IsValidPtrFast2(uworldAddr))
        return 0;

    const uintptr_t collectionsData =
        Memory::read<uintptr_t>(uworldAddr + Offsets::LevelCollections);
    const int32_t collectionsNum =
        Memory::read<int32_t>(uworldAddr + Offsets::LevelCollections + 8);
    if (collectionsData && Memory::IsValidPtrFast2(collectionsData)
        && collectionsNum > 0 && collectionsNum <= 16) {
        const int limit = (collectionsNum > 4) ? 4 : collectionsNum;
        for (int i = 0; i < limit; ++i) {
            const uintptr_t collection =
                collectionsData + static_cast<uintptr_t>(i) * Offsets::LevelCollection_Stride;
            const uintptr_t gs = Memory::read<uintptr_t>(
                collection + Offsets::LevelCollection_GameState);
            if (gs && Memory::IsValidPtrFast2(gs)) {
                const uintptr_t arrData =
                    Memory::read<uintptr_t>(gs + Offsets::GameState_PlayerArray);
                const int32_t arrNum =
                    Memory::read<int32_t>(gs + Offsets::GameState_PlayerArray + 8);
                if (arrData && Memory::IsValidPtrFast2(arrData) && arrNum > 0 && arrNum <= 128)
                    return gs;
            }
        }
    }

    const uint64_t base = Memory::getBaseAddress();
    if (base) {
        const uintptr_t globalGs =
            Memory::read<uintptr_t>(base + Offsets::GameStateGlobalRva);
        if (globalGs && Memory::IsValidPtrFast2(globalGs))
            return globalGs;
    }
    return 0;
}

bool Engine::ResolveLevelActors(uintptr_t persistentLevel, uintptr_t& outActorsData, int& outActorCount)
{
    outActorsData = 0;
    outActorCount = 0;

    if (!persistentLevel || !Memory::IsValidPtrFast2(persistentLevel))
        return false;

    int32_t count = 0;
    uintptr_t data = 0;
    if (!WorldScan::ReadLevelActors(persistentLevel, data, count))
        return false;
    if (!Memory::IsValidPtrFast2(data))
        return false;

    outActorsData = data;
    outActorCount = count;
    return true;
}

// The rung's own primitives; the ladder names them (GiArrayBackRef / GiArraySlot0).

uintptr_t Engine::ResolveLocalPlayerFromController(uintptr_t playerController,
    PlayerChain::Rung* outRung, bool* outBackRef)
{
    if (outRung)
        *outRung = PlayerChain::Rung::None;
    if (outBackRef)
        *outBackRef = false;
    if (!playerController || !Memory::IsValidPtrFast2(playerController))
        return 0;

    // Help: LocalPlayer::PLAYER_CONTROLLER 0xA0 must point back at this PC.
    auto lpPointsAtPc = [&](uintptr_t lp) -> bool {
        if (!IsUsableObjectPtr(lp) || Engine::LooksLikeUtf16Garbage(lp))
            return false;
        return Memory::read<uintptr_t>(lp + Offsets::LocalPlayer_PlayerController)
            == playerController;
    };

    // 1. Dump-sourced path: the controller keeps its local player at +0x4B0
    //    *encrypted* (APlayerController::GetLocalPlayer, sub_3680940), so the
    //    plain-pointer scan below could never find it on this build. Decrypt,
    //    then demand the 0xA0 back-pointer as identity proof.
    const uintptr_t decrypted = PlayerLink::FromController(playerController);
    if (IsUsableObjectPtr(decrypted)) {
        if (lpPointsAtPc(decrypted)) {
            if (outRung)
                *outRung = PlayerChain::Rung::PcDecryptBackRef;
            if (outBackRef)
                *outBackRef = true;
            return decrypted;
        }
        // Live object, but 0xA0 does not confirm it yet (lobby / pre-pair). Keep
        // it as the weaker candidate and let the scan try to beat it.
        const uintptr_t weakCandidate = decrypted;
        for (std::ptrdiff_t off = 0x80; off <= 0x800; off += 0x8) {
            const uintptr_t lp = Memory::read<uintptr_t>(playerController + off);
            if (lpPointsAtPc(lp)) {
                if (outRung)
                    *outRung = PlayerChain::Rung::PcSlotScan;
                if (outBackRef)
                    *outBackRef = true;
                return lp;
            }
        }
        if (outRung)
            *outRung = PlayerChain::Rung::PcDecryptOnly;
        return weakCandidate;
    }

    // 2. Legacy fallback: scan the controller for a slot that points back at it.
    for (std::ptrdiff_t off = 0x80; off <= 0x800; off += 0x8) {
        const uintptr_t lp = Memory::read<uintptr_t>(playerController + off);
        if (lpPointsAtPc(lp)) {
            if (outRung)
                *outRung = PlayerChain::Rung::PcSlotScan;
            if (outBackRef)
                *outBackRef = true;
            return lp;
        }
    }

    return 0;
}

static bool PawnHasWorldPosition(uint64_t pawn)
{
    const uintptr_t root = Memory::read<uintptr_t>(pawn + Offsets::RootComponent);
    if (!root || !Memory::IsValidPtrFast2(root))
        return false;

    const Vector3 pos = Memory::read<Vector3>(root + Offsets::RelativeLocation);
    const float magSq = static_cast<float>(
        pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
    return magSq > 10000.f && magSq < 1.0e14f;
}

// Dump-verified identity for a player controller.
//
// sdk/SDK.txt (20260922) reflects APlayerController::bIsLocalPlayerController at
// 0xD64 (byte 0xD64, mask 0x1), and Angelscript.PioneerPlayerController — the
// class this game actually spawns — inherits it at the same offset. That flag is
// the engine's own "this controller belongs to a local player" bit, set when the
// controller is created from a ULocalPlayer, so it identifies the local PC on its
// own: no pawn, no PlayerCameraManager, no FOV heuristics.
//
// The one caveat is that a random object read at +0xD64 can have the bit set by
// chance, so the caller can require a second field (PlayerState) as a cheap
// "this really is an AController" proof.
bool Engine::IsLocalPlayerController(uintptr_t pc)
{
    if (!pc || !Memory::IsValidPtrFast2(pc))
        return false;

    const uint8_t flags = Memory::read<uint8_t>(
        pc + Offsets::PlayerController_bIsLocalPlayerController);
    return (flags & Offsets::PlayerController_bIsLocalPlayerController_Mask) != 0;
}

bool Engine::IsLocalPlayerControllerConfirmed(uintptr_t pc)
{
    if (!IsLocalPlayerController(pc))
        return false;

    // AController::PlayerState (Engine.Controller.PlayerState, 0x3D0) is non-null
    // on every live controller — a live local PC always has one, so this drops the
    // chance-coincidence reads without touching the camera slot.
    const uintptr_t playerState =
        Memory::read<uintptr_t>(pc + Offsets::AController_PlayerState);
    return playerState != 0 && Memory::IsValidPtrFast2(playerState);
}

bool Engine::ControllerHasValidPcm(uintptr_t pc)
{
    if (!pc || !Memory::IsValidPtrFast2(pc))
        return false;

    const uintptr_t pcm =
        Memory::read_nocache<uintptr_t>(pc + Offsets::APlayerCameraManager);
    if (!pcm || !Memory::IsValidPtrFast2(pcm))
        return false;

    // Camera validity must include the live POV, not DefaultFOV alone. A stale
    // object can retain a perfectly plausible default FOV after its view is gone.
    return PcmLivePovLooksSane(pcm);
}

bool Engine::ResolvePcFromLevelCameraManager(
    uintptr_t level,
    uintptr_t actorsData,
    uintptr_t& outPc,
    uintptr_t& outPawn,
    uintptr_t& outPcm)
{
    // Prefer all-Levels FName PCM path (help/esp.txt). Fall back to single-level
    // scan only when CollectLevelActors is unavailable.
    {
        uintptr_t gworld = 0;
        uintptr_t persistent = 0;
        {
            std::shared_lock<std::shared_mutex> lock(m_stateMutex);
            gworld = GWorld;
            persistent = PersistentLevel;
        }
        const uintptr_t pcm = GetCameraManagerFromActors();
        if (pcm) {
            const uintptr_t owner = Memory::read_nocache<uintptr_t>(pcm + Offsets::PCOwner);
            if (owner && Memory::IsValidPtrFast2(owner)) {
                const uintptr_t pawn = Engine::ReadAcknowledgedPawn(owner);
                if (pawn && Memory::IsValidPtrFast2(pawn) && PawnHasWorldPosition(pawn)) {
                    outPc = owner;
                    outPawn = pawn;
                    outPcm = pcm;
                    return true;
                }
            }
        }
        (void)gworld;
        (void)persistent;
    }

    if (!level || !actorsData)
        return false;

    int actorCount = 0;
    uintptr_t resolvedActors = actorsData;
    if (!Engine::ResolveLevelActors(level, resolvedActors, actorCount))
        return false;

    const int scanLimit = (actorCount < 10000) ? actorCount : 10000;
    if (!InitConsts())
        return false;

    for (int i = 0; i < scanLimit; ++i) {
        const uintptr_t candidate = Memory::read<uintptr_t>(
            resolvedActors + static_cast<size_t>(i) * sizeof(uintptr_t));
        if (!candidate || !Memory::IsValidPtrFast2(candidate))
            continue;

        std::string cn = GetActorClassFName(candidate);
        if (cn.find("PlayerCameraManager") == std::string::npos
            && cn.find("CameraManager") == std::string::npos) {
            const std::string fname = GetActorFNameString(candidate);
            if (fname.find("PlayerCameraManager") == std::string::npos
                && fname.find("CameraManager") == std::string::npos)
                continue;
        }
        if (!PcmLivePovLooksSane(candidate))
            continue;

        const uintptr_t owner = Memory::read_nocache<uintptr_t>(candidate + Offsets::PCOwner);
        if (!owner || !Memory::IsValidPtrFast2(owner))
            continue;
        const uintptr_t pawn = Engine::ReadAcknowledgedPawn(owner);
        if (!pawn || !Memory::IsValidPtrFast2(pawn) || !PawnHasWorldPosition(pawn))
            continue;

        outPc = owner;
        outPawn = pawn;
        outPcm = candidate;
        return true;
    }

    return false;
}

static bool TryControllerAndPawn(uint64_t controller, uintptr_t& outController, uintptr_t& outPawn,
    bool requireValidPcm = false)
{
    if (!controller || !Memory::IsValidPtrFast2(controller))
        return false;

    const uintptr_t pawn = Engine::ReadAcknowledgedPawn(controller);
    if (!pawn || !Memory::IsValidPtrFast2(pawn) || !PawnHasWorldPosition(pawn))
        return false;

    const uintptr_t pcm = Memory::read_nocache<uintptr_t>(controller + Offsets::APlayerCameraManager);
    if (pcm && Memory::IsValidPtrFast2(pcm)) {
        const uintptr_t pcmOwner = Memory::read_nocache<uintptr_t>(pcm + Offsets::PCOwner);
        if (pcmOwner == controller && PcmLivePovLooksSane(pcm)) {
            outController = controller;
            outPawn = pawn;
            return true;
        }
    }

    if (requireValidPcm)
        return false;

    outController = controller;
    outPawn = pawn;
    return true;
}

bool Engine::ResolveLocalPlayerChainFromActors(uintptr_t persistentLevel, uintptr_t actorsArray, int actorCount,
    uintptr_t gameInstance, uintptr_t& outController, uintptr_t& outPawn)
{
    outController = 0;
    outPawn = 0;

    if (!persistentLevel || !actorsArray || !Memory::IsValidPtrFast2(persistentLevel)
        || !Memory::IsValidPtrFast2(actorsArray))
        return false;

    if (actorCount <= 0 || actorCount > 10000)
        return false;

    std::vector<uint64_t> actors(static_cast<size_t>(actorCount));
    if (!Memory::ReadRaw(actorsArray, actors.data(), actors.size() * sizeof(uint64_t)))
        return false;

    const int scanLimit = (actorCount < 2048) ? actorCount : 2048;

    // Pass 0: dump-verified identity. APlayerController::bIsLocalPlayerController
    // (0xD64) is the engine's own flag and the 20260922 dump reflects it for the
    // class this game actually spawns (Angelscript.PioneerPlayerController), so it
    // outranks every heuristic (camera FOV, name sniffing). PlayerState proves the
    // actor is an AController; a placed pawn is a bonus, not a requirement, so a
    // hub/lobby controller with no pawn still resolves.
    uintptr_t flagPcNoPawn = 0;
    for (int i = 0; i < scanLimit; ++i) {
        const uint64_t actor = actors[static_cast<size_t>(i)];
        if (!actor || !Engine::IsValidPointer(actor))
            continue;
        if (!Engine::IsLocalPlayerControllerConfirmed(actor))
            continue;

        const uintptr_t pawn = Engine::ReadAcknowledgedPawn(actor);
        if (pawn && Engine::IsValidPointer(pawn) && PawnHasWorldPosition(pawn)) {
            outController = actor;
            outPawn = pawn;
            return true;
        }
        if (!flagPcNoPawn)
            flagPcNoPawn = actor;
    }
    if (flagPcNoPawn) {
        outController = flagPcNoPawn;
        const uintptr_t pawn = Engine::ReadAcknowledgedPawn(flagPcNoPawn);
        outPawn = (pawn && Engine::IsValidPointer(pawn)) ? pawn : 0;
        return true;
    }

    // Pass 1: controllers with valid PCM @ PC+0x48 (backup strong path).
    for (int i = 0; i < scanLimit; ++i) {
        const uint64_t actor = actors[static_cast<size_t>(i)];
        if (TryControllerAndPawn(actor, outController, outPawn, true))
            return true;
    }

    if (InitConsts()) {
        for (int i = 0; i < scanLimit; ++i) {
            const uint64_t actor = actors[static_cast<size_t>(i)];
            if (!actor || !IsValidPointer(actor))
                continue;

            const std::string fname = GetActorFNameString(actor);
            if (fname.find("PioneerPlayerController") == std::string::npos
                && fname.find("BP_PioneerPlayerController") == std::string::npos)
                continue;

            if (TryControllerAndPawn(actor, outController, outPawn, true))
                return true;
        }
    }

    return false;
}

uintptr_t Engine::GetCameraManagerFromActors(uintptr_t worldOverride,
    uintptr_t levelOverride, uintptr_t pcOverride)
{
	// Camera discovery is called by Update, the camera worker, and frame building.
	// Serialize the scan/cache so the static generation-scoped pointer is not
	// raced by concurrent readers.
	static std::mutex s_pcmMutex;
	std::lock_guard<std::mutex> pcmLock(s_pcmMutex);

	// help/esp.txt: FName PCM over all Levels; LP not required; prefer PCOwner match;
	// live POV FOV + location sanity (not DefaultFOV-only / FOV-scan).
	uintptr_t pc = pcOverride;
	uintptr_t gworld = worldOverride;
	uintptr_t persistent = levelOverride;
	if (!pc || !gworld || !persistent) {
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		if (!pc)
			pc = PlayerController;
		if (!gworld)
			gworld = GWorld;
		if (!persistent)
			persistent = PersistentLevel;
	}
	uintptr_t pcmDirect = 0;
	if (pc) {
		pcmDirect = Memory::read_nocache<uintptr_t>(pc + Offsets::APlayerCameraManager);
		const uintptr_t owner = pcmDirect
			? Memory::read_nocache<uintptr_t>(pcmDirect + Offsets::PCOwner)
			: 0;
		if (pcmDirect && IsValidPointer(pcmDirect)
			&& PcmLivePovLooksSane(pcmDirect)
			&& (!owner || owner == pc))
			return pcmDirect;
	}

	// Cache only within one world generation. Pointer validity alone is not
	// enough: freed actors can leave a numerically valid stale address behind.
	static uintptr_t s_cachedPcm = 0;
	static uint64_t s_cachedGeneration = UINT64_MAX;
	const uint64_t generation = m_worldGeneration.load(std::memory_order_acquire);
	if (s_cachedGeneration != generation) {
		s_cachedGeneration = generation;
		s_cachedPcm = 0;
	}
	if (s_cachedPcm && IsValidPointer(s_cachedPcm)
		&& PcmLivePovLooksSane(s_cachedPcm)) {
		const uintptr_t owner = Memory::read_nocache<uintptr_t>(
			s_cachedPcm + Offsets::PCOwner);
		if (!pc || !owner || owner == pc)
			return s_cachedPcm;
		s_cachedPcm = 0;
	}

	std::vector<uint64_t> actors;
	WorldScan::CollectLevelActors(gworld, persistent, actors);
	if (actors.empty()) {
		const uintptr_t owner = pcmDirect
			? Memory::read_nocache<uintptr_t>(pcmDirect + Offsets::PCOwner)
			: 0;
		return pcmDirect && IsValidPointer(pcmDirect)
			&& PcmLivePovLooksSane(pcmDirect)
			&& (!pc || !owner || owner == pc) ? pcmDirect : 0;
	}

	const bool fnameReady = InitConsts();
	uintptr_t ownedHit = 0;
	uintptr_t fnameHit = 0;

	// One-shot diagnostic: log first 10 actor class names + probe PCM offsets on first 5
	{
		static bool s_diagDone = false;
		if (!s_diagDone && fnameReady && !actors.empty()) {
			s_diagDone = true;
			std::ofstream diag(kArcDebugLogPath, std::ios::app);
			int logged = 0;
			const int limit = (actors.size() < 10) ? (int)actors.size() : 10;
			for (int idx = 0; idx < limit; ++idx) {
				const uintptr_t cand = static_cast<uintptr_t>(actors[idx]);
				if (cand && IsValidPointer(cand)) {
					std::string cn = GetActorClassFName(cand);
					std::string fn = GetActorFNameString(cand);
					diag << "actor_class class=" << cn << " fname=" << fn
						<< " addr=" << cand << std::endl;
					++logged;
				}
			}
			int probed = 0;
			const int probeLimit = (actors.size() < 5) ? (int)actors.size() : 5;
			for (int idx = 0; idx < probeLimit; ++idx) {
				const uintptr_t cand = static_cast<uintptr_t>(actors[idx]);
				if (!cand || !IsValidPointer(cand)) continue;
				const float fov = Memory::read_nocache<float>(cand + Offsets::CameraPOV_FOV);
				const float defFov = Memory::read_nocache<float>(cand + Offsets::DefaultFOV);
				const float fov4C8 = Memory::read_nocache<float>(cand + 0x4C8);
				const float fovCA0 = Memory::read_nocache<float>(cand + 0xCA0);
				const uintptr_t pcOwner = Memory::read_nocache<uintptr_t>(cand + Offsets::PCOwner);
				diag << "pcm_probe addr=" << std::hex << cand << std::dec
					<< " fov@510=" << fov
					<< " defFov@3E8=" << defFov
					<< " fov@4C8=" << fov4C8
					<< " fov@CA0=" << fovCA0
					<< " pcOwner@3D0=" << pcOwner
					<< " class=" << GetActorClassFName(cand)
					<< std::endl;
				++probed;
			}
		}
	}

	auto classOrInstanceIsPcm = [&](uintptr_t candidate) -> bool {
		if (!fnameReady)
			return false;
		std::string cn = GetActorClassFName(candidate);
		if (cn.find("PlayerCameraManager") != std::string::npos
			|| cn.find("CameraManager") != std::string::npos)
			return true;
		const std::string fn = GetActorFNameString(candidate);
		return fn.find("PlayerCameraManager") != std::string::npos
			|| fn.find("CameraManager") != std::string::npos;
	};

	int pcmNameHits = 0;
	int pcmValidationFails = 0;
	for (uint64_t a : actors) {
		const uintptr_t candidate = static_cast<uintptr_t>(a);
		if (!candidate || !IsValidPointer(candidate))
			continue;
		if (!classOrInstanceIsPcm(candidate))
			continue;
		++pcmNameHits;
		if (!PcmLivePovLooksSane(candidate)) {
			++pcmValidationFails;
			continue;
		}

		const uintptr_t owner = Memory::read_nocache<uintptr_t>(
			candidate + Offsets::PCOwner);
		if (pc && owner == pc)
			return candidate;
		if (owner && Memory::IsValidPtrFast2(owner)) {
			const uintptr_t pawn = Engine::ReadAcknowledgedPawn(owner);
			if (pawn && Memory::IsValidPtrFast2(pawn) && PawnHasWorldPosition(pawn)
				&& !ownedHit)
				ownedHit = candidate;
		} else if (!fnameHit) {
			fnameHit = candidate;
		}
	}

	if (ownedHit) {
		s_cachedPcm = ownedHit;
		return ownedHit;
	}
	if (fnameHit) {
		s_cachedPcm = fnameHit;
		return fnameHit;
	}
	// Log PCM discovery failure details (one-shot per generation)
	{
		static uint64_t s_lastLogGen = UINT64_MAX;
		if (s_lastLogGen != generation) {
			s_lastLogGen = generation;
			std::ofstream diag(kArcDebugLogPath, std::ios::app);
			diag << "pcm_not_found actors=" << actors.size()
				<< " nameHits=" << pcmNameHits
				<< " validationFails=" << pcmValidationFails
				<< " fnameReady=" << fnameReady
				<< " pc=" << pc << std::endl;
		}
	}
	if (pcmDirect && IsValidPointer(pcmDirect)
		&& PcmLivePovLooksSane(pcmDirect)) {
		const uintptr_t owner = Memory::read_nocache<uintptr_t>(
			pcmDirect + Offsets::PCOwner);
		if (!pc || !owner || owner == pc) {
			s_cachedPcm = pcmDirect;
			return pcmDirect;
		}
	}
	return 0;
}

// SHARED GATE — grep callers before edit
bool Engine::getAllowType(const std::string& actorName, int category) const
{
    // Radar-only mode must admit bots too (scanner already gates on show_radar).
    const bool wantBots = var::showRobots || var::robotAimEnabled || var::show_radar;

    if (robotsList.find(actorName) != robotsList.end())
        return wantBots;

    // category 3: RobotList — only real robot-list names / fname maps.
    // NEVER accept arbitrary strings via NormalizeBotDisplayName echo
    // (that admitted "GC Electrified" as a bot — c190fb near_bot_w2s_fail).
    if (wantBots && category == 3) {
        if (actorName.empty())
            return false;
        if (actorName == kBotStructAdmissionToken)
            return true;
        if (robotsList.find(actorName) != robotsList.end())
            return true;
        if (const std::string fromPat = LookupEnemyBotByFName(actorName); !fromPat.empty()
            && robotsList.find(fromPat) != robotsList.end())
            return true;
        if (const std::string fromDisplay = LookupEnemyBotDisplayLabel(actorName);
            !fromDisplay.empty() && robotsList.find(fromDisplay) != robotsList.end())
            return true;
        return IsAcceptedBotEspLabel(
            *const_cast<Engine*>(this), actorName, std::string{});
    }

    if (actorName == "Loot Item" || actorName == "World Item")
        return WorldCategoryEnabled(static_cast<int>(WorldItemCategory::Other));

    if (actorName == "Raider stock")
        return WorldCategoryEnabled(static_cast<int>(WorldItemCategory::RaiderCache));

    if (actorName == "Arc Cargoship")
        return WorldCategoryEnabled(static_cast<int>(WorldItemCategory::ArcCargoship));

    if (actorName == "Corpse")
        return WorldCategoryEnabled(static_cast<int>(WorldItemCategory::Corpse));

    return false;
}

bool Engine::getAllowWorldEntry(const WorldCacheEntry& entry) const
{
    // NEAR-FIELD REVEAL ("standind in front of a crate, 2 min, no esp"): a
    // world object within touching distance is what the user is looking at —
    // category toggles and loot filters must never hide it. Toggles govern
    // distance clutter only.
    constexpr float kNearRevealM = 15.f;
    // Master switches always win, including near-field reveal.
    if (!var::enable_world || !var::showLoot)
        return false;
    if (entry.Distance >= 0.f && entry.Distance <= kNearRevealM)
        return true;
    if (!WorldCategoryEnabled(entry.worldCategory))
        return false;

    return PassesLootPickupFilters(WorldLootFilterView{
        entry.worldCategory,
        entry.ActorName,
        entry.ItemDisplayName,
        entry.lootValue,
        entry.lootRarityTier});
}

namespace {

/** Union of all former inventory/weapon prefix tables (longest match first). */
std::string StripWeaponAssetName(std::string name)
{
    static const char* kPrefixes[] = {
        "WeaponVisuals_",
        "WeaponVisuals",
        "DA_WeaponItem_",
        "DA_ItemDataAsset_",
        "BP_WeaponActor_",
        "BP_ItemActor_",
        "BP_WeaponActor",
        "BP_ItemActor",
        "BP_Weapon_",
        "BP_Item_",
        "BP_WItem_",
        "DA_Item_",
        "Default__",
        "BP_Weapon",
        "DA_",
        "BP_",
    };
    for (const char* prefix : kPrefixes) {
        const size_t len = std::strlen(prefix);
        if (name.size() >= len && name.compare(0, len, prefix) == 0) {
            name = name.substr(len);
            break;
        }
    }
    // Blueprint / visual variant suffixes: _C, _A, _B, _01_A → drop trailing _X
    // (single letter) after optional blueprint _C.
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0)
        name.resize(name.size() - 2);
    if (name.size() > 2) {
        const size_t us = name.rfind('_');
        if (us != std::string::npos && us + 2 == name.size()) {
            const char letter = name[us + 1];
            if ((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))
                name.resize(us);
        }
    }
    return name;
}

} // namespace

std::string Engine::GetWeaponName(const std::string& internal_name) {
    if (internal_name.empty())
        return {};

    const std::string stripped = StripWeaponAssetName(internal_name);

    static const std::unordered_map<std::string, std::string> processMap = {     
        { "Pneumatic_01",                     "Kettle" },
        { "AssaultRifle_LowTier_01",           "Rattler" },
        { "Burst_01",                          "Arpeggio" },
        { "Heavy_01",                          "Bettina" },
        { "AssaultRifle_Bullpup_01_C",         "Tempest" },
        { "SMG_LowTier_01",                    "Stitcher" },
        { "SMG_02",                            "Bobcat" },
        { "PumpAction_01",                     "Il Toro" },
        { "Shotgun_SemiAuto_01",               "Vulcano" },
        { "Sniper_BoltAction_01",              "Osprey" },
        { "Sniper_Energy_01",                  "Jupiter" },
        { "Pistol_Silenced_01",                "Hairpin" },
        { "Pistol_01",                         "Burletta" },
        { "SingleAction_01",                   "Anvil" },
        { "Pistol_HighPower_01",               "Venator" },
        {"Pistol_SemiAuto_02",                  "Venator"},
        { "LMG_Standard_01",                   "Torrente" },
        { "BreachAction_01",                   "Ferro" },
        { "LeverAction_01",                    "Renegade" },
        { "AssaultRifle_Bullpup_01",           "Arpeggio" },
        { "Launcher_AntiArc_Medium_01_C",      "Hullcracker" },
        { "Launcher_AntiArc_Medium_01",        "Hullcracker" },
        { "Launcher_AntiArc_SingleShot_01",    "Rascal" },
        { "LMG_Medium_01",                     "Torrente" },
        { "Special_BeamRifle_01",              "Equalizer" },
        { "Beam_01",                           "Equalizer"},
        { "SniperRifle_BoltAction_Medium_01",  "Osprey" },

        { "BasicMeleeWeapon",                  "Melee" },
        { "HealingHoT_Small",                  "Bandage" },
        { "HealingHoT_Improvised",             "Herbal Bandage" },
        { "HealingHoT_Sterilized",             "Sterilized Bandage" },
        { "AdrenalineShot",                    "Adrenaline Shot" },
        { "Consumable_ShieldOverTimePack",     "Shield Recharger" },
        { "Armor_Patcher",                     "Surge Shield Recharger" },
        { "ShieldOverTimePack",                "Shield Recharger" },
        { "Defibrillator",                     "Defibrillator" },
        { "JumpMine_Impulse",                  "Impulse Mine" },
        { "SmokeGrenade",                      "Smoke Grenade" },
        { "ScatterMissileGrenade",             "Wolfpack Grenade" },
        { "StunGrenade",                       "Showstopper Grenade" },
        { "FragGrenade",                       "Frag Grenade" },
        { "GasGrenade",                        "Gas Grenade" },
        { "LightGrenade",                      "Light Stick" },
    };

    if (auto it = processMap.find(internal_name); it != processMap.end()) {
        return it->second;
    }
    if (auto it = processMap.find(stripped); it != processMap.end()) {
        return it->second;
    }

    // BP_WeaponActor_SMG_01 → stripped "SMG_01". Resolve via ST_ItemNames /
    // asset_index (e.g. ST_ITEMNAME_FIREARM_SMG_01 → "Bobcat") before the
    // underscore humanizer turns it into "SMG 01".
    auto resolveFromTables = [](const std::string& token) -> std::string {
        if (token.empty())
            return {};
        if (const std::string fromLoc = LookupByInternalToken(token); !fromLoc.empty())
            return fromLoc;
        if (const std::string fromDa =
                LookupByAssetName(std::string("DA_Item_") + token); !fromDa.empty())
            return fromDa;
        if (const std::string fromAsset = LookupByAssetName(token); !fromAsset.empty())
            return fromAsset;
        return {};
    };
    if (const std::string resolved = resolveFromTables(stripped); !resolved.empty())
        return resolved;
    if (const std::string resolvedRaw = resolveFromTables(internal_name); !resolvedRaw.empty())
        return resolvedRaw;

    // Longest-match wins: "Pistol_HighPower_01" must beat the generic "Pistol_01",
    // "SMG_LowTier_01" must beat "SMG_02", etc. An unordered first-match would
    // otherwise mislabel whole weapon families.
    {
        std::string best;
        size_t bestLen = 0;
        for (const auto& survivor : processMap) {
            if (survivor.first.size() <= bestLen)
                continue;
            if (internal_name.find(survivor.first) != std::string::npos
                || stripped.find(survivor.first) != std::string::npos) {
                bestLen = survivor.first.size();
                best = survivor.second;
            }
        }
        if (!best.empty())
            return best;
    }

    std::string human = stripped;
    std::replace(human.begin(), human.end(), '_', ' ');
    if (!human.empty()) {
        // Never ship unresolved WeaponVisuals_* as ESP text.
        std::string humanLower = human;
        for (char& c : humanLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (humanLower.find("weaponvisuals") != std::string::npos)
            return {};
        return human;
    }

    return {};
}

bool Engine::IsPlayerWeaponEspLabel(const std::string& label)
{
    if (label.empty())
        return false;

    std::string lower = label;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower == "unarmed")
        return false;

    // Never show unresolved visual-asset labels on ESP.
    if (lower.find("weaponvisuals") != std::string::npos)
        return false;

    static const char* kJunk[] = {
        "blueprint",
        "recipe",
        "schematic",
        "outfit",
        "cosmetic",
        "emote",
        "augment",
        "quest",
        "keycard",
        "salvage",
    };
    for (const char* junk : kJunk) {
        if (lower.find(junk) != std::string::npos)
            return false;
    }
    return true;
}

namespace {

bool FnameMatchesLootItemBucket(const std::string& actorNameLower)
{
    // Same order as FnameLooksLikeDroppedPickup: floor BP_ItemActor / DA shells
    // win before inventory-service exclusions (Canister was wiped by "itemactor").
    if (actorNameLower.find("bp_pickupbase") != std::string::npos
        || actorNameLower.find("pickupbase") != std::string::npos
        || actorNameLower.find("bp_pickup") != std::string::npos
        || actorNameLower.find("bp_itemactor_") != std::string::npos
        || actorNameLower.find("bp_item_") != std::string::npos
        || actorNameLower.find("da_item_") != std::string::npos
        || actorNameLower.find("wid_") != std::string::npos)
        return true;
    if (IsInventoryWorldFnameExcluded(actorNameLower))
        return false;
    return false;
}

bool FnameSubstringMatchExcludedFromLootScan(const std::string& key)
{
    return key == "pickup" || key == "container";
}

} // namespace

// SHARED GATE — grep callers before edit
std::string Engine::getEntityType(const std::string& actorName)
{
    static const std::unordered_map<std::string, std::string> processMap = {
        {"bp_arc_cargoship", "Arc Cargoship"},
        {"cargoship", "Arc Cargoship"},
        {"arc_cargo", "Arc Cargoship"},

        {"bp_raidercache", "Raider stock"},
        {"raidercache", "Raider stock"},
        {"raider_cache", "Raider stock"},
        {"bp_pioneercharacter", "Corpse"},
        {"pioneercharacter", "Corpse"},
        {"corpse", "Corpse"},
        {"deadplayer", "Corpse"},
        {"player corpse", "Corpse"},

        { "bp_pickupbase", "Loot Item" },
        { "pickup", "Loot Item" },
        { "container", "Loot Item" },
    };

    const std::string actorNameLower = toLower(actorName);

    if (FnameMatchesLootItemBucket(actorNameLower))
        return "Loot Item";

    if (auto it = processMap.find(actorNameLower); it != processMap.end()) {
        return it->second;
    }

    if (const std::string botType = LookupBotClassToken(actorName); !botType.empty())
        return botType;

    {
        std::string bestType;
        size_t bestLen = 0;
        for (const auto& survivor : processMap) {
            if (survivor.first.size() <= bestLen)
                continue;
            if (FnameSubstringMatchExcludedFromLootScan(survivor.first))
                continue;
            if (actorNameLower.find(survivor.first) != std::string::npos) {
                bestLen = survivor.first.size();
                bestType = survivor.second;
            }
        }
        if (!bestType.empty())
            return bestType;
    }

    if (const std::string botType = LookupEnemyBotByFName(actorName); !botType.empty())
        return botType;

    if (const std::string display = LookupDisplayByFNameAssetIndex(actorName); !display.empty()) {
        const std::string fromDisplay = LookupEnemyBotDisplayLabel(display);
        if (robotsList.find(fromDisplay) != robotsList.end())
            return fromDisplay;
    }

    if (FnameAdmitsWorldActor(actorName))
        return "World Item";

    return "Invalid";
}


inline std::string utf16_to_utf8(const uint16_t* data, size_t len) {
    if (!data || len == 0) return std::string();

    std::wstring wstr(reinterpret_cast<const wchar_t*>(data), len);
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return std::string();

    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size_needed, nullptr, nullptr);
    return result;
}

namespace {

std::string ReadHoverDisplayNameAt(uint64_t hover_base)
{
    auto accept = [](std::string s) -> std::string {
        if (s.empty())
            return {};
        s = FormatEspDisplayLabel(s);
        if (s.empty() || IsGenericWorldEspLabel(s) || IsJunkWorldEspLabel(s)
            || IsGarbledEspLabel(s) || !IsPlausibleEspLabel(s))
            return {};
        return s;
    };

    uint64_t l0 = Memory::read<uint64_t>(hover_base);
    if (!l0 || !Memory::IsValidPtrFast2(l0))
        goto fallback;
    {
        uint64_t l1 = Memory::read<uint64_t>(l0 + 0x10);
        if (!l1 || !Memory::IsValidPtrFast2(l1)) goto fallback;

        uint64_t key_ptr = Memory::read<uint64_t>(l1 + 0x18);
        if (!key_ptr || !Memory::IsValidPtrFast2(key_ptr)) goto fallback;

        uint16_t kbuf[64] = {};
        Memory::ReadRaw(key_ptr, kbuf, sizeof(kbuf));
        std::string loc_key;
        for (int i = 0; i < 63 && kbuf[i]; ++i)
            loc_key += (char)(kbuf[i] & 0x7F);

        if (loc_key.find("ST_") != 0 && loc_key.find("ID_") != 0) {
            if (const std::string fromLoc = LookupByLocKey(loc_key); fromLoc.length() >= 2)
                return accept(fromLoc);
            goto fallback;
        }

        if (const std::string fromLoc = LookupByLocKey(loc_key); fromLoc.length() >= 2)
            return accept(fromLoc);

        uint64_t l2 = Memory::read<uint64_t>(l1 + 0x30);
        if (!l2 || !Memory::IsValidPtrFast2(l2)) goto fallback;

        uint64_t wstr = Memory::read<uint64_t>(l2 + 0x10);
        if (!wstr || !Memory::IsValidPtrFast2(wstr)) goto fallback;

        uint16_t buf[128] = {};
        Memory::ReadRaw(wstr, buf, sizeof(buf));
        std::string result;
        for (int i = 0; i < 127 && buf[i]; ++i) {
            uint16_t c = buf[i];
            if (c < 0x80) result += (char)c;
            else if (c < 0x800) {
                result += (char)(0xC0 | (c >> 6));
                result += (char)(0x80 | (c & 0x3F));
            }
            else {
                result += (char)(0xE0 | (c >> 12));
                result += (char)(0x80 | ((c >> 6) & 0x3F));
                result += (char)(0x80 | (c & 0x3F));
            }
        }
        if (result.length() >= 2)
            return accept(result);
    }

fallback:
    {
        uint64_t da = Memory::read<uint64_t>(hover_base + 0x20);
        if (!da || !Memory::IsValidPtrFast2(da))
            return "";

        std::string name = steam_decrypt::GetActorFNameString(da);
        if (const std::string fromAsset = LookupByAssetName(name); !fromAsset.empty())
            return accept(fromAsset);
        if (const std::string fromWorld = LookupWorldObjectByFName(name); !fromWorld.empty())
            return accept(fromWorld);
        for (const char* p : { "DA_", "WID_", "BP_", "Item_" }) {
            if (name.find(p) == 0) { name.erase(0, strlen(p)); break; }
        }
        for (auto& c : name) if (c == '_') c = ' ';
        if (!name.empty() && name.find("Default__") == std::string::npos) {
            if (const std::string human = HumanizeActorFName(name); !human.empty())
                return accept(human);
            return accept(name);
        }
    }

    return "";
}

std::string TryReadEnglishItemNameFromHover(uint64_t actor, std::ptrdiff_t hover_off)
{
    return ReadHoverDisplayNameAt(actor + static_cast<uint64_t>(hover_off));
}

std::string TryResolveLootObjectDisplay(uintptr_t object)
{
    if (!object || !Memory::IsValidPtrFast2(object))
        return "";

    for (const std::ptrdiff_t hover_off : {
             Offsets::UIHoverData,
             Offsets::UIHoverData_Pickup,
         }) {
        const std::string fromHover = TryReadEnglishItemNameFromHover(object, hover_off);
        if (!fromHover.empty() && !IsGenericWorldEspLabel(fromHover))
            return fromHover;
    }

    std::string name = steam_decrypt::GetActorFNameString(object);
    if (name.empty() || FnameLooksLikeEngineSubobjectClass(name))
        return "";

    if (const std::string fromAsset = LookupByAssetName(name); !fromAsset.empty()
        && !IsGenericWorldEspLabel(fromAsset))
        return fromAsset;
    if (const std::string fromWorld = LookupWorldObjectByFName(name); !fromWorld.empty()
        && !IsGenericWorldEspLabel(fromWorld))
        return fromWorld;
    if (const std::string human = HumanizeActorFName(name); !human.empty()
        && !IsGenericWorldEspLabel(human))
        return human;

    return "";
}

} // namespace

std::string GetActorDataAssetFName(uint64_t actor)
{
    if (!actor)
        return "";

    extern Engine engine;

    auto acceptAssetName = [&](const std::string& name) -> bool {
        if (name.empty())
            return false;
        std::string lower = name;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Cosmetic outfit DAs (DA_OI_Outfit_*) are not floor-loot names — reading
        // them produced "OI Outfit Agile Astronaut Color White" for Air Freshener.
        if (lower.find("da_oi_outfit") != std::string::npos
            || lower.find("oi_outfit") != std::string::npos
            || lower.find("_outfit_") != std::string::npos
            || lower.find("outfit_agile") != std::string::npos
            || lower.find("outfit_abandoned") != std::string::npos)
            return false;
        if (lower.find("da_item") != std::string::npos
            || lower.find("wid_") != std::string::npos
            || lower.find("bp_pickup") != std::string::npos)
            return true;
        if (name.rfind("DA_", 0) == 0
            || name.rfind("WID_", 0) == 0
            || name.rfind("BP_", 0) == 0)
            return true;
        return !LookupByAssetName(name).empty();
    };

    auto readDaName = [&](uint64_t da) -> std::string {
        if (!da || !Memory::IsValidPtrFast2(da))
            return {};
        std::string name = engine.GetActorFNameStringCached(da);
        if (name.empty())
            name = engine.GetActorFNameString(da);
        if (name.empty())
            name = steam_decrypt::GetActorFNameString(da);
        if (acceptAssetName(name))
            return name;
        return {};
    };

    if (const std::string fromItemDa = readDaName(
            Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::ItemDataAsset)));
        !fromItemDa.empty())
        return fromItemDa;

    if (const std::string fromPickupDa = readDaName(
            Memory::read<uint64_t>(
                actor + static_cast<uint64_t>(Offsets::Pickup_DefaultPickupDataAsset)));
        !fromPickupDa.empty())
        return fromPickupDa;

    static const std::ptrdiff_t kHoverOffsets[] = {
        Offsets::UIHoverData,
        Offsets::UIHoverData_Pickup,
    };

    for (const std::ptrdiff_t hover_off : kHoverOffsets) {
        const uint64_t hover_base = actor + static_cast<uint64_t>(hover_off);
        const uint64_t da = Memory::read<uint64_t>(hover_base + 0x20);
        if (const std::string assetName = readDaName(da); !assetName.empty())
            return assetName;
    }

    std::string actorFname = engine.GetActorFNameStringCached(actor);
    if (actorFname.empty())
        actorFname = engine.GetActorFNameString(actor);
    if (actorFname.empty())
        actorFname = steam_decrypt::GetActorFNameString(actor);
    if (acceptAssetName(actorFname))
        return actorFname;

    return "";
}

std::string Engine::GetEnglishItemName(uint64_t actor)
{
    if (!actor)
        return "";

    auto polish = [](std::string s) -> std::string {
        if (s.empty() || IsGenericWorldEspLabel(s))
            return {};
        const std::string raw = s;
        s = FormatEspDisplayLabel(s);
        if (s.empty() || IsJunkWorldEspLabel(s) || IsGarbledEspLabel(s)
            || !IsPlausibleEspLabel(s))
            return {};
        // Vocabulary gate: a genuine hover name exists in the game's own
        // loc/asset tables. FName/string decrypt sludge can pass every shape
        // check above but never matches — reject it so the CSV chain or the
        // next pass resolves the real name instead ("random item name" fix).
        if (!IsKnownItemDisplayName(s) && !IsKnownItemDisplayName(raw))
            return {};
        return s;
    };

    static const std::ptrdiff_t kHoverOffsets[] = {
        Offsets::UIHoverData,
        Offsets::UIHoverData_Pickup,
    };

    for (const std::ptrdiff_t hover_off : kHoverOffsets) {
        const std::string fromHover = TryReadEnglishItemNameFromHover(actor, hover_off);
        if (!fromHover.empty()) {
            if (const std::string polished = polish(fromHover); !polished.empty())
                return polished;
        }
    }

    const uintptr_t lootCompDefault =
        Memory::read<uintptr_t>(actor + Offsets::LootInteractionComponent);
    const uintptr_t lootCompContainer =
        Memory::read<uintptr_t>(actor + Offsets::LootInteraction_Container);
    const uintptr_t lootCompSimple =
        Memory::read<uintptr_t>(actor + Offsets::SimpleLootActivity_LootInteraction);
    const uintptr_t lootCompItemContainer =
        Memory::read<uintptr_t>(actor + Offsets::SimpleLootActivity_ItemContainer);

    const uintptr_t lootComps[] = {
        lootCompDefault,
        lootCompContainer,
        lootCompSimple,
        lootCompItemContainer,
    };
    for (const uintptr_t lootComp : lootComps) {
        if (!lootComp || !Memory::IsValidPtrFast2(lootComp))
            continue;

        if (const std::string fromComp = TryResolveLootObjectDisplay(lootComp); !fromComp.empty()) {
            if (const std::string polished = polish(fromComp); !polished.empty())
                return polished;
        }

        const uint64_t linked = Memory::read<uint64_t>(lootComp + 0x20);
        if (linked && Memory::IsValidPtrFast2(linked)) {
            if (const std::string fromLinked = TryResolveLootObjectDisplay(linked); !fromLinked.empty()) {
                if (const std::string polished = polish(fromLinked); !polished.empty())
                    return polished;
            }
        }
    }

    return "";
}

// ---- crate contents preview (see Core/CrateContents.hpp) --------------------
//
// Best-effort walk of APickup::SpawnItems: each element is either an inline
// FItemUIHoverData (ItemUIHoverData_Size bytes) or a pointer to one. Every read
// is sanity-gated - a miss skips the stack and the crate just shows its plain
// label, never garbage.

// Rarity through the pickup resolution chain (CL ~1315578): data asset ->
// resolved item class (PickupDataAsset::RESOLVED_ITEM_CLASS) -> CDO
// (UClass::DEFAULT_OBJECT) -> ItemBase::QUALITY_LEVEL (0-3). Every hop is
// pointer-gated, so a wrong or moved slot reads as -1 (unresolved), never as a
// garbage rarity.
static int RarityFromResolvedClass(uintptr_t dataAsset)
{
    if (!IsUsableObjectPtr(dataAsset))
        return -1;
    const uintptr_t cls = Memory::read_nocache<uintptr_t>(
        dataAsset + Offsets::PickupDataAsset_ResolvedItemClass);
    if (!IsUsableObjectPtr(cls))
        return -1;
    const uintptr_t cdo = Memory::read_nocache<uintptr_t>(
        cls + Offsets::UClass_DefaultObjectSlot);
    if (!IsUsableObjectPtr(cdo))
        return -1;
    const int32_t quality = Memory::read_nocache<int32_t>(
        cdo + Offsets::ItemBase_Quality);
    return (quality >= 0 && quality <= 3) ? quality : -1;
}
struct CrateTArrayHdr {
    uintptr_t data = 0;
    int32_t num = 0;
    int32_t max = 0;
};

static bool PlausibleCrateArray(const CrateTArrayHdr& a)
{
    return a.num > 0 && a.num <= 24 && a.max >= a.num
        && a.max <= 256 && Engine::IsPlausibleUsermodePtr(a.data);
}

static std::string ResolveCrateDataAssetName(uintptr_t dataAsset)
{
    if (!IsUsableObjectPtr(dataAsset))
        return {};

    std::string raw = engine.GetActorFNameStringCached(dataAsset);
    if (raw.empty())
        raw = steam_decrypt::GetActorFNameString(dataAsset);
    if (raw.empty())
        return {};

    std::string resolved = LookupByAssetName(raw);
    if (resolved.empty())
        resolved = LookupWorldObjectByFName(raw);
    if (resolved.empty()) {
        for (const char* prefix : { "DA_", "WID_", "BP_", "Item_" }) {
            if (raw.rfind(prefix, 0) == 0) {
                raw.erase(0, std::strlen(prefix));
                break;
            }
        }
        for (char& c : raw) {
            if (c == '_')
                c = ' ';
        }
        resolved = raw;
    }

    resolved = FormatEspDisplayLabel(resolved);
    if (resolved.empty() || IsJunkWorldEspLabel(resolved)
        || IsGarbledEspLabel(resolved) || !IsPlausibleEspLabel(resolved))
        return {};
    return resolved;
}

static int PlausibleCrateAmount(uintptr_t element, uintptr_t stride)
{
    // FItemContainerItem is not fully reflected in the adopted SDK. Prefer the
    // old hover slots first, then accept only small, non-negative stack values
    // from the reflected item record. The default keeps the item name useful
    // even when the quantity field is not exposed by the current dump.
    for (uintptr_t off : { 0x18ull, 0x1Cull, 0x10ull, 0x08ull,
                           0x20ull, 0x28ull, 0x30ull }) {
        if (off + sizeof(int32_t) > stride)
            continue;
        const int32_t value = Memory::read_nocache<int32_t>(element + off);
        if (value > 0 && value <= CrateContents::kMaxAmount)
            return value;
    }
    for (uintptr_t off = 0; off + sizeof(int32_t) <= stride; off += 4) {
        const int32_t value = Memory::read_nocache<int32_t>(element + off);
        if (value > 0 && value <= 99)
            return value;
    }
    return 1;
}

static bool ReadCrateItemElement(
    uintptr_t element, uintptr_t stride, CrateContents::Stack& out)
{
    static constexpr uintptr_t kAssetOffsets[] = {
        0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30,
        0x38, 0x40, 0x48, 0x50, 0x58, 0x60
    };
    for (const uintptr_t off : kAssetOffsets) {
        if (off + sizeof(uintptr_t) > stride)
            continue;
        const uintptr_t dataAsset =
            Memory::read_nocache<uintptr_t>(element + off);
        if (!IsUsableObjectPtr(dataAsset))
            continue;
        const std::string name = ResolveCrateDataAssetName(dataAsset);
        if (name.empty())
            continue;
        out.name = name;
        out.amount = PlausibleCrateAmount(element, stride);
        out.maxStack = 0;
        out.rarity = RarityFromResolvedClass(dataAsset);
        return true;
    }
    return false;
}

static int ReadCrateArray(
    const CrateTArrayHdr& array, CrateContents::Stack* out, int cap)
{
    // Current SDK: FItemContainerItem is 0x70 bytes. Keep the two hover layouts
    // as compatibility fallbacks for pickup actors and older live containers.
    static constexpr uintptr_t kStrides[] = { 0x70, 0x28, 0x10 };
    int count = 0;
    std::unordered_set<std::string> seen;
    for (const uintptr_t stride : kStrides) {
        for (int32_t i = 0; i < array.num && count < cap; ++i) {
            const uintptr_t element = array.data
                + static_cast<uintptr_t>(i) * stride;
            CrateContents::Stack st;
            if (!ReadCrateItemElement(element, stride, st)
                || !seen.insert(st.name).second)
                continue;
            out[count++] = std::move(st);
        }
        if (count > 0)
            break;
    }
    return count;
}

static int ReadCrateContentsFromComponent(
    uintptr_t component, CrateContents::Stack* out, int cap)
{
    if (!IsUsableObjectPtr(component))
        return 0;

    // UItemContainerComponent is 0x520 bytes in the adopted SDK, but the
    // content array is not emitted as a named property. Probe aligned TArray
    // headers inside the component and accept only arrays whose elements yield
    // a real item asset name. This avoids guessing another raw offset.
    for (uintptr_t off = 0; off < 0x500; off += sizeof(uintptr_t)) {
        const CrateTArrayHdr array = Memory::read_nocache<CrateTArrayHdr>(
            component + off);
        if (!PlausibleCrateArray(array))
            continue;
        const int count = ReadCrateArray(array, out, cap);
        if (count > 0)
            return count;
    }
    return 0;
}

static void AddCrateItemContainerCandidate(
    uintptr_t actor, uintptr_t candidate,
    std::unordered_set<uintptr_t>& seen,
    std::vector<uintptr_t>& components)
{
    if (!IsUsableObjectPtr(candidate) || !seen.insert(candidate).second)
        return;
    const std::string cls = engine.GetActorClassFName(candidate);
    const std::string lower = ToLowerCopy(cls);
    if (lower.find("itemcontainercomponent") != std::string::npos
        || engine.ResolveOwningActor(candidate, 3) == actor)
        components.push_back(candidate);
}

static int ReadCrateContentsFromItemContainer(
    uintptr_t actor, CrateContents::Stack* out, int cap)
{
    std::unordered_set<uintptr_t> seen;
    std::vector<uintptr_t> components;
    AddCrateItemContainerCandidate(actor,
        Memory::read_nocache<uintptr_t>(actor + Offsets::LootContainer_ItemContainer),
        seen, components);
    // 0xBC0 is the current SDK slot; retain the runtime-profile 0xBD8 above as
    // the primary candidate because both layouts have existed across drops.
    AddCrateItemContainerCandidate(actor,
        Memory::read_nocache<uintptr_t>(actor + 0xBC0), seen, components);
    AddCrateItemContainerCandidate(actor,
        Memory::read_nocache<uintptr_t>(actor + Offsets::SimpleLootActivity_ItemContainer),
        seen, components);

    const CrateTArrayHdr instance = Memory::read_nocache<CrateTArrayHdr>(
        actor + Offsets::Actor_InstanceComponents);
    if (instance.num > 0 && instance.num <= 64
        && instance.max >= instance.num && instance.max <= 64
        && Engine::IsPlausibleUsermodePtr(instance.data)) {
        for (int32_t i = 0; i < instance.num; ++i) {
            AddCrateItemContainerCandidate(actor,
                Memory::read_nocache<uintptr_t>(
                    instance.data + static_cast<uintptr_t>(i) * sizeof(uintptr_t)),
                seen, components);
        }
    }

    for (const uintptr_t component : components) {
        const int count = ReadCrateContentsFromComponent(component, out, cap);
        if (count > 0)
            return count;
    }
    return 0;
}

int Engine::ReadCrateContents(uintptr_t actor, CrateContents::Stack* out, int cap)
{
    if (!actor || !out || cap <= 0 || !Memory::IsValidPtrFast2(actor))
        return 0;

    const CrateTArrayHdr items = Memory::read_nocache<CrateTArrayHdr>(
        actor + static_cast<uint64_t>(Offsets::BP_PickupBase_SpawnItems));
    if (PlausibleCrateArray(items)) {
        int count = 0;
        bool usedVisibleFallback = false;
        for (int32_t i = 0; i < items.num && count < cap; ++i) {
            // Layout (a): an inline FItemUIHoverData at element stride.
            uint64_t hover = items.data
                + static_cast<uint64_t>(i) * Offsets::ItemUIHoverData_Size;
            uint64_t da = Memory::read<uint64_t>(
                hover + static_cast<uint64_t>(Offsets::ItemUIHoverData_DataAsset));
            if (!Engine::IsPlausibleUsermodePtr(da)) {
                // Layout (b): the element is a pointer to the hover struct.
                hover = Memory::read<uint64_t>(
                    items.data + static_cast<uint64_t>(i) * sizeof(uint64_t));
                if (!Engine::IsPlausibleUsermodePtr(hover))
                    continue;
                da = Memory::read<uint64_t>(
                    hover + static_cast<uint64_t>(Offsets::ItemUIHoverData_DataAsset));
                if (!Engine::IsPlausibleUsermodePtr(da))
                    continue;
            }

            CrateContents::Stack st;
            st.amount = Memory::read<int32_t>(
                hover + static_cast<uint64_t>(Offsets::ItemUIHoverData_Amount));
            st.maxStack = Memory::read<int32_t>(
                hover + static_cast<uint64_t>(Offsets::ItemUIHoverData_MaxStack));
            st.rarity = RarityFromResolvedClass(da);
            if (!CrateContents::AmountPlausible(st.amount, st.maxStack)) {
                if (usedVisibleFallback)
                    continue;
                const int32_t visible = Memory::read<int32_t>(
                    actor + static_cast<uint64_t>(Offsets::Pickup_VisibleAmount));
                if (visible <= 0 || !CrateContents::AmountPlausible(visible, 0))
                    continue;
                usedVisibleFallback = true;
                st.amount = visible;
                st.maxStack = 0;
            }

            st.name = ReadHoverDisplayNameAt(hover);
            if (st.name.empty())
                st.name = ResolveCrateDataAssetName(da);
            if (st.name.empty() || IsJunkWorldEspLabel(st.name)
                || IsGarbledEspLabel(st.name))
                continue;
            out[count++] = std::move(st);
        }
        if (count > 0)
            return count;
    }

    // ALootContainerSingle does not inherit APickup::SpawnItems. Its content
    // lives on the owned UItemContainerComponent, so the old path above can
    // only ever describe ground loot. Resolve the component structurally here.
    return ReadCrateContentsFromItemContainer(actor, out, cap);
}

// ---- game offsets -----------------------------------------------------------

std::string Engine::GetActorFNameString(uint64_t actor_base)
{
    return steam_decrypt::GetActorFNameString(actor_base);
}

int32_t Engine::GetActorFNameId(uint64_t actor_base)
{
    return steam_decrypt::GetActorFNameId(actor_base);
}

std::string Engine::GetActorFNameStringCached(uintptr_t actor_base)
{
    if (!IsValidPointer(actor_base))
        return "";

    if (!g_fnameTablesReady) {
        if (!InitConsts())
            return GetActorFNameString(actor_base);
    }

    // Single cache layer: steam_decrypt::CachedNameString (by comp_index).
    return GetActorFNameString(actor_base);
}

std::string Engine::GetActorClassFName(uintptr_t actor_base)
{
    if (!IsValidPointer(actor_base))
        return "";

    InitConsts();

    auto readClassNameFromPtr = [&](uintptr_t class_ptr) -> std::string {
        if (!class_ptr || !IsValidPointer(class_ptr))
            return {};

        std::string name = GetActorFNameStringCached(class_ptr);
        if (name.empty())
            name = GetActorFNameString(class_ptr);
        if (name.empty())
            name = steam_decrypt::GetActorFNameString(class_ptr);
        if (!name.empty() && steam_decrypt::IsPlausibleFNameText(name)
            && !FnameLooksLikeEngineSubobjectClass(name))
            return name;
        return {};
    };

    if (const uintptr_t class_ptr = steam_decrypt::GetActorClassPtr(actor_base)) {
        if (std::string name = readClassNameFromPtr(class_ptr); !name.empty())
            return name;
    }

    static const std::ptrdiff_t kClassOffsets[] = { 0x10, 0x8 };
    for (const std::ptrdiff_t off : kClassOffsets) {
        const uint64_t uclass =
            Memory::read<uint64_t>(actor_base + static_cast<uint64_t>(off));
        if (!uclass || !Memory::IsValidPtrFast2(uclass))
            continue;
        if (std::string name = readClassNameFromPtr(static_cast<uintptr_t>(uclass));
            !name.empty())
            return name;
    }
    return {};
}

void Engine::ClearFNameCache()
{
    FNameCache::Instance().Clear();
    steam_decrypt::ClearNameCache();
}

bool Engine::InitConsts()
{
    if (g_fnameTablesReady)
        return true;

    const uint64_t base = Memory::getBaseAddress();
    if (!base)
        return false;

    steam_decrypt::ResetTables();
    ClearFNameCache();

    if (!steam_decrypt::InitTables(base))
        return false;

    g_fnameTablesReady = true;
    return true;
}

uintptr_t Engine::ResolveInventoryPtr(uintptr_t raw)
{
    if (!raw)
        return 0;
    if (Memory::IsValidPtrFast2(raw))
        return raw;
    return 0;
}

void Engine::ReadPlayerInventory(uintptr_t pawn, std::string& outWeaponName, int& outWeaponQuality,
    int& outWeaponClip,
    std::string& outStowed0, int& outStowedQ0,
    std::string& outStowed1, int& outStowedQ1,
    float& outArmorPlates, float& outArmorPerPlate,
    int& outArmorTier, std::string& outArmorName,
    bool* outResolved)
{
    if (outResolved)
        *outResolved = false;
    outWeaponName.clear();
    outWeaponQuality = -1;
    outWeaponClip = 0;
    outStowed0.clear();
    outStowedQ0 = -1;
    outStowed1.clear();
    outStowedQ1 = -1;
    outArmorPlates = 0.f;
    outArmorPerPlate = 0.f;
    outArmorTier = -1;
    outArmorName.clear();

    if (!pawn)
        return;

    const uintptr_t invRaw = Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    const uintptr_t invComp = ResolveInventoryPtr(invRaw);
    if (!invComp)
        return;
    if (outResolved)
        *outResolved = true;

    // Stowed slot 0 @ +0x330 (FInventoryStowedWeaponActor, SDK CL-1341255)
    StowedWeaponInfo slot0 = Memory::read<StowedWeaponInfo>(invComp + Offsets::StowedWeaponSlot0);
    if (slot0.WeaponVisual && Memory::IsValidPtrFast2(slot0.WeaponVisual)) {
        std::string name = GetActorFNameString(slot0.WeaponVisual);
        if (!name.empty()) {
            const std::string stripped = StripWeaponAssetName(name);
            outStowed0 = GetWeaponName(stripped.empty() ? name : stripped);
        }
        if (slot0.WeaponQuality >= 0 && slot0.WeaponQuality <= 3)
            outStowedQ0 = slot0.WeaponQuality + 1;  // 0-3 → tier I-IV = 1-4
    }

    // Stowed slot 1 @ +0x370
    StowedWeaponInfo slot1 = Memory::read<StowedWeaponInfo>(invComp + Offsets::StowedWeaponSlot1);
    if (slot1.WeaponVisual && Memory::IsValidPtrFast2(slot1.WeaponVisual)) {
        std::string name = GetActorFNameString(slot1.WeaponVisual);
        if (!name.empty()) {
            const std::string stripped = StripWeaponAssetName(name);
            outStowed1 = GetWeaponName(stripped.empty() ? name : stripped);
        }
        if (slot1.WeaponQuality >= 0 && slot1.WeaponQuality <= 3)
            outStowedQ1 = slot1.WeaponQuality + 1;
    }

    // Primary: iterate CurrentItemActors TArray @ 0x4B0 (SDK CL-1341255).
    {
        auto tryItemArray = [&](std::ptrdiff_t arrOff) {
            const uint64_t data = Memory::read<uint64_t>(invComp + arrOff);
            const int32_t count = Memory::read<int32_t>(invComp + arrOff + 0x8);
            if (!data || count <= 0 || count > 64)
                return;
            for (int32_t i = 0; i < count; ++i) {
                const uintptr_t itemActor = Memory::read<uintptr_t>(
                    static_cast<uintptr_t>(data) + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                const uintptr_t resolved = ResolveInventoryPtr(itemActor);
                if (!resolved)
                    continue;
                std::string nm = GetActorFNameString(resolved);
                if (nm.empty())
                    nm = GetActorClassFName(resolved);
                if (nm.empty())
                    continue;
                std::string stripped = StripWeaponAssetName(nm);
                const std::string friendly = GetWeaponName(stripped.empty() ? nm : stripped);
                if (!IsPlayerWeaponEspLabel(friendly))
                    continue;
                outWeaponName = friendly;
                const int q = GetWeaponQualityFromActor(resolved);
                if (q >= 0 && q <= 3)
                    outWeaponQuality = q + 1;
                const uint16_t clip = Memory::read<uint16_t>(
                    static_cast<uintptr_t>(resolved) + Offsets::WeaponClip);
                if (clip > 0 && clip < 500)
                    outWeaponClip = static_cast<int>(clip);
                return;
            }
        };
        tryItemArray(Offsets::CurrentItemActors);
        if (!IsPlayerWeaponEspLabel(outWeaponName))
            tryItemArray(Offsets::LocalCurrentItemActors);
    }

    // Fallback name: resolve the actually-held weapon actor (replicated slot
    // first, then a level-actor Instigator/Owner scan for remotes) when the
    // TArray path came up empty. NOTE: the old "EquippedPrimaryItem @ 0x518"
    // fallback was wrong — 0x518 is EquippedArmor (SDK CL-1341255), an armor
    // UItemBase*, never a weapon; it fed armor pointers into weapon name/clip
    // reads (audit #36).
    if (outWeaponName.empty()) {
        const uintptr_t held = WorldScan::ResolvePreferredHeldItemActor(pawn);
        if (held) {
            std::string nm = GetActorFNameString(held);
            if (nm.empty())
                nm = GetActorClassFName(held);
            if (!nm.empty()) {
                const std::string stripped = StripWeaponAssetName(nm);
                const std::string friendly =
                    GetWeaponName(stripped.empty() ? nm : stripped);
                if (IsPlayerWeaponEspLabel(friendly))
                    outWeaponName = friendly;
            }
        }
    }

    // Drop blueprints/recipes from stowed + equipped; promote a real stowed gun
    // when primary is empty so ESP never shows "Unarmed" beside "Kettle".
    if (!IsPlayerWeaponEspLabel(outStowed0)) {
        outStowed0.clear();
        outStowedQ0 = -1;
    }
    if (!IsPlayerWeaponEspLabel(outStowed1)) {
        outStowed1.clear();
        outStowedQ1 = -1;
    }
    if (!IsPlayerWeaponEspLabel(outWeaponName)) {
        outWeaponName.clear();
        outWeaponQuality = -1;
        if (!outStowed0.empty()) {
            outWeaponName = outStowed0;
            outWeaponQuality = outStowedQ0;
            outWeaponClip = 0;   // re-read from the held actor below
            outStowed0.clear();
            outStowedQ0 = -1;
        } else if (!outStowed1.empty()) {
            outWeaponName = outStowed1;
            outWeaponQuality = outStowedQ1;
            outWeaponClip = 0;   // re-read from the held actor below
            outStowed1.clear();
            outStowedQ1 = -1;
        }
    }

    // Match equipped name to stowed slots for quality
    if (!outWeaponName.empty()) {
        if (outWeaponName == outStowed0) outWeaponQuality = outStowedQ0;
        else if (outWeaponName == outStowed1) outWeaponQuality = outStowedQ1;
    }
    // Avoid duplicating the active gun in the stowed list.
    if (!outWeaponName.empty() && outWeaponName == outStowed0) {
        outStowed0.clear();
        outStowedQ0 = -1;
    }
    if (!outWeaponName.empty() && outWeaponName == outStowed1) {
        outStowed1.clear();
        outStowedQ1 = -1;
    }
    // If no quality/clip from stowed, try direct read from the held weapon actor.
    if (outWeaponQuality < 0 || outWeaponClip <= 0) {
        const uintptr_t held = WorldScan::ResolvePreferredHeldItemActor(pawn);
        if (held) {
            outWeaponQuality = GetWeaponQualityFromActor(held);
            if (outWeaponQuality >= 0 && outWeaponQuality <= 3)
                outWeaponQuality += 1;  // 0-3 → tier I-IV
            else if (outWeaponQuality == 4)
                outWeaponQuality = 4;
            // Read ammo clip from held weapon actor fallback
            if (outWeaponClip <= 0) {
                const uint16_t clip = Memory::read<uint16_t>(
                    static_cast<uintptr_t>(held) + Offsets::WeaponClip);
                if (clip > 0 && clip < 500)
                    outWeaponClip = static_cast<int>(clip);
            }
        }
    }

    // Read armor from EquippedArmor at +0x518
    const uintptr_t armorRaw = Memory::read<uintptr_t>(invComp + Offsets::EquippedArmor);
    const uintptr_t armorItem = ResolveInventoryPtr(armorRaw);
    if (armorItem) {
        outArmorPlates = static_cast<float>(Memory::read<int32_t>(armorItem + 0x264));
        outArmorPerPlate = static_cast<float>(Memory::read<float>(armorItem + 0x268));
        // Armor tier + name for the loadout readout: the armor UItemBase* carries
        // the same quality byte weapon actors use (0-3 = I-IV after the shift).
        const int armorQ = GetWeaponQualityFromActor(armorItem);
        if (armorQ >= 0 && armorQ <= 3)
            outArmorTier = armorQ + 1;
        outArmorName = GetEnglishItemName(armorItem);
    }
}

// Full kit readout (phase 2): the inventory slots the loadout line does not
// cover - stowed tool actor, safe pouch item (+ rarity), belt/backpack slot
// capacity. Best-effort: every miss just leaves the KitParts segment empty.
void Engine::ReadPlayerKit(uintptr_t pawn, LoadoutFormat::KitParts& outKit)
{
    if (!pawn)
        return;
    const uintptr_t invRaw = Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    const uintptr_t invComp = ResolveInventoryPtr(invRaw);
    if (!invComp)
        return;

    // Stowed tool slot (InventoryComponent::STOWED_TOOL_ACTOR).
    const uintptr_t tool = ResolveInventoryPtr(Memory::read<uintptr_t>(
        invComp + Offsets::Inventory_StowedToolActor));
    if (tool) {
        std::string name = GetEnglishItemName(tool);
        if (name.empty())
            name = StripWeaponAssetName(GetActorFNameString(tool));
        if (!name.empty())
            outKit.tool = std::move(name);
    }

    // Safe pouch item (UItemBase*) + EItemRarity (ItemBase::QUALITY_LEVEL).
    const uintptr_t pouch = ResolveInventoryPtr(Memory::read<uintptr_t>(
        invComp + Offsets::Inventory_SafePouch));
    if (pouch) {
        outKit.pouch = GetEnglishItemName(pouch);
        const int32_t quality = Memory::read_nocache<int32_t>(
            pouch + Offsets::ItemBase_Quality);
        if (quality >= 0 && quality <= 3)
            outKit.pouchRarity = quality;
    }

    // Belt / backpack containers: UItemContainer::ItemLimit is the slot
    // capacity ("how big is their bag" intel from across the room).
    const uintptr_t belt = ResolveInventoryPtr(Memory::read<uintptr_t>(
        invComp + Offsets::Inventory_Belt));
    if (belt) {
        const int32_t limit = Memory::read_nocache<int32_t>(
            belt + Offsets::ItemContainer_ItemLimit);
        if (limit >= 1 && limit <= 64)
            outKit.beltSlots = limit;
    }
    const uintptr_t pack = ResolveInventoryPtr(Memory::read<uintptr_t>(
        invComp + Offsets::Inventory_Backpack));
    if (pack) {
        const int32_t limit = Memory::read_nocache<int32_t>(
            pack + Offsets::ItemContainer_ItemLimit);
        if (limit >= 1 && limit <= 64)
            outKit.packSlots = limit;
    }
}

// ---- Player intel (DBNO / look arrows / identity) -----------------------------

bool Engine::ReadPlayerDbnoState(uintptr_t playerState, bool& outBrokenArmor,
    bool* outResolved)
{
    outBrokenArmor = false;
    if (outResolved)
        *outResolved = false;
    if (!IsUsableObjectPtr(playerState))
        return false;
    // FPlayerHealthInfo is inline on PioneerPlayerState and the sources
    // disagree on its base (0x550 probed vs 0x588 drop). Accept the first
    // block whose (health, max) double pair is sane before trusting its flag
    // bytes, so a mis-anchored read can never report "downed".
    static const std::ptrdiff_t kBases[] = {
        Offsets::PlayerHealthInfoBase, Offsets::HealthInfo };
    for (const std::ptrdiff_t baseOff : kBases) {
        const uintptr_t blk = playerState + baseOff;
        const double maxHp =
            Memory::read_nocache<double>(blk + Offsets::PHI_MaxHealth);
        const double hp = Memory::read_nocache<double>(blk + Offsets::PHI_Health);
        if (!(maxHp >= 1.0 && maxHp <= 5000.0))
            continue;
        if (!(hp >= -1.0 && hp <= maxHp + 250.0))
            continue;
        outBrokenArmor =
            0 != Memory::read_nocache<uint8_t>(blk + Offsets::PHI_BrokenArmorByte);
        if (outResolved)
            *outResolved = true;
        return 0 != Memory::read_nocache<uint8_t>(blk + Offsets::PHI_DBNOByte);
    }
    return false;
}

bool Engine::ReadReviveTimer(uintptr_t pawn, float& outElapsed, float& outTotal)
{
    outElapsed = 0.f;
    outTotal = 0.f;
    if (!IsUsableObjectPtr(pawn))
        return false;
    const uintptr_t comps =
        Memory::read_nocache<uintptr_t>(pawn + Offsets::Actor_InstanceComponents);
    const int32_t count = Memory::read_nocache<int32_t>(
        pawn + Offsets::Actor_InstanceComponents + 0x8);
    if (!IsUsableObjectPtr(comps) || count <= 0 || count > 96)
        return false;
    for (int32_t i = 0; i < count; ++i) {
        const uintptr_t comp = Memory::read_nocache<uintptr_t>(
            comps + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
        if (!IsUsableObjectPtr(comp))
            continue;
        if (Memory::read_nocache<uint8_t>(
                comp + Offsets::Interaction_CurrentInteractionState) > 16)
            continue;
        // Defib (revive) window first, bleedout window second. The pair order
        // is not documented, so both orders run the plausibility gate.
        for (const std::ptrdiff_t timerOff :
             { Offsets::Interact_DefibTimerFloats, Offsets::Interact_DBNOTimerFloats }) {
            const float a = Memory::read_nocache<float>(comp + timerOff);
            const float b = Memory::read_nocache<float>(comp + timerOff + 0x4);
            if (ReviveBadge::RemainSeconds(a, b) >= 0.f) {
                outElapsed = a;
                outTotal = b;
                return true;
            }
            if (ReviveBadge::RemainSeconds(b, a) >= 0.f) {
                outElapsed = b;
                outTotal = a;
                return true;
            }
        }
    }
    return false;
}

uint64_t Engine::ReadPlayerSteamId(uintptr_t playerState, bool* outResolved)
{
    if (outResolved)
        *outResolved = false;
    if (!IsUsableObjectPtr(playerState))
        return 0;
    const uintptr_t comp = Memory::read_nocache<uintptr_t>(
        playerState + Offsets::PS_PlatformIdComponent);
    if (!IsUsableObjectPtr(comp))
        return 0;
    if (outResolved)
        *outResolved = true;
    const uintptr_t repl = comp + Offsets::PlatformId_Repl;
    // FUniqueNetIdRepl TVariant: type index 0 is the shared-ptr alternative
    // (walkable); anything else is the inline FAccountId, which replicates as
    // TArray<uint8> ReplicationBytes holding the decimal account id (the drop:
    // usually empty on PC clients, so this rarely fires). Digit-gated so
    // garbage bytes can never mint an id.
    if (0 != Memory::read_nocache<uint8_t>(repl + Offsets::NetIdRepl_TypeIndex)) {
        const uintptr_t data = Memory::read_nocache<uintptr_t>(
            repl + Offsets::NetIdRepl_ReplBytesData);
        const int32_t num = Memory::read_nocache<int32_t>(
            repl + Offsets::NetIdRepl_ReplBytesCount);
        if (!IsUsableObjectPtr(data) || num < 5 || num > 20)
            return 0;
        uint8_t digits[20] = {};
        for (int32_t i = 0; i < num; ++i)
            digits[i] = Memory::read_nocache<uint8_t>(
                data + static_cast<uintptr_t>(i));
        return SquadRoster::ParseDecimalId(digits, num);
    }
    const uintptr_t netId =
        Memory::read_nocache<uintptr_t>(repl + Offsets::NetIdRepl_Object);
    if (!IsUsableObjectPtr(netId))
        return 0;
    const uint64_t payload =
        Memory::read_nocache<uint64_t>(netId + Offsets::NetId_Payload);
    return SquadRoster::PlausibleSteamId(payload) ? payload : 0;
}

// PlayerState status bits (phase 2): 1=bot, 2=spectator, 4=finished round.
// The PioneerPlayerState extension block is only trusted when its achievement
// component slot reads as null-or-valid - a wrong anchor must mean "no tags",
// never "[Bot]" on a real player.
uint8_t Engine::ReadPlayerStatusTags(uintptr_t playerState)
{
    if (!IsUsableObjectPtr(playerState))
        return 0;
    const uintptr_t achieve = Memory::read_nocache<uintptr_t>(
        playerState + Offsets::PS_AchievementComponent);
    if (achieve != 0 && !IsUsableObjectPtr(achieve))
        return 0;
    uint8_t flags = 0;
    const uint8_t botByte = Memory::read_nocache<uint8_t>(
        playerState + Offsets::PS_BotStateByte);
    if ((botByte & Offsets::PS_BotStateBotMask) != 0)
        flags |= 1u;
    if ((botByte & Offsets::PS_BotStateSpectatorMask) != 0)
        flags |= 2u;
    const uint8_t finished = Memory::read_nocache<uint8_t>(
        playerState + Offsets::PS_FinishedRoundByte);
    if ((finished & Offsets::PS_FinishedRoundMask) != 0)
        flags |= 4u;
    return flags;
}

// Item rarity (0-3) for a ground pickup through the resolution chain, or -1
// when the chain does not resolve.
int Engine::ReadItemRarityFromPickup(uintptr_t actor)
{
    if (!IsUsableObjectPtr(actor))
        return -1;
    const uintptr_t dataAsset = Memory::read_nocache<uintptr_t>(
        actor + Offsets::Pickup_DefaultDataAsset);
    return RarityFromResolvedClass(dataAsset);
}

// Container dispenser state (phase 2): how many dispenser drop ports the
// container ejects loot from (0 = none or unresolved), plus whether its
// socket-loot mesh (LootContainerSingle::SOCKET_LOOT_CONTAINER_MESH) resolved.
int Engine::ReadContainerDispenserPorts(uintptr_t actor, bool& outSocketMeshOk)
{
    outSocketMeshOk = false;
    if (!IsUsableObjectPtr(actor))
        return 0;
    const uintptr_t mesh = Memory::read_nocache<uintptr_t>(
        actor + Offsets::LootContainer_SocketMesh);
    outSocketMeshOk = IsUsableObjectPtr(mesh);

    const uintptr_t li = Memory::read_nocache<uintptr_t>(
        actor + Offsets::LootInteractionComponent);
    if (!IsUsableObjectPtr(li))
        return 0;

    // Block coherence before trusting the dispenser array: the acquisition
    // enum byte (values are not in the dump - it only gates) and the loot-ping
    // icon offset (LWC double Vector) must read sane.
    const uint8_t method = Memory::read_nocache<uint8_t>(
        li + Offsets::LootInteract_AcquisitionMethod);
    if (method > 16)
        return 0;
    const double kMaxIconOffset = 5000.0; // 50 m local offset ceiling
    for (int axis = 0; axis < 3; ++axis) {
        const double v = Memory::read_nocache<double>(
            li + Offsets::LootInteract_PingIconOffset
            + static_cast<uintptr_t>(axis) * 0x8);
        if (!(v >= -kMaxIconOffset && v <= kMaxIconOffset))
            return 0;
    }

    // TArray<Vector> DispenserLocations: one Vector per dispenser drop port.
    const uintptr_t data = Memory::read_nocache<uintptr_t>(
        li + Offsets::LootInteract_DispenserLocations);
    const int32_t num = Memory::read_nocache<int32_t>(
        li + Offsets::LootInteract_DispenserLocations + 0x8);
    const int32_t max = Memory::read_nocache<int32_t>(
        li + Offsets::LootInteract_DispenserLocations + 0xC);
    if (num <= 0 || num > 32 || max < num || 256 < max)
        return 0;
    if (!IsUsableObjectPtr(data))
        return 0;
    return num;
}

// ---- Bot intel (vision cones / per-part damage) -------------------------------

bool Engine::ReadBotVision(uintptr_t actor, float& outRadiusCm, float& outHalfAngleDeg,
    uint8_t& outAlertness, uint8_t& outCombatPhase)
{
    outRadiusCm = 0.f;
    outHalfAngleDeg = 0.f;
    outAlertness = 0;
    outCombatPhase = 0;
    if (!IsUsableObjectPtr(actor))
        return false;

    float halfDeg = 0.f;
    // Primary: the live-verified AIStateService block on the constructable.
    if (const uintptr_t svc = Memory::read_nocache<uintptr_t>(
            actor + Offsets::Constructable_AIStateService);
        IsUsableObjectPtr(svc)) {
        outAlertness =
            Memory::read_nocache<uint8_t>(svc + Offsets::AIState_Alertness);
        outCombatPhase =
            Memory::read_nocache<uint8_t>(svc + Offsets::AIState_CombatPhase);
        const float rangeCm =
            Memory::read_nocache<float>(svc + Offsets::AIState_SightRange);
        const uint8_t halfByte =
            Memory::read_nocache<uint8_t>(svc + Offsets::AIState_SightHalfAngle);
        if (VisionCone::PlausibleSight(rangeCm, static_cast<float>(halfByte))
            || (rangeCm > 100.f && rangeCm < 200000.f)) {
            if (rangeCm > 100.f && rangeCm < 200000.f) {
                outRadiusCm = rangeCm;
                // Compressed degrees: the byte reads as whole degrees when it
                // lands in a plausible half-angle range.
                if (halfByte >= 5 && halfByte <= 180)
                    halfDeg = static_cast<float>(halfByte);
            }
        }
    }
    // Secondary: AISenseConfigSight (real floats win when the chain resolves).
    if (const uintptr_t ctrl =
            Memory::read_nocache<uintptr_t>(actor + Offsets::Pawn_Controller);
        IsUsableObjectPtr(ctrl)) {
        const uintptr_t percep = Memory::read_nocache<uintptr_t>(
            ctrl + Offsets::AIController_Perception);
        if (IsUsableObjectPtr(percep)) {
            const uintptr_t arr = Memory::read_nocache<uintptr_t>(
                percep + Offsets::AIPerception_SensesConfig);
            const int32_t count = Memory::read_nocache<int32_t>(
                percep + Offsets::AIPerception_SensesConfig + 0x8);
            if (IsUsableObjectPtr(arr) && count > 0 && count <= 8) {
                for (int32_t i = 0; i < count; ++i) {
                    const uintptr_t cfg = Memory::read_nocache<uintptr_t>(
                        arr + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                    if (!IsUsableObjectPtr(cfg))
                        continue;
                    const float radius = Memory::read_nocache<float>(
                        cfg + Offsets::AISight_SightRadius);
                    const float loseRadius = Memory::read_nocache<float>(
                        cfg + Offsets::AISight_LoseSightRadius);
                    const float deg = Memory::read_nocache<float>(
                        cfg + Offsets::AISight_PeripheralDeg);
                    if (VisionCone::PlausibleSight(radius, deg)
                        && loseRadius >= radius * 0.5f) {
                        outRadiusCm = radius;
                        halfDeg = deg;
                        break;
                    }
                }
            }
        }
    }
    outHalfAngleDeg = halfDeg;
    return outRadiusCm > 0.f;
}

int Engine::ReadBotPartHp(uintptr_t actor, float* out, int cap, int& outDestroyedParts)
{
    outDestroyedParts = 0;
    if (!out || cap <= 0 || !IsUsableObjectPtr(actor))
        return 0;
    const uintptr_t svc = Memory::read_nocache<uintptr_t>(
        actor + Offsets::Constructable_HealthService);
    if (!IsUsableObjectPtr(svc))
        return 0;
    const uintptr_t data =
        Memory::read_nocache<uintptr_t>(svc + Offsets::PartHpArray);
    const int32_t count =
        Memory::read_nocache<int32_t>(svc + Offsets::PartHpArray + 0x8);
    if (!IsUsableObjectPtr(data) || count <= 0 || count > 512)
        return 0;
    const int n = count < cap ? count : cap;
    for (int i = 0; i < n; ++i) {
        const float hp = Memory::read_nocache<float>(
            data + static_cast<uintptr_t>(i) * sizeof(float));
        out[i] = (hp >= 0.f && hp <= 4.f) ? hp : -1.f;  // -1 = unreadable slot
    }
    // Style drivers: per-part destroyed flags (best-effort aggregate count).
    const uintptr_t drivers =
        Memory::read_nocache<uintptr_t>(actor + Offsets::StyleDrivers);
    const int32_t driverCount =
        Memory::read_nocache<int32_t>(actor + Offsets::StyleDrivers + 0x8);
    if (IsUsableObjectPtr(drivers) && driverCount > 0 && driverCount <= 32) {
        for (int32_t i = 0; i < driverCount; ++i) {
            const uintptr_t drv = Memory::read_nocache<uintptr_t>(
                drivers + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
            if (!IsUsableObjectPtr(drv))
                continue;
            if (0 != Memory::read_nocache<uint8_t>(
                    drv + Offsets::Style_IsDestroyedByte))
                ++outDestroyedParts;
        }
    }
    return n;
}

// ---- Raid intel (dashboard / activity feed / map radar) -----------------------

void Engine::RefreshRadarBounds()
{
    // AWorldPartitionMiniMap is an AInfo in the level: find it via the cached
    // actor array (worker-side only), at most one rescan per 10s while absent.
    if (!m_miniMapActor || !IsValidPointer(m_miniMapActor)) {
        m_miniMapActor = 0;
        static uint64_t s_scanStampMs = 0;
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const uintptr_t actors = Actors;
        const int count = ActorsCount;
        if (actors && count > 0 && count <= 400000
            && (s_scanStampMs == 0 || nowMs - s_scanStampMs >= 10000)) {
            s_scanStampMs = nowMs;
            for (int i = 0; i < count && !m_miniMapActor; ++i) {
                const uintptr_t actor = Memory::read<uintptr_t>(
                    actors + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                if (!IsUsableObjectPtr(actor))
                    continue;
                const std::string fname = toLower(GetActorFNameStringCached(actor));
                if (fname.find("worldpartitionminimap") == std::string::npos
                    && fname.find("minimap") == std::string::npos)
                    continue;
                m_miniMapActor = actor;
            }
        }
    }
    if (!m_miniMapActor)
        return;

    // FBox::Min @ +0x00, FBox::Max @ +0x18 as doubles (LWC). The float layout
    // is tried when the double block reads as garbage.
    auto readBounds = [&](bool asDouble) {
        RadarProjection::Bounds b;
        auto readOff = [&](std::ptrdiff_t off) -> double {
            return asDouble
                ? Memory::read_nocache<double>(m_miniMapActor + off)
                : static_cast<double>(
                    Memory::read_nocache<float>(m_miniMapActor + off));
        };
        b.minX = readOff(Offsets::MiniMap_WorldBounds);
        b.minY = readOff(Offsets::MiniMap_WorldBounds + 0x8);
        b.maxX = readOff(Offsets::MiniMap_WorldBounds + 0x18);
        b.maxY = readOff(Offsets::MiniMap_WorldBounds + 0x20);
        return b;
    };
    RadarProjection::Bounds bounds = readBounds(true);
    if (!RadarProjection::BoundsPlausible(bounds))
        bounds = readBounds(false);
    // WORLD_UNITS_PER_PIXEL calibrates the map scale - a plausible value is
    // the second proof that this really is the minimap block before the radar
    // trusts its bounds.
    const float unitsPerPixel = Memory::read_nocache<float>(
        m_miniMapActor + Offsets::MiniMap_UnitsPerPixel);
    if (!(unitsPerPixel > 0.1f && unitsPerPixel < 10000.f))
        return;
    if (!RadarProjection::BoundsPlausible(bounds))
        return;
    std::unique_lock<std::shared_mutex> lock(m_radarBoundsMutex);
    m_radarBounds = bounds;
    m_radarBoundsValid = true;
}

void Engine::RefreshRaidDashboard()
{
    // 500ms throttle: the HUD extrapolates the clock between refreshes.
    static std::atomic<uint64_t> s_lastMs{ 0 };
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t lastMs = s_lastMs.load(std::memory_order_relaxed);
    if (nowMs < lastMs || nowMs - lastMs < 500)
        return;
    s_lastMs.store(nowMs, std::memory_order_relaxed);

    if (var::show_radar)
        RefreshRadarBounds();
    if (!var::show_raid_hud)
        return;

    const uintptr_t gs = AGameStateBase;
    if (!IsUsableObjectPtr(gs))
        return;

    RaidHudState st;
    // FStageInfo is inline at game_state + STAGE_INFO per the drop. When that
    // block reads as a pointer instead, the deref form is tried before the
    // clock is declared unknown.
    st.timeLeftS = Memory::read_nocache<double>(
        gs + Offsets::GameState_StageInfoRef + Offsets::StageInfo_TimeLeftOff);
    st.graceS = Memory::read_nocache<double>(
        gs + Offsets::GameState_StageInfoRef + Offsets::StageInfo_GraceTimeOff);
    if (!RaidClock::ClockPlausible(st.timeLeftS)) {
        const uintptr_t stagePtr = Memory::read_nocache<uintptr_t>(
            gs + Offsets::GameState_StageInfoRef);
        if (IsUsableObjectPtr(stagePtr)) {
            const double timeLeft = Memory::read_nocache<double>(
                stagePtr + Offsets::StageInfo_TimeLeftOff);
            const double grace = Memory::read_nocache<double>(
                stagePtr + Offsets::StageInfo_GraceTimeOff);
            if (RaidClock::ClockPlausible(timeLeft)) {
                st.timeLeftS = timeLeft;
                st.graceS = grace;
            }
        }
    }
    if (!RaidClock::ClockPlausible(st.timeLeftS))
        st.timeLeftS = -1.0;
    if (!(st.graceS >= 0.0 && st.graceS < 3600.0))
        st.graceS = -1.0;

    st.gamePhase =
        Memory::read_nocache<int32_t>(gs + Offsets::GameState_GamePhase);
    st.enemyCount =
        Memory::read_nocache<int32_t>(gs + Offsets::GameState_EnemyCount);
    st.pickupCount =
        Memory::read_nocache<int32_t>(gs + Offsets::GameState_PickupCount);
    if (!(st.gamePhase >= 0 && st.gamePhase <= 16))
        st.gamePhase = -1;
    if (!(st.enemyCount >= 0 && st.enemyCount <= 100000))
        st.enemyCount = -1;
    if (!(st.pickupCount >= 0 && st.pickupCount <= 1000000))
        st.pickupCount = -1;
    st.stampMs = nowMs;
    st.valid =
        st.timeLeftS >= 0.0 || st.enemyCount >= 0 || st.gamePhase >= 0;
    {
        std::unique_lock<std::shared_mutex> lock(m_raidHudMutex);
        m_raidHud = st;
    }
}

Engine::RaidHudState Engine::GetRaidHud() const
{
    std::shared_lock<std::shared_mutex> lock(m_raidHudMutex);
    return m_raidHud;
}

RadarProjection::Bounds Engine::GetRadarBounds(bool& outValid) const
{
    std::shared_lock<std::shared_mutex> lock(m_radarBoundsMutex);
    outValid = m_radarBoundsValid;
    return m_radarBounds;
}

void Engine::PushActivity(uint64_t key, const char* text)
{
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::unique_lock<std::shared_mutex> lock(m_feedMutex);
    m_activityFeed.Push(nowMs, key, text);
    m_activityFeed.Expire(nowMs, 45000);
}

ActivityFeed::Feed Engine::GetActivityFeed() const
{
    std::shared_lock<std::shared_mutex> lock(m_feedMutex);
    return m_activityFeed;
}

// ═════════════════════════════════════════════════════════════════════════════
// World rungs
//
// These were file-local helpers in Update.cpp, which is exactly why the chain's
// resolution order could not live in one place: the ladder in
// Core/PlayerChain.hpp can only own the order when every rung is reachable from
// it. help/sdk.txt: PL @ 0x120, LevelCollections[i]+0x20, Levels[].
// ═════════════════════════════════════════════════════════════════════════════

uintptr_t Engine::ReadWorldSlot(uint64_t base)
{
    if (!base)
        return 0;
    // Bypass the VMM page cache - a stale GWorld slot blocks raid re-entry until
    // the exe is restarted.
    const uintptr_t slot = Memory::read_nocache<uintptr_t>(
        base + static_cast<uint64_t>(Offsets::UWorld));
    // Preserve the literal slot value for the ladder/overlay even when it is
    // not a valid UWorld pointer. The next rung owns validation; discarding it
    // here made the documented intermediary dereference unreachable and lied
    // about the raw value (GWorldRaw became zero).
    return slot == UINTPTR_MAX ? 0 : slot;
}

uintptr_t Engine::WorldSlotInner(uintptr_t slot)
{
    if (!IsPlausibleObjPtr(slot))
        return 0;
    // NOCACHE: a cached deref froze the world (log gwSrc:2 - MainMenu served
    // mid-raid, TheDam served on the home screen).
    const uintptr_t inner = Memory::read_nocache<uintptr_t>(slot);
    return IsPlausibleObjPtr(inner) ? inner : 0;
}

bool Engine::LevelLooksOwnedByWorld(uintptr_t level, uintptr_t world)
{
    if (!IsPlausibleObjPtr(level) || !world)
        return false;
    const uintptr_t owning = Memory::read<uintptr_t>(level + Offsets::Level_OwningWorld);
    if (owning == world)
        return true;
    uintptr_t data = 0;
    int32_t count = 0;
    return WorldScan::ReadLevelActors(level, data, count)
        && IsPlausibleObjPtr(data) && count > 0 && count <= 10000;
}

uintptr_t Engine::ResolvePersistentLevel(uintptr_t world)
{
    if (!IsPlausibleObjPtr(world))
        return 0;

    const uintptr_t level = Memory::read<uintptr_t>(world + Offsets::PersistentLevel);
    if (LevelLooksOwnedByWorld(level, world))
        return level;

    const uintptr_t collectionsData =
        Memory::read<uintptr_t>(world + Offsets::LevelCollections);
    const int32_t collectionsNum =
        Memory::read<int32_t>(world + Offsets::LevelCollections + 8);
    if (IsPlausibleObjPtr(collectionsData) && collectionsNum > 0 && collectionsNum <= 16) {
        const int limit = (collectionsNum > 4) ? 4 : collectionsNum;
        for (int i = 0; i < limit; ++i) {
            const uintptr_t collection =
                collectionsData + static_cast<uintptr_t>(i) * Offsets::LevelCollection_Stride;
            const uintptr_t candidate = Memory::read<uintptr_t>(
                collection + Offsets::LevelCollection_PersistentLevel);
            if (LevelLooksOwnedByWorld(candidate, world))
                return candidate;
        }
    }

    const uintptr_t levelsData = Memory::read<uintptr_t>(world + Offsets::Levels);
    const int32_t levelsNum = Memory::read<int32_t>(world + Offsets::Levels + 8);
    if (IsPlausibleObjPtr(levelsData) && levelsNum > 0 && levelsNum < 512) {
        const int limit = (levelsNum > 8) ? 8 : levelsNum;
        for (int i = 0; i < limit; ++i) {
            const uintptr_t candidate = Memory::read<uintptr_t>(
                levelsData + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
            if (LevelLooksOwnedByWorld(candidate, world))
                return candidate;
        }
    }

    return 0;
}

uintptr_t Engine::WorldFromGameStateGlobal(uint64_t base)
{
    if (!base)
        return 0;
    const uintptr_t gs = Memory::read<uintptr_t>(base + Offsets::GameStateGlobalRva);
    if (!IsPlausibleObjPtr(gs))
        return 0;

    const uintptr_t arrData = Memory::read<uintptr_t>(gs + Offsets::GameState_PlayerArray);
    const int32_t arrNum = Memory::read<int32_t>(gs + Offsets::GameState_PlayerArray + 8);
    if (!IsPlausibleObjPtr(arrData) || arrNum <= 0 || arrNum > 128)
        return 0;

    // A GameState's Outer is the world it belongs to. The decrypt gives that one
    // hop; the slot scan below stays for the CLs where it fails.
    const OuterLink::Hop hop = OuterLink::FromObject(gs);
    if (hop.ptr && ResolvePersistentLevel(hop.ptr))
        return hop.ptr;

    static const std::ptrdiff_t kOuterCands[] = { 0x20, 0x28, 0x18, 0x30, 0x10, 0x40 };
    for (std::ptrdiff_t off : kOuterCands) {
        const uintptr_t cand = Memory::read<uintptr_t>(gs + off);
        if (ResolvePersistentLevel(cand))
            return cand;
    }
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// The ladder's host
//
// One primitive per rung (the contract lives in Core/PlayerChain.hpp). Every one
// of them applies its own proof and returns 0 when that proof fails, so the
// ladder's fixed order is the only thing that decides which value wins - and a
// rung that answers is named in the trace instead of an integer tag.
// ═════════════════════════════════════════════════════════════════════════════

static bool IsGoodLocalPlayerPtr(uintptr_t lp)
{
    return lp != 0 && lp != UINTPTR_MAX
        && lp >= 0x1000 && lp < 0x7FFFFFFFFFFF
        && Memory::IsValidPtrFast2(lp);
}

struct LadderHost {
    Engine& eng;
    uint64_t base = 0;
    uintptr_t actors = 0;
    int actorCount = 0;
    // What the ladder resolved so far, so the camera-manager rung can scan the
    // level it is actually looking at instead of the previous tick's publish.
    uintptr_t world = 0;
    uintptr_t level = 0;
    uintptr_t pcSeen = 0;

    // ── World ──
    uintptr_t WorldSlot(uint64_t b) { return eng.ReadWorldSlot(b); }
    bool IsWorld(uintptr_t w)
    {
        if (!eng.ResolvePersistentLevel(w))
            return false;
        world = w;
        return true;
    }
    uintptr_t WorldSlotInner(uintptr_t slot)
    {
        const uintptr_t inner = eng.WorldSlotInner(slot);
        return (inner && eng.ResolvePersistentLevel(inner)) ? inner : 0;
    }
    uintptr_t WorldFromGameStateGlobal(uint64_t b)
    {
        return eng.WorldFromGameStateGlobal(b);
    }
    uintptr_t LevelOfWorld(uintptr_t w)
    {
        world = w;
        level = eng.ResolvePersistentLevel(w);
        return level;
    }
    bool LevelActors(uintptr_t level, uintptr_t& outActors, int& outCount)
    {
        if (!eng.ResolveLevelActors(level, outActors, outCount)) {
            outActors = 0;
            outCount = 0;
            actors = 0;
            actorCount = 0;
            return false;
        }
        actors = outActors;
        actorCount = outCount;
        return true;
    }

    // ── GameInstance ──
    uintptr_t GiOwningSlot(uintptr_t world, bool& backRef)
    {
        return GiOwningSlotRung(world, backRef);
    }
    uintptr_t GiFromOuter(uintptr_t lp, PlayerChain::Rung& which)
    {
        return GiFromOuterRung(lp, which);
    }
    uintptr_t GiFromDecrypt(uintptr_t world) { return GiFromDecryptSlot(world); }
    uintptr_t GiFromLegacySlot(uintptr_t world) { return GiFromLegacySlotRung(world); }
    uintptr_t GiFromScan(uintptr_t world) { return GiFromScanRung(world); }
    bool IsGameInstance(uintptr_t gi) { return ValidateGameInstance(gi) != 0; }

    // ── Controller ──
    // Dump-verified cache: bIsLocalPlayerController (SDK 0xD64) keeps the PC
    // across hub ticks and pawn-less lobbies, where the pair cache cannot hit.
    bool PcFromCachedFlag(uintptr_t cachedPc, uintptr_t& pc)
    {
        if (!cachedPc || !eng.IsValidPointer(cachedPc)
            || !eng.IsLocalPlayerControllerConfirmed(cachedPc))
            return false;
        pc = cachedPc;
        pcSeen = cachedPc;
        return true;
    }

    /**
     * Keep the cached pair while the pawn's root still sits at a sane world
     * position. DefaultFOV/ViewTarget flap must NOT be part of this gate: it
     * cleared the cache every ~0.5s and forced the ~1200-actor camera-manager
     * scan (~1s of DMA freezes) on every flap.
     */
    bool PcFromCachedPair(uintptr_t cachedPc, uintptr_t cachedPawn,
        uintptr_t& pc, uintptr_t& pawn)
    {
        if (!cachedPc || !cachedPawn
            || !eng.IsValidPointer(cachedPc) || !eng.IsValidPointer(cachedPawn))
            return false;
        const uintptr_t root =
            Memory::read_nocache<uintptr_t>(cachedPawn + Offsets::RootComponent);
        if (!root || !eng.IsValidPointer(root)) {
            ++eng.m_pairCacheFailStreak;
            return false;
        }
        Vector3 pos = Memory::read_nocache<Vector3>(root + Offsets::RelativeLocation);
        float magSq = static_cast<float>(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
        if (magSq <= 10000.f || magSq >= 1.0e14f) {
            const std::ptrdiff_t ctw = Engine::ProbeComponentToWorldOffset(root);
            pos = Engine::ToVector3(Memory::read_nocache<Engine::FVector3d>(
                root + ctw + Offsets::Transform_Translation));
            magSq = static_cast<float>(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
        }
        if (magSq <= 10000.f || magSq >= 1.0e14f) {
            pos = Engine::ReadActorReplicatedLocation(cachedPawn);
            magSq = static_cast<float>(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
        }
        if (magSq <= 10000.f || magSq >= 1.0e14f) {
            ++eng.m_pairCacheFailStreak;
            return false;
        }
        eng.m_pairCacheFailStreak = 0;
        pc = cachedPc;
        pawn = cachedPawn;
        pcSeen = cachedPc;
        return true;
    }

    bool PcFromCamManager(uintptr_t level, uintptr_t actorsArr, int count,
        uintptr_t& pc, uintptr_t& pawn, uintptr_t& pcm)
    {
        (void)count;
        if (!level || !actorsArr)
            return false;
        uintptr_t foundPc = 0, foundPawn = 0, foundPcm = 0;
        if (!eng.ResolvePcFromLevelCameraManager(
                level, actorsArr, foundPc, foundPawn, foundPcm))
            return false;
        pc = foundPc;
        pawn = foundPawn;
        pcm = foundPcm;
        pcSeen = foundPc;
        return true;
    }

    bool PcFromActorScan(uintptr_t level, uintptr_t actorsArr, int count,
        uintptr_t gi, uintptr_t& pc, uintptr_t& pawn)
    {
        (void)gi;
        if (!level || !actorsArr || count <= 0)
            return false;
        uintptr_t scannedPc = 0, scannedPawn = 0;
        // IsLocalPlayerController is the dump-reflected flag (0xD64): it accepts
        // the controller even when the live-pinned PC+0x4D0 camera slot is
        // garbage on this build.
        // A failed scan is not logged here any more: the ladder's own state says
        // which rung the controller came from and how many were rejected.
        if (!eng.ResolveLocalPlayerChainFromActors(
                level, actorsArr, count, 0, scannedPc, scannedPawn)
            || !(eng.IsLocalPlayerController(scannedPc)
                 || eng.ControllerHasValidPcm(scannedPc)))
            return false;
        pc = scannedPc;
        pawn = scannedPawn;
        pcSeen = scannedPc;
        return true;
    }

    /** UGameInstance::LocalPlayers -> ULocalPlayer[0] -> PlayerController. */
    bool PcFromGiArray(uintptr_t gi, uintptr_t& pc, uintptr_t& pawn, bool& flagProved)
    {
        flagProved = false;
        if (!gi)
            return false;
        const uintptr_t arrData = Memory::read<uintptr_t>(gi + Offsets::LocalPlayers);
        const int arrNum = Memory::read<int>(gi + Offsets::LocalPlayers + 8);
        if (!arrData || !Memory::IsValidPtrFast2(arrData) || arrNum <= 0 || arrNum > 16)
            return false;
        const int limit = (arrNum > 4) ? 4 : arrNum;
        for (int i = 0; i < limit; ++i) {
            const uintptr_t slot =
                Memory::read<uintptr_t>(arrData + static_cast<size_t>(i) * sizeof(uintptr_t));
            if (!IsUsableObjectPtr(slot))
                continue;
            const uintptr_t candidate =
                Memory::read<uintptr_t>(slot + Offsets::LocalPlayer_PlayerController);
            if (!eng.IsValidPointer(candidate))
                continue;
            const bool engineSaysLocal = eng.IsLocalPlayerControllerConfirmed(candidate);
            const uintptr_t ackPawn = Engine::ReadAcknowledgedPawn(candidate);
            bool pawnOk = false;
            if (ackPawn && eng.IsValidPointer(ackPawn)) {
                const uintptr_t root =
                    Memory::read<uintptr_t>(ackPawn + Offsets::RootComponent);
                pawnOk = root && eng.IsValidPointer(root);
            }
            if (!engineSaysLocal && !pawnOk)
                continue;
            pc = candidate;
            pawn = pawnOk ? ackPawn : 0;
            flagProved = engineSaysLocal;
            pcSeen = candidate;
            return true;
        }
        return false;
    }

    uintptr_t PcmFromPc(uintptr_t pc)
    {
        if (!pc || !eng.ControllerHasValidPcm(pc))
            return 0;
        const uintptr_t raw =
            Memory::read_nocache<uintptr_t>(pc + Offsets::APlayerCameraManager);
        return (raw && eng.IsValidPointer(raw)) ? raw : 0;
    }
    uintptr_t PcmFromActors()
    {
        return eng.GetCameraManagerFromActors(world, level, pcSeen);
    }

    // ── LocalPlayer ──
    /** UGameInstance::LocalPlayers slot: the full chain, or one that owns `pc`. */
    uintptr_t LpFromGiArray(uintptr_t gi, uintptr_t pc, bool& backRef)
    {
        backRef = false;
        uintptr_t lp = 0;
        uintptr_t pcFromChain = 0;
        if (ValidateGameInstance(gi, &lp, &pcFromChain) && IsGoodLocalPlayerPtr(lp)) {
            backRef = (pc == 0)
                || Memory::read<uintptr_t>(lp + Offsets::LocalPlayer_PlayerController) == pc;
            return lp;
        }
        if (!pc)
            return 0;
        const uintptr_t arrData = Memory::read<uintptr_t>(gi + Offsets::LocalPlayers);
        const int arrNum = Memory::read<int>(gi + Offsets::LocalPlayers + 8);
        if (!arrData || !Memory::IsValidPtrFast2(arrData) || arrNum <= 0 || arrNum > 16)
            return 0;
        const int limit = (arrNum > 8) ? 8 : arrNum;
        for (int i = 0; i < limit; ++i) {
            const uintptr_t slot =
                Memory::read<uintptr_t>(arrData + static_cast<size_t>(i) * sizeof(uintptr_t));
            if (!IsGoodLocalPlayerPtr(slot))
                continue;
            if (Memory::read<uintptr_t>(slot + Offsets::LocalPlayer_PlayerController) == pc) {
                backRef = true;
                return slot;
            }
        }
        return 0;
    }

    uintptr_t LpFromController(uintptr_t pc, PlayerChain::Rung& which, bool& backRef)
    {
        return eng.ResolveLocalPlayerFromController(pc, &which, &backRef);
    }

    // ── Pawn / PlayerState / Root ──
    uintptr_t PawnFromPc(uintptr_t pc, PlayerChain::Rung& which)
    {
        which = PlayerChain::Rung::None;
        if (!pc)
            return 0;
        // SDK dump: AController.Pawn @ 0x3F0 is primary; 0x3D8/0x408 are
        // heuristic fallbacks only, and every candidate is pointer-gated.
        struct Cand { std::ptrdiff_t off; PlayerChain::Rung rung; };
        static const Cand kCands[] = {
            { Offsets::AcknowledgedPawn, PlayerChain::Rung::PcAcknowledged },
            { Offsets::AcknowledgedPawn_Fallback, PlayerChain::Rung::PcAcknowledgedAlt },
            { Offsets::Controller_Character, PlayerChain::Rung::PcCharacter },
        };
        for (const Cand& c : kCands) {
            const uintptr_t pawn = Memory::read<uintptr_t>(pc + c.off);
            if (pawn && eng.IsValidPointer(pawn)) {
                which = c.rung;
                return pawn;
            }
        }
        return 0;
    }

    /** AController::PlayerState (0x3D0) - the pawn's own, then the PC's. */
    uintptr_t StateFromPawn(uintptr_t pawn)
    {
        if (!pawn)
            return 0;
        const uintptr_t ps = Memory::read<uintptr_t>(pawn + Offsets::APlayerState);
        return (ps && eng.IsValidPointer(ps)) ? ps : 0;
    }
    uintptr_t StateFromPc(uintptr_t pc)
    {
        if (!pc)
            return 0;
        const uintptr_t ps = Memory::read<uintptr_t>(pc + Offsets::AController_PlayerState);
        return (ps && eng.IsValidPointer(ps)) ? ps : 0;
    }
    uintptr_t RootFromPawn(uintptr_t pawn)
    {
        if (!pawn)
            return 0;
        const uintptr_t root = Memory::read<uintptr_t>(pawn + Offsets::RootComponent);
        return (root && eng.IsValidPointer(root)) ? root : 0;
    }
};

void Engine::ResolvePlayerChain(uint64_t base, const PlayerChain::Seed& seed,
    uintptr_t& outActors, int& outActorCount)
{
    LadderHost host{ *this, base };
    PlayerChain::State st;
    PlayerChain::Resolve(st, host, seed);

    outActors = host.actors;
    outActorCount = host.actorCount;
    m_pairCacheInvalidated = m_pairCacheFailStreak >= 8;

    std::unique_lock<std::shared_mutex> lock(m_stateMutex);
    st.tick = m_chain.tick + 1;
    m_chain = st;
}

PlayerChain::State Engine::GetChainState() const
{
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return m_chain;
}

std::string Engine::GetChainTrace() const
{
    std::shared_lock<std::shared_mutex> lock(m_stateMutex);
    return PlayerChain::Trace(m_chain);
}
