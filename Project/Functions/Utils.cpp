#include "../Core/Engine.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <stdint.h>
#include <stdlib.h>

#include <Windows.h>
#include <vector>


#include "../Core/AssetNames.h"
#include "../Functions/RobotList.h"
#include "../Core/Cache.hpp"
#include "../Core/Memory.h"
#include "../Core/SteamDecrypt.hpp"
#include "../Core/WorldItemCategory.h"

#include "../Interface/OverlayHost.h"

#include <atomic>

namespace {

std::atomic<float> g_projViewportW{ 1920.f };
std::atomic<float> g_projViewportH{ 1080.f };

bool PovMatchesPawnView(const Engine::FVector3d& pov, const Vector3& pawn)
{
	const double dx = std::fabs(pov.x - static_cast<double>(pawn.x));
	const double dy = std::fabs(pov.y - static_cast<double>(pawn.y));
	const double dz = std::fabs(pov.z - static_cast<double>(pawn.z));
	// Third-person camera stays within ~40m of pawn on each axis.
	return dx < 4000.0 && dy < 4000.0 && dz < 4000.0;
}

bool IsPlausibleWorldPosD(const Engine::FVector3d& p)
{
	return ::IsPlausibleWorldPos(Vector3{
		static_cast<float>(p.x),
		static_cast<float>(p.y),
		static_cast<float>(p.z)
	});
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
	Engine::FVector3d merged{};
	merged.x = IsPlausiblePitch(povRot.x)
		? povRot.x
		: (hasPc ? static_cast<double>(ctrlRot.x) : 0.0);
	merged.y = IsPlausibleYaw(povRot.y)
		? povRot.y
		: (hasPc ? static_cast<double>(ctrlRot.y) : 0.0);
	merged.z = std::isfinite(povRot.z) && std::fabs(povRot.z) <= 180.0
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
	auto IsPcmValid = [&](uintptr_t p) {
		if (!p || !IsValidPointer(p)) return false;
        const float f = Memory::read_nocache<float>(p + Offsets::DefaultFOV);
		return f > 1.0f && f < 179.0f;
	};

	if (pc && !IsPcmValid(pcm))
		pcm = Memory::read<uintptr_t>(pc + Offsets::APlayerCameraManager);
	if (!IsPcmValid(pcm) && pc)
		pcm = GetCameraManagerFromActors();
	if (!IsPcmValid(pcm)) {
		uintptr_t level = 0;
		uintptr_t actors = 0;
		{
			std::shared_lock<std::shared_mutex> lock(m_stateMutex);
			level = PersistentLevel;
			actors = Actors;
		}
		if (level && actors) {
			uintptr_t pcmPc = pc;
			uintptr_t pcmPawn = pawn;
			uintptr_t foundPcm = pcm;
			if (ResolvePcFromLevelCameraManager(level, actors, pcmPc, pcmPawn, foundPcm)) {
				pcm = foundPcm;
				if (pcmPc && pcmPc != pc) {
					std::unique_lock<std::shared_mutex> lock(m_stateMutex);
					PlayerController = pcmPc;
					AcknowledgedPawn = pcmPawn;
					pc = pcmPc;
					pawn = pcmPawn;
				}
			}
		}
	}

	const bool pcmOk = pcm && IsValidPointer(pcm);

	float povFov = 0.f;
	float defFov = 0.f;
	float jsonFov = 0.f;
	FVector3d povLoc{};
	FVector3d povRot{};

	if (pcmOk) {
		const uintptr_t povBase =
			pcm + Offsets::ViewTargetTarget + Offsets::ViewTargetPOV;
        povLoc = Memory::read_nocache<FVector3d>(povBase + Offsets::CameraLocation);
        povRot = Memory::read_nocache<FVector3d>(povBase + Offsets::CameraRotation);
        povFov = Memory::read_nocache<float>(povBase + Offsets::CameraFOV);
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

// Deprecated for worker threads — only Update() may refresh g_Camera (see Update.cpp).
// Retained for manual/debug use; do not add new callers.
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

namespace {

// UE-style camera axes (matches ARC reference TransformWorldToScreen).
void RotationGetAxes(const Vector3& rot, Vector3& axisX, Vector3& axisY, Vector3& axisZ)
{
	constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
	auto toVector = [&](double pitchDeg, double yawDeg) -> Vector3 {
		const double pitch = pitchDeg * kDegToRad;
		const double yaw = yawDeg * kDegToRad;
		const double cp = std::cos(pitch);
		const double sp = std::sin(pitch);
		const double cy = std::cos(yaw);
		const double sy = std::sin(yaw);
		return Vector3{ cp * cy, cp * sy, sp };
	};

	axisX = toVector(rot.x, rot.y);

	Vector3 right = toVector(0.0, rot.y + 90.0);
	right.z = 0.0;
	axisY = right;

	axisZ = toVector(rot.x + 90.0, rot.y);
}

} // namespace

bool Engine::ProjectWorldLocationToScreen(
	Vector3 WorldLocation,
	Vector3& screen,
	const CameraCache& CameraInfo
)
{
	if (WorldLocation.x == 0.0 &&
		WorldLocation.y == 0.0 &&
		WorldLocation.z == 0.0)
		return false;

	if (CameraInfo.FOV <= 1.0f || CameraInfo.FOV > 179.0f)
		return false;

	Vector3 axisX{};
	Vector3 axisY{};
	Vector3 axisZ{};
	RotationGetAxes(CameraInfo.Rotation, axisX, axisY, axisZ);

	const Vector3 delta = WorldLocation - CameraInfo.Location;
	const double transformedX =
		delta.x * axisY.x + delta.y * axisY.y + delta.z * axisY.z;
	const double transformedY =
		delta.x * axisZ.x + delta.y * axisZ.y + delta.z * axisZ.z;
	const double transformedZ =
		delta.x * axisX.x + delta.y * axisX.y + delta.z * axisX.z;

	if (transformedZ < 1.0)
		return false;

	const double overlayW = static_cast<double>(
		g_projViewportW.load(std::memory_order_relaxed));
	const double overlayH = static_cast<double>(
		g_projViewportH.load(std::memory_order_relaxed));
	const double centerX = overlayW * 0.5;
	const double centerY = overlayH * 0.5;

	constexpr double kPi = 3.14159265358979323846;
	const double tanHalfFov = std::tan(static_cast<double>(CameraInfo.FOV) * kPi / 360.0);
	if (tanHalfFov < 0.001)
		return false;

	const double scale = centerY / tanHalfFov;
	screen.x = centerX + (transformedX * scale) / transformedZ;
	screen.y = centerY - (transformedY * scale) / transformedZ;

	if (!std::isfinite(screen.x) || !std::isfinite(screen.y))
		return false;
	if (screen.x < -overlayW * 0.5 || screen.x > overlayW * 1.5 ||
		screen.y < -overlayH * 0.5 || screen.y > overlayH * 1.5)
		return false;

	return true;
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

bool Engine::ProjectWorldLocationToRadar(
	const Vector3& myWorldLocation,
	const Vector3& enemyWorldLocation,
	float myYaw,
	Vector3& outRadar)
{
	Vector3 delta = enemyWorldLocation - myWorldLocation;
	delta.z = 0.0;

	const float distance = static_cast<float>(std::sqrt(delta.x * delta.x + delta.y * delta.y));

	const float radarRange = 80.0f;

	if (distance > radarRange)
	{
		delta.x *= radarRange / distance;
		delta.y *= radarRange / distance;
	}

	const float yawRad = static_cast<float>(DegToRad(static_cast<double>(myYaw)));

	const float cosYaw = static_cast<float>(std::cos(yawRad));
	const float sinYaw = static_cast<float>(std::sin(yawRad));

	const float forward = static_cast<float>(delta.x * cosYaw + delta.y * sinYaw);
	const float right = static_cast<float>(-delta.x * sinYaw + delta.y * cosYaw);

	outRadar.x = right / radarRange;
	outRadar.y = forward / radarRange;

	outRadar.z = static_cast<float>(distance / radarRange);

	return true;
}

bool Engine::Has(const std::string& s, const char* sub)
{
    return s.find(sub) != std::string::npos;
}

std::uintptr_t Engine::GetBoneArrayDecrypt(std::uintptr_t skeletalmesh)
{
    return steam_decrypt::GetBoneArrayDecrypt(skeletalmesh);
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
    return p != 0 && p != UINTPTR_MAX && Memory::IsValidPtrFast2(p);
}

static bool ValidatePlayerController(uintptr_t pc, uintptr_t& outPawn, bool requirePawnWorld)
{
    if (!IsUsableObjectPtr(pc))
        return false;

    const uintptr_t pawn = Memory::read<uintptr_t>(pc + Offsets::AcknowledgedPawn);
    const uintptr_t pcm = Memory::read<uintptr_t>(pc + Offsets::APlayerCameraManager);

    bool pcmOk = false;
    if (IsUsableObjectPtr(pcm)) {
        const float fov = Memory::read<float>(pcm + Offsets::DefaultFOV);
        const uintptr_t owner = Memory::read<uintptr_t>(pcm + Offsets::PCOwner);
        pcmOk = fov > 1.f && fov < 179.f && owner == pc;
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

static bool LooksLikeUtf16GarbageQword(uintptr_t p)
{
    if (!p)
        return true;
    int asciiPairs = 0;
    for (int i = 0; i < 4; ++i) {
        const unsigned char lo = static_cast<unsigned char>((p >> (i * 16)) & 0xFF);
        const unsigned char hi = static_cast<unsigned char>((p >> (i * 16 + 8)) & 0xFF);
        if (hi == 0 && lo >= 0x20 && lo < 0x7F)
            ++asciiPairs;
    }
    return asciiPairs >= 3;
}

static bool ValidateGameInstance(uintptr_t gi, uintptr_t* outLocalPlayer = nullptr,
    uintptr_t* outPlayerController = nullptr)
{
    if (!IsUsableObjectPtr(gi) || LooksLikeUtf16GarbageQword(gi))
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
        if (outLocalPlayer && IsUsableObjectPtr(slot0) && !LooksLikeUtf16GarbageQword(slot0))
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

uintptr_t Engine::GetGameInstance(uint64_t uworldAddr)
{
    if (!uworldAddr || !Memory::IsValidPtrFast2(uworldAddr))
        return 0;

    auto tryGi = [&](uintptr_t gi) -> uintptr_t {
        return ValidateGameInstance(gi) ? gi : 0;
    };

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

    const uintptr_t data = Memory::read<uintptr_t>(persistentLevel + Offsets::AActors);
    const int count = Memory::read<int>(persistentLevel + Offsets::ActorsCount);
    if (!data || count <= 0 || count > 10000)
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
        if (!IsUsableObjectPtr(lp) || LooksLikeUtf16GarbageQword(lp))
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

    const uintptr_t pcm = Memory::read<uintptr_t>(pc + Offsets::APlayerCameraManager);
    if (!pcm || !Memory::IsValidPtrFast2(pcm))
        return false;

    const float fov = Memory::read<float>(pcm + Offsets::DefaultFOV);
    return fov > 1.f && fov < 179.f;
}

bool Engine::ResolvePcFromLevelCameraManager(
    uintptr_t level,
    uintptr_t actorsData,
    uintptr_t& outPc,
    uintptr_t& outPawn,
    uintptr_t& outPcm)
{
    if (!level || !actorsData)
        return false;

    int actorCount = 0;
    uintptr_t resolvedActors = actorsData;
    if (!Engine::ResolveLevelActors(level, resolvedActors, actorCount))
        return false;

    const int scanLimit = (actorCount < 10000) ? actorCount : 10000;
    for (int i = 0; i < scanLimit; ++i) {
        const uintptr_t candidate = Memory::read<uintptr_t>(
            resolvedActors + static_cast<size_t>(i) * sizeof(uintptr_t));
        if (!candidate || !Memory::IsValidPtrFast2(candidate))
            continue;

        const float fov = Memory::read<float>(candidate + Offsets::DefaultFOV);
        if (fov <= 1.f || fov >= 179.f)
            continue;

        const uintptr_t owner = Memory::read<uintptr_t>(candidate + Offsets::PCOwner);
        if (!owner || !Memory::IsValidPtrFast2(owner))
            continue;

        const uintptr_t pawn = Memory::read<uintptr_t>(owner + Offsets::AcknowledgedPawn);
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

    const uintptr_t pawn = Memory::read<uintptr_t>(controller + Offsets::AcknowledgedPawn);
    if (!pawn || !Memory::IsValidPtrFast2(pawn) || !PawnHasWorldPosition(pawn))
        return false;

    const uintptr_t pcm = Memory::read<uintptr_t>(controller + Offsets::APlayerCameraManager);
    if (pcm && Memory::IsValidPtrFast2(pcm)) {
        const uintptr_t pcmOwner = Memory::read<uintptr_t>(pcm + Offsets::PCOwner);
        const float fov = Memory::read<float>(pcm + Offsets::DefaultFOV);
        if (pcmOwner == controller && fov > 1.f && fov < 179.f) {
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
	uintptr_t pc = 0;
	uintptr_t level = 0;
	uintptr_t actorsData = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		pc = PlayerController;
		level = PersistentLevel;
		actorsData = Actors;
	}

	if (!pc)
		return 0;

	const uintptr_t pcmDirect = Memory::read<uintptr_t>(pc + Offsets::APlayerCameraManager);
	if (pcmDirect && IsValidPointer(pcmDirect)) {
		const float fov = Memory::read<float>(pcmDirect + Offsets::DefaultFOV);
		if (fov > 1.f && fov < 179.f)
			return pcmDirect;
	}

	if (!level || !actorsData)
		return 0;

	int actorCount = 0;
	uintptr_t resolvedActors = 0;
	if (!ResolveLevelActors(level, resolvedActors, actorCount))
		return 0;
	if (resolvedActors != actorsData)
		actorsData = resolvedActors;

	const int scanLimit = (actorCount < 10000) ? actorCount : 10000;
	for (int i = 0; i < scanLimit; ++i) {
		const uintptr_t candidate = Memory::read<uintptr_t>(
			actorsData + static_cast<size_t>(i) * sizeof(uintptr_t));
		if (!candidate || !IsValidPointer(candidate))
			continue;

		if (Memory::read<uintptr_t>(candidate + Offsets::PCOwner) != pc)
			continue;

		const float fov = Memory::read<float>(candidate + Offsets::DefaultFOV);
		if (fov > 1.f && fov < 179.f)
			return candidate;
	}

	if (pcmDirect && IsValidPointer(pcmDirect))
		return pcmDirect;

	return 0;
}

// SHARED GATE — grep callers before edit
bool Engine::getAllowType(const std::string& actorName, int category) const
{
    // Radar-only mode must admit bots too (scanner already gates on show_radar).
    const bool wantBots = var::showRobots || var::robotAimEnabled || var::show_radar;

    if (robotsList.find(actorName) != robotsList.end())
        return wantBots;

    // category 3: RobotList — Lookup fname/display maps first, then accepted labels.
    if (wantBots && category == 3) {
        if (actorName.empty())
            return false;
        if (actorName == kBotStructAdmissionToken)
            return true;
        if (!LookupEnemyBotByFName(actorName).empty())
            return true;
        if (robotsList.find(actorName) == robotsList.end()) {
            const std::string fromDisplay = LookupEnemyBotDisplayLabel(actorName);
            if (!fromDisplay.empty())
                return true;
        }
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

std::string Engine::GetWeaponName(const std::string& internal_name) {
    if (internal_name.empty())
        return {};

    auto stripPrefixes = [](std::string name) -> std::string {
        static const char* kPrefixes[] = {
            "BP_WeaponActor_",
            "BP_Weapon_",
            "BP_WeaponActor",
            "BP_Weapon",
            "BP_ItemActor_",
            "BP_ItemActor",
        };
        for (const char* prefix : kPrefixes) {
            const size_t len = std::strlen(prefix);
            if (name.size() >= len && name.compare(0, len, prefix) == 0) {
                name = name.substr(len);
                break;
            }
        }
        if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0)
            name.resize(name.size() - 2);
        return name;
    };

    const std::string stripped = stripPrefixes(internal_name);

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
        { "LMG_Medium_01",                     "Torrente" },
        { "Special_BeamRifle_01",              "Equalizer" },
        { "Beam_01",                           "Equalizer"},
        { "SniperRifle_BoltAction_Medium_01",  "Osprey" },

        { "BasicMeleeWeapon",                  "Melee" },
        { "HealingHoT_Small",                  "Bandage" },
        { "HealingHoT_Improvised",             "Herbal Bandage" },
        { "AdrenalineShot",                    "Adrenaline Shot" },
        { "Consumable_ShieldOverTimePack",     "Shield Recharge" },
        { "Defibrillator",                     "Defibrillator" },
        { "JumpMine_Impulse",                  "Impulse Mine" },
        { "SmokeGrenade",                      "Smoke Grenade" },
        { "ScatterMissileGrenade",             "Wolfpack Grenade" },
        { "StunGrenade",                       "Showstopper Grenade" }
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
    if (!human.empty())
        return human;

    return {};
}

namespace {

bool FnameMatchesLootItemBucket(const std::string& actorNameLower)
{
    if (IsInventoryWorldFnameExcluded(actorNameLower))
        return false;
    if (actorNameLower.find("bp_pickupbase") != std::string::npos)
        return true;
    if (actorNameLower.find("pickupbase") != std::string::npos)
        return true;
    if (actorNameLower.find("bp_pickup") != std::string::npos)
        return true;
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
        s = FormatEspDisplayLabel(s);
        if (s.empty() || IsJunkWorldEspLabel(s) || IsGarbledEspLabel(s)
            || !IsPlausibleEspLabel(s))
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