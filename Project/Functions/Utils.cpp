#include "../Core/Engine.h"
#include "../Interface/Utils/UpgradeOptions.hpp"
#include "../Core/AgentLog.h"
#include "../Core/StringUtils.hpp"
#include <cmath>
#include <immintrin.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
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
#include "../Core/NameCrypto.hpp"
#include "../Core/WorldItemCategory.h"

#include "../Interface/OverlayHost.h"

#include <atomic>
#include <thread>
#include <type_traits>

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



// Class identity is independent of the codec-protected controller camera slot.
bool CameraManagerClass(uintptr_t object)
{
    uintptr_t cls = steam_decrypt::GetActorClassPtr(object);
    std::unordered_set<uintptr_t> seen;
    for (int depth = 0; cls && depth < 16 && seen.insert(cls).second; ++depth) {
        if (steam_decrypt::GetActorFNameString(cls) == "PlayerCameraManager")
            return true;
        cls = Memory::read<uintptr_t>(cls + Offsets::UStruct_SuperStruct);
    }
    return false;
}

// CL-1426848 dump: APlayerCameraManager::ViewTarget sits at 0x4C0, TViewTarget::POV
// at +0x10, and MinimalViewInfo carries Location 0x08, Rotation 0x28, FOV 0x48.
// Read the whole view in one complete nocache transaction; never mix a pawn-eye
// location or controller rotation with this camera, and never read handle keys.
bool ReadPlainCamera(uintptr_t pcm, Engine::CameraCache& result)
{
    if (!Memory::IsValidPtrFast2(pcm) || (pcm & 7)) return false;
    // 0x4C covers the last field the gate reads (FOV at POV+0x48) plus one float.
    uint8_t view[0x4C]{};
    if (!PCIMemory::ReadVirtualMemoryNoCache(pcm + Offsets::CameraPOVBase, view, sizeof(view)))
        return false;
    Engine::FVector3d loc{}, rot{}; float fov = 0;
    std::memcpy(&loc, view + (Offsets::CameraPOV_Location - Offsets::CameraPOVBase), sizeof(loc));
    std::memcpy(&rot, view + (Offsets::CameraPOV_Rotation - Offsets::CameraPOVBase), sizeof(rot));
    std::memcpy(&fov, view + (Offsets::CameraPOV_FOV - Offsets::CameraPOVBase), sizeof(fov));
    if (!IsPlausibleWorldPosD(loc) || !std::isfinite(fov) || fov <= 1.f || fov >= 179.f
        || !std::isfinite(rot.x) || !std::isfinite(rot.y) || !std::isfinite(rot.z)
        || std::fabs(rot.x) > 90.0 || std::fabs(rot.y) > 360.0 || std::fabs(rot.z) > 180.0)
        return false;
    result = { Engine::ToVector3(loc), Engine::ToVector3(rot), fov };
    static std::mutex logMutex;
    std::lock_guard<std::mutex> logLock(logMutex);
    static auto lastLog = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastLog >= std::chrono::seconds(2)) {
        lastLog = now;
        std::ostringstream f;
        f << "{\"ev\":\"camera_plain_sample\",\"pcm\":" << pcm << ",\"fov\":" << fov
          << ",\"loc\":[" << loc.x << "," << loc.y << "," << loc.z
          << "],\"rot\":[" << rot.x << "," << rot.y << "," << rot.z << "]}\n";
        EntityDiagnostics::LogRaw(f.str());
    }
    return true;
}

bool PcmLivePovLooksSane(uintptr_t pcm)
{
    Engine::CameraCache camera{};
    return ReadPlainCamera(pcm, camera);
}

/** Complete-read gate: a short or failed DMA read must never become a
 *  plausible zero value. On failure `value` is left untouched. */
template <typename T>
bool ReadComplete(uintptr_t address, T& value)
{
    T candidate{};
    if (!PCIMemory::ReadVirtualMemoryNoCache(address, &candidate, sizeof(T)))
        return false;
    value = candidate;
    return true;
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
    (void)pc; (void)pawn; (void)actors; (void)nocacheFov;
    pcm = GetCameraManagerFromActors(0, level, pc);
    return pcm != 0 && PcmLivePovLooksSane(pcm);
}

bool Engine::RefreshCameraFromViewTarget()
{
    const uintptr_t pcm = GetCameraManagerFromActors();
    CameraCache camera{};
    if (!pcm || !ReadPlainCamera(pcm, camera)) return false;
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        PlayerCameraManager = pcm;
    }
    {
        std::unique_lock<std::shared_mutex> cameraLock(m_cameraMutex);
        g_Camera = camera;
    }
    // #region agent log
    // Transform self-check (worker thread, one sample per 5 s). The local pawn's
    // own root sits within a few metres of the camera eye, so whichever candidate
    // translation offset lands nearest the POV is the live one — a stale
    // ComponentToWorld after a game patch shows up here as "every entity is
    // kilometres away" instead of as silently wrong boxes. Read-only: never
    // writes a position, never gates a draw.
    {
        static std::mutex probeMutex;
        static auto lastProbe = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> probeLock(probeMutex);
        if (now - lastProbe >= std::chrono::seconds(5)) {
            lastProbe = now;
            uintptr_t pawn = 0;
            {
                std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
                pawn = AcknowledgedPawn;
            }
            const uintptr_t root = (pawn && IsUsableObjectPtr(pawn))
                ? Memory::read_nocache<uintptr_t>(pawn + Offsets::RootComponent) : 0;
            if (root && Memory::IsValidPtrFast2(root)) {
                static const std::ptrdiff_t kCandidates[] = {
                    0x2F0, 0x300, 0x310, 0x320, 0x330, 0x340, 0x350 };
                std::ostringstream f;
                f << "{\"ev\":\"pawn_transform_probe\",\"root\":" << root
                  << ",\"cam\":[" << camera.Location.x << "," << camera.Location.y
                  << "," << camera.Location.z << "]";
                for (const std::ptrdiff_t off : kCandidates) {
                    const FVector3d v = Memory::read_nocache<FVector3d>(root + off);
                    const double dx = v.x - static_cast<double>(camera.Location.x);
                    const double dy = v.y - static_cast<double>(camera.Location.y);
                    const double dz = v.z - static_cast<double>(camera.Location.z);
                    const double dm = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
                    char key[16];
                    std::snprintf(key, sizeof(key), "0x%llX",
                        static_cast<unsigned long long>(off));
                    f << ",\"" << key << "\":{\"v\":[" << v.x << "," << v.y
                      << "," << v.z << "],\"dM\":" << dm << "}";
                }
                f << "}\n";
                EntityDiagnostics::LogRaw(f.str());
            }
        }
    }
    // #endregion
    return true;
}

bool Engine::TryBuildCameraFromPcmPov(uintptr_t pcm, CameraCache& outCamera) const
{
    return ReadPlainCamera(pcm, outCamera);
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
	// T4.5: the FOV axis is a setting, not an assumption. Vertical (the default)
	// is the reading this projection has always used, so nothing moves unless the
	// user asks for the horizontal reading that the aim cone already uses.
	const EngineProjection::FovAxis axis = UpgradeOptions::fovAxisHorizontal
		? EngineProjection::FovAxis::Horizontal
		: EngineProjection::FovAxis::Vertical;
	return EngineProjection::ProjectWorldLocationToScreen(
		WorldLocation,
		screen,
		CameraInfo.Location,
		CameraInfo.Rotation,
		CameraInfo.FOV,
		overlayW,
		overlayH,
		axis);
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
        // The engine's own local-controller flag is the stronger proof of the two
        // and the one that does not depend on the camera or on a replicated pawn:
        // bIsLocalPlayerController (0xDB8, reflected for the APlayerController
        // this game spawns) is set when the engine creates the controller from a
        // ULocalPlayer. Requiring the POV check here rejected the *real* local
        // player on this build, because the manager's POV is not readable at its
        // SDK offsets — the arc capture records the manager holding an encoded
        // handle to a heap FCameraCacheEntry instead. The live session showed the
        // consequence: a controller and its PlayerState resolved while no pawn
        // had replicated yet, so every GameInstance candidate failed this gate.
        if (!engine.IsLocalPlayerControllerConfirmed(pc)) {
            if (!IsUsableObjectPtr(pawn) || !Engine::ResolveActorRoot(pawn))
                return false;
        }
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

    // SDK (Engine.GameInstance::LocalPlayers): reflected array is at 0x108.
    // Keep 0x120 only as an unverified prior-build fallback; do not prefer it.
    static const std::ptrdiff_t kLpArrOffs[] = { Offsets::LocalPlayers };
    for (std::ptrdiff_t off : kLpArrOffs) {
        if (TryLocalPlayerChain(gi, off, lp, pc)) {
            if (outLocalPlayer)
                *outLocalPlayer = lp;
            if (outPlayerController)
                *outPlayerController = pc;
            return true;
        }
    }

    (void)world; // No current-build GI->World back-reference is verified.
    for (std::ptrdiff_t off : kLpArrOffs) {
        const GITArrayHdr arr = ReadGIArray(gi, off);
        if (!IsUsableObjectPtr(arr.Data) || IsModuleSpace(arr.Data)
            || arr.Num < 1 || arr.Num > 4 || arr.Max < arr.Num || arr.Max > 4)
            continue;
        uintptr_t lp = 0, pc = 0, pawn = 0;
        if (!TryLocalPlayersSlots(arr.Data, arr.Num, lp, pc, pawn, false))
            continue;
        if (outLocalPlayer) *outLocalPlayer = lp;
        if (outPlayerController) *outPlayerController = pc;
        return true;
    }

    // Menu / pre-spawn: the engine has not created a PlayerController yet, so
    // every PC gate above fails and the GI is rejected, leaving LocalPlayer at 0.
    // Accept the reflected LocalPlayers array when a slot is a live ULocalPlayer
    // whose PlayerController is still null (or is itself a live object).
    for (std::ptrdiff_t off : kLpArrOffs) {
        const GITArrayHdr arr = ReadGIArray(gi, off);
        if (!IsUsableObjectPtr(arr.Data) || IsModuleSpace(arr.Data)
            || arr.Num < 1 || arr.Num > 4 || arr.Max < arr.Num || arr.Max > 4)
            continue;
        for (int i = 0; i < arr.Num; ++i) {
            const uintptr_t slot =
                Memory::read<uintptr_t>(arr.Data + static_cast<size_t>(i) * sizeof(uintptr_t));
            if (!IsUsableObjectPtr(slot) || Engine::LooksLikeUtf16Garbage(slot))
                continue;
            const uintptr_t pcField =
                Memory::read<uintptr_t>(slot + Offsets::LocalPlayer_PlayerController);
            if (pcField && !IsUsableObjectPtr(pcField))
                continue;
            if (outLocalPlayer) *outLocalPlayer = slot;
            if (outPlayerController) *outPlayerController = pcField;
            return true;
        }
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

// Prior-build fallback: older builds stored a SIMD-encrypted GI pointer at
// UWorld+0x478; decrypt that blob with the legacy PSHUFB pipeline.
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
    const uintptr_t plain = Memory::read_nocache<uintptr_t>(world + Offsets::OwningGameInstance);
    if (!IsUsableObjectPtr(plain))
        return 0;
    // +0x478 is the SDK's UWorld::OwningGameInstance slot (Engine.World). The drop does not
    // define a GI->World field at +0x2F0, so identity comes from LocalPlayers,
    // not from that stale back-reference guess.
    if (ValidateGameInstance(plain, nullptr, nullptr, world)) {
        backRef = true;
        return plain;
    }
    return 0;
}

/** Retired rung: SIMD slot decode superseded by PlayerChain::GiFromTls. */
static constexpr uintptr_t GiFromDecryptSlot(uintptr_t) { return 0; }

/** Retired rung: Legacy 0x3B0 slot retired in CL-1424961+. */
static constexpr uintptr_t GiFromLegacySlotRung(uintptr_t) { return 0; }

/** Last resort: scan UWorld for a slot whose GI chain validates. */
static uintptr_t GiFromScanRung(uintptr_t world)
{
    // A miss costs ~224 uncached reads plus validation of every usable slot. It
    // ran on every tick while the GI was unknown, so retry it on a cadence.
    static constexpr auto kGiScanRetry = std::chrono::milliseconds(1500);
    static uintptr_t s_missWorld = 0;
    static std::chrono::steady_clock::time_point s_nextTry{};
    const auto now = std::chrono::steady_clock::now();
    if (world == s_missWorld && now < s_nextTry)
        return 0;

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
    if (loose) {
        s_missWorld = 0;
        return loose;
    }
    s_missWorld = world;
    s_nextTry = std::chrono::steady_clock::now() + kGiScanRetry;
    return 0;
}

// Rate-limited record of the LocalPlayers array a GI candidate presented, so a
// live raid log shows the real pointer/count instead of only the reject label.
static void LogLpArrayReject(uintptr_t gi, uintptr_t arrData, int32_t num, int32_t max, const char* reason)
{
    static std::chrono::steady_clock::time_point s_nextLog{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextLog)
        return;
    s_nextLog = now + std::chrono::seconds(2);
    std::ostringstream f;
    f << "{\"ev\":\"gi_lp_array_reject\",\"reason\":\"" << reason
      << "\",\"gi\":" << gi << ",\"arr\":" << arrData
      << ",\"num\":" << num << ",\"max\":" << max << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

// Names the first check a GameInstance candidate fails, mirroring the gates in
// ValidateGameInstance, so the TLS rung log says *why* a candidate was rejected.
static const char* GiRejectReason(uintptr_t gi)
{
    if (!gi)
        return "null";
    if (!IsUsableObjectPtr(gi))
        return "not_usable_ptr";
    if (Engine::LooksLikeUtf16Garbage(gi))
        return "utf16_garbage";
    if (IsModuleSpace(gi))
        return "module_space";
    const uintptr_t arrData = Memory::read<uintptr_t>(gi + Offsets::LocalPlayers);
    const int32_t num = Memory::read<int32_t>(gi + Offsets::LocalPlayers + 8);
    const int32_t max = Memory::read<int32_t>(gi + Offsets::LocalPlayers + 12);
    if (!IsUsableObjectPtr(arrData) || IsModuleSpace(arrData)) {
        LogLpArrayReject(gi, arrData, num, max, "lp_array_data_bad");
        return "lp_array_data_bad";
    }
    if (num < 1 || num > 4) {
        LogLpArrayReject(gi, arrData, num, max, "lp_num_out_of_range");
        return "lp_num_out_of_range";
    }
    if (max < num || max > 4)
        return "lp_max_out_of_range";
    bool anySlot = false;
    for (int i = 0; i < num; ++i) {
        const uintptr_t slot =
            Memory::read<uintptr_t>(arrData + static_cast<size_t>(i) * sizeof(uintptr_t));
        if (IsUsableObjectPtr(slot)) {
            anySlot = true;
            break;
        }
    }
    if (!anySlot)
        return "lp_slots_unusable";
    return "lp_slots_rejected";
}

// ── GameInstance rung: the per-thread TLS block ──────────────────────────────
// There is no GameInstance pointer in UWorld on this build. The arc capture
// (sdk/arc_decrypt_export.json) swept UWorld +0x8..+0x800 for a plain pointer and
// for an encoded one and recorded "not found" for both, and the engine sweep
// (+0x8..+0x2000) found none either — so a GI that is only reachable from the
// world is exactly the rung that cannot answer here. What does is the TLS group of
// the thread that created the GameInstance: Core/NameCrypto.hpp carries the
// v20261009 / CL-1424961 layout (validity flag at +0xEC0, stage-A blocks from
// +0xED0, 8 x 0x80 stride, GI at block #2 + 0x18 via that block's vtable pointer).
// Every candidate still has to pass ValidateGameInstance, so a thread whose TLS
// block belongs to another subsystem, or a build whose layout moved, publishes
// nothing rather than a plausible-looking number.
struct GiTlsReader {
    bool Read(uint64_t addr, void* dst, size_t n) const
    {
        return steam_decrypt::MemRead(addr, dst, n);
    }
};

// The thread map plus the TLS slot scan cost ~1.1k reads PER THREAD
// (FindTlsBaseRef walks the whole slot array) across ~170 target threads, so one
// derivation is seconds of DMA. It used to run inline on the game tick whenever
// the GameInstance was unknown (hub / pre-pair): one Update then held the shared
// scan gate for ~7-12s and starved every other scanner — the bot/player blink
// and the "34s to first ESP". The derivation now runs on a background worker; the
// rung only reads the published hit and re-proves it with the cheap
// ValidateGameInstance each tick, so no tick ever blocks on the walk.
static std::atomic<uintptr_t> g_giTlsHit{ 0 };
static constexpr uint64_t kTlsRetryMs = 1500;

static uint64_t SteadyMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Worker-only. One full walk of every target thread's TLS block; returns the
// first GameInstance that validates. Never runs on a game tick, so its seconds
// of DMA cannot hold the scan gate.
static bool GiTlsScanOnce(uintptr_t& outGi)
{
    outGi = 0;
    if (!Memory::getProcessID())
        return false;

    const GiTlsReader reader;
    // The GameInstance thread is not necessarily the lowest TID, so scan every
    // thread's TLS block. For each, try the CL-1424961 reference decrypt, then
    // brute-force the stage-A blocks for a pointer that validates as a GI.
    const std::vector<uint64_t> tebs = Memory::GetAllTargetTebs();
    uintptr_t bestTlsBase = 0;
    int scanned = 0;
    // One record per thread plus one summary. The logger drops any single line
    // over 16 KB whole, so one record covering every thread would vanish.
    auto logSummary = [&](uintptr_t gi) {
        std::ostringstream s;
        s << "{\"ev\":\"gi_tls_probe\",\"tebs\":" << tebs.size() << ",\"scanned\":" << scanned
          << ",\"bestTlsBase\":" << bestTlsBase << ",\"gi\":" << gi << "}\n";
        EntityDiagnostics::LogRaw(s.str());
    };
    for (const uint64_t teb : tebs) {
        const uintptr_t tlsBase = static_cast<uintptr_t>(
            namecrypto::FindTlsBaseRef(teb, reader));
        if (!tlsBase)
            continue;
        ++scanned;
        if (!bestTlsBase)
            bestTlsBase = tlsBase;

        // Per-thread record: how many candidates were rejected, and by which gate.
        std::map<std::string, int> rejects;
        auto check = [&](const char* src, uintptr_t v) -> bool {
            if (ValidateGameInstance(v, nullptr, nullptr, 0))
                return true;
            ++rejects[std::string(src) + ":" + GiRejectReason(v)];
            return false;
        };
        auto probeStageA = [&]() -> uintptr_t {
            // Brute-force: a plain (or singly-de-referenced) GI pointer anywhere in
            // the stage-A block group.
            const uintptr_t stageA = tlsBase + 0xED0;
            for (int b = 0; b < 8; ++b) {
                const uintptr_t p = stageA + static_cast<uintptr_t>(b) * 0x80;
                for (std::ptrdiff_t off = 0; off <= 0x78; off += 8) {
                    const uintptr_t v = Memory::read_nocache<uintptr_t>(p + off);
                    if (!IsUsableObjectPtr(v))
                        continue;
                    if (check("direct", v))
                        return v;
                    const uintptr_t d = Memory::read_nocache<uintptr_t>(v);
                    if (IsUsableObjectPtr(d) && check("deref", d))
                        return d;
                    const uintptr_t d18 = Memory::read_nocache<uintptr_t>(v + 0x18);
                    if (IsUsableObjectPtr(d18) && check("deref18", d18))
                        return d18;
                }
            }
            return 0;
        };

        uint8_t validFlag = 0;
        reader.Read(tlsBase + 0xEC0, &validFlag, 1);

        uintptr_t found = static_cast<uintptr_t>(
            namecrypto::DecryptGameInstanceTlsRef(teb, reader));
        if (!check("ref", found))
            found = 0;
        if (!found) {
            found = static_cast<uintptr_t>(
                namecrypto::DecryptGameInstanceTls(teb, reader));
            if (!check("decrypt", found))
                found = 0;
        }
        if (!found)
            found = probeStageA();

        // One line per thread, so each stays under the logger's per-line cap.
        std::ostringstream t;
        t << "{\"ev\":\"gi_tls_thread\",\"teb\":" << teb << ",\"tlsBase\":" << tlsBase
          << ",\"validFlag\":" << static_cast<int>(validFlag) << ",\"rejects\":{";
        bool firstReject = true;
        for (const auto& [key, count] : rejects) {
            t << (firstReject ? "" : ",") << "\"" << key << "\":" << count;
            firstReject = false;
        }
        t << "}";
        if (found)
            t << ",\"gi\":" << found;
        t << "}\n";
        EntityDiagnostics::LogRaw(t.str());

        if (found) {
            outGi = found;
            logSummary(found);
            return true;
        }
    }
    logSummary(0);
    return false;
}

// Background GameInstance TLS resolver: scans once, publishes the hit, re-proves
// it cheaply while it holds, and retries the walk after kTlsRetryMs on a miss.
static void GiTlsWorker()
{
    for (;;) {
        const uintptr_t hit = g_giTlsHit.load(std::memory_order_acquire);
        if (hit) {
            if (ValidateGameInstance(hit, nullptr, nullptr, 0)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            g_giTlsHit.store(0, std::memory_order_release);
        }
        uintptr_t found = 0;
        if (GiTlsScanOnce(found))
            g_giTlsHit.store(found, std::memory_order_release);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(kTlsRetryMs));
    }
}

// The rung is a pure getter now: kick the worker once, then read its published,
// cheaply re-proved hit. A miss costs one ValidateGameInstance, never the walk.
static uintptr_t GiFromTlsRung()
{
    static std::once_flag s_startOnce;
    std::call_once(s_startOnce, [] { std::thread(GiTlsWorker).detach(); });
    const uintptr_t hit = g_giTlsHit.load(std::memory_order_acquire);
    if (hit && ValidateGameInstance(hit, nullptr, nullptr, 0))
        return hit;
    return 0;
}

// ── GameInstance rung: the GUObjectArray class scan ───────────────────────────
// No UWorld slot can answer on this build: sdk/arc_decrypt_export.json (runtime
// capture) swept UWorld +0x8..+0x800 for a plain pointer and for an encoded one
// and recorded "not found" for both, and the engine sweep (+0x8..+0x2000) found
// none either — the same reason GiFromScan and the TLS probe are dry here. What
// the capture *did* resolve the GI through is the object array itself, matched
// by class (BP_PioneerGameInstance_C). So the deterministic answer to "is there
// a GameInstance?" is: walk GUObjectArray, keep objects whose *class* name
// contains "GameInstance", and prove each with its reflected LocalPlayers array
// (the 0x108 slot is SDK-confirmed). Unlike ValidateGameInstance this proof
// needs no live controller, so the GI — which exists from engine init, before
// the match — resolves within seconds of launch. The walk is cursor-based and
// rate-limited (~82k candidates/s, one chunk at a time), and a hit is re-proved
// by the class + array check on every tick, so a stale pointer cannot survive a
// world change. The registry/chunk decode below is the same one the
// camera-manager fallback (Engine::GetCameraManagerFromActors) proves; it is
// deliberately self-contained rather than factored, because that walk steps
// @256 items/500ms and this one needs a ~300x faster cadence to honor the
// verify panel's 10 s requirement.
struct GiObjectScanCursor {
    uint32_t cursor = 0;              // object index since the last decode
    uintptr_t gi = 0;                 // accepted GameInstance
    uintptr_t giClass = 0;            // its class ptr (re-proved from, no re-decode)
    std::unordered_map<uintptr_t, bool> classIsGi;   // class ptr -> match
    uint32_t classMatches = 0;        // GameInstance-named objects this pass
    uint32_t arrayRejects = 0;        // ...of which failed the LocalPlayers proof
    uint32_t readFails = 0;           // failed chunk/item reads this pass
    uint64_t nextFailLogMs = 0;       // rate limit for read-failure logging
};

// Memoized class-name filter: the array holds ~642k objects but ~19k distinct
// classes, so GetActorFNameString (a real name-table walk) runs once per class.
static uintptr_t GiClassIfMatch(uintptr_t object,
    std::unordered_map<uintptr_t, bool>& cache)
{
    const uintptr_t cls = steam_decrypt::GetActorClassPtr(object);
    if (!cls)
        return 0;
    const auto it = cache.find(cls);
    if (it != cache.end())
        return it->second ? cls : 0;
    const std::string cn = steam_decrypt::GetActorFNameString(cls);
    const bool match = !cn.empty()
        && cn.find("GameInstance") != std::string::npos
        && cn.find("Default__") == std::string::npos
        && cn.find("Subsystem") == std::string::npos;
    cache.emplace(cls, match);
    return match ? cls : 0;
}

// Cheap identity proof that works before the controller exists: the known-
// GameInstance class plus a sane reflected LocalPlayers TArray at 0xF8 whose
// first slots look like LocalPlayer objects (the array exists from engine
// spawn, so this holds even with no controller yet). It is strictly weaker than
// ValidateGameInstance (which wants the LP->PC chain and is the gate for every
// other rung), but it is exactly what engine-spawn-time can provide, and the
// full chain is re-proved by the normal ladder once the controller is up.
static bool LooksLikeGameInstanceByClass(uintptr_t gi, uintptr_t giClass)
{
    if (!IsUsableObjectPtr(gi) || Engine::LooksLikeUtf16Garbage(gi) || IsModuleSpace(gi))
        return false;
    const uintptr_t arrData = Memory::read<uintptr_t>(gi + Offsets::LocalPlayers);
    const int32_t arrNum = Memory::read<int32_t>(gi + Offsets::LocalPlayers + 8);
    const int32_t arrMax = Memory::read<int32_t>(gi + Offsets::LocalPlayers + 12);
    if (!IsUsableObjectPtr(arrData) || IsModuleSpace(arrData)
        || arrNum < 1 || arrNum > 4 || arrMax < arrNum || arrMax > 4)
        return false;
    const int limit = (arrNum > 4) ? 4 : arrNum;
    for (int i = 0; i < limit; ++i) {
        const uintptr_t slot =
            Memory::read<uintptr_t>(arrData + static_cast<size_t>(i) * sizeof(uintptr_t));
        if (IsUsableObjectPtr(slot) && !Engine::LooksLikeUtf16Garbage(slot))
            return giClass != 0;
    }
    return false;
}

// Header exits used to be silent, so a scan that never reached its chunk loop
// looked identical to a scan that ran and found nothing. Rate-limited log.
static uintptr_t GiObjectScanHeaderFail(const char* stage, uint64_t detail)
{
    static uint64_t s_nextLogMs = 0;
    const uint64_t now = SteadyMs();
    if (now >= s_nextLogMs) {
        s_nextLogMs = now + 5000;
        std::ostringstream f;
        f << "{\"ev\":\"gi_object_scan_header_fail\",\"stage\":\"" << stage
          << "\",\"detail\":" << detail << "}\n";
        EntityDiagnostics::LogRaw(f.str());
    }
    return 0;
}

// Scans one chunk (up to 8192 items) and returns the first accepted
// GameInstance, or 0. Runs only on the background worker below, so it is not
// budgeted per tick; the worker owns the cursor exclusively.
static uintptr_t GiObjectScanStep(GiObjectScanCursor& s)
{
    const uint64_t now = SteadyMs();

    const uintptr_t base = Memory::getBaseAddress();
    uint32_t enc[2]{}, x[2]{}, a[2]{};
    if (!base)
        return GiObjectScanHeaderFail("base", 0);
    // Each read reports its own address so a failing log line names the read.
    if (!PCIMemory::ReadVirtualMemoryNoCache(base + Offsets::GObjectsRva, enc, sizeof(enc)))
        return GiObjectScanHeaderFail("gobjects_enc_read", base + Offsets::GObjectsRva);
    if (!PCIMemory::ReadVirtualMemoryNoCache(base + Offsets::GObjectsKey1Rva, x, sizeof(x)))
        return GiObjectScanHeaderFail("gobjects_key1_read", base + Offsets::GObjectsKey1Rva);
    if (!PCIMemory::ReadVirtualMemoryNoCache(base + Offsets::GObjectsKey2Rva, a, sizeof(a)))
        return GiObjectScanHeaderFail("gobjects_key2_read", base + Offsets::GObjectsKey2Rva);
    const uintptr_t registry =
        static_cast<uint64_t>(namecrypto::Rol32(enc[0] ^ x[0], 29) + a[0])
        | (static_cast<uint64_t>(namecrypto::Rol32(enc[1] ^ x[1], 29) + a[1]) << 32);
    uint32_t countEnc = 0;
    uint64_t chunksEnc = 0;
    if (!Memory::IsValidPtrFast2(registry))
        return GiObjectScanHeaderFail("registry_ptr", registry);
    if (!ReadComplete(registry + 0x14, countEnc)
        || !ReadComplete(registry + 0x20, chunksEnc))
        return GiObjectScanHeaderFail("registry_read", registry);
    const uint32_t count = _byteswap_ulong(countEnc ^ 0xA4E88268u);
    const uintptr_t chunks = _byteswap_uint64(chunksEnc ^ 0x26D0446400000000ULL);
    if (count < 1 || count > 2000000)
        return GiObjectScanHeaderFail("count", count);
    if (!Memory::IsValidPtrFast2(chunks))
        return GiObjectScanHeaderFail("chunks_ptr", chunks);

    if (s.cursor >= count) {
        // A full pass finished with no hit: log it so a "GI still red after 10 s"
        // report can tell "scan never completed" from "scan completed, no match".
        std::ostringstream f;
        f << "{\"ev\":\"gi_object_scan_pass\",\"count\":" << count
          << ",\"classMatches\":" << s.classMatches
          << ",\"arrayRejects\":" << s.arrayRejects
          << ",\"readFails\":" << s.readFails << "}\n";
        EntityDiagnostics::LogRaw(f.str());
        s.classMatches = 0;
        s.arrayRejects = 0;
        s.readFails = 0;
        s.cursor = 0;
    }
    constexpr uint32_t kItemsPerChunk = 0x10000;
    const uint32_t capacity = kItemsPerChunk - (s.cursor % kItemsPerChunk);
    const uint32_t n = (std::min)(8192u, (std::min)(count - s.cursor, capacity));
    uintptr_t chunk = 0;
    if (!ReadComplete(chunks + (s.cursor / kItemsPerChunk) * 8, chunk)
        || !Memory::IsValidPtrFast2(chunk)) {
        ++s.readFails;
        if (now >= s.nextFailLogMs) {
            s.nextFailLogMs = now + 5000;
            std::ostringstream f;
            f << "{\"ev\":\"gi_object_scan_read_fail\",\"stage\":\"chunk\",\"cursor\":" << s.cursor << "}\n";
            EntityDiagnostics::LogRaw(f.str());
        }
        s.cursor = 0;
        return 0;
    }
    std::vector<uint8_t> items(static_cast<size_t>(n) * 24);
    if (!PCIMemory::ReadVirtualMemoryNoCache(
            chunk + static_cast<size_t>(s.cursor % kItemsPerChunk) * 24,
            items.data(), items.size())) {
        ++s.readFails;
        if (now >= s.nextFailLogMs) {
            s.nextFailLogMs = now + 5000;
            std::ostringstream f;
            f << "{\"ev\":\"gi_object_scan_read_fail\",\"stage\":\"items\",\"cursor\":" << s.cursor << "}\n";
            EntityDiagnostics::LogRaw(f.str());
        }
        return 0;
    }
    const uint32_t batchBase = s.cursor;
    s.cursor += n;

    for (uint32_t i = 0; i < n; ++i) {
        for (const std::ptrdiff_t off : { std::ptrdiff_t{8}, std::ptrdiff_t{0} }) {
            uintptr_t object = 0;
            std::memcpy(&object,
                items.data() + static_cast<size_t>(i) * 24 + off, sizeof(object));
            if (!IsUsableObjectPtr(object))
                continue;
            const uintptr_t cls = GiClassIfMatch(object, s.classIsGi);
            if (!cls)
                continue;
            ++s.classMatches;
            if (!LooksLikeGameInstanceByClass(object, cls)) {
                ++s.arrayRejects;
                continue;
            }
            std::ostringstream f;
            f << "{\"ev\":\"gi_object_scan_hit\",\"gi\":" << object
              << ",\"cls\":" << cls << ",\"index\":" << (s.cursor - n + i) << "}\n";
            EntityDiagnostics::LogRaw(f.str());
            s.gi = object;
            s.giClass = cls;
            return object;
        }
    }
    return 0;
}

// Published result of the background scan. Written only by the worker thread;
// the game-tick rung just reads it, so a scan never blocks a tick.
static std::atomic<uintptr_t> g_giObjectScanHit{ 0 };

// Background GameInstance scan: runs chunks back-to-back at DMA speed, yields
// briefly between chunks, rechecks an accepted GI every 250 ms, and restarts
// the pass (after a short pause) when it is invalidated or no GI is found.
static void GiObjectScanWorker()
{
    GiObjectScanCursor s;
    for (;;) {
        if (s.gi) {
            if (LooksLikeGameInstanceByClass(s.gi, s.giClass)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            g_giObjectScanHit.store(0, std::memory_order_release);
            s = GiObjectScanCursor{};
        }
        const uint32_t before = s.cursor;
        const uintptr_t hit = GiObjectScanStep(s);
        if (hit) {
            g_giObjectScanHit.store(hit, std::memory_order_release);
            continue;
        }
        // No progress means a header or chunk read failed: back off so a dead
        // scan does not hammer DMA. Progress just yields to other threads.
        if (s.cursor == before || s.cursor == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        else
            std::this_thread::yield();
    }
}

static uintptr_t GiFromObjectScanRung()
{
    static std::once_flag s_startOnce;
    std::call_once(s_startOnce, [] { std::thread(GiObjectScanWorker).detach(); });
    return g_giObjectScanHit.load(std::memory_order_acquire);
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
    {
        // Rate-limited probe: raw PC+0x4B0, decrypted LP, and LP+0xA0 back-ref.
        static uint64_t s_lpProbeNextMs = 0;
        const uint64_t probeNow = SteadyMs();
        if (probeNow >= s_lpProbeNextMs) {
            s_lpProbeNextMs = probeNow + 5000;
            const uintptr_t raw = Memory::read<uintptr_t>(playerController + 0x4B0);
            const uintptr_t back = IsUsableObjectPtr(decrypted)
                ? Memory::read<uintptr_t>(decrypted + 0xA0) : 0;
            std::ostringstream f;
            f << "{\"ev\":\"lp_from_controller\",\"pc\":" << playerController
              << ",\"raw4B0\":" << raw << ",\"decrypted\":" << decrypted
              << ",\"backA0\":" << back << ",\"backMatches\":"
              << ((back == playerController) ? "true" : "false") << "}\n";
            EntityDiagnostics::LogRaw(f.str());
        }
    }
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
// sdk/sdk.txt (2026-10-09) reflects APlayerController::bIsLocalPlayerController at
// 0xDB8 (byte 0xDB8, mask 0x1), and Angelscript.PioneerPlayerController — the
// class this game actually spawns — inherits it at the same offset. That flag is
// the engine's own "this controller belongs to a local player" bit, set when the
// controller is created from a ULocalPlayer, so it identifies the local PC on its
// own: no pawn, no PlayerCameraManager, no FOV heuristics.
//    // The one caveat is that a random object read at +0xDB8 can have the bit set by

// chance, so the caller can require a second field (PlayerState) as a cheap
// "this really is an AController" proof.
// The flag byte alone is not proof of a PlayerController: other objects carry the
// same byte. Require a PlayerController ancestor in the object's class chain.
static bool ObjectIsPlayerControllerClass(uintptr_t object)
{
    uintptr_t cls = steam_decrypt::GetActorClassPtr(object);
    std::unordered_set<uintptr_t> seen;
    for (int depth = 0; cls && depth < 16 && seen.insert(cls).second; ++depth) {
        if (steam_decrypt::GetActorFNameString(cls) == "PlayerController")
            return true;
        cls = Memory::read<uintptr_t>(cls + Offsets::UStruct_SuperStruct);
    }
    return false;
}

bool Engine::IsLocalPlayerController(uintptr_t pc)
{
    if (!pc || !Memory::IsValidPtrFast2(pc))
        return false;

    const uint8_t flags = Memory::read<uint8_t>(
        pc + Offsets::PlayerController_bIsLocalPlayerController);
    if ((flags & Offsets::PlayerController_bIsLocalPlayerController_Mask) == 0)
        return false;
    return ObjectIsPlayerControllerClass(pc);
}

bool Engine::IsLocalPlayerControllerConfirmed(uintptr_t pc)
{
    if (!IsLocalPlayerController(pc))
        return false;
    const uintptr_t playerState = Memory::read_nocache<uintptr_t>(
        pc + Offsets::AController_PlayerState);
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

    // Single rotating, time-budgeted sweep with all three identity checks per
    // actor, in priority order:
    //   (1) dump-verified bIsLocalPlayerController (0xDB8) — engine's own flag,
    //       outranks every heuristic; PlayerState proves AController; a pawned
    //       controller is preferred but a hub/lobby one with no pawn still counts.
    //   (2) PCM @ PC+0x48 (backup strong path).
    //   (3) class-name sniff (last resort).
    // The original ran three independent full ~2k-actor passes on EACH 16ms
    // Update tick. Every per-actor property read is a DMA round-trip, so while
    // the controller was unresolvable (party/enter wait, cold cache) one Update
    // call held the shared scan gate ~9-12s and starved RobotList/EntityList —
    // the "34s to first bot ESP" and bots/players blinking out. Capping the
    // sweep by wall time keeps the gate open; the cheap flag check still answers
    // as soon as the local controller's actor falls inside a window.
    static int s_cursor = 0;
    if (s_cursor < 0 || s_cursor >= scanLimit)
        s_cursor = 0;
    static uintptr_t s_noPawnPc = 0;
    const bool constsOk = InitConsts();
    const uint64_t deadline = SteadyMs() + 40;
    int i = s_cursor;
    for (; i < scanLimit; ++i) {
        if (SteadyMs() >= deadline)
            break;
        const uint64_t actor = actors[static_cast<size_t>(i)];
        if (!actor || !IsValidPointer(actor))
            continue;

        // (1) dump-verified identity: the engine's own local-controller flag.
        if (IsLocalPlayerControllerConfirmed(actor)) {
            const uintptr_t pawn = ReadAcknowledgedPawn(actor);
            if (pawn && IsValidPointer(pawn) && PawnHasWorldPosition(pawn)) {
                outController = actor;
                outPawn = pawn;
                s_noPawnPc = 0;
                s_cursor = (i + 1 >= scanLimit) ? 0 : i + 1;
                return true;
            }
            if (!s_noPawnPc)
                s_noPawnPc = actor;
        }

        // (2) PCM @ PC+0x48.
        if (TryControllerAndPawn(actor, outController, outPawn, true)) {
            s_noPawnPc = 0;
            s_cursor = (i + 1 >= scanLimit) ? 0 : i + 1;
            return true;
        }

        // (3) class-name sniff (last resort).
        if (constsOk) {
            const std::string fname = GetActorFNameString(actor);
            if (fname.find("PioneerPlayerController") != std::string::npos
                || fname.find("BP_PioneerPlayerController") != std::string::npos) {
                if (TryControllerAndPawn(actor, outController, outPawn, true)) {
                    s_noPawnPc = 0;
                    s_cursor = (i + 1 >= scanLimit) ? 0 : i + 1;
                    return true;
                }
            }
        }
    }
    const bool wrapped = (i >= scanLimit);
    s_cursor = wrapped ? 0 : i;
    // A no-pawn flagged controller is only reported after a full sweep found no
    // pawned one, so a mid-array window cannot shadow the real local controller.
    if (wrapped && s_noPawnPc) {
        outController = s_noPawnPc;
        const uintptr_t pawn = ReadAcknowledgedPawn(s_noPawnPc);
        outPawn = (pawn && IsValidPointer(pawn)) ? pawn : 0;
        s_noPawnPc = 0;
        return true;
    }

    return false;
}

uintptr_t Engine::GetCameraManagerFromActors(uintptr_t worldOverride,
    uintptr_t levelOverride, uintptr_t pcOverride)
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static uint64_t oldGeneration = UINT64_MAX;
    static uintptr_t cached = 0;
    static uint32_t cursor = 0;
    static auto nextScan = std::chrono::steady_clock::time_point{};
    const uint64_t generation = m_worldGeneration.load(std::memory_order_acquire);
    if (generation != oldGeneration) {
        oldGeneration = generation; cached = 0; cursor = 0; nextScan = {};
    }
    if (cached && CameraManagerClass(cached) && PcmLivePovLooksSane(cached)) return cached;
    cached = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now < nextScan) return 0;
    nextScan = now + std::chrono::milliseconds(500);
    uintptr_t world = worldOverride, level = levelOverride, pc = pcOverride;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        if (!world) world = GWorld;
        if (!level) level = PersistentLevel;
        if (!pc) pc = PlayerController;
    }
    InitConsts();
    auto accept = [&](uintptr_t object) {
        if (!Memory::IsValidPtrFast2(object) || !CameraManagerClass(object)) return false;
        const std::string name = GetActorFNameString(object);
        if (name.empty() || name.find("Default__") != std::string::npos) return false;
        const uintptr_t owner = Memory::read_nocache<uintptr_t>(object + Offsets::PCOwner);
        std::ostringstream fs;
        fs << "{\"ev\":\"camera_manager_candidate\",\"pcm\":" << object
           << ",\"owner\":" << owner << ",\"pc\":" << pc << "}\n";
        EntityDiagnostics::LogRaw(fs.str());
        // The reference finds the local PlayerCameraManager by class alone and
        // reads its plain POV. This is a local client with exactly one camera
        // manager, and the manager's PCOwner back-ref is codec-protected on this
        // build (it reads zero), so demanding owner == pc rejected the genuine
        // manager every tick and left PCM=none. Class + sane POV is enough.
        if (!PcmLivePovLooksSane(object)) return false;
        cached = object;
        return true;
    };
    std::vector<uint64_t> actors;
    WorldScan::CollectLevelActors(world, level, actors);
    for (uint64_t object : actors) if (accept(static_cast<uintptr_t>(object))) return cached;

    // SDK1 CL-1424961 GUObjectArray variant. Unlike the stale generic offsets,
    // this source specifies ROL29, count+0x14, and the chunk pointer cipher.
    const uintptr_t base = Memory::getBaseAddress();
    uint32_t enc[2]{}, x[2]{}, a[2]{};
    if (!base || !PCIMemory::ReadVirtualMemoryNoCache(base + 0x10E423D0, enc, sizeof(enc))
        || !PCIMemory::ReadVirtualMemoryNoCache(base + 0xD538AA0, x, sizeof(x))
        || !PCIMemory::ReadVirtualMemoryNoCache(base + 0xD538AB0, a, sizeof(a))) return 0;
    const uintptr_t registry = static_cast<uint64_t>(namecrypto::Rol32(enc[0] ^ x[0], 29) + a[0])
        | (static_cast<uint64_t>(namecrypto::Rol32(enc[1] ^ x[1], 29) + a[1]) << 32);
    static auto lastAnchorLog = std::chrono::steady_clock::time_point{};
    if (now - lastAnchorLog >= std::chrono::seconds(5)) {
        lastAnchorLog = now;
        std::ostringstream f;
        f << "{\"ev\":\"camera_registry_anchor\",\"registry\":" << registry
          << ",\"actors\":" << actors.size() << "}\n";
        EntityDiagnostics::LogRaw(f.str());
    }
    uint32_t countEnc = 0; uint64_t chunksEnc = 0;
    if (!Memory::IsValidPtrFast2(registry)
        || !ReadComplete(registry + 0x14, countEnc) || !ReadComplete(registry + 0x20, chunksEnc)) return 0;
    const uint32_t count = _byteswap_ulong(countEnc ^ 0xA4E88268u);
    const uintptr_t chunks = _byteswap_uint64(chunksEnc ^ 0x26D0446400000000ULL);
    static auto lastRegistryLog = std::chrono::steady_clock::time_point{};
    if (now - lastRegistryLog >= std::chrono::seconds(5)) {
        lastRegistryLog = now;
        std::ostringstream f;
        f << "{\"ev\":\"camera_registry\",\"registry\":" << registry
          << ",\"count\":" << count << ",\"chunks\":" << chunks << ",\"cursor\":" << cursor << "}\n";
        EntityDiagnostics::LogRaw(f.str());
    }
    if (!count || count > 2000000 || !Memory::IsValidPtrFast2(chunks)) return 0;
    if (cursor >= count) cursor = 0;
    const uint32_t n = (std::min)(256u, (std::min)(count - cursor, 65536u - cursor % 65536u));
    uintptr_t chunk = 0;
    if (!ReadComplete(chunks + (cursor / 65536u) * 8, chunk) || !Memory::IsValidPtrFast2(chunk)) return 0;
    std::vector<uint8_t> items(static_cast<size_t>(n) * 24);
    if (!PCIMemory::ReadVirtualMemoryNoCache(chunk + static_cast<size_t>(cursor % 65536u) * 24,
            items.data(), items.size())) return 0;
    cursor += n;
    for (uint32_t i = 0; i < n; ++i) {
        // SDK1's preamble and constants disagree about item+0 versus item+8.
        // Both candidates must pass the same class and local-owner proof.
        for (size_t off : { size_t(0), size_t(8) }) {
            uintptr_t object = 0;
            std::memcpy(&object, items.data() + static_cast<size_t>(i) * 24 + off, sizeof(object));
            if (accept(object)) return cached;
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
    const std::string lower = StringUtils::ToLowerCopy(cls);
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
        auto tryItemArray = [&](std::ptrdiff_t arrOff, size_t stride) {
            const uint64_t data = Memory::read<uint64_t>(invComp + arrOff);
            const int32_t count = Memory::read<int32_t>(invComp + arrOff + 0x8);
            if (!data || count <= 0 || count > 64)
                return;
            for (int32_t i = 0; i < count; ++i) {
                const uintptr_t itemActor = Memory::read<uintptr_t>(
                    static_cast<uintptr_t>(data) + static_cast<uintptr_t>(i) * stride);
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
        // CurrentItemActors is InventoryItemActor[0x20]; local array holds pointers.
        tryItemArray(Offsets::CurrentItemActors, 0x20);
        if (!IsPlayerWeaponEspLabel(outWeaponName))
            tryItemArray(Offsets::LocalCurrentItemActors, sizeof(uintptr_t));
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
    const uint64_t generation = m_worldGeneration.load(std::memory_order_acquire);
    {
        std::unique_lock<std::shared_mutex> lock(m_radarBoundsMutex);
        if (m_radarBoundsGeneration != generation)
            m_miniMapActor = 0;
        m_radarBoundsValid = false; // every failed refresh invalidates calibration
        m_radarBoundsGeneration = generation;
    }
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

    // SDK1 FBox is double-vector based: Min +0x00, Max +0x18.
    // Do not reinterpret failed double reads as a speculative float schema.
    auto readBounds = [&] {
        RadarProjection::Bounds b;
        auto readOff = [&](std::ptrdiff_t off) -> double {
            return Memory::read_nocache<double>(m_miniMapActor + off);
        };
        b.minX = readOff(Offsets::MiniMap_WorldBounds);
        b.minY = readOff(Offsets::MiniMap_WorldBounds + 0x8);
        b.maxX = readOff(Offsets::MiniMap_WorldBounds + 0x18);
        b.maxY = readOff(Offsets::MiniMap_WorldBounds + 0x20);
        return b;
    };
    RadarProjection::Bounds bounds = readBounds();
    // WORLD_UNITS_PER_PIXEL calibrates the map scale - a plausible value is
    // the second proof that this really is the minimap block before the radar
    // trusts its bounds.
    const int32_t unitsPerPixel = Memory::read_nocache<int32_t>(
        m_miniMapActor + Offsets::MiniMap_UnitsPerPixel);
    if (!(unitsPerPixel > 0 && unitsPerPixel < 10000))
        return;
    if (!RadarProjection::BoundsPlausible(bounds))
        return;
    std::unique_lock<std::shared_mutex> lock(m_radarBoundsMutex);
    if (generation != m_worldGeneration.load(std::memory_order_acquire))
        return;
    m_radarBounds = bounds;
    m_radarBoundsStampMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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

    const uint64_t generation = m_worldGeneration.load(std::memory_order_acquire);
    const uintptr_t gs = AGameStateBase;
    if (!var::show_raid_hud) {
        std::unique_lock<std::shared_mutex> lock(m_raidHudMutex);
        m_raidHud = {};
    }
    if (!var::show_objectives) {
        std::unique_lock<std::shared_mutex> lock(m_objectiveMutex);
        m_objectives = {};
    }
    if (var::show_radar)
        RefreshRadarBounds();
    // Objectives live under the same toggle-independent worker tick as the rest
    // of the dashboard, so the raid HUD staying off does not starve them.
    if (var::show_objectives)
        RefreshObjectives(gs, generation, nowMs);
    if (!var::show_raid_hud)
        return;

    if (!IsUsableObjectPtr(gs)) {
        std::unique_lock<std::shared_mutex> lock(m_raidHudMutex);
        m_raidHud = {};
        return;
    }

    RaidHudState st;
    st.worldGeneration = generation;
    // Current SDK StageInfo is inline. Failed reads remain unknown, not zero;
    // never reinterpret the inline block as a speculative pointer layout.
    ReadComplete(gs + Offsets::GameState_StageInfoRef + Offsets::StageInfo_TimeLeftOff,
        st.timeLeftS);
    ReadComplete(gs + Offsets::GameState_StageInfoRef + Offsets::StageInfo_GraceTimeOff,
        st.graceS);
    if (!RaidClock::ClockPlausible(st.timeLeftS))
        st.timeLeftS = -1.0;
    if (!(st.graceS >= 0.0 && st.graceS < 3600.0))
        st.graceS = -1.0;

    uint8_t phase = 0;
    if (ReadComplete(gs + Offsets::GameState_GamePhase, phase))
        st.gamePhase = phase;
    ReadComplete(gs + Offsets::GameState_EnemyCount, st.enemyCount);
    ReadComplete(gs + Offsets::GameState_PickupCount, st.pickupCount);
    // Map condition removed - was reading garbage (-1254576712)
    // st.mapConditionKnown = ReadComplete(gs + Offsets::GameState_MapConditionId, st.mapConditionId);
    st.mapConditionKnown = false;
    st.mapConditionId = 0;
    uintptr_t healthInfo = 0;
    uint8_t healthStatus = 0;
    if (ReadComplete(gs + Offsets::GameState_ServerHealthInfo, healthInfo)
        && IsUsableObjectPtr(healthInfo)
        && GetActorClassFName(healthInfo).find("ServerHealthInfoComponent") != std::string::npos
        && ReadComplete(healthInfo + Offsets::ServerHealth_CurrentStatus, healthStatus)
        && healthStatus <= 2)
        st.serverHealthStatus = healthStatus;
    if (!(st.gamePhase >= 0 && st.gamePhase <= 4))
        st.gamePhase = -1;
    if (!(st.enemyCount >= 0 && st.enemyCount <= 100000))
        st.enemyCount = -1;
    if (!(st.pickupCount >= 0 && st.pickupCount <= 1000000))
        st.pickupCount = -1;
    st.stampMs = nowMs;
    st.valid =
        st.timeLeftS >= 0.0 || st.enemyCount >= 0 || st.gamePhase >= 0
        || st.serverHealthStatus >= 0;
    {
        std::unique_lock<std::shared_mutex> lock(m_raidHudMutex);
        if (generation == m_worldGeneration.load(std::memory_order_acquire))
            m_raidHud = st;
    }
}

Engine::RaidHudState Engine::GetRaidHud() const
{
    std::shared_lock<std::shared_mutex> lock(m_raidHudMutex);
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (m_raidHud.worldGeneration != m_worldGeneration.load(std::memory_order_acquire)
        || nowMs < m_raidHud.stampMs || nowMs - m_raidHud.stampMs > 2000)
        return {};
    return m_raidHud;
}

void Engine::RefreshObjectives(uintptr_t gameState, uint64_t generation, uint64_t nowMs)
{
    auto clear = [this]() {
        std::unique_lock<std::shared_mutex> lock(m_objectiveMutex);
        m_objectives = {};
    };
    if (!IsUsableObjectPtr(gameState)) {
        clear();
        return;
    }

    // Every hop is class-checked: an address that merely looks like an object
    // must not be published as a live objective component.
    uintptr_t component = 0;
    if (!ReadComplete(gameState + Offsets::GameState_MainObjectiveComponent, component)
        || !IsUsableObjectPtr(component)
        || GetActorClassFName(component).find("MainObjectiveComponent") == std::string::npos) {
        clear();
        return;
    }

    // MainObjectives is a TArray<MainObjectiveEntry*>; only the first entry is
    // summarised, and only when the header itself is coherent.
    uintptr_t entries = 0;
    int32_t entryCount = 0;
    if (!ReadComplete(component + Offsets::MainObjective_Entries, entries)
        || !ReadComplete(component + Offsets::MainObjective_Entries + 0x8, entryCount)
        || entryCount < 1 || entryCount > 16 || !IsUsableObjectPtr(entries)) {
        clear();
        return;
    }

    uintptr_t entry = 0;
    if (!ReadComplete(entries, entry) || !IsUsableObjectPtr(entry)) {
        clear();
        return;
    }

    ObjectiveBrief::Brief brief;
    ReadComplete(entry + Offsets::MainObjectiveEntry_ID, brief.objectiveId);
    if (brief.objectiveId < 0)
        brief.objectiveId = -1;

    uint8_t state = 0;
    if (ReadComplete(entry + Offsets::MainObjectiveEntry_State, state)
        && ObjectiveBrief::StatePlausible(state))
        brief.state = state;

    int32_t count = -1;
    int32_t max = -1;
    int32_t percent = 0;
    ReadComplete(
        entry + Offsets::MainObjectiveEntry_Progress + Offsets::ObjectiveProgress_Count,
        count);
    ReadComplete(
        entry + Offsets::MainObjectiveEntry_Progress + Offsets::ObjectiveProgress_Max,
        max);
    ReadComplete(
        entry + Offsets::MainObjectiveEntry_Progress + Offsets::ObjectiveProgress_bIsPercent,
        percent);
    if (ObjectiveBrief::ProgressPlausible(count, max)) {
        brief.progressCount = count;
        brief.progressMax = max;
        brief.isPercent = percent != 0;
    }

    double seconds = -1.0;
    if (ReadComplete(
            entry + Offsets::MainObjectiveEntry_Timer + Offsets::ObjectiveTimer_Seconds,
            seconds)
        && seconds >= 0.0 && seconds < 86400.0)
        brief.timerSeconds = seconds;
    uint8_t timerType = 0;
    if (ReadComplete(
            entry + Offsets::MainObjectiveEntry_Timer + Offsets::ObjectiveTimer_Type,
            timerType)
        && ObjectiveBrief::TimerTypePlausible(timerType))
        brief.timerType = timerType;

    // Task entries: count only. Their FText summaries need a resolved text
    // table and are deliberately not read as raw bytes.
    uintptr_t tasks = 0;
    int32_t taskCount = 0;
    if (ReadComplete(entry + Offsets::MainObjectiveEntry_Tasks, tasks)
        && ReadComplete(entry + Offsets::MainObjectiveEntry_Tasks + 0x8, taskCount)
        && taskCount >= 0 && taskCount <= 64 && IsUsableObjectPtr(tasks))
        brief.taskCount = taskCount;

    // An entry with no usable field is unknown, not an empty objective.
    const bool anyKnown = brief.objectiveId >= 0 || brief.state >= 0
        || brief.progressMax >= 0 || brief.timerSeconds >= 0.0 || brief.taskCount >= 0;
    if (!anyKnown) {
        clear();
        return;
    }

    brief.valid = true;
    brief.stampMs = nowMs;
    brief.worldGeneration = generation;
    std::unique_lock<std::shared_mutex> lock(m_objectiveMutex);
    if (generation == m_worldGeneration.load(std::memory_order_acquire))
        m_objectives = brief;
}

ObjectiveBrief::Brief Engine::GetObjectives() const
{
    std::shared_lock<std::shared_mutex> lock(m_objectiveMutex);
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (m_objectives.worldGeneration != m_worldGeneration.load(std::memory_order_acquire)
        || nowMs < m_objectives.stampMs || nowMs - m_objectives.stampMs > 2000)
        return {};
    return m_objectives;
}

RadarProjection::Bounds Engine::GetRadarBounds(bool& outValid) const
{
    std::shared_lock<std::shared_mutex> lock(m_radarBoundsMutex);
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    outValid = m_radarBoundsValid
        && m_radarBoundsGeneration == m_worldGeneration.load(std::memory_order_acquire)
        && nowMs >= m_radarBoundsStampMs && nowMs - m_radarBoundsStampMs <= 2000;
    return m_radarBounds;
}

void Engine::PushActivity(uint64_t key, const char* text)
{
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::unique_lock<std::shared_mutex> lock(m_feedMutex);
    const uint64_t generation = m_worldGeneration.load(std::memory_order_acquire);
    if (m_feedGeneration != generation) {
        m_activityFeed = {};
        m_feedGeneration = generation;
    }
    m_activityFeed.Push(nowMs, key, text);
    m_activityFeed.Expire(nowMs, 45000);
}

ActivityFeed::Feed Engine::GetActivityFeed() const
{
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::shared_lock<std::shared_mutex> lock(m_feedMutex);
    if (m_feedGeneration != m_worldGeneration.load(std::memory_order_acquire))
        return {};
    ActivityFeed::Feed snapshot = m_activityFeed;
    // Expire the paint snapshot even when no new scanner event arrives.
    snapshot.Expire(nowMs, 45000);
    return snapshot;
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
    auto rawSlot = [base](std::ptrdiff_t rva) -> uintptr_t {
        const uintptr_t slot = Memory::read_nocache<uintptr_t>(
            base + static_cast<uint64_t>(rva));
        return slot == UINTPTR_MAX ? 0 : slot;
    };
    // The global is a double deref ([RVA] -> intermediary -> UWorld), so a slot
    // counts only when it is itself a world or its inner value is one.
    auto slotLeadsToWorld = [this](uintptr_t slot) -> bool {
        if (!slot)
            return false;
        if (ResolvePersistentLevel(slot))
            return true;
        const uintptr_t inner = WorldSlotInner(slot);
        return inner != 0 && ResolvePersistentLevel(inner) != 0;
    };
    // Primary GWorld global for the current build.
    const uintptr_t primary = rawSlot(Offsets::UWorld);
    if (slotLeadsToWorld(primary))
        return primary;
    // Fallback: the adjacent build's GWorld global, probed only when the primary
    // slot leads to no live UWorld. The literal primary value is preserved for
    // the overlay when neither resolves.
    const uintptr_t alt = rawSlot(Offsets::UWorld_Alt);
    if (slotLeadsToWorld(alt))
        return alt;
    return primary;
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

    const uintptr_t level = Memory::read_nocache<uintptr_t>(world + Offsets::PersistentLevel);
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
    return Engine::IsUsableObjectPtr(lp)
        && !Engine::LooksLikeUtf16Garbage(lp);
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
    uintptr_t GiFromTls() { return GiFromTlsRung(); }
    uintptr_t GiFromOuter(uintptr_t lp, PlayerChain::Rung& which)
    {
        return GiFromOuterRung(lp, which);
    }
    uintptr_t GiFromDecrypt(uintptr_t world) { return GiFromDecryptSlot(world); }
    uintptr_t GiFromLegacySlot(uintptr_t world) { return GiFromLegacySlotRung(world); }
    uintptr_t GiFromScan(uintptr_t world) { return GiFromScanRung(world); }
    uintptr_t GiFromObjectScan() { return GiFromObjectScanRung(); }
    bool IsGameInstance(uintptr_t gi) { return ValidateGameInstance(gi) != 0; }

    // ── Controller ──
    // Dump-verified cache: bIsLocalPlayerController (SDK 0xDB8) keeps the PC
    // across hub ticks and pawn-less lobbies, where the pair cache cannot hit.
    bool CachedPcMatchesLocalPlayer(uintptr_t gi, uintptr_t cachedPc)
    {
        if (!IsUsableObjectPtr(gi) || Engine::LooksLikeUtf16Garbage(gi)
            || IsModuleSpace(gi) || !cachedPc)
            return true; // No trustworthy current GI: preserve normal fallbacks.

        const uintptr_t data = Memory::read_nocache<uintptr_t>(gi + Offsets::LocalPlayers);
        const int count = Memory::read_nocache<int>(gi + Offsets::LocalPlayers + 8);
        if (!IsUsableObjectPtr(data) || count <= 0 || count > 16)
            return true; // An unreadable/empty array is not evidence of staleness.

        bool foundController = false;
        const int limit = (count > 4) ? 4 : count;
        for (int i = 0; i < limit; ++i) {
            const uintptr_t lp = Memory::read_nocache<uintptr_t>(
                data + static_cast<size_t>(i) * sizeof(uintptr_t));
            if (!IsGoodLocalPlayerPtr(lp))
                continue;

            const uintptr_t currentPc = Memory::read_nocache<uintptr_t>(
                lp + Offsets::LocalPlayer_PlayerController);
            if (!IsUsableObjectPtr(currentPc))
                continue;
            foundController = true;
            if (currentPc == cachedPc)
                return true;
        }
        return !foundController;
    }

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
        // IsLocalPlayerController is the dump-reflected flag (0xDB8): it accepts
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
        // No controller yet (menu / engine spawn): the object-scan rung proved the
        // GI by this same reflected array (its LocalPlayers@0x108 slots), so the
        // first good slot is the real menu local player. This only fills the LP
        // hop early; the controller/pawn hops below still need a live controller
        // and stay red until the match, exactly as before.
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
            if (!pc || Memory::read<uintptr_t>(slot + Offsets::LocalPlayer_PlayerController) == pc) {
                backRef = (pc != 0);
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
            if (c.off <= 0)
                continue;
            const uintptr_t pawn = Memory::read_nocache<uintptr_t>(pc + c.off);
            if (Engine::IsUsableObjectPtr(pawn) && Engine::ResolveActorRoot(pawn)) {
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
        return Engine::ResolveActorRoot(pawn);
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
