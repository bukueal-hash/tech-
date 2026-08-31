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
        const FVector3d pawnWorld = Memory::read_nocache<FVector3d>(
			rootComp + Offsets::ComponentToWorld + 0x20);
		pawnPos = ToVector3(pawnWorld);
		if (!::IsPlausibleWorldPos(pawnPos))
            pawnPos = Memory::read_nocache<Vector3>(rootComp + Offsets::RelativeLocation);
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

static bool ValidateGameInstance(uintptr_t gi, uintptr_t* outLocalPlayer = nullptr,
    uintptr_t* outPlayerController = nullptr)
{
    if (!IsUsableObjectPtr(gi) || Engine::LooksLikeUtf16Garbage(gi))
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

    // Structural fallback: accept GI when LocalPlayers looks like a real TArray even
    // if LP→PC validation failed (transient FOV/PCM glitch mid-raid). Still publish LP
    // slot 0 when present so OwningGI/LocalPlayer do not go permanently red.
    for (std::ptrdiff_t off : kLpArrOffs) {
        const GITArrayHdr arr = ReadGIArray(gi, off);
        if (!IsUsableObjectPtr(arr.Data))
            continue;
        if (arr.Num < 0 || arr.Num > 16)
            continue;
        if (arr.Max < arr.Num || arr.Max > 64)
            continue;
        const uintptr_t slot0 = Memory::read<uintptr_t>(arr.Data);
        if (outLocalPlayer && IsUsableObjectPtr(slot0) && !Engine::LooksLikeUtf16Garbage(slot0))
            *outLocalPlayer = slot0;
        if (outPlayerController)
            *outPlayerController = 0;
        return true;
    }

    return false;
}

/** Once LP is known, recover GI via UObject Outer candidates (help has no GI decrypt). */
uintptr_t Engine::ResolveGameInstanceFromLocalPlayer(uintptr_t localPlayer)
{
    if (!IsUsableObjectPtr(localPlayer))
        return 0;

    static const std::ptrdiff_t kOuterCands[] = {
        0x20, 0x28, 0x18, 0x30, 0x10, 0x38, 0x40, 0x48, 0x50,
        0x58, 0x60, 0x68, 0x70, 0x78, 0x80,
    };
    for (std::ptrdiff_t off : kOuterCands) {
        const uintptr_t cand = Memory::read<uintptr_t>(localPlayer + off);
        if (!ValidateGameInstance(cand))
            continue;
        return cand;
    }
    return 0;
}

// CL-1341255: UWorld+0x478 (OwningGameInstance) is SIMD-encrypted — a plain
// read returns 0. Decrypt: 16B blob @ world+0x478, PSHUFB with the 16-byte
// mask @ base+0xB09C350, XOR qword lanes with keys @ base+0xB06C380/0xB06C390.
// Validate via the GI struct's world back-ref @ GI+0x2F0.
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

    // Strong check: the GI struct points back at its owning UWorld @ +0x2F0.
    const uintptr_t backRef = Memory::read<uintptr_t>(
        gi + Offsets::GameInstance_WorldBackRef);
    if (backRef == world)
        return gi;

    // Back-ref mismatch — still accept when the full LP→PC chain validates.
    return ValidateGameInstance(gi) ? gi : 0;
}

uintptr_t Engine::GetGameInstance(uint64_t uworldAddr)
{
    if (!uworldAddr || !Memory::IsValidPtrFast2(uworldAddr))
        return 0;

    auto tryGi = [&](uintptr_t gi) -> uintptr_t {
        return ValidateGameInstance(gi) ? gi : 0;
    };

    // CL-1341255: the slot is SIMD-encrypted — decrypt first.
    if (const uintptr_t giDec = DecryptGameInstancePointer(
            static_cast<uintptr_t>(uworldAddr)); giDec)
        return giDec;

    // Help: GAME_INSTANCE 0x4D8 (was 0x3B0, may be encrypted at slot).
    static const std::ptrdiff_t kGiOffs[] = {
        Offsets::OwningGameInstance,
        static_cast<std::ptrdiff_t>(0x3B0),
    };
    for (std::ptrdiff_t off : kGiOffs) {
        const uintptr_t gi = Memory::read<uintptr_t>(uworldAddr + off);
        if (const uintptr_t ok = tryGi(gi))
            return ok;
    }

    // Encrypted slot: scan UWorld for any pointer that has help LocalPlayers→LP→PC@0xA0.
    for (std::ptrdiff_t off = 0x80; off <= 0x700; off += 0x8) {
        if (off == Offsets::OwningGameInstance || off == 0x3B0)
            continue;
        const uintptr_t gi = Memory::read<uintptr_t>(uworldAddr + off);
        if (const uintptr_t ok = tryGi(gi))
            return ok;
    }

    return 0;
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

bool Engine::ResolveLocalPlayerFromGameInstance(uintptr_t gameInstance, uintptr_t& outLocalPlayer,
    uintptr_t& outPlayerController)
{
    return ValidateGameInstance(gameInstance, &outLocalPlayer, &outPlayerController);
}

uintptr_t Engine::ResolveLocalPlayerFromController(uintptr_t playerController)
{
    if (!playerController || !Memory::IsValidPtrFast2(playerController))
        return 0;

    // Help: LocalPlayer::PLAYER_CONTROLLER 0xA0 must point back at this PC.
    auto lpPointsAtPc = [&](uintptr_t lp) -> bool {
        if (!IsUsableObjectPtr(lp) || Engine::LooksLikeUtf16Garbage(lp))
            return false;
        return Memory::read<uintptr_t>(lp + Offsets::LocalPlayer_PlayerController)
            == playerController;
    };

    for (std::ptrdiff_t off = 0x80; off <= 0x800; off += 0x8) {
        const uintptr_t lp = Memory::read<uintptr_t>(playerController + off);
        if (lpPointsAtPc(lp))
            return lp;
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

uintptr_t Engine::GetCameraManagerFromActors()
{
	// Camera discovery is called by Update, the camera worker, and frame building.
	// Serialize the scan/cache so the static generation-scoped pointer is not
	// raced by concurrent readers.
	static std::mutex s_pcmMutex;
	std::lock_guard<std::mutex> pcmLock(s_pcmMutex);

	// help/esp.txt: FName PCM over all Levels; LP not required; prefer PCOwner match;
	// live POV FOV + location sanity (not DefaultFOV-only / FOV-scan).
	uintptr_t pc = 0;
	uintptr_t gworld = 0;
	uintptr_t persistent = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		pc = PlayerController;
		gworld = GWorld;
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
    if (!var::enable_world)
        return false;
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

std::string TryReadEnglishItemNameFromHover(uint64_t actor, std::ptrdiff_t hover_off)
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

    const uint64_t hover_base = actor + static_cast<uint64_t>(hover_off);

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
    float& outArmorPlates, float& outArmorPerPlate)
{
    outWeaponName.clear();
    outWeaponQuality = -1;
    outWeaponClip = 0;
    outStowed0.clear();
    outStowedQ0 = -1;
    outStowed1.clear();
    outStowedQ1 = -1;
    outArmorPlates = 0.f;
    outArmorPerPlate = 0.f;

    if (!pawn)
        return;

    const uintptr_t invRaw = Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    const uintptr_t invComp = ResolveInventoryPtr(invRaw);
    if (!invComp)
        return;

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
    }
}