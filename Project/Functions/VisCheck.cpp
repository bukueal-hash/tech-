#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"
#include "../Core/IntervalTimer.h"
#include "CollisionLos.h"
#include <immintrin.h>
#include <iostream>
#include <cstring>

namespace {

struct MeshVisProbe {
	float lastSubmit = 0.f;
	float lastRender = 0.f;
	float lastRenderOnScreen = 0.f;
	float decryptedLrtos = 0.f;
	float worldTimeSeconds = 0.f;
	bool onScreen = false;
	bool recent = false;
	bool visible = false;
	const char* pathUsed = "fail";
};

static bool IsPlausibleRenderTime(float t)
{
	return std::isfinite(t) && t > 0.001f && t < 1e8f;
}

// UC pg185 #3689 — encrypted LastRenderTimeOnScreen @ mesh+0x488.
// structuralOk=false means caller should try the legacy plain path.
struct EncVisAttempt {
	bool structuralOk = false;
	bool visible = false;
	float lrtos = 0.f;
	float worldTime = 0.f;
};

static EncVisAttempt TryEncryptedRenderVis(uintptr_t mesh)
{
	EncVisAttempt out{};
	if (!mesh || !Memory::IsValidPtrFast2(mesh))
		return out;

	constexpr uint32_t kXorKey = 0xFA3CBF38u;
	constexpr std::ptrdiff_t kEncLrtosOff = 0x488;
	constexpr std::ptrdiff_t kWorldPrivateOff = 0x148;
	constexpr std::ptrdiff_t kTimeSecondsOff = 0x950;
	constexpr float kThreshold = 0.2f;

	const uint32_t enc = Memory::read<uint32_t>(mesh + kEncLrtosOff);
	if (enc == 0)
		return out; // structural — try plain

	const uint32_t decrypted = _byteswap_ulong(enc ^ kXorKey);
	float lastRenderOnScreen = 0.f;
	std::memcpy(&lastRenderOnScreen, &decrypted, sizeof(lastRenderOnScreen));

	const uintptr_t worldPrivate = Memory::read<uintptr_t>(mesh + kWorldPrivateOff);
	if (!worldPrivate || !Memory::IsValidPtrFast2(worldPrivate))
		return out; // structural — try plain

	const double timeSeconds = Memory::read<double>(worldPrivate + kTimeSecondsOff);

	out.structuralOk = true;
	out.lrtos = lastRenderOnScreen;
	out.worldTime = static_cast<float>(timeSeconds);

	// Encrypted path is authoritative once structure is valid. Implausible
	// timing fail-opens (do not fall back to dead plain fields).
	if (!IsPlausibleRenderTime(lastRenderOnScreen)
		|| !std::isfinite(timeSeconds) || timeSeconds <= 0.0) {
		out.visible = true;
		return out;
	}

	out.visible = static_cast<float>(timeSeconds) - lastRenderOnScreen < kThreshold;
	return out;
}

static bool TryPlainRenderVis(uintptr_t mesh, MeshVisProbe* probeOut)
{
	if (!mesh || !Memory::IsValidPtrFast2(mesh))
		return false;

	const float lastSubmitTime =
		Memory::read<float>(mesh + Offsets::LastSubmitTime);
	const float lastRenderTime =
		Memory::read<float>(mesh + Offsets::LastRenderTime);
	const float lastRenderTimeOnScreen =
		Memory::read<float>(mesh + Offsets::LastRenderTimeOnScreen);

	if (probeOut) {
		probeOut->lastSubmit = lastSubmitTime;
		probeOut->lastRender = lastRenderTime;
		probeOut->lastRenderOnScreen = lastRenderTimeOnScreen;
	}

	if (!IsPlausibleRenderTime(lastSubmitTime)
		|| !IsPlausibleRenderTime(lastRenderTime)
		|| !IsPlausibleRenderTime(lastRenderTimeOnScreen))
		return false;

	const bool isOnScreen = (lastRenderTime == lastRenderTimeOnScreen);
	const bool isRecent = (lastSubmitTime - lastRenderTime) <= 0.06f;
	if (probeOut) {
		probeOut->onScreen = isOnScreen;
		probeOut->recent = isRecent;
		probeOut->decryptedLrtos = lastRenderTimeOnScreen;
	}
	return isOnScreen && isRecent;
}

static bool MeshRenderTimeVisible(uintptr_t mesh)
{
	if (!mesh || !Memory::IsValidPtrFast2(mesh))
		return false;

	// Primary: UC-confirmed encrypted decrypt (CL-1315578 / UC pg185 #3689).
	const EncVisAttempt enc = TryEncryptedRenderVis(mesh);
	if (enc.structuralOk)
		return enc.visible;

	// Structural failure only — legacy plain triple-read @ mesh+0x4C4.
	return TryPlainRenderVis(mesh, nullptr);
}

static MeshVisProbe ProbeMeshVisibility(uintptr_t mesh)
{
	MeshVisProbe probe{};
	if (!mesh)
		return probe;

	// Always capture legacy plain fields for debug comparison.
	probe.lastSubmit = Memory::read<float>(mesh + Offsets::LastSubmitTime);
	probe.lastRender = Memory::read<float>(mesh + Offsets::LastRenderTime);
	probe.lastRenderOnScreen =
		Memory::read<float>(mesh + Offsets::LastRenderTimeOnScreen);
	if (IsPlausibleRenderTime(probe.lastSubmit)
		&& IsPlausibleRenderTime(probe.lastRender)
		&& IsPlausibleRenderTime(probe.lastRenderOnScreen)) {
		probe.onScreen = (probe.lastRender == probe.lastRenderOnScreen);
		probe.recent = (probe.lastSubmit - probe.lastRender) <= 0.06f;
	}

	const EncVisAttempt enc = TryEncryptedRenderVis(mesh);
	if (enc.structuralOk) {
		probe.pathUsed = "enc";
		probe.decryptedLrtos = enc.lrtos;
		probe.worldTimeSeconds = enc.worldTime;
		probe.visible = enc.visible;
		return probe;
	}

	MeshVisProbe plainProbe = probe;
	const bool plainVis = TryPlainRenderVis(mesh, &plainProbe);
	if (IsPlausibleRenderTime(plainProbe.lastSubmit)
		|| IsPlausibleRenderTime(plainProbe.lastRender)
		|| IsPlausibleRenderTime(plainProbe.lastRenderOnScreen)) {
		probe.pathUsed = "plain";
		probe.lastSubmit = plainProbe.lastSubmit;
		probe.lastRender = plainProbe.lastRender;
		probe.lastRenderOnScreen = plainProbe.lastRenderOnScreen;
		probe.onScreen = plainProbe.onScreen;
		probe.recent = plainProbe.recent;
		probe.decryptedLrtos = plainProbe.decryptedLrtos;
		probe.visible = plainVis;
		return probe;
	}

	probe.pathUsed = "fail";
	probe.visible = false;
	return probe;
}

} // namespace

bool Engine::Visible(uintptr_t mesh) const
{
	return MeshRenderTimeVisible(mesh);
}

bool Engine::HasLineOfSight(const Vector3& from, const Vector3& to) const
{
	if (!var::obstruction_check)
		return true;
	return CollisionLos::IsVisible(from, to);
}

Engine::VisCheckDebugStats Engine::CollectVisCheckDebugStats() const
{
	VisCheckDebugStats stats{};
	stats.collisionLosEnabled = var::obstruction_check;
	stats.collisionTriCount = static_cast<int>(CollisionLos::TriangleCount());
	stats.collisionSmcCount = static_cast<int>(CollisionLos::LastSmcCount());
	stats.collisionRebuilding = CollisionLos::IsRebuilding();

	auto applySample = [&](const MeshVisProbe& probe) {
		stats.sampleSubmit = probe.lastSubmit;
		stats.sampleRender = probe.lastRender;
		stats.sampleRenderScr = probe.lastRenderOnScreen;
		stats.sampleOnScreen = probe.onScreen;
		stats.sampleRecent = probe.recent;
		stats.sampleVisible = probe.visible;
		stats.sampleDecryptedLrtos = probe.decryptedLrtos;
		stats.sampleWorldTimeSeconds = probe.worldTimeSeconds;
		stats.samplePathUsed = probe.pathUsed ? probe.pathUsed : "fail";
		stats.hasSample = true;
	};

	{
		std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
		for (const auto& [key, actor] : playerCache) {
			if (!actor.Drawing)
				continue;
			++stats.playersTotal;
			if (actor.isVisible)
				++stats.playersMeshVisible;
			if (!stats.hasSample) {
				const uintptr_t mesh = actor.actorMesh
					? actor.actorMesh
					: GetActorSkeletalMesh(key);
				if (mesh && IsValidPointer(mesh))
					applySample(ProbeMeshVisibility(mesh));
			}
		}
	}

	{
		std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
		for (const auto& [key, entry] : robotCache) {
			if (!entry.Drawing)
				continue;
			++stats.botsTotal;
			if (entry.isVisible)
				++stats.botsMeshVisible;
			if (!stats.hasSample) {
				const uintptr_t mesh = entry.Mesh
					? entry.Mesh
					: GetActorSkeletalMesh(key);
				if (mesh && IsValidPointer(mesh))
					applySample(ProbeMeshVisibility(mesh));
			}
		}
	}

	return stats;
}

void Engine::PrintVisCheckDebugConsole()
{
	if (!var::show_debug_overlay)
		return;

	static IntervalTimer timer(500);
	if (!timer.fire())
		return;

	const VisCheckDebugStats stats = CollectVisCheckDebugStats();
	std::cout << "[debugVisCheck] meshMode=render_time"
		<< " collisionLos=" << (stats.collisionLosEnabled ? "on" : "off")
		<< " tris=" << stats.collisionTriCount
		<< " smc=" << stats.collisionSmcCount
		<< " rebuilding=" << (stats.collisionRebuilding ? 1 : 0)
		<< " players=" << stats.playersMeshVisible << '/' << stats.playersTotal
		<< " bots=" << stats.botsMeshVisible << '/' << stats.botsTotal;
	if (stats.hasSample) {
		const float delta = stats.sampleWorldTimeSeconds - stats.sampleDecryptedLrtos;
		std::cout << " path=" << (stats.samplePathUsed ? stats.samplePathUsed : "fail")
			<< " lrtos=" << stats.sampleDecryptedLrtos
			<< " worldTime=" << stats.sampleWorldTimeSeconds
			<< " delta=" << delta
			<< " sample submit=" << stats.sampleSubmit
			<< " render=" << stats.sampleRender
			<< " onScr=" << stats.sampleRenderScr
			<< " onScreen=" << (stats.sampleOnScreen ? 1 : 0)
			<< " recent=" << (stats.sampleRecent ? 1 : 0)
			<< " visible=" << (stats.sampleVisible ? 1 : 0);
	}
	std::cout << std::endl;
}

bool Engine::VisibleActor(uintptr_t actor) const
{
	if (!actor)
		return false;

	const uintptr_t skel =
		Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
	if (skel && IsValidPointer(skel) && Visible(skel))
		return true;

	const uintptr_t embark = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
	if (embark && IsValidPointer(embark) && Visible(embark))
		return true;

	return false;
}

bool Engine::VisibleBotActor(uintptr_t actor) const
{
	if (!actor)
		return false;

	const uintptr_t embark = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
	if (embark && IsValidPointer(embark) && Visible(embark))
		return true;

	const uintptr_t skel =
		Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
	if (skel && IsValidPointer(skel) && Visible(skel))
		return true;

	return false;
}

static bool MeshHasEncryptedBoneBlock(uintptr_t mesh)
{
	if (!mesh)
		return false;

	__m128i enc780{};
	if (Memory::ReadRaw(mesh + Offsets::Encrypted, &enc780, sizeof(enc780))
		&& _mm_cvtsi128_si64(enc780) != 0)
		return true;

	__m128i enc830{};
	return Memory::ReadRaw(mesh + Offsets::LodSelect, &enc830, sizeof(enc830))
		&& _mm_cvtsi128_si64(enc830) != 0;
}

uintptr_t Engine::GetActorBoneMesh(uintptr_t actor)
{
	if (!actor)
		return 0;

	const uintptr_t embark =
		Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
	const uintptr_t skel =
		Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);

	if (embark && IsValidPointer(embark) && MeshHasEncryptedBoneBlock(embark))
		return embark;
	if (skel && IsValidPointer(skel) && MeshHasEncryptedBoneBlock(skel))
		return skel;

	return GetActorSkeletalMesh(actor);
}

uintptr_t Engine::GetActorSkeletalMesh(uintptr_t actor) const
{
	if (!actor)
		return 0;

	uintptr_t mesh = Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
	if (mesh && IsValidPointer(mesh))
		return mesh;

	mesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
	if (mesh && IsValidPointer(mesh))
		return mesh;

	return 0;
}
