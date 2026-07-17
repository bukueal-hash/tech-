#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/IntervalTimer.h"
#include "../Interface/Utils/Variables/index.h"
#include "WorldScanCommon.h"

#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace {


uintptr_t ReadWorldFromSlot(uint64_t base, std::ptrdiff_t slotOff)
{
	// Bypass VMM page cache — stale GWorld blocks raid re-entry until exe restart.
	const uintptr_t slot = Memory::read_nocache<uintptr_t>(base + slotOff);
	return Engine::IsPlausibleObjPtr(slot) ? slot : 0;
}

/** Help CL-1315578: PL @ 0x158 (also try prior 0x110), LevelCollections[i]+0x20, Levels[]. */
bool LevelLooksOwnedByWorld(uintptr_t level, uintptr_t world)
{
	if (!Engine::IsPlausibleObjPtr(level) || !world)
		return false;
	const uintptr_t owning = Memory::read<uintptr_t>(level + Offsets::Level_OwningWorld);
	if (owning == world)
		return true;
	const uintptr_t data = Memory::read<uintptr_t>(level + Offsets::AActors);
	const int count = Memory::read<int>(level + Offsets::ActorsCount);
	return Engine::IsPlausibleObjPtr(data) && count > 0 && count <= 10000;
}

uintptr_t ResolvePersistentLevelHelp(uintptr_t world)
{
	if (!Engine::IsPlausibleObjPtr(world))
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
	if (Engine::IsPlausibleObjPtr(collectionsData) && collectionsNum > 0 && collectionsNum <= 16) {
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
	if (Engine::IsPlausibleObjPtr(levelsData) && levelsNum > 0 && levelsNum < 512) {
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
	if (!Engine::IsPlausibleObjPtr(gs))
		return 0;

	const uintptr_t arrData = Memory::read<uintptr_t>(gs + Offsets::GameState_PlayerArray);
	const int32_t arrNum = Memory::read<int32_t>(gs + Offsets::GameState_PlayerArray + 8);
	if (!Engine::IsPlausibleObjPtr(arrData) || arrNum <= 0 || arrNum > 128)
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
	if (!Engine::IsPlausibleObjPtr(slot))
		return 0;
	if (ResolvePersistentLevelHelp(slot))
		return slot;
	// Some builds store GWorld** (slot → UWorld*). Accept only if PL validates.
	const uintptr_t inner = Memory::read<uintptr_t>(slot);
	if (Engine::IsPlausibleObjPtr(inner) && ResolvePersistentLevelHelp(inner))
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

// Soft retain when a single Update() frame fails to re-resolve the PC chain.
static uintptr_t s_retainPC = 0;
static uintptr_t s_retainPawn = 0;
static uintptr_t s_retainRoot = 0;
static uintptr_t s_retainGI = 0;
static uintptr_t s_retainLP = 0;
static uintptr_t s_retainPCM = 0;
static uintptr_t s_retainPS = 0;
static std::chrono::steady_clock::time_point s_retainGoodAt{};
static int s_cacheFailStreak = 0;
static constexpr int kCacheFailClearAfter = 8; // ~8 Update ticks before nuking s_cachedPC
static constexpr auto kPcChainRetainMs = std::chrono::milliseconds(2500);

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

static bool FNameTokensMatch(uintptr_t world, uintptr_t level, const char* const* tokens, int count)
{
	for (int i = 0; i < count; ++i) {
		const char* tok = tokens[i];
		if (ObjectFNameContains(world, tok) || ObjectFNameContains(level, tok))
			return true;
	}
	return false;
}

static constexpr const char* kStrictHubTokens[] = {
	"sprocket", "mainmenu", "frontend", "front_end",
	"lobby", "loadout", "hideout", "menuworld", "bilguun",
	// Post-raid score screen (log: world "EndofRound"). Not listing it kept
	// raidRaw true on the stay path until the world pointer changed.
	"endofround",
};
static constexpr int kStrictHubTokenCount =
	static_cast<int>(sizeof(kStrictHubTokens) / sizeof(kStrictHubTokens[0]));

static bool LooksLikeStrictHubWorld(uintptr_t world, uintptr_t level)
{
	return FNameTokensMatch(world, level, kStrictHubTokens, kStrictHubTokenCount);
}

static bool LooksLikeHubWorld(uintptr_t world, uintptr_t level)
{
	// Strict hub only — do NOT treat "persistence" as hub (false mid-raid leaves).
	return LooksLikeStrictHubWorld(world, level);
}

static bool PawnLooksLikeHubCharacter(uintptr_t pawn)
{
	static const char* kHubPawnTokens[] = {
		"sprocket", "mainmenu", "lobby", "loadout", "hideout",
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

/** Human-readable reason for IsInRaidRaw==false (diagnostics only — does not change gates). */
static const char* DiagnoseRaidRawReason(
	uintptr_t gw, uintptr_t pl, uintptr_t actors, uintptr_t pawn,
	uintptr_t pc, uintptr_t root, bool espActive)
{
	if (!gw || !pl)
		return "no_world";
	if (espActive) {
		if (LooksLikeStrictHubWorld(gw, pl))
			return "strict_hub_world";
		if (pawn && PawnLooksLikeHubCharacter(pawn))
			return "hub_pawn";
		return "ok_stay";
	}
	if (!actors)
		return "no_actors";
	if (!pawn)
		return "no_pawn";
	if (!pc)
		return "no_pc";
	const int actorCount = Memory::read<int>(pl + Offsets::ActorsCount);
	if (actorCount <= 0 || actorCount > 10000)
		return "bad_actor_count";
	uintptr_t r = root;
	if (!Memory::IsValidPtrFast2(r))
		r = 0;
	if (!r)
		r = Engine::ResolveActorRoot(pawn);
	if (!r)
		return "no_root";
	Vector3 pos = Memory::read<Vector3>(r + Offsets::RelativeLocation);
	if (!IsPlausibleWorldPos(pos))
		pos = Engine::ReadSceneWorldPos(r);
	if (!IsPlausibleWorldPos(pos))
		return "bad_pos";
	if (LooksLikeHubWorld(gw, pl))
		return "hub_world";
	if (PawnLooksLikeHubCharacter(pawn))
		return "hub_pawn";
	if (!PawnHasRaidCombatComponents(pawn))
		return "no_combat_comps";
	return "ok_enter";
}

static std::string RaidJsonEscape(std::string s)
{
	std::string o;
	o.reserve(s.size());
	for (char c : s) {
		if (c == '"' || c == '\\')
			o.push_back('\\');
		if (static_cast<unsigned char>(c) < 32)
			continue;
		o.push_back(c);
	}
	return o;
}

static std::string PeekObjFName(uintptr_t obj)
{
	if (!obj || !Memory::IsValidPtrFast2(obj))
		return {};
	return steam_decrypt::GetActorFNameString(obj);
}

static const char* MatchHubToken(uintptr_t world, uintptr_t level)
{
	for (int i = 0; i < kStrictHubTokenCount; ++i) {
		const char* tok = kStrictHubTokens[i];
		if (ObjectFNameContains(world, tok) || ObjectFNameContains(level, tok))
			return tok;
	}
	return "";
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
	const uint64_t base = Memory::getBaseAddress();
	const uintptr_t tGWorld = ResolveBestGWorld(base);
	if (!tGWorld) {
		HandleWorldLost();
		TickRaidGate();
		return;
	}

	// --- Read entire pointer chain into locals (NO lock held, slow I/O here) ---
	uintptr_t tGameInstance = GetGameInstance(tGWorld), tPersistentLevel = 0, tLocalPlayer = 0;
	uintptr_t tPlayerController = 0, tAcknowledgedPawn = 0;
	uintptr_t tRootComponent = 0, tActors = 0, tPlayerState = 0;

	tPersistentLevel = ResolvePersistentLevelHelp(tGWorld);

	CheckWorldTransition(tGWorld, tPersistentLevel);
	{
		static uintptr_t s_lastGWorldForLp = 0;
		if (tGWorld != s_lastGWorldForLp) {
			s_cachedLocalPlayer = 0;
			s_lastGWorldForLp = tGWorld;
		}
	}

	int actorCount = 0;
	if (tPersistentLevel)
		ResolveLevelActors(tPersistentLevel, tActors, actorCount);

	// Resolve PC via actor scan (backup: Pioneer PC owns PCM @ PC+0x48).
	const char* pcPath = "none";
	float dbgPcmFov = 0.f;
	uintptr_t dbgPcmPtr = 0;
	const char* cacheInvalidateReason = "none";
	bool cacheInvalidatedThisFrame = false;
	{
		// Keep cached PC while pawn root looks sane. Do NOT require
		// ControllerHasValidPcm here — DefaultFOV/ViewTarget flap cleared cache
		// every ~0.5s (pc_drop_fov_gate), forcing ResolvePcFromLevelCameraManager
		// over ~1200 actors and ~1s DMA freezes (post-fix logs still 100+ drops).
		if (s_cachedPC && s_cachedPawn
			&& IsValidPointer(s_cachedPC) && IsValidPointer(s_cachedPawn)) {
			const uintptr_t root =
				Memory::read_nocache<uintptr_t>(s_cachedPawn + Offsets::RootComponent);
			if (root && IsValidPointer(root)) {
				Vector3 pos =
					Memory::read_nocache<Vector3>(root + Offsets::RelativeLocation);
				float magSq = static_cast<float>(
					pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
				if (magSq <= 10000.f || magSq >= 1.0e14f) {
					const Engine::FVector3d w =
						Memory::read_nocache<Engine::FVector3d>(
							root + Offsets::ComponentToWorld + 0x20);
					pos = Engine::ToVector3(w);
					magSq = static_cast<float>(
						pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
				}
				if (magSq > 10000.f && magSq < 1.0e14f) {
					tPlayerController = s_cachedPC;
					tAcknowledgedPawn = s_cachedPawn;
					pcPath = "cached";
					s_cacheFailStreak = 0;
					if (s_cachedPC && IsValidPointer(s_cachedPC)) {
						dbgPcmPtr = Memory::read_nocache<uintptr_t>(
							s_cachedPC + Offsets::APlayerCameraManager);
						if (dbgPcmPtr && Memory::IsValidPtrFast2(dbgPcmPtr))
							dbgPcmFov = Memory::read_nocache<float>(
								dbgPcmPtr + Offsets::DefaultFOV);
					}
				} else {
					cacheInvalidateReason = "pos_magSq";
					++s_cacheFailStreak;
				}
			} else {
				cacheInvalidateReason = "pos_magSq";
				++s_cacheFailStreak;
			}
			if (s_cacheFailStreak >= kCacheFailClearAfter
				&& std::strcmp(cacheInvalidateReason, "pos_magSq") == 0) {
				cacheInvalidatedThisFrame = true;
				s_cachedPC = 0;
				s_cachedPawn = 0;
				s_cacheFailStreak = 0;
			}
		} else if (s_cachedPC || s_cachedPawn) {
			cacheInvalidateReason = "pair_incomplete";
			++s_cacheFailStreak;
			if (s_cacheFailStreak >= kCacheFailClearAfter) {
				cacheInvalidatedThisFrame = true;
				s_cachedPC = 0;
				s_cachedPawn = 0;
				s_cacheFailStreak = 0;
			}
		}

		// Prefer PCM FName → PCOwner@0x420 → AckPawn (help/esp.txt Step 4–5).
		// LocalPlayer/GI→LP may be encrypted — never gate identity on LP.
		if (!tPlayerController && tPersistentLevel && tActors) {
			uintptr_t pcmPc = 0;
			uintptr_t pcmPawn = 0;
			uintptr_t pcmDummy = 0;
			if (Engine::ResolvePcFromLevelCameraManager(
				tPersistentLevel, tActors, pcmPc, pcmPawn, pcmDummy)) {
				tPlayerController = s_cachedPC = pcmPc;
				tAcknowledgedPawn = s_cachedPawn = pcmPawn;
				pcPath = "cam_mgr";
				dbgPcmPtr = pcmDummy;
				if (dbgPcmPtr && Memory::IsValidPtrFast2(dbgPcmPtr))
					dbgPcmFov = Memory::read<float>(dbgPcmPtr + Offsets::DefaultFOV);
			}
		}

		if (!tPlayerController && tPersistentLevel && tActors && actorCount > 0) {
			uintptr_t scannedPc = 0;
			uintptr_t scannedPawn = 0;
			if (ResolveLocalPlayerChainFromActors(
				tPersistentLevel, tActors, actorCount, tGameInstance, scannedPc, scannedPawn)
				&& Engine::ControllerHasValidPcm(scannedPc)) {
				tPlayerController = s_cachedPC = scannedPc;
				tAcknowledgedPawn = s_cachedPawn = scannedPawn;
				pcPath = "actor_scan";
			}
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

	// LP resolution is best-effort only (encrypted GI→LP is non-fatal).
	// Camera / self identity already comes from PCM→PCOwner→AckPawn above.
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
	// Publish level/actors early so GetCameraManagerFromActors can FName-scan
	// even when PC is still missing (LP=0 must not block camera).
	{
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		if (tPlayerController)
			PlayerController = tPlayerController;
		if (tAcknowledgedPawn)
			AcknowledgedPawn = tAcknowledgedPawn;
		if (tRootComponent)
			RootComponent = tRootComponent;
		if (tPlayerState)
			PlayerState = tPlayerState;
		PersistentLevel = tPersistentLevel;
		Actors = tActors;
	}

	tPCM = GetCameraManagerFromActors();
	if (!tPCM && tPlayerController)
		tPCM = Memory::read<uintptr_t>(tPlayerController + Offsets::APlayerCameraManager);
	if ((!tPCM || !tPlayerController) && tPersistentLevel && tActors) {
		uintptr_t pcmPc = tPlayerController;
		uintptr_t pcmPawn = tAcknowledgedPawn;
		uintptr_t foundPcm = tPCM;
		if (Engine::ResolvePcFromLevelCameraManager(
			tPersistentLevel, tActors, pcmPc, pcmPawn, foundPcm)) {
			tPCM = foundPcm;
			tPlayerController = pcmPc;
			tAcknowledgedPawn = pcmPawn;
			s_cachedPC = pcmPc;
			s_cachedPawn = pcmPawn;
			if (std::strcmp(pcPath, "none") == 0 || std::strcmp(pcPath, "retain") == 0)
				pcPath = "cam_mgr";
			if (!tRootComponent && tAcknowledgedPawn)
				tRootComponent = Memory::read<uintptr_t>(
					tAcknowledgedPawn + Offsets::RootComponent);
		}
	}

	// Retain last known-good PC chain for a short window so a transient FOV/PCM
	// glitch cannot zero the entire local-player pipeline mid-raid.
	const auto nowRetain = std::chrono::steady_clock::now();
	bool usedRetain = false;
	if (tPlayerController && tAcknowledgedPawn) {
		s_retainPC = tPlayerController;
		s_retainPawn = tAcknowledgedPawn;
		s_retainRoot = tRootComponent;
		s_retainGI = tGameInstance;
		s_retainLP = tLocalPlayer;
		s_retainPCM = tPCM;
		s_retainPS = tPlayerState;
		s_retainGoodAt = nowRetain;
	} else if (s_retainPC && (nowRetain - s_retainGoodAt) < kPcChainRetainMs) {
		if (!tPlayerController) {
			tPlayerController = s_retainPC;
			usedRetain = true;
			pcPath = "retain";
		}
		if (!tAcknowledgedPawn)
			tAcknowledgedPawn = s_retainPawn;
		if (!tRootComponent)
			tRootComponent = s_retainRoot;
		if (!tGameInstance)
			tGameInstance = s_retainGI;
		if (!IsGoodLocalPlayer(tLocalPlayer) && IsGoodLocalPlayer(s_retainLP))
			tLocalPlayer = s_retainLP;
		if (!tPCM)
			tPCM = s_retainPCM;
		if (!tPlayerState)
			tPlayerState = s_retainPS;
		if (!s_cachedPC) {
			s_cachedPC = s_retainPC;
			s_cachedPawn = s_retainPawn;
		}
	} else if (!tPlayerController) {
		s_retainPC = 0;
		s_retainPawn = 0;
		s_retainRoot = 0;
		s_retainGI = 0;
		s_retainLP = 0;
		s_retainPCM = 0;
		s_retainPS = 0;
	}
	(void)usedRetain;

	// --- Publish all state atomically (brief lock ~microseconds) ---
	{
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		GWorld = tGWorld;
		GameInstance = tGameInstance;
		OwningGameInstance = tGameInstance;
		PersistentLevel = tPersistentLevel;
		localplayer = tLocalPlayer;
		PlayerController = tPlayerController;
		// Only actor-scan finds the real PioneerPlayerController by fname;
		// do not mirror generic GI/PCM PC into this field.
		if (std::strcmp(pcPath, "actor_scan") == 0)
			PioneerPlayerController = tPlayerController;
		AcknowledgedPawn = tAcknowledgedPawn;
		RootComponent = tRootComponent;
		PlayerState = tPlayerState;
		Actors = tActors;
		PlayerCameraManager = tPCM;
		AGameStateBase = ResolveGameStateFromWorld(tGWorld);
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

	// Camera refresh does not require LocalPlayer. Prefer PCM ViewTarget POV
	// (0x4B0+0x10); ResolvePcFromLevelCameraManager recovers PC if needed.
	if (IsEspRaidActive() && (tPlayerController || tPCM || (tPersistentLevel && tActors)))
		RefreshCameraFromViewTarget();

	TickRaidGate();
	TickEspCacheReadiness();

	// Do NOT clear s_cachedPC every hub tick. That forced ResolvePcFromLevelCameraManager
	// over the menu actor list every Update and caused multi-hundred-ms DMA stalls
	// while waiting in MainMenu.
	// Cache is already reset on world_change / raid_left / ResetRaidTransitionState.

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

	if (!sGWorld || !sPersistentLevel)
		return false;

	// Stay path: once ESP raid is active, only lose raid on world loss or hub.
	// Do not require PC/pawn/root/combat — mid-raid DMA flakes were firing [raid] left.
	// Do NOT use "pawn gone + party<=1": solo raids always have partyCount==1, so a
	// brief null pawn read falsely dropped EspRaid and forced a slow re-arm.
	if (m_espRaidActive.load(std::memory_order_acquire)) {
		if (LooksLikeStrictHubWorld(sGWorld, sPersistentLevel))
			return false;
		if (sAcknowledgedPawn && PawnLooksLikeHubCharacter(sAcknowledgedPawn))
			return false;
		return true;
	}

	// Enter / cold path — stricter.
	if (!sActors || !sAcknowledgedPawn || !sPlayerController)
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
	s_retainPC = 0;
	s_retainPawn = 0;
	s_retainRoot = 0;
	s_retainGI = 0;
	s_retainLP = 0;
	s_retainPCM = 0;
	s_retainPS = 0;
	s_cacheFailStreak = 0;
	g_Camera = {};
	ClearFNameCache();
	g_fnameTablesReady = false;
	steam_decrypt::ResetTables();
	ArcActorType::RuntimeActorTypeOffset() = -1;
}

void Engine::HandleWorldLost()
{
	if (m_lastWorldPtr == 0
		&& !m_espRaidActive.load(std::memory_order_acquire))
		return;

	// Drop VMM page/TLB/VAD caches so the next GWorld resolve is live.
	PCIMemory::FullRefresh();

	{
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		GWorld = 0;
		GameInstance = 0;
		OwningGameInstance = 0;
		PersistentLevel = 0;
		localplayer = 0;
		PlayerController = 0;
		PioneerPlayerController = 0;
		AcknowledgedPawn = 0;
		RootComponent = 0;
		PlayerState = 0;
		Actors = 0;
		PlayerCameraManager = 0;
		AGameStateBase = 0;
	}

	m_lastWorldPtr = 0;
	m_lastPersistentLevel = 0;
	m_espRaidActive.store(false, std::memory_order_release);
	m_espDrawReady.store(false, std::memory_order_release);
	m_raidEnterPending = false;
	m_partyEnterPending = false;
	m_raidFalseSince = {};
	ResetRaidTransitionState();
	ClearEspCaches();
	std::cout << "[raid] world_lost" << std::endl;
}

void Engine::CheckWorldTransition(uintptr_t newWorld, uintptr_t newPersistentLevel)
{
	if (newWorld == m_lastWorldPtr && newPersistentLevel == m_lastPersistentLevel)
		return;

	// Drop VMM page/TLB/VAD caches so freshly-allocated raid memory is visible
	// immediately (otherwise READCACHE/PROCCACHE TTLs can stall until restart).
	PCIMemory::FullRefresh();

	ResetRaidTransitionState();
	ClearEspCaches();

	m_lastWorldPtr = newWorld;
	m_lastPersistentLevel = newPersistentLevel;
	m_worldGeneration.fetch_add(1, std::memory_order_release);
	m_espRaidActive.store(false, std::memory_order_release);
	m_espDrawReady.store(false, std::memory_order_release);
	m_raidEnterPending = false;
	m_partyEnterPending = false;
	m_raidFalseSince = {};
}

void Engine::ClearEspCaches()
{
	WorldScan::ClearCachedActorPtrs();
	WorldScan::ClearItemScannerStaticState();
	WorldScan::ClearContainerScannerStaticState();
	WorldScan::ClearRobotScannerStaticState();
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
	{
		std::unique_lock<std::shared_mutex> lock(m_espFrameMutex);
		m_lastEspFrame = {};
	}
	entityStarted.store(false, std::memory_order_release);
	m_espDrawReady.store(false, std::memory_order_release);
	m_lastEspFrameValid.store(false, std::memory_order_release);
}

int Engine::CountGameStatePlayerArray() const
{
	uintptr_t gs = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		gs = AGameStateBase;
	}
	if (!gs || !IsUsableObjectPtr(gs))
		return 0;

	const int32_t arrNum =
		Memory::read<int32_t>(gs + Offsets::GameState_PlayerArray + 8);
	if (arrNum < 0 || arrNum > 64)
		return 0;
	return static_cast<int>(arrNum);
}

void Engine::TickEspCacheReadiness()
{
	if (!m_espRaidActive.load(std::memory_order_acquire)) {
		m_espDrawReady.store(false, std::memory_order_release);
		return;
	}
	if (m_espDrawReady.load(std::memory_order_acquire))
		return;

	const size_t players = PlayerCacheCount();
	const size_t bots = RobotCacheCount();
	const size_t world = WorldCacheCount();

	if (players > 0 && bots > 0 && world > 0) {
		m_espDrawReady.store(true, std::memory_order_release);
		std::cout << "[raid] caches_ready"
			<< " players=" << players
			<< " bots=" << bots
			<< " world=" << world
			<< std::endl;
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (m_raidArmedSince.time_since_epoch().count() == 0)
		return;
	if (now - m_raidArmedSince < kEspCacheReadySoftMs)
		return;
	if (!entityStarted.load(std::memory_order_acquire))
		return;
	if (players + bots + world == 0)
		return;

	m_espDrawReady.store(true, std::memory_order_release);
	std::cout << "[raid] caches_ready_soft"
		<< " players=" << players
		<< " bots=" << bots
		<< " world=" << world
		<< std::endl;
}

void Engine::TickRaidGate()
{
	const bool raw = IsInRaidRaw();
	m_raidRaw.store(raw, std::memory_order_release);

	const auto now = std::chrono::steady_clock::now();
	const bool active = m_espRaidActive.load(std::memory_order_acquire);

	uintptr_t gw = 0, pl = 0, actors = 0, pc = 0, pawn = 0, root = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_stateMutex);
		gw = GWorld;
		pl = PersistentLevel;
		actors = Actors;
		pc = PlayerController;
		pawn = AcknowledgedPawn;
		root = RootComponent;
	}
	const int partyCount = CountGameStatePlayerArray();
	const char* rawReason = DiagnoseRaidRawReason(
		gw, pl, actors, pawn, pc, root, active);

	if (!raw) {
		m_raidEnterPending = false;
		m_partyEnterPending = false;
		if (!active) {
			m_raidFalseSince = {};
			return;
		}
		if (m_raidFalseSince.time_since_epoch().count() == 0)
			m_raidFalseSince = now;

		if (var::show_debug_overlay) {
			static std::chrono::steady_clock::time_point s_lastHoldLog{};
			if (s_lastHoldLog.time_since_epoch().count() == 0
				|| now - s_lastHoldLog >= std::chrono::seconds(1)) {
				s_lastHoldLog = now;
				const bool hub = LooksLikeStrictHubWorld(gw, pl);
				std::cout << "[raid] raw_false_hold"
					<< " gWorld=" << std::hex << gw
					<< " hub=" << std::dec << (hub ? 1 : 0)
					<< " pc=" << std::hex << pc
					<< " pawn=" << pawn << std::dec
					<< " reason=" << rawReason
					<< std::endl;
			}
		}

		if (now - m_raidFalseSince < kRaidRawFalseGraceMs)
			return;

		m_espRaidActive.store(false, std::memory_order_release);
		m_espDrawReady.store(false, std::memory_order_release);
		m_raidFalseSince = {};
		m_raidArmedSince = {};
		ResetRaidTransitionState();
		ClearEspCaches();
		std::cout << "[raid] left" << std::endl;
		return;
	}

	m_raidFalseSince = {};
	if (active)
		return;

	if (partyCount >= kPartyMinPlayers) {
		if (!m_partyEnterPending) {
			m_partyEnterPending = true;
			m_partyDebSince = now;
			m_raidEnterPending = false;
			std::cout << "[raid] party_wait players=" << partyCount << std::endl;
			return;
		}
		if (now - m_partyDebSince < kPartyEnterDelayMs)
			return;
	} else {
		m_partyEnterPending = false;
	}

	if (!m_raidEnterPending) {
		m_raidEnterPending = true;
		m_raidDebSince = now;
		std::cout << "[raid] enter_wait players=" << partyCount << std::endl;
		return;
	}

	if (now - m_raidDebSince < kRaidEnterDelayMs)
		return;

	m_raidEnterPending = false;
	m_partyEnterPending = false;
	// World transitions already ResetRaidTransitionState. Re-reset here wiped
	// FName/decrypt tables and camera every arm → ESP looked "broken" for seconds.
	ClearEspCaches();
	m_espDrawReady.store(false, std::memory_order_release);
	m_raidArmedSince = now;
	m_espRaidActive.store(true, std::memory_order_release);
	m_worldGeneration.fetch_add(1, std::memory_order_release);
	std::cout << "[raid] entered players=" << partyCount << std::endl;
}
