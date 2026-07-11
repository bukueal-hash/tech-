#include "../Core/Engine.h"
#include "WorldScanCommon.h"

#include <chrono>
#include <cctype>
#include <cmath>
#include <iostream>
#include <string>

namespace {

bool LooksLikeUtf16Garbage(uintptr_t p)
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

bool IsPlausibleObjPtr(uintptr_t p)
{
	return p != 0 && p != UINTPTR_MAX
		&& !LooksLikeUtf16Garbage(p)
		&& Memory::IsValidPtrFast2(p);
}

uintptr_t ReadWorldFromSlot(uint64_t base, std::ptrdiff_t slotOff)
{
	const uintptr_t slot = Memory::read<uintptr_t>(base + slotOff);
	return IsPlausibleObjPtr(slot) ? slot : 0;
}

/** Help CL-1315578: PL @ 0x158 (also try prior 0x110), LevelCollections[i]+0x20, Levels[]. */
bool LevelLooksOwnedByWorld(uintptr_t level, uintptr_t world)
{
	if (!IsPlausibleObjPtr(level) || !world)
		return false;
	const uintptr_t owning = Memory::read<uintptr_t>(level + Offsets::Level_OwningWorld);
	if (owning == world)
		return true;
	const uintptr_t data = Memory::read<uintptr_t>(level + Offsets::AActors);
	const int count = Memory::read<int>(level + Offsets::ActorsCount);
	return IsPlausibleObjPtr(data) && count > 0 && count <= 10000;
}

uintptr_t ResolvePersistentLevelHelp(uintptr_t world)
{
	if (!IsPlausibleObjPtr(world))
		return 0;

	// Locked help 0x158; prior CL 0x110 only as probe (does not edit Offsets.h).
	static const std::ptrdiff_t kPlOffs[] = {
		Offsets::PersistentLevel,
		static_cast<std::ptrdiff_t>(0x110),
	};
	for (std::ptrdiff_t plOff : kPlOffs) {
		const uintptr_t level = Memory::read<uintptr_t>(world + plOff);
		if (LevelLooksOwnedByWorld(level, world))
			return level;
	}

	const uintptr_t collectionsData =
		Memory::read<uintptr_t>(world + Offsets::LevelCollections);
	const int32_t collectionsNum =
		Memory::read<int32_t>(world + Offsets::LevelCollections + 8);
	if (IsPlausibleObjPtr(collectionsData) && collectionsNum > 0 && collectionsNum <= 16) {
		const int limit = (collectionsNum > 4) ? 4 : collectionsNum;
		for (int i = 0; i < limit; ++i) {
			const uintptr_t collection =
				collectionsData + static_cast<uintptr_t>(i) * Offsets::LevelCollection_Stride;
			const uintptr_t level = Memory::read<uintptr_t>(
				collection + Offsets::LevelCollection_PersistentLevel);
			if (LevelLooksOwnedByWorld(level, world))
				return level;
		}
	}

	const uintptr_t levelsData = Memory::read<uintptr_t>(world + Offsets::Levels);
	const int32_t levelsNum = Memory::read<int32_t>(world + Offsets::Levels + 8);
	if (IsPlausibleObjPtr(levelsData) && levelsNum > 0 && levelsNum < 512) {
		const int limit = (levelsNum > 8) ? 8 : levelsNum;
		for (int i = 0; i < limit; ++i) {
			const uintptr_t level = Memory::read<uintptr_t>(
				levelsData + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
			if (LevelLooksOwnedByWorld(level, world))
				return level;
		}
	}

	return 0;
}

/** Help GAME_STATE_GLOBAL_RVA — Outer-ish fields may point at UWorld when GWorld slot is stale. */
uintptr_t TryWorldFromGameStateGlobal(uint64_t base)
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

	static const std::ptrdiff_t kOuterCands[] = { 0x20, 0x28, 0x18, 0x30, 0x10, 0x40 };
	for (std::ptrdiff_t off : kOuterCands) {
		const uintptr_t cand = Memory::read<uintptr_t>(gs + off);
		if (ResolvePersistentLevelHelp(cand))
			return cand;
	}
	return 0;
}

uintptr_t PickValidWorld(uintptr_t slot)
{
	if (!IsPlausibleObjPtr(slot))
		return 0;
	if (ResolvePersistentLevelHelp(slot))
		return slot;
	// Some builds store GWorld** (slot → UWorld*). Accept only if PL validates.
	const uintptr_t inner = Memory::read<uintptr_t>(slot);
	if (IsPlausibleObjPtr(inner) && ResolvePersistentLevelHelp(inner))
		return inner;
	return 0;
}

bool ChainHasWorldPosition(uintptr_t pc, uintptr_t pawn, uintptr_t root)
{
	(void)pc;
	if (!pawn || !root || !Memory::IsValidPtrFast2(pawn))
		return false;
	const Vector3 pos = Memory::read<Vector3>(root + Offsets::RelativeLocation);
	const float magSq = static_cast<float>(
		pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
	return magSq > 10000.f && magSq < 1.0e14f;
}

static uintptr_t s_cachedPC = 0;
static uintptr_t s_cachedPawn = 0;
static uintptr_t s_cachedLocalPlayer = 0;

static bool IsGoodLocalPlayer(uintptr_t lp)
{
	return lp != 0 && lp != UINTPTR_MAX
		&& lp >= 0x1000 && lp < 0x7FFFFFFFFFFF
		&& Memory::IsValidPtrFast2(lp);
}

static bool FNameLowerContains(const std::string& name, const char* token)
{
	if (name.empty() || !token || !*token)
		return false;
	std::string lower = name;
	for (char& c : lower)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return lower.find(token) != std::string::npos;
}

static bool ObjectFNameContains(uintptr_t obj, const char* token)
{
	if (!obj || !Memory::IsValidPtrFast2(obj))
		return false;
	return FNameLowerContains(steam_decrypt::GetActorFNameString(obj), token);
}

static constexpr int kRaidPopulationThreshold = 80;

static bool FNameTokensMatch(uintptr_t world, uintptr_t level, const char* const* tokens, int count)
{
	for (int i = 0; i < count; ++i) {
		const char* tok = tokens[i];
		if (ObjectFNameContains(world, tok) || ObjectFNameContains(level, tok))
			return true;
	}
	return false;
}

static bool LooksLikeStrictHubWorld(uintptr_t world, uintptr_t level)
{
	static const char* kStrictHubTokens[] = {
		"sprocket", "mainmenu", "frontend", "front_end",
		"lobby", "loadout", "hideout", "menuworld", "bilguun",
	};
	return FNameTokensMatch(world, level, kStrictHubTokens,
		static_cast<int>(sizeof(kStrictHubTokens) / sizeof(kStrictHubTokens[0])));
}

static bool LooksLikeHubWorld(uintptr_t world, uintptr_t level)
{
	if (LooksLikeStrictHubWorld(world, level))
		return true;

	static const char* kExtraHubTokens[] = { "persistence" };
	return FNameTokensMatch(world, level, kExtraHubTokens,
		static_cast<int>(sizeof(kExtraHubTokens) / sizeof(kExtraHubTokens[0])));
}

static bool PawnLooksLikeHubCharacter(uintptr_t pawn)
{
	static const char* kHubPawnTokens[] = {
		"persistence", "sprocket", "mainmenu", "lobby", "loadout", "hideout",
	};
	for (const char* tok : kHubPawnTokens) {
		if (ObjectFNameContains(pawn, tok))
			return true;
	}
	return false;
}

static bool PawnHasRaidCombatComponents(uintptr_t pawn)
{
	if (!pawn || !Memory::IsValidPtrFast2(pawn))
		return false;

	const uintptr_t ps = Memory::read<uintptr_t>(pawn + Offsets::APlayerState);
	if (ps && Memory::IsValidPtrFast2(ps)) {
		const double maxHp =
			Memory::read<double>(ps + Offsets::PlayerState_MaxHealth);
		if (maxHp > 1.0 && maxHp < 10000.0)
			return true;
	}

	const uintptr_t healthComp =
		Memory::read<uintptr_t>(pawn + Offsets::HealthComponent);
	if (healthComp && Memory::IsValidPtrFast2(healthComp))
		return true;

	const uintptr_t invComp =
		Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
	return invComp && Memory::IsValidPtrFast2(invComp);
}

} // namespace

uintptr_t Engine::ResolveBestGWorld(uint64_t base)
{
	m_gWorldRaw.store(0, std::memory_order_relaxed);
	m_gWorldFailStep.store(1, std::memory_order_relaxed);

	if (!base)
		return 0;

	const uintptr_t slot = ReadWorldFromSlot(base, Offsets::UWorld);
	m_gWorldRaw.store(slot, std::memory_order_relaxed);

	uintptr_t world = PickValidWorld(slot);
	if (!world)
		world = TryWorldFromGameStateGlobal(base);

	if (world && ResolvePersistentLevelHelp(world)) {
		m_gWorldFailStep.store(0, std::memory_order_relaxed);
		return world;
	}

	if (!slot)
		m_gWorldFailStep.store(1, std::memory_order_relaxed);
	else
		m_gWorldFailStep.store(2, std::memory_order_relaxed);
	return 0;
}

void Engine::Update() {
	static int s_gworldMissStreak = 0;
	static auto s_gworldMissSince = std::chrono::steady_clock::time_point{};

	const uint64_t base = Memory::getBaseAddress();
	const uintptr_t tGWorld = ResolveBestGWorld(base);
	if (!tGWorld) {
		const auto now = std::chrono::steady_clock::now();
		if (s_gworldMissSince.time_since_epoch().count() == 0)
			s_gworldMissSince = now;
		++s_gworldMissStreak;
		if (s_gworldMissStreak >= 180
			|| now - s_gworldMissSince >= std::chrono::seconds(3)) {
			ResetRaidTransitionState();
			s_gworldMissStreak = 0;
			s_gworldMissSince = {};
		}
		TickRaidGate();
		return;
	}
	s_gworldMissStreak = 0;
	s_gworldMissSince = {};

	// Check for world transition (clears caches under their own locks)
	CheckWorldChange(tGWorld);
	{
		static uintptr_t s_lastGWorldForLp = 0;
		if (tGWorld != s_lastGWorldForLp) {
			s_cachedLocalPlayer = 0;
			s_lastGWorldForLp = tGWorld;
		}
	}

	// --- Read entire pointer chain into locals (NO lock held, slow I/O here) ---
	uintptr_t tGameInstance = GetGameInstance(tGWorld), tPersistentLevel = 0, tLocalPlayer = 0;
	uintptr_t tPlayerController = 0, tAcknowledgedPawn = 0;
	uintptr_t tRootComponent = 0, tActors = 0, tPlayerState = 0;

	tPersistentLevel = ResolvePersistentLevelHelp(tGWorld);

	int actorCount = 0;
	if (tPersistentLevel)
		ResolveLevelActors(tPersistentLevel, tActors, actorCount);

	// Resolve PC via actor scan (backup: Pioneer PC owns PCM @ PC+0x48).
	{
		if (s_cachedPC && s_cachedPawn && Engine::ControllerHasValidPcm(s_cachedPC)
			&& IsValidPointer(s_cachedPC) && IsValidPointer(s_cachedPawn)) {
			const uintptr_t root = Memory::read<uintptr_t>(s_cachedPawn + Offsets::RootComponent);
			if (root && IsValidPointer(root)) {
				const Vector3 pos = Memory::read<Vector3>(root + Offsets::RelativeLocation);
				const float magSq = static_cast<float>(
		pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
				if (magSq > 10000.f && magSq < 1.0e14f) {
					tPlayerController = s_cachedPC;
					tAcknowledgedPawn = s_cachedPawn;
				}
			}
		} else if (s_cachedPC || s_cachedPawn) {
			s_cachedPC = 0;
			s_cachedPawn = 0;
		}

		if (!tPlayerController && tPersistentLevel && tActors && actorCount > 0) {
			uintptr_t scannedPc = 0;
			uintptr_t scannedPawn = 0;
			if (ResolveLocalPlayerChainFromActors(
				tPersistentLevel, tActors, actorCount, tGameInstance, scannedPc, scannedPawn)
				&& Engine::ControllerHasValidPcm(scannedPc)) {
				tPlayerController = s_cachedPC = scannedPc;
				tAcknowledgedPawn = s_cachedPawn = scannedPawn;
			}
		}
	}

	if (!tPlayerController && tPersistentLevel && tActors) {
		uintptr_t pcmPc = 0;
		uintptr_t pcmPawn = 0;
		uintptr_t pcmDummy = 0;
		if (Engine::ResolvePcFromLevelCameraManager(
			tPersistentLevel, tActors, pcmPc, pcmPawn, pcmDummy)) {
			tPlayerController = s_cachedPC = pcmPc;
			tAcknowledgedPawn = s_cachedPawn = pcmPawn;
		}
	}

	if (tPlayerController && !tAcknowledgedPawn)
		tAcknowledgedPawn = Memory::read<uintptr_t>(tPlayerController + Offsets::AcknowledgedPawn);

	if (tAcknowledgedPawn)
		tRootComponent = Memory::read<uintptr_t>(tAcknowledgedPawn + Offsets::RootComponent);

	if (tAcknowledgedPawn) {
		// Help: Actor/PC PLAYER_STATE 0x3A8. LocalAckPlayerState(0x3F0) is CHARACTER on PC — not PS on pawn.
		tPlayerState = Memory::read<uintptr_t>(tAcknowledgedPawn + Offsets::APlayerState);
		if ((!tPlayerState || !IsValidPointer(tPlayerState)) && tPlayerController)
			tPlayerState = Memory::read<uintptr_t>(
				tPlayerController + Offsets::AController_PlayerState);
		if (tPlayerState && !IsValidPointer(tPlayerState))
			tPlayerState = 0;
	}

	// LP from GameInstance — validated; slot scan must match known PC (avoids 0xFF.. garbage).
	if (tGameInstance) {
		uintptr_t giLp = 0;
		uintptr_t giPcDummy = 0;
		if (ResolveLocalPlayerFromGameInstance(tGameInstance, giLp, giPcDummy)
			&& IsGoodLocalPlayer(giLp)) {
			tLocalPlayer = giLp;
		}

		if (!IsGoodLocalPlayer(tLocalPlayer) && tPlayerController) {
			auto slotOwnsPc = [&](uintptr_t slot) -> bool {
				if (!IsGoodLocalPlayer(slot))
					return false;
				return Memory::read<uintptr_t>(slot + Offsets::LocalPlayer_PlayerController)
					== tPlayerController;
			};

			auto tryLpSlots = [&](uintptr_t data, int count) -> bool {
				if (!data || !Memory::IsValidPtrFast2(data) || count <= 0)
					return false;
				const int limit = (count > 8) ? 8 : count;
				for (int i = 0; i < limit; ++i) {
					const uintptr_t slot = Memory::read<uintptr_t>(
						data + static_cast<size_t>(i) * sizeof(uintptr_t));
					if (slotOwnsPc(slot)) {
						tLocalPlayer = slot;
						return true;
					}
				}
				return false;
			};

			const uintptr_t arrData = Memory::read<uintptr_t>(
				tGameInstance + Offsets::LocalPlayers);
			const int arrNum = Memory::read<int>(tGameInstance + Offsets::LocalPlayers + 8);
			tryLpSlots(arrData, (arrNum > 0 && arrNum <= 16) ? arrNum : 4);
		}
	}

	if (!IsGoodLocalPlayer(tLocalPlayer) && tPlayerController)
		tLocalPlayer = ResolveLocalPlayerFromController(tPlayerController);

	// Help GI@0x4D8 may be encrypted — recover from LP Outer once LP is known.
	if (!tGameInstance && IsGoodLocalPlayer(tLocalPlayer))
		tGameInstance = ResolveGameInstanceFromLocalPlayer(tLocalPlayer);

	if (!IsGoodLocalPlayer(tLocalPlayer) && tGameInstance) {
		const uintptr_t arrData = Memory::read<uintptr_t>(
			tGameInstance + Offsets::LocalPlayers);
		const int arrNum = Memory::read<int>(tGameInstance + Offsets::LocalPlayers + 8);
		if (arrData && Memory::IsValidPtrFast2(arrData) && arrNum > 0 && arrNum <= 16) {
			const uintptr_t slot0 = Memory::read<uintptr_t>(arrData);
			if (IsGoodLocalPlayer(slot0))
				tLocalPlayer = slot0;
		}
	}

	if (!IsGoodLocalPlayer(tLocalPlayer) && IsGoodLocalPlayer(s_cachedLocalPlayer))
		tLocalPlayer = s_cachedLocalPlayer;
	else if (IsGoodLocalPlayer(tLocalPlayer))
		s_cachedLocalPlayer = tLocalPlayer;

	uintptr_t tPCM = 0;
	if (tPlayerController) {
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		PlayerController = tPlayerController;
		AcknowledgedPawn = tAcknowledgedPawn;
		RootComponent = tRootComponent;
		PlayerState = tPlayerState;
		PersistentLevel = tPersistentLevel;
		Actors = tActors;
		stateLock.unlock();

		tPCM = GetCameraManagerFromActors();
		if (!tPCM)
			tPCM = Memory::read<uintptr_t>(tPlayerController + Offsets::APlayerCameraManager);
		if (!tPCM && tPersistentLevel && tActors) {
			uintptr_t pcmPc = tPlayerController;
			uintptr_t pcmPawn = tAcknowledgedPawn;
			if (Engine::ResolvePcFromLevelCameraManager(
				tPersistentLevel, tActors, pcmPc, pcmPawn, tPCM)) {
				tPlayerController = pcmPc;
				tAcknowledgedPawn = pcmPawn;
				s_cachedPC = pcmPc;
				s_cachedPawn = pcmPawn;
			}
		}
	}

	// --- Publish all state atomically (brief lock ~microseconds) ---
	{
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		GWorld = tGWorld;
		GameInstance = tGameInstance;
		OwningGameInstance = tGameInstance;
		PersistentLevel = tPersistentLevel;
		localplayer = tLocalPlayer;
		PlayerController = tPlayerController;
		PioneerPlayerController = tPlayerController;
		AcknowledgedPawn = tAcknowledgedPawn;
		RootComponent = tRootComponent;
		PlayerState = tPlayerState;
		Actors = tActors;
		PlayerCameraManager = tPCM;
	}

	{
		static int s_dmaUpdates = 0;
		static auto s_lastFpsTime = std::chrono::steady_clock::now();
		++s_dmaUpdates;
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastFpsTime).count();
		if (elapsed >= 1000) {
			s_dmaUpdates = 0;
			s_lastFpsTime = now;
		}
	}

	if (tPlayerController)
		RefreshCameraFromViewTarget();

	TickRaidGate();

	// Original baseline re-resolved PC every Update; never reuse last raid's cached chain in hub/load.
	if (!m_raidRaw.load(std::memory_order_acquire)) {
		s_cachedPC = 0;
		s_cachedPawn = 0;
	}

	if (IsEspRaidActive()
		&& tPlayerController && tAcknowledgedPawn && tRootComponent) {
		entityStarted.store(true, std::memory_order_release);
	}
}

bool Engine::IsInRaidRaw() const
{
	uintptr_t sGWorld = 0;
	uintptr_t sPersistentLevel = 0;
	uintptr_t sActors = 0;
	uintptr_t sAcknowledgedPawn = 0;
	uintptr_t sPlayerController = 0;
	uintptr_t sRootComponent = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		sGWorld = GWorld;
		sPersistentLevel = PersistentLevel;
		sActors = Actors;
		sAcknowledgedPawn = AcknowledgedPawn;
		sPlayerController = PlayerController;
		sRootComponent = RootComponent;
	}

	if (!sGWorld || !sPersistentLevel || !sActors || !sAcknowledgedPawn || !sPlayerController)
		return false;

	const int actorCount = Memory::read<int>(sPersistentLevel + Offsets::ActorsCount);
	if (actorCount <= 0 || actorCount > 10000)
		return false;

	uintptr_t root = sRootComponent;
	if (!IsUsableObjectPtr(root))
		root = 0;
	if (!root)
		root = Engine::ResolveActorRoot(sAcknowledgedPawn);
	if (!root)
		return false;

	Vector3 pos = Memory::read<Vector3>(root + Offsets::RelativeLocation);
	if (!IsPlausibleWorldPos(pos))
		pos = Engine::ReadSceneWorldPos(root);
	if (!IsPlausibleWorldPos(pos))
		return false;

	const bool populatedWorld = actorCount >= kRaidPopulationThreshold;

	if (populatedWorld) {
		if (PawnLooksLikeHubCharacter(sAcknowledgedPawn))
			return false;
		if (LooksLikeHubWorld(sGWorld, sPersistentLevel))
			return false;
		if (!PawnHasRaidCombatComponents(sAcknowledgedPawn))
			return false;
		return true;
	}

	if (LooksLikeHubWorld(sGWorld, sPersistentLevel))
		return false;
	if (PawnLooksLikeHubCharacter(sAcknowledgedPawn))
		return false;
	if (!PawnHasRaidCombatComponents(sAcknowledgedPawn))
		return false;

	return true;
}

Engine::EngineStateSnapshot Engine::GetStateSnapshot() const
{
	EngineStateSnapshot snap{};
	std::shared_lock<std::shared_mutex> lock(m_stateMutex);
	snap.gWorld = GWorld;
	snap.gWorldRaw = m_gWorldRaw.load(std::memory_order_relaxed);
	snap.gWorldFailStep = m_gWorldFailStep.load(std::memory_order_relaxed);
	snap.persistentLevel = PersistentLevel;
	snap.actors = Actors;
	snap.playerController = PlayerController;
	snap.acknowledgedPawn = AcknowledgedPawn;
	snap.rootComponent = RootComponent;
	snap.playerCameraManager = PlayerCameraManager;
	snap.owningGameInstance = OwningGameInstance;
	snap.localPlayer = localplayer;
	return snap;
}

void Engine::ResetRaidTransitionState()
{
	s_cachedPC = 0;
	s_cachedPawn = 0;
	s_cachedLocalPlayer = 0;
	g_Camera = {};
	ClearFNameCache();
	g_bReadSimdConsts = false;
	g_bReadFNameKeyTable = false;
	steam_decrypt::ResetTables();
}

void Engine::ClearEspCaches()
{
	WorldScan::ClearCachedActorPtrs();
	{
		std::unique_lock<std::shared_mutex> lk(m_playerCacheMutex);
		playerCache.clear();
	}
	{
		std::unique_lock<std::shared_mutex> lk(m_containerCacheMutex);
		containerCache.clear();
	}
	{
		std::unique_lock<std::shared_mutex> lk(m_itemCacheMutex);
		itemCache.clear();
	}
	{
		std::unique_lock<std::shared_mutex> lk(m_robotCacheMutex);
		robotCache.clear();
	}
	for (EspRenderSnapshot& snap : m_espSnapshots) {
		snap.players.clear();
		snap.world.clear();
		snap.robots.clear();
	}
	{
		std::unique_lock<std::shared_mutex> lock(m_espFrameMutex);
		m_lastEspFrame = {};
	}
	entityStarted.store(false, std::memory_order_release);
}

void Engine::TickRaidGate()
{
	const bool raw = IsInRaidRaw();
	m_raidRaw.store(raw, std::memory_order_release);

	const auto now = std::chrono::steady_clock::now();
	const bool active = m_espRaidActive.load(std::memory_order_acquire);

	if (!raw) {
		m_raidEnterPending = false;
		if (!active) {
			m_raidFalseSince = {};
			return;
		}
		if (m_raidFalseSince.time_since_epoch().count() == 0)
			m_raidFalseSince = now;
		if (now - m_raidFalseSince < kRaidRawFalseGraceMs)
			return;

		m_espRaidActive.store(false, std::memory_order_release);
		m_raidFalseSince = {};
		ResetRaidTransitionState();
		ClearEspCaches();
		std::cout << "[raid] left" << std::endl;
		return;
	}

	m_raidFalseSince = {};
	if (active)
		return;

	if (!m_raidEnterPending) {
		m_raidEnterPending = true;
		m_raidDebSince = now;
		return;
	}

	if (now - m_raidDebSince >= kRaidEnterDelayMs) {
		m_raidEnterPending = false;
		ResetRaidTransitionState();
		m_espRaidActive.store(true, std::memory_order_release);
		ClearEspCaches();
		m_worldGeneration.fetch_add(1, std::memory_order_release);
		std::cout << "[raid] entered" << std::endl;
	}
}