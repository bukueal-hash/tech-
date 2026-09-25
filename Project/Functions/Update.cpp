#include "../Core/Engine.h"
#include "../Core/Reflection.hpp"   // SDK FField/FProperty walker + FString self-check
#include "../Core/AgentLog.h"
#include "../Core/ActorType.h"
#include "../Core/IntervalTimer.h"
#include "../Interface/Utils/Variables/index.h"
#include "WorldScanCommon.h"
#include "CollisionMirror.h"

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
#include <string_view>

namespace {


// The world rungs these used to be live on Engine now (Functions/Utils.cpp), so
// the ladder in Core/PlayerChain.hpp can order them: Engine::ReadWorldSlot,
// Engine::ResolvePersistentLevel, Engine::WorldFromGameStateGlobal.

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
// The world the ladder resolved last tick; it is the seed for the world hop's
// cache rung (which re-proves it before trusting it).
static uintptr_t s_lastWorld = 0;

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

static std::string PeekObjFName(uintptr_t obj)
{
	if (!obj || !Memory::IsValidPtrFast2(obj))
		return {};
	return steam_decrypt::GetActorFNameString(obj);
}

// Map-name raid signal (user model: "in a map = in a raid").
// Raid persistent levels follow UE naming: TheDam_02_P, Spaceport_01_P, ...
// Hub/menu worlds (MainMenu, EndofRound, Disco*, Sprocket) never end in _P.
static bool WorldNameLooksLikeRaidMap(uintptr_t world, uintptr_t level)
{
	if (LooksLikeStrictHubWorld(world, level))
		return false;
	std::string n = PeekObjFName(world);
	if (n.size() < 4)
		return false;
	for (char& c : n)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return n.compare(n.size() - 2, 2, "_p") == 0;
}

/** Human-readable reason for IsInRaidRaw==false (diagnostics only — does not change gates). */
static const char* DiagnoseRaidRawReason(
	uintptr_t gw, uintptr_t pl, uintptr_t actors, uintptr_t pawn,
	uintptr_t pc, uintptr_t root, bool espActive)
{
	if (!gw || !pl)
		return "no_world";
	if (LooksLikeStrictHubWorld(gw, pl))
		return "hub_world";
	if (WorldNameLooksLikeRaidMap(gw, pl))
		return "raid_map";
	if (espActive) {
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
	int32_t actorCount = 0;
	uintptr_t actorData = 0;
	if (!WorldScan::ReadLevelActors(pl, actorData, actorCount))
		return "no_actors";
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

static const char* MatchHubToken(uintptr_t world, uintptr_t level)
{
	for (int i = 0; i < kStrictHubTokenCount; ++i) {
		const char* tok = kStrictHubTokens[i];
		if (ObjectFNameContains(world, tok) || ObjectFNameContains(level, tok))
			return tok;
	}
	return "";
}

// One-shot backup rescan 5s after ESP arms (user spec).
static bool s_backupScanPending = false;

// #region agent log
static void AgentRaidLog(
	const char* hypothesisId,
	const char* location,
	const char* message,
	const std::string& dataJson)
{
	// Verify taps: only raid session lifecycle messages reach the real log
	// (heartbeat is throttled to 1 Hz upstream). Everything else stays on NUL
	// so per-frame noise can never reappear.
	const std::string_view msg(message);
	const bool verifyTap =
		msg == "raid_hb" || msg == "raid_edge" || msg == "raid_entered"
		|| msg == "raid_left" || msg == "world_lost";
	std::ofstream f(verifyTap ? kArcVerifyPath : kArcDebugLogPath, std::ios::app);
	if (!f)
		return;
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	f << "{\"sessionId\":\"c190fb\",\"runId\":\"pre-fix\",\"hypothesisId\":\""
		<< hypothesisId << "\",\"location\":\"" << location
		<< "\",\"message\":\"" << message << "\",\"data\":" << dataJson
		<< ",\"timestamp\":" << ms << "}\n";
}
// #endregion

} // namespace

static void TracePcDiscovery(
    const char* pcPath,
    uintptr_t pc, uintptr_t pawn, uintptr_t root, uintptr_t pcm,
    uintptr_t gi, uintptr_t lp,
    int actorCount,
    const char* detail)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "pc=%llX pawn=%llX root=%llX pcm=%llX gi=%llX lp=%llX actors=%d path=%s detail=%s",
        (unsigned long long)pc, (unsigned long long)pawn,
        (unsigned long long)root, (unsigned long long)pcm,
        (unsigned long long)gi, (unsigned long long)lp,
        actorCount, pcPath, detail ? detail : "");
    AgentRaidLog("TR", "Update.cpp:TracePcDiscovery", pcPath, buf);
}

void Engine::Update() {
	const uint64_t base = Memory::getBaseAddress();

	// SDK FString self-check (one-shot per session): reads the game's
	// verification FString (Offsets::FStringVerificationRva) and records which
	// player-name scramble actually decodes it; the proven one is then tried
	// first for every name read.
	if (!m_fstrChecked && base) {
		const Reflection::FStringCheck check = Reflection::CheckFStringPipeline(base);
		m_fstrChecked = true;
		m_fstrOk = check.readOk;
		m_fstrKey = check.candidate;
		m_fstrText = check.text;
		// candidate 4 is the SDK drop's own FString pipeline (rol 14, state
		// advance before the XOR) — see steam_decrypt::DecryptPlayerNameSdk.
		if (check.candidate >= 0 && check.candidate <= 4)
			steam_decrypt::SetPreferredNameKey(check.candidate);
	}

	// ── The player chain: one ladder, one state (Core/PlayerChain.hpp) ──
	// The five overlapping passes are gone. The world (slot / inner / GameState),
	// the GameInstance (owning slot / LP outer / decrypt / legacy / scan), the
	// controller (flag / pair cache / camera manager / actor scan / GI array), the
	// local player (GI array / PC decrypt / PC slot scan) and the pawn all live in
	// one ordered walk that names the rung it used. This tick only seeds that walk
	// from the previous one, then publishes what the ladder decided.
	const auto nowRetain = std::chrono::steady_clock::now();
	PlayerChain::Seed seed;
	seed.base = base;
	seed.world = s_lastWorld;
	seed.worldStillValid = s_lastWorld != 0
		&& ResolvePersistentLevel(s_lastWorld) != 0;
	seed.pc = s_cachedPC;
	seed.pawn = s_cachedPawn;
	seed.gi = s_retainGI;
	seed.lp = IsGoodLocalPlayer(s_cachedLocalPlayer) ? s_cachedLocalPlayer : s_retainLP;
	seed.root = s_retainRoot;
	seed.pcm = s_retainPCM;
	seed.ps = s_retainPS;
	seed.retainValid = s_retainPC != 0
		&& (nowRetain - s_retainGoodAt) < kPcChainRetainMs;

	uintptr_t tActors = 0;
	int actorCount = 0;
	ResolvePlayerChain(base, seed, tActors, actorCount);
	const PlayerChain::State chain = GetChainState();

	const uintptr_t tGWorld = chain.Ptr(PlayerChain::Hop::World);
	if (!tGWorld) {
		HandleWorldLost();
		TickRaidGate();
		return;
	}

	const uintptr_t tPersistentLevel = chain.Ptr(PlayerChain::Hop::Level);
	CheckWorldTransition(tGWorld, tPersistentLevel);
	{
		static uintptr_t s_lastGWorldForLp = 0;
		if (tGWorld != s_lastGWorldForLp) {
			s_cachedLocalPlayer = 0;
			s_lastGWorldForLp = tGWorld;
		}
	}
	s_lastWorld = tGWorld;

	// Straight out of the ladder - nothing below may re-decide a hop.
	uintptr_t tGameInstance = chain.Ptr(PlayerChain::Hop::GameInstance);
	uintptr_t tLocalPlayer = chain.Ptr(PlayerChain::Hop::LocalPlayer);
	uintptr_t tPlayerController = chain.Ptr(PlayerChain::Hop::Controller);
	uintptr_t tAcknowledgedPawn = chain.Ptr(PlayerChain::Hop::Pawn);
	uintptr_t tRootComponent = chain.Ptr(PlayerChain::Hop::Root);
	uintptr_t tPlayerState = chain.Ptr(PlayerChain::Hop::State);
	uintptr_t tPCM = chain.Ptr(PlayerChain::Hop::CameraManager);

	// The rung that won the controller hop is the path tag the panel and the raid
	// logs used to get from a hand-maintained string.
	const char* pcPath = PlayerChain::RungName(
		chain.RungOf(PlayerChain::Hop::Controller));
	const bool cacheInvalidatedThisFrame = m_pairCacheInvalidated;
	if (cacheInvalidatedThisFrame) {
		s_cachedPC = 0;
		s_cachedPawn = 0;
		m_pairCacheFailStreak = 0;
	}
	if (tPlayerController)
		s_cachedPC = tPlayerController;
	if (tAcknowledgedPawn)
		s_cachedPawn = tAcknowledgedPawn;
	if (IsGoodLocalPlayer(tLocalPlayer))
		s_cachedLocalPlayer = tLocalPlayer;

	// Retain window: record what the ladder ended with. The ladder's own cached
	// rungs replay these values, so this never publishes anything itself; the
	// entries are dropped once the window has expired.
	if (tPlayerController && tAcknowledgedPawn) {
		s_retainPC = tPlayerController;
		s_retainPawn = tAcknowledgedPawn;
		s_retainRoot = tRootComponent;
		s_retainGI = tGameInstance;
		s_retainLP = tLocalPlayer;
		if (tPCM)
			s_retainPCM = tPCM;
		s_retainPS = tPlayerState;
		s_retainGoodAt = nowRetain;
	} else if (nowRetain - s_retainGoodAt >= kPcChainRetainMs) {
		s_retainPC = 0;
		s_retainPawn = 0;
		s_retainRoot = 0;
		s_retainGI = 0;
		s_retainLP = 0;
		s_retainPCM = 0;
		s_retainPS = 0;
	}


	// Log final PC discovery result
	{
		static int s_traceCounter = 0;
		if (++s_traceCounter % 120 == 1) {  // log every ~2s at 60fps Update
			TracePcDiscovery(pcPath, tPlayerController, tAcknowledgedPawn,
				tRootComponent, tPCM, tGameInstance, tLocalPlayer, actorCount,
				cacheInvalidatedThisFrame ? "pairCache" : nullptr);
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
		// The ladder's controller rung is authoritative; no later camera scan
		// or legacy pass may overwrite it.
		if (chain.RungOf(PlayerChain::Hop::Controller) == PlayerChain::Rung::ActorScan)
			PioneerPlayerController = tPlayerController;
		AcknowledgedPawn = tAcknowledgedPawn;
		RootComponent = tRootComponent;
		PlayerState = tPlayerState;
		Actors = tActors;
		ActorsCount = actorCount;
		if (tPCM)
			PlayerCameraManager = tPCM;
		AGameStateBase = ResolveGameStateFromWorld(tGWorld);
		if (var::show_raid_hud || var::show_radar)
			RefreshRaidDashboard();
	}

	// SDK reflection walker (Core/Reflection.hpp): when the pawn changes, decode
	// its UClass through the SDK's FField/FProperty routines (SuperStruct chain +
	// PropertyLink chain, property name + offset decoders). Bounded to the first
	// properties so the walk stays cheap; DMA reads happen outside the lock.
	if (base && tAcknowledgedPawn && tAcknowledgedPawn != m_reflectPawn) {
		const uintptr_t uclass = Reflection::ClassOf(tAcknowledgedPawn);
		const Reflection::ClassReport report =
			Reflection::BuildReport(uclass, base, 48, /*wantTypes=*/false);

		std::string sample;
		for (size_t i = 0; i < report.properties.size() && i < 3; ++i) {
			char line[128];
			std::snprintf(line, sizeof(line), "%s%s=0x%X", i ? ", " : "",
				report.properties[i].name.c_str(), report.properties[i].offset);
			sample += line;
		}

		{
			std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
			m_reflectPawn = tAcknowledgedPawn;
			m_reflectDepth = static_cast<int>(report.chain.size());
			m_reflectProps = static_cast<int>(report.properties.size());
			m_reflectOk = !report.chain.empty() && !report.properties.empty();
			m_reflectSample = std::move(sample);
		}
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

	// Camera refresh is owned by the dedicated 8ms camera worker. Running the
	// same PCM discovery ladder here duplicated an unbounded actor scan inside
	// the Update scan gate; one transient VMM stall held the gate for ~167s and
	// starved EntityList/RobotList, leaving ESP caches empty until the stall
	// ended. Keep Update focused on state publication and raid transitions.
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

	// Collision mirror: throttled background rebuild of world static collision.
	// Gate-checks inside (movement / 20s force / first build); the heavy DMA
	// runs on its own spawned thread, never here.
	if (IsEspRaidActive()) {
		CollisionMirror::ScheduleRebuild(GWorld, PersistentLevel,
			Vector3(g_Camera.Location.x, g_Camera.Location.y, g_Camera.Location.z));
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

	// Primary signal (user model): the loaded map IS the raid state.
	// Raid map loaded (TheDam_02_P etc.) => in raid. Hub world => out.
	if (LooksLikeStrictHubWorld(sGWorld, sPersistentLevel))
		return false;
	if (WorldNameLooksLikeRaidMap(sGWorld, sPersistentLevel))
		return true;

	// Stay path: once ESP raid is active, only lose raid on world loss or hub.
	// Do not require PC/pawn/root/combat — mid-raid DMA flakes were firing [raid] left.
	// Do NOT use "pawn gone + party<=1": solo raids always have partyCount==1, so a
	// brief null pawn read falsely dropped EspRaid and forced a slow re-arm.
	if (m_espRaidActive.load(std::memory_order_acquire)) {
		if (sAcknowledgedPawn && PawnLooksLikeHubCharacter(sAcknowledgedPawn))
			return false;
		return true;
	}

	// Fallback enter path — map name unreadable; use pawn/controller heuristics.
	if (!sActors || !sAcknowledgedPawn || !sPlayerController)
		return false;

	int32_t actorCount = 0;
	uintptr_t actorData = 0;
	if (!WorldScan::ReadLevelActors(sPersistentLevel, actorData, actorCount)
		|| actorCount <= 0 || actorCount > 10000)
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
	// The whole chain in one shot: every hop's pointer, the rung that answered it,
	// its identity proof and the rung counts. Replaces the four src tags.
	snap.chain = m_chain;
	snap.gWorldRaw = m_chain.worldRaw;
	snap.gWorldFailStep = m_chain.worldFailStep;
	snap.persistentLevel = PersistentLevel;
	snap.actors = Actors;
	snap.playerController = PlayerController;
	snap.acknowledgedPawn = AcknowledgedPawn;
	snap.rootComponent = RootComponent;
	snap.playerCameraManager = PlayerCameraManager;
	snap.owningGameInstance = OwningGameInstance;
	snap.localPlayer = localplayer;
	snap.reflectOk = m_reflectOk;
	snap.reflectDepth = m_reflectDepth;
	snap.reflectProps = m_reflectProps;
	snap.reflectSample = m_reflectSample;
	snap.fstrOk = m_fstrOk;
	snap.fstrKey = m_fstrKey;
	snap.fstrText = m_fstrText;
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
	{
		std::unique_lock<std::shared_mutex> cameraLock(m_cameraMutex);
		g_Camera = {};
	}
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

	const bool wasActive = m_espRaidActive.load(std::memory_order_acquire);

	// Transient read-stall hold: if the raid was active and the world resolved
	// fine within the grace window, this is a VMM hiccup (fStep=1/2 stall), not
	// a world change. Nuking camera + caches here blanked ALL ESP for seconds
	// per stall (user-visible flicker). Hold last-good state; the 12s bound
	// means a genuine unload still clears promptly. Hub transitions never reach
	// this path (CheckWorldTransition handles valid->valid changes).
	{
		const auto nowSteady = std::chrono::steady_clock::now();
		if (wasActive
			&& m_lastGoodWorldTime.time_since_epoch().count() != 0
			&& nowSteady - m_lastGoodWorldTime < kRaidRawFalseGraceMs) {
			// #region agent log
			AgentRaidLog("H2", "Update.cpp:HandleWorldLost", "world_lost_hold",
				std::string("{\"held\":1}"));
			// #endregion
			return;
		}
	}

	m_lastWorldPtr = 0;
	m_lastPersistentLevel = 0;
	m_espRaidActive.store(false, std::memory_order_release);
	m_espDrawReady.store(false, std::memory_order_release);
	m_raidEnterPending = false;
	m_partyEnterPending = false;
	m_raidFalseSince = {};
	m_worldGeneration.fetch_add(1, std::memory_order_release);
	ResetRaidTransitionState();
	ClearEspCaches();
	std::cout << "[raid] world_lost" << std::endl;
	// #region agent log
	{
		std::ostringstream d;
		d << "{\"wasActive\":" << (wasActive ? 1 : 0) << "}";
		AgentRaidLog("H2", "Update.cpp:HandleWorldLost", "world_lost", d.str());
	}
	// #endregion
}

void Engine::CheckWorldTransition(uintptr_t newWorld, uintptr_t newPersistentLevel)
{
	// Heartbeat for the stall-hold: update on EVERY tick the world resolves
	// non-zero, not only on transitions. Otherwise the timestamp goes stale
	// after 12 s in the same raid and HandleWorldLost's hold can never pass.
	if (newWorld)
		m_lastGoodWorldTime = std::chrono::steady_clock::now();

	if (newWorld == m_lastWorldPtr && newPersistentLevel == m_lastPersistentLevel)
		return;

	// Drop VMM page/TLB/VAD caches so freshly-allocated raid memory is visible
	// immediately (otherwise READCACHE/PROCCACHE TTLs can stall until restart).
	PCIMemory::FullRefresh();

	const bool wasActive = m_espRaidActive.load(std::memory_order_acquire);
	const std::string oldName = PeekObjFName(m_lastWorldPtr);
	const std::string newName = PeekObjFName(newWorld);
	const std::string newLevel = PeekObjFName(newPersistentLevel);
	const char* hubTok = MatchHubToken(newWorld, newPersistentLevel);

	ResetRaidTransitionState();
	ClearEspCaches();

	m_lastWorldPtr = newWorld;
	m_lastPersistentLevel = newPersistentLevel;
	if (newWorld)
		m_lastGoodWorldTime = std::chrono::steady_clock::now();
	m_worldGeneration.fetch_add(1, std::memory_order_release);
	m_espRaidActive.store(false, std::memory_order_release);
	m_espDrawReady.store(false, std::memory_order_release);
	m_raidEnterPending = false;
	m_partyEnterPending = false;
	m_raidFalseSince = {};

	// #region agent log
	{
		std::ostringstream d;
		d << "{\"wasActive\":" << (wasActive ? 1 : 0)
			<< ",\"oldName\":\"" << RaidJsonEscape(oldName) << "\""
			<< ",\"newName\":\"" << RaidJsonEscape(newName) << "\""
			<< ",\"newLevel\":\"" << RaidJsonEscape(newLevel) << "\""
			<< ",\"hubTok\":\"" << hubTok << "\""
			<< ",\"newWorld\":" << newWorld
			<< "}";
		AgentRaidLog("H2", "Update.cpp:CheckWorldTransition", "world_change", d.str());
	}
	// #endregion
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
		m_espFrameShared.reset();
		m_espPaintFrame.store(nullptr);
		m_espAimFrame.store(nullptr);
	}
	ResetEspFrameDiagnostics();
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
	int32_t plActorCount = -1;
	if (pl && Memory::IsValidPtrFast2(pl)) {
		uintptr_t ad = 0;
		int32_t ac = 0;
		if (WorldScan::ReadLevelActors(pl, ad, ac))
			plActorCount = ac;
	}
	const int actorCount = plActorCount;
	const char* rawReason = DiagnoseRaidRawReason(
		gw, pl, actors, pawn, pc, root, active);
	const bool combat = pawn && PawnHasRaidCombatComponents(pawn);

	// #region agent log
	{
		static std::chrono::steady_clock::time_point s_lastHb{};
		static bool s_prevRaw = false;
		static bool s_prevActive = false;
		static std::string s_prevReason;
		const bool reasonChanged = s_prevReason != rawReason;
		const bool edge = (raw != s_prevRaw) || (active != s_prevActive) || reasonChanged;
		if (edge || s_lastHb.time_since_epoch().count() == 0
			|| now - s_lastHb >= std::chrono::seconds(1)) {
			s_lastHb = now;
			s_prevRaw = raw;
			s_prevActive = active;
			s_prevReason = rawReason;
			std::ostringstream d;
			d << "{\"raw\":" << (raw ? 1 : 0)
				<< ",\"active\":" << (active ? 1 : 0)
				<< ",\"reason\":\"" << rawReason << "\""
				<< ",\"party\":" << partyCount
				<< ",\"actors\":" << actorCount
				<< ",\"hasPawn\":" << (pawn ? 1 : 0)
				<< ",\"hasPc\":" << (pc ? 1 : 0)
				<< ",\"hasActors\":" << (actors ? 1 : 0)
				<< ",\"combat\":" << (combat ? 1 : 0)
				<< ",\"partyPend\":" << (m_partyEnterPending ? 1 : 0)
				<< ",\"raidPend\":" << (m_raidEnterPending ? 1 : 0)
				<< ",\"hubTok\":\"" << MatchHubToken(gw, pl) << "\""
				<< ",\"wName\":\"" << RaidJsonEscape(PeekObjFName(gw)) << "\"";
			// G1/G2/G3: which resolver produced GWorld + live slot value/name.
			const PlayerChain::State chain = GetChainState();
			const uintptr_t slotRaw = chain.worldRaw;
			d << ",\"gw\":" << gw
				<< ",\"gwRaw\":" << slotRaw
				<< ",\"gwSrc\":\"" << PlayerChain::RungName(
					chain.RungOf(PlayerChain::Hop::World)) << "\""
				// The whole ladder, hop by hop: winner, tries and rejects.
				<< ",\"chain\":\"" << PlayerChain::Trace(chain) << "\""
				// Resolver failure step during no_world voids: 1=no slot read,
				// 2=slot read but level deref failed, 0=healthy.
				<< ",\"fStep\":" << chain.worldFailStep;
			if (slotRaw && slotRaw != gw)
				d << ",\"slotName\":\"" << RaidJsonEscape(PeekObjFName(slotRaw)) << "\"";
			d << "}";
			AgentRaidLog(edge ? "R" : "R", "Update.cpp:TickRaidGate",
				edge ? "raid_edge" : "raid_hb", d.str());
		}
	}
	// #endregion

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
		// #region agent log
		AgentRaidLog("R", "Update.cpp:TickRaidGate", "raid_left",
			std::string("{\"reason\":\"") + rawReason + "\"}");
		// #endregion
		return;
	}

	m_raidFalseSince = {};
	if (active) {
		// User spec: one backup rescan 5s after ESP arms, in case the first
		// scan ran against a half-loaded map. Flush VMM cache + clear caches
		// once; scanners repopulate immediately.
		if (s_backupScanPending
			&& m_raidArmedSince.time_since_epoch().count() != 0
			&& now - m_raidArmedSince >= std::chrono::seconds(5)) {
			s_backupScanPending = false;
			PCIMemory::FullRefresh();
			ClearEspCaches();
			std::cout << "[raid] backup_rescan" << std::endl;
			// #region agent log
			AgentRaidLog("H3", "Update.cpp:TickRaidGate", "backup_rescan", "{}");
			// #endregion
		}
		return;
	}

	if (partyCount >= kPartyMinPlayers) {
		if (!m_partyEnterPending) {
			m_partyEnterPending = true;
			m_partyDebSince = now;
			m_raidEnterPending = false;
			std::cout << "[raid] party_wait players=" << partyCount << std::endl;
			// #region agent log
			{
				std::ostringstream d;
				d << "{\"party\":" << partyCount << "}";
				AgentRaidLog("R", "Update.cpp:TickRaidGate", "party_wait", d.str());
			}
			// #endregion
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
		// #region agent log
		{
			std::ostringstream d;
			d << "{\"party\":" << partyCount << ",\"reason\":\"" << rawReason << "\"}";
			AgentRaidLog("R", "Update.cpp:TickRaidGate", "enter_wait", d.str());
		}
		// #endregion
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
	s_backupScanPending = true;
	m_espRaidActive.store(true, std::memory_order_release);
	m_worldGeneration.fetch_add(1, std::memory_order_release);
	std::cout << "[raid] entered players=" << partyCount << std::endl;
	// #region agent log
	{
		std::ostringstream d;
		d << "{\"party\":" << partyCount
			<< ",\"reason\":\"" << rawReason << "\""
			<< ",\"wName\":\"" << RaidJsonEscape(PeekObjFName(gw)) << "\""
			<< ",\"hubTok\":\"" << MatchHubToken(gw, pl) << "\""
			<< ",\"combat\":" << (combat ? 1 : 0)
			<< "}";
		AgentRaidLog("H3", "Update.cpp:TickRaidGate", "raid_entered", d.str());
	}
	// #endregion
}
