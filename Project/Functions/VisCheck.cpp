#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"
#include "../Core/IntervalTimer.h"
#include <immintrin.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <shared_mutex>

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

Engine::VisCheckDebugStats Engine::CollectVisCheckDebugStats() const
{
	VisCheckDebugStats stats{};

	// Paint/debug path must stay DMA-free AND non-blocking. Blocking shared_lock
	// here waits behind unique_lock waiters while ItemList debug probes hold
	// shared+DMA (writer-preference) — same class as overlayMs=595==paint_gap.
	{
		std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			for (const auto& [key, actor] : playerCache) {
				(void)key;
				if (!actor.Drawing)
					continue;
				++stats.playersTotal;
				if (actor.isVisible)
					++stats.playersMeshVisible;
			}
		}
	}

	{
		std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			for (const auto& [key, entry] : robotCache) {
				(void)key;
				if (!entry.Drawing)
					continue;
				++stats.botsTotal;
				if (entry.isVisible)
					++stats.botsMeshVisible;
			}
		}
	}

	return stats;
}

void Engine::TryCaptureDebugOverlaySnap(DebugOverlaySnap& io) const
{
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	auto msOf = [](clock::time_point a, clock::time_point b) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
	};

	DebugOverlaySnap next = io;
	next.base = Memory::getBaseAddress();
	next.actorCount = 0;
	next.gotState = next.gotFrame = next.gotPlayer = 0;
	next.gotWorld = next.gotRobot = next.gotCam = 0;

	{
		const auto tA = clock::now();
		std::shared_lock<std::shared_mutex> lock(m_stateMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			next.gotState = 1;
			next.state.gWorld = GWorld;
			next.state.gWorldRaw = m_gWorldRaw.load(std::memory_order_relaxed);
			next.state.gWorldFailStep = m_gWorldFailStep.load(std::memory_order_relaxed);
			next.state.persistentLevel = PersistentLevel;
			next.state.actors = Actors;
			next.state.playerController = PlayerController;
			next.state.acknowledgedPawn = AcknowledgedPawn;
			next.state.rootComponent = RootComponent;
			next.state.playerCameraManager = PlayerCameraManager;
			next.state.owningGameInstance = OwningGameInstance;
			next.state.localPlayer = localplayer;
			next.playerState = PlayerState;
		}
		next.msState = msOf(tA, clock::now());
	}
	{
		const auto tA = clock::now();
		std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
		if (lock.owns_lock() && m_lastEspFrame.valid) {
			next.gotFrame = 1;
			next.drawTargets = m_lastEspFrame.players.size();
			next.robotDrawSz = m_lastEspFrame.robots.size();
			next.worldDrawSz = m_lastEspFrame.world.size();
		}
		next.msFrame = msOf(tA, clock::now());
	}
	{
		const auto tA = clock::now();
		std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			next.gotPlayer = 1;
			next.playerCacheSz = playerCache.size();
			next.visDbg.playersTotal = 0;
			next.visDbg.playersMeshVisible = 0;
			size_t drawN = 0;
			for (const auto& [key, actor] : playerCache) {
				(void)key;
				if (!actor.Drawing)
					continue;
				if (!(actor.isAlly && var::hide_allies))
					++drawN;
				++next.visDbg.playersTotal;
				if (actor.isVisible)
					++next.visDbg.playersMeshVisible;
			}
			if (!next.gotFrame)
				next.drawTargets = drawN;
		}
		next.msPlayer = msOf(tA, clock::now());
	}
	{
		const auto tA = clock::now();
		size_t contN = 0, itemN = 0, contDraw = 0, itemDraw = 0;
		bool okC = false, okI = false;
		{
			std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex, std::try_to_lock);
			if (lock.owns_lock()) {
				okC = true;
				contN = containerCache.size();
				for (const auto& [key, e] : containerCache) {
					(void)key;
					if (e.Drawing)
						++contDraw;
				}
			}
		}
		{
			std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex, std::try_to_lock);
			if (lock.owns_lock()) {
				okI = true;
				itemN = itemCache.size();
				for (const auto& [key, e] : itemCache) {
					(void)key;
					if (e.Drawing)
						++itemDraw;
				}
			}
		}
		if (okC || okI) {
			next.gotWorld = 1;
			if (okC && okI)
				next.worldCacheSz = contN + itemN;
			else if (okC)
				next.worldCacheSz = contN;
			else
				next.worldCacheSz = itemN;
			if (!next.gotFrame)
				next.worldDrawSz = contDraw + itemDraw;
		}
		next.msWorld = msOf(tA, clock::now());
	}
	{
		const auto tA = clock::now();
		std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			next.gotRobot = 1;
			next.visDbg.botsTotal = 0;
			next.visDbg.botsMeshVisible = 0;
			size_t drawN = 0;
			for (const auto& [key, entry] : robotCache) {
				(void)key;
				if (!entry.Drawing)
					continue;
				++drawN;
				++next.visDbg.botsTotal;
				if (entry.isVisible)
					++next.visDbg.botsMeshVisible;
			}
			if (!next.gotFrame)
				next.robotDrawSz = drawN;
		}
		next.msRobot = msOf(tA, clock::now());
	}
	{
		const auto tA = clock::now();
		std::shared_lock<std::shared_mutex> lock(m_cameraMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			next.gotCam = 1;
			next.camFov = g_Camera.FOV;
		}
		next.msCam = msOf(tA, clock::now());
	}

	next.totalMs = msOf(t0, clock::now());
	io = next;
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
