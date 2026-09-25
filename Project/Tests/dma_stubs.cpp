// Out-of-line PCIMemory symbols declared in DMA/Memory.h but defined in
// DMA/Memory.cpp (not part of the test build). Project.Tests exercises only
// decode math via the steam_decrypt::g_memReadOverride seam, so these
// production read paths are never reached — the stubs exist only so the
// linker resolves the header's declarations.

#pragma warning(push)
#pragma warning(disable : 4201) // vmmdll.h nameless struct/union (ThirdParty)
#include "DMA/Memory.h"
#pragma warning(pop)

// Out-of-line PCIMemory state. Core/Memory.h reads these through
// getBaseAddress()/GetModuleSize(), so the decode + reflection suites need them
// to resolve even though no DMA backend is linked in.
DWORD PCIMemory::processId = 0;
VMM_HANDLE PCIMemory::hVMM = nullptr;
uintptr_t PCIMemory::moduleBase = 0;
DWORD PCIMemory::moduleImageSize = 0;
bool PCIMemory::s_memMap = false;
std::string PCIMemory::attachedExe;

PCIMemory::~PCIMemory() {}

bool PCIMemory::read(uintptr_t address, void* buffer, size_t size) const
{
    (void)address;
    (void)buffer;
    (void)size;
    return false;
}