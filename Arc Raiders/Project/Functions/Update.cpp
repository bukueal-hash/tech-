#include "../Core/Engine.h"

void Engine::Update() {
	auto GworldPtr = Memory::read<uintptr_t>(Memory::getBaseAddress() + Offsets::UWorld);
	if (!GworldPtr) {
		ClearFNameCache();
		return;
	}

	auto tGWorld = Memory::read<uintptr_t>(GworldPtr);
	if (!tGWorld) {
		ClearFNameCache();
		return;
	}

	// Check for world transition (clears caches under their own locks)
	CheckWorldChange(tGWorld);

	// --- Read entire pointer chain into locals (NO lock held, slow I/O here) ---
	uintptr_t tGameInstance = 0, tPersistentLevel = 0, tLocalPlayer = 0;
	uintptr_t tPlayerController = 0, tAcknowledgedPawn = 0;
	uintptr_t tRootComponent = 0, tActors = 0, tPlayerState = 0;

	tGameInstance = GetGameInstance(tGWorld);

	if (tGameInstance) {
		tPersistentLevel = Memory::read<uintptr_t>(tGWorld + Offsets::PersistentLevel);

		uintptr_t local_players = Memory::read<uintptr_t>(tGameInstance + Offsets::LocalPlayers);
		if (local_players) {
			tLocalPlayer = Memory::read<uintptr_t>(local_players);
			if (tLocalPlayer) {
				tPlayerController = Memory::read<uintptr_t>(tLocalPlayer + Offsets::APlayerController);
			}
		}
	}

	if (tPlayerController) {
		tAcknowledgedPawn = Memory::read<uintptr_t>(tPlayerController + Offsets::AcknowledgedPawn);
		tPlayerState = Memory::read<uintptr_t>(tPlayerController + Offsets::APlayerState);
	}

	if (tAcknowledgedPawn) {
		tRootComponent = Memory::read<uintptr_t>(tAcknowledgedPawn + Offsets::RootComponent);
	}

	if (tPersistentLevel) {
		tActors = Memory::read<uintptr_t>(tPersistentLevel + Offsets::AActors);
	}

	// --- Publish all state atomically (brief lock ~microseconds) ---
	{
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		GWorld = tGWorld;
		GameInstance = tGameInstance;
		PersistentLevel = tPersistentLevel;
		localplayer = tLocalPlayer;
		PlayerController = tPlayerController;
		AcknowledgedPawn = tAcknowledgedPawn;
		RootComponent = tRootComponent;
		PlayerState = tPlayerState;
		Actors = tActors;
	}

	// GetCameraManagerFromActors reads member vars — call after they're published
	uintptr_t tPCM = tPlayerController ? GetCameraManagerFromActors() : 0;
	{
		std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
		PlayerCameraManager = tPCM;
	}

	if (debugLog) {
		std::cout << "GWorld()-> " << std::hex << tGWorld << std::dec << std::endl;
		std::cout << "PersistentLevel()-> " << std::hex << tPersistentLevel << std::dec << std::endl;
		std::cout << "GameInstance()-> " << std::hex << tGameInstance << std::dec << std::endl;
		std::cout << "PlayerCameraManager()-> " << std::hex << tPCM << std::dec << std::endl;
		std::cout << "LocalPlayer()-> " << std::hex << tLocalPlayer << std::dec << std::endl;
		std::cout << "PlayerController()-> " << std::hex << tPlayerController << std::dec << std::endl;
		std::cout << "AcknowledgedPawn()-> " << std::hex << tAcknowledgedPawn << std::dec << std::endl;
		std::cout << "APlayerState()-> " << std::hex << tPlayerState << std::dec << std::endl;
		std::cout << "RootComponent()-> " << std::hex << tRootComponent << std::dec << std::endl;
		std::cout << "AActors: " << std::hex << tActors << std::dec << std::endl;
		if (tRootComponent) {
			auto myPosition = Memory::read<Vector3>(tRootComponent + Offsets::RelativeLocation);
			printf("myPosition: [%.2f] [%.2f] [%.2f]\n", myPosition.x, myPosition.y, myPosition.z);
		}
	}
}