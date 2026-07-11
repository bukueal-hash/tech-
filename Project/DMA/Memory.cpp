#include "Memory.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <codecvt>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void ApplyVmmCacheTiming(VMM_HANDLE h)
{
    if (!h)
        return;
    VMMDLL_ConfigSet(h, VMMDLL_OPT_CONFIG_TICK_PERIOD, 100);
    VMMDLL_ConfigSet(h, VMMDLL_OPT_CONFIG_READCACHE_TICKS, 500);
    VMMDLL_ConfigSet(h, VMMDLL_OPT_CONFIG_TLBCACHE_TICKS, 2000);
    VMMDLL_ConfigSet(h, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL, 500);
    VMMDLL_ConfigSet(h, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL, 6000);
}

// Help CL-1315578 — validate only; Offsets.h unchanged.
static constexpr uint64_t kHelpUWorldRva = 0xE91A288ULL;
static constexpr uint64_t kHelpPersistentLevel = 0x158ULL;
static constexpr uint64_t kHelpLevelOwningWorld = 0x130ULL;
static constexpr uint64_t kHelpActors = 0x108ULL;
static constexpr uint64_t kHelpActorCount = 0x110ULL;
static constexpr uint64_t kHelpLevelCollections = 0x370ULL;
static constexpr uint64_t kHelpLevelCollectionStride = 0x78ULL;
static constexpr uint64_t kHelpCollectionPersistentLevel = 0x20ULL;

static uint64_t g_dtbFileSize = 0x80000;

static VOID cbDtbAddFile(_Inout_ HANDLE /*h*/, _In_ LPCSTR uszName, _In_ ULONG64 cb,
    _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO /*pExInfo*/)
{
    if (uszName && strcmp(uszName, "dtb.txt") == 0)
        g_dtbFileSize = cb ? cb : 0x80000;
}

static bool LooksLikeUtf16(uintptr_t p)
{
    if (!p)
        return true;
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        const unsigned char lo = static_cast<unsigned char>((p >> (i * 16)) & 0xFF);
        const unsigned char hi = static_cast<unsigned char>((p >> (i * 16 + 8)) & 0xFF);
        if (hi == 0 && lo >= 0x20 && lo < 0x7F)
            ++n;
    }
    return n >= 3;
}

static bool ReadU64(VMM_HANDLE h, DWORD pid, uint64_t va, uint64_t& out)
{
    out = 0;
    DWORD cb = 0;
    return VMMDLL_MemReadEx(h, pid, va, reinterpret_cast<PBYTE>(&out), sizeof(out), &cb, VMMDLL_FLAG_NOCACHE)
        && cb == sizeof(out);
}

static bool ReadI32(VMM_HANDLE h, DWORD pid, uint64_t va, int32_t& out)
{
    out = 0;
    DWORD cb = 0;
    return VMMDLL_MemReadEx(h, pid, va, reinterpret_cast<PBYTE>(&out), sizeof(out), &cb, VMMDLL_FLAG_NOCACHE)
        && cb == sizeof(out);
}

static bool HasMz(VMM_HANDLE h, DWORD pid, uintptr_t base)
{
    uint16_t mz = 0;
    DWORD cb = 0;
    return base
        && VMMDLL_MemReadEx(h, pid, base, reinterpret_cast<PBYTE>(&mz), sizeof(mz), &cb, VMMDLL_FLAG_NOCACHE)
        && cb == sizeof(mz) && mz == 0x5A4D;
}

static bool LevelOwnedByWorld(VMM_HANDLE h, DWORD pid, uint64_t level, uint64_t world)
{
    if (!level || LooksLikeUtf16(static_cast<uintptr_t>(level)) || level < 0x10000)
        return false;
    uint64_t owning = 0;
    if (ReadU64(h, pid, level + kHelpLevelOwningWorld, owning) && owning == world)
        return true;
    uint64_t actors = 0;
    int32_t count = 0;
    if (!ReadU64(h, pid, level + kHelpActors, actors) || LooksLikeUtf16(static_cast<uintptr_t>(actors)))
        return false;
    if (!ReadI32(h, pid, level + kHelpActorCount, count))
        return false;
    return actors != 0 && count > 0 && count <= 10000;
}

/** Help: UWorld → PL 0x158 / LevelCollections+0x20. */
static bool ProbeHelpWorld(VMM_HANDLE h, DWORD pid, uintptr_t base)
{
    if (!HasMz(h, pid, base))
        return false;
    uint64_t world = 0;
    if (!ReadU64(h, pid, base + kHelpUWorldRva, world)
        || LooksLikeUtf16(static_cast<uintptr_t>(world)) || world < 0x10000)
        return false;

    uint64_t level = 0;
    if (ReadU64(h, pid, world + kHelpPersistentLevel, level)
        && LevelOwnedByWorld(h, pid, level, world))
        return true;

    uint64_t colData = 0;
    int32_t colNum = 0;
    if (ReadU64(h, pid, world + kHelpLevelCollections, colData)
        && ReadI32(h, pid, world + kHelpLevelCollections + 8, colNum)
        && colData && !LooksLikeUtf16(static_cast<uintptr_t>(colData))
        && colNum > 0 && colNum <= 16) {
        const int limit = colNum > 4 ? 4 : colNum;
        for (int i = 0; i < limit; ++i) {
            const uint64_t col = colData + static_cast<uint64_t>(i) * kHelpLevelCollectionStride;
            if (ReadU64(h, pid, col + kHelpCollectionPersistentLevel, level)
                && LevelOwnedByWorld(h, pid, level, world))
                return true;
        }
    }
    return false;
}

static void PushUniqueU64(std::vector<uint64_t>& v, uint64_t x)
{
    if (!x)
        return;
    for (uint64_t e : v) {
        if (e == x)
            return;
    }
    v.push_back(x);
}

static void PushUniqueBase(std::vector<std::pair<uintptr_t, DWORD>>& v, uintptr_t base, DWORD size)
{
    if (!base)
        return;
    for (const auto& e : v) {
        if (e.first == base)
            return;
    }
    v.emplace_back(base, size);
}

/**
 * Optional DTB correction after attach. Never fails attach — only upgrades moduleBase
 * when help UWorld→PersistentLevel validates.
 */
static void TryImproveModuleBase(VMM_HANDLE h, DWORD pid, const char* gameExe,
    uintptr_t& moduleBase, DWORD& moduleImageSize)
{
    if (!h || !pid || !gameExe || !moduleBase)
        return;
    if (ProbeHelpWorld(h, pid, moduleBase))
        return; // already good (even if preferred base)

    if (!VMMDLL_InitializePlugins(h))
        return;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (int i = 0; i < 80; ++i) {
        BYTE bytes[8] = {};
        DWORD n = 0;
        const NTSTATUS st = VMMDLL_VfsReadU(h, "\\misc\\procinfo\\progress_percent.txt", bytes, 3, &n, 0);
        if ((st == VMMDLL_STATUS_SUCCESS || st == VMMDLL_STATUS_END_OF_FILE)
            && atoi(reinterpret_cast<char*>(bytes)) == 100)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    g_dtbFileSize = 0x80000;
    VMMDLL_VFS_FILELIST2 vfs{};
    vfs.dwVersion = VMMDLL_VFS_FILELIST_VERSION;
    vfs.pfnAddFile = cbDtbAddFile;
    if (!VMMDLL_VfsListU(h, "\\misc\\procinfo\\", &vfs))
        return;

    std::unique_ptr<BYTE[]> buf(new BYTE[static_cast<size_t>(g_dtbFileSize) + 1]());
    DWORD read = 0;
    const NTSTATUS st = VMMDLL_VfsReadU(h, "\\misc\\procinfo\\dtb.txt", buf.get(),
        static_cast<DWORD>(g_dtbFileSize), &read, 0);
    if (st != VMMDLL_STATUS_SUCCESS && st != VMMDLL_STATUS_END_OF_FILE)
        return;

    std::vector<uint64_t> dtbs;
    VMMDLL_PROCESS_INFORMATION info{};
    info.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    SIZE_T infoCb = sizeof(info);
    if (VMMDLL_ProcessGetInformation(h, pid, &info, &infoCb)) {
        PushUniqueU64(dtbs, info.paDTB);
        PushUniqueU64(dtbs, info.paDTB_UserOpt);
    }

    std::istringstream iss(reinterpret_cast<char*>(buf.get()));
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream ls(line);
        uint32_t index = 0, linePid = 0;
        uint64_t dtb = 0, ka = 0;
        std::string name;
        if (!(ls >> std::hex >> index >> std::dec >> linePid >> std::hex >> dtb >> ka >> name))
            continue;
        if (linePid == 0 || linePid == pid)
            PushUniqueU64(dtbs, dtb);
        if (!name.empty() && (strstr(name.c_str(), "Pioneer") || _stricmp(name.c_str(), gameExe) == 0))
            PushUniqueU64(dtbs, dtb);
    }

    const uint64_t originalDtb = info.paDTB;
    for (uint64_t dtb : dtbs) {
        VMMDLL_ConfigSet(h, VMMDLL_OPT_PROCESS_DTB | pid, dtb);
        VMMDLL_ConfigSet(h, VMMDLL_OPT_REFRESH_ALL, 1);

        std::vector<std::pair<uintptr_t, DWORD>> bases;
        PushUniqueBase(bases, moduleBase, moduleImageSize);

        PVMMDLL_MAP_MODULE pMod = nullptr;
        if (VMMDLL_Map_GetModuleU(h, pid, &pMod, 0) && pMod) {
            for (DWORD i = 0; i < pMod->cMap; ++i) {
                const auto& m = pMod->pMap[i];
                if (m.uszText && (strstr(m.uszText, "Pioneer") || _stricmp(m.uszText, gameExe) == 0))
                    PushUniqueBase(bases, static_cast<uintptr_t>(m.vaBase), m.cbImageSize);
            }
            VMMDLL_MemFree(pMod);
        }

        PVMMDLL_MAP_VAD pVad = nullptr;
        if (VMMDLL_Map_GetVadU(h, pid, TRUE, &pVad) && pVad) {
            for (DWORD i = 0; i < pVad->cMap; ++i) {
                const auto& v = pVad->pMap[i];
                if (!v.fImage || v.vaEnd <= v.vaStart)
                    continue;
                const uint64_t sz = v.vaEnd - v.vaStart + 1;
                if (sz < 8ull * 1024 * 1024)
                    continue;
                const bool named = v.uszText && strstr(v.uszText, "Pioneer");
                if (!named && sz < 40ull * 1024 * 1024)
                    continue;
                const uintptr_t start = static_cast<uintptr_t>(v.vaStart);
                if (!HasMz(h, pid, start))
                    continue;
                PushUniqueBase(bases, start, static_cast<DWORD>(sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : sz));
            }
            VMMDLL_MemFree(pVad);
        }

        for (const auto& b : bases) {
            if (!ProbeHelpWorld(h, pid, b.first))
                continue;
            moduleBase = b.first;
            moduleImageSize = b.second;
            std::cout << "[+] Module base corrected: 0x" << std::hex << moduleBase << std::dec << std::endl;
            return;
        }
    }

    if (originalDtb)
        VMMDLL_ConfigSet(h, VMMDLL_OPT_PROCESS_DTB | pid, originalDtb);
}

DWORD PCIMemory::processId = 0;
VMM_HANDLE PCIMemory::hVMM = nullptr;
uintptr_t PCIMemory::moduleBase = 0;
DWORD PCIMemory::moduleImageSize = 0;
bool PCIMemory::s_memMap = false;
std::string PCIMemory::attachedExe{};

PCIMemory::~PCIMemory() {
    if (hVMM) {
        VMMDLL_Close(hVMM);
        hVMM = nullptr;
    }
}

bool PCIMemory::Initialize(const char* gameExe) {
    if (!gameExe || !*gameExe)
        return false;

    auto attachProcess = [&]() -> bool {
        if (!hVMM)
            return false;
        if (!VMMDLL_PidGetFromName(hVMM, gameExe, &processId) || !processId)
            return false;

        PVMMDLL_MAP_MODULEENTRY pEntry = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(hVMM, processId, gameExe, &pEntry, 0) && pEntry) {
            moduleBase = static_cast<uintptr_t>(pEntry->vaBase);
            moduleImageSize = pEntry->cbImageSize;
            VMMDLL_MemFree(pEntry);
        } else {
            moduleBase = 0;
            moduleImageSize = 0;
        }

        vmProcess.dwProcID = processId;
        vmProcess.dwModBase = moduleBase;
        vmProcess.dwImageSize = moduleImageSize;
        attachedExe = gameExe;

        // Never blocks attach — only upgrades base when help GWorld chain validates.
        TryImproveModuleBase(hVMM, processId, gameExe, moduleBase, moduleImageSize);
        vmProcess.dwModBase = moduleBase;
        vmProcess.dwImageSize = moduleImageSize;

        std::cout << "[+] DMA attached: " << gameExe << " PID: " << processId
            << " Base: 0x" << std::hex << moduleBase << std::dec << std::endl;
        return moduleBase != 0;
    };

    if (hVMM && processId && moduleBase) {
        DWORD pid = 0;
        if (VMMDLL_PidGetFromName(hVMM, gameExe, &pid) && pid == processId) {
            TryImproveModuleBase(hVMM, processId, gameExe, moduleBase, moduleImageSize);
            vmProcess.dwModBase = moduleBase;
            vmProcess.dwImageSize = moduleImageSize;
            return true;
        }
    }

    if (hVMM && attachProcess())
        return true;

    if (hVMM) {
        VMMDLL_Close(hVMM);
        hVMM = nullptr;
    }
    processId = 0;
    moduleBase = 0;
    moduleImageSize = 0;

    static const char* deviceTypes[] = {
        "fpga://algo=0",
        "fpga://algo=1",
        "usb3380",
        "pcie_fpga",
        "ftd3xx",
        "ftdi",
        "enigma-x1",
        nullptr
    };

    int currentDevice = 0;
    while (deviceTypes[currentDevice]) {
        std::vector<std::string> parts;
        std::vector<LPCSTR> argv;
        DWORD argc = 0;

        parts.emplace_back("");
        parts.emplace_back("-printf");
        parts.emplace_back("-disable-python");
        parts.emplace_back("-memmap");
        parts.emplace_back("auto");

        char remote[520] = {};
        const DWORD n = GetEnvironmentVariableA("RAIDER_LEECH_REMOTE", remote, static_cast<DWORD>(sizeof(remote)));
        if (n > 0 && n < sizeof(remote) && remote[0]) {
            parts.emplace_back("-device");
            parts.emplace_back("existingremote");
            parts.emplace_back("-remote");
            parts.emplace_back(remote);
            std::cout << "[*] LeechCore: existingremote -> " << remote << std::endl;
        } else {
            parts.emplace_back("-device");
            parts.emplace_back(deviceTypes[currentDevice]);
            std::cout << "[*] LeechCore: -device " << deviceTypes[currentDevice] << std::endl;
        }

        argv.reserve(parts.size());
        for (const auto& s : parts)
            argv.push_back(s.c_str());
        argc = static_cast<DWORD>(argv.size());

        PLC_CONFIG_ERRORINFO pLcErrorInfo = nullptr;
        hVMM = VMMDLL_InitializeEx(argc, argv.data(), &pLcErrorInfo);

        if (hVMM) {
            ApplyVmmCacheTiming(hVMM);
            if (attachProcess()) {
                std::cout << "[+] DMA OK. Device: " << deviceTypes[currentDevice] << std::endl;
                return true;
            }
            std::cout << "[!] Process not found: " << gameExe << " (VMM up, retry attach)" << std::endl;
            VMMDLL_Close(hVMM);
            hVMM = nullptr;
        } else {
            if (pLcErrorInfo) {
                if (pLcErrorInfo->wszUserText[0]) {
                    const WCHAR* w = pLcErrorInfo->wszUserText;
                    size_t n = wcsnlen(w, 16384);
                    std::wstring msg(w, n);
                    std::string utf8 = WideToUtf8(msg);
                    std::cerr << "[!] LeechCore (" << deviceTypes[currentDevice] << "): " << utf8 << std::endl;
                }
                LcMemFree(pLcErrorInfo);
            }
        }

        currentDevice++;
        Sleep(100);
    }

    std::cout << "[!] All device types failed. Retrying...\n";
    return false;
}

bool PCIMemory::InitializeVmmOnly() {
    if (hVMM) return true;

    static const char* deviceTypes[] = {
        "fpga://algo=0",
        "fpga://algo=1",
        "usb3380",
        "pcie_fpga",
        "ftd3xx",
        "ftdi",
        "enigma-x1",
        nullptr
    };

    int currentDevice = 0;
    while (deviceTypes[currentDevice]) {
        std::vector<std::string> parts;
        std::vector<LPCSTR> argv;

        parts.emplace_back("");
        parts.emplace_back("-printf");
        parts.emplace_back("-disable-python");
        parts.emplace_back("-memmap");
        parts.emplace_back("auto");

        char remote[520] = {};
        const DWORD n = GetEnvironmentVariableA("RAIDER_LEECH_REMOTE", remote, static_cast<DWORD>(sizeof(remote)));
        if (n > 0 && n < sizeof(remote) && remote[0]) {
            parts.emplace_back("-device");
            parts.emplace_back("existingremote");
            parts.emplace_back("-remote");
            parts.emplace_back(remote);
        } else {
            parts.emplace_back("-device");
            parts.emplace_back(deviceTypes[currentDevice]);
        }

        argv.reserve(parts.size());
        for (const auto& s : parts)
            argv.push_back(s.c_str());

        PLC_CONFIG_ERRORINFO pLcErrorInfo = nullptr;
        hVMM = VMMDLL_InitializeEx(static_cast<DWORD>(argv.size()), argv.data(), &pLcErrorInfo);

        if (hVMM) {
            ApplyVmmCacheTiming(hVMM);
            std::cout << "[VMM] DMA initialized via " << deviceTypes[currentDevice] << " (no game process)" << std::endl;
            return true;
        }

        if (pLcErrorInfo) {
            LcMemFree(pLcErrorInfo);
        }

        currentDevice++;
        Sleep(100);
    }

    std::cout << "[VMM] All device types failed" << std::endl;
    return false;
}

bool PCIMemory::reconnect(const char* gameExe) {
    if (hVMM) {
        VMMDLL_Close(hVMM);
        hVMM = nullptr;
    }
    processId = 0;
    moduleBase = 0;
    attachedExe.clear();
    return Initialize(gameExe);
}

bool PCIMemory::read(uintptr_t address, void* buffer, size_t size) const {
    return ReadVirtualMemoryEx(address, buffer, size);
}

bool PCIMemory::write(uintptr_t address, const void* buffer, size_t size) {
    if (hVMM && processId) {
        return VMMDLL_MemWrite(hVMM, processId, address, reinterpret_cast<PBYTE>(const_cast<void*>(buffer)), static_cast<DWORD>(size)) == TRUE;
    }
    return false;
}

DWORD PCIMemory::GetPidFromName(const char* processName) const {
    if (!hVMM || !processName)
        return 0;
    DWORD pid = 0;
    if (VMMDLL_PidGetFromName(hVMM, processName, &pid))
        return pid;
    return 0;
}

std::vector<DWORD> PCIMemory::GetPidListFromName(const char* processName) const {
    std::vector<DWORD> list;
    if (!hVMM || !processName)
        return list;

    PVMMDLL_PROCESS_INFORMATION processInfo = nullptr;
    DWORD totalProcesses = 0;
    if (!VMMDLL_ProcessGetInformationAll(hVMM, &processInfo, &totalProcesses)) {
        return list;
    }

    for (DWORD i = 0; i < totalProcesses; ++i) {
        if (strstr(processInfo[i].szNameLong, processName))
            list.push_back(processInfo[i].dwPID);
    }

    VMMDLL_MemFree(processInfo);
    return list;
}

uint64_t DmaMem::FindSignature(const char* signature, uint64_t rangeStart, uint64_t rangeEnd, int pid) {
    if (!signature || !*signature || rangeStart >= rangeEnd)
        return 0;
    std::vector<uint8_t> buffer(static_cast<size_t>(rangeEnd - rangeStart));
    DWORD cbRead = 0;
    auto h = GetVmm();
    if (!h)
        return 0;
    if (!VMMDLL_MemReadEx(h, static_cast<DWORD>(pid), rangeStart, buffer.data(), static_cast<DWORD>(buffer.size()), &cbRead, 0))
        return 0;

    const char* pat = signature;
    uint64_t firstMatch = 0;
    for (uint64_t i = rangeStart; i < rangeEnd; i++) {
        const bool match = (*pat == '?') ? true : (buffer[static_cast<size_t>(i - rangeStart)] == static_cast<uint8_t>(strtol(pat, nullptr, 16)));
        if (match) {
            if (!firstMatch)
                firstMatch = i;
            if (!pat[2])
                break;
            pat += (*pat == '?') ? 2 : 3;
        } else {
            pat = signature;
            firstMatch = 0;
        }
    }
    return firstMatch;
}

bool PCIMemory::FullRefresh()
{
    if (!hVMM)
        return false;
    return VMMDLL_ConfigSet(hVMM, VMMDLL_OPT_REFRESH_ALL, 1) != FALSE;
}

bool PCIMemory::PartialMemRefresh()
{
    if (!hVMM)
        return false;
    return VMMDLL_ConfigSet(hVMM, VMMDLL_OPT_REFRESH_FREQ_MEM_PARTIAL, 1) != FALSE;
}

bool PCIMemory::PartialTlbRefresh()
{
    if (!hVMM)
        return false;
    return VMMDLL_ConfigSet(hVMM, VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL, 1) != FALSE;
}

bool PCIMemory::SmartTlbRefreshIfDue()
{
    return false;
}
