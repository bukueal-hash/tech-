#include "ModuleImageCache.h"

#include "DumperState.h"

#include "../DMA/Memory.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace Dumper {

namespace {

constexpr uint32_t kMaxImageSize = 256u * 1024u * 1024u;
constexpr size_t kPageSize = 4096u;
constexpr size_t kMinBytesForSigScan = 512u * 1024u;
constexpr size_t kMinReadableBytes = 64u * 1024u * 1024u;

struct PeLayout {
    uint32_t sizeOfImage = 0;
    uint32_t textRva = 0;
    uint32_t textSize = 0;
    uint32_t rdataRva = 0;
    uint32_t rdataSize = 0;
    bool peValid = false;
};

bool ReadPeLayout(uint64_t moduleBase, PeLayout& out)
{
    out = {};
    uint8_t dosPage[kPageSize]{};
    if (!PCIMemory::ReadVirtualMemoryNoCache(
            static_cast<uintptr_t>(moduleBase), dosPage, sizeof(dosPage))
        && !PCIMemory::ReadVirtualMemory(
            static_cast<uintptr_t>(moduleBase), dosPage, sizeof(dosPage)))
        return false;
    if (dosPage[0] != 'M' || dosPage[1] != 'Z')
        return false;

    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, dosPage + 0x3C, sizeof(e_lfanew));
    if (e_lfanew < 0x40 || e_lfanew > 0x1000)
        return false;

    uint8_t peBuf[4096]{};
    if (!PCIMemory::ReadVirtualMemoryNoCache(
            static_cast<uintptr_t>(moduleBase + e_lfanew), peBuf, sizeof(peBuf))
        && !PCIMemory::ReadVirtualMemory(
            static_cast<uintptr_t>(moduleBase + e_lfanew), peBuf, sizeof(peBuf)))
        return false;
    if (std::memcmp(peBuf, "PE\0\0", 4) != 0)
        return false;

    const uint16_t optMagic = *reinterpret_cast<uint16_t*>(peBuf + 0x18);
    if (optMagic != 0x20Bu)
        return false;

    std::memcpy(&out.sizeOfImage, peBuf + 0x50, sizeof(out.sizeOfImage));
    const uint16_t numSections = *reinterpret_cast<uint16_t*>(peBuf + 0x06);
    const uint16_t optHeaderSize = *reinterpret_cast<uint16_t*>(peBuf + 0x14);
    const size_t sectionOff = static_cast<size_t>(0x18 + optHeaderSize);
    if (sectionOff + static_cast<size_t>(numSections) * 40 > sizeof(peBuf))
        return false;

    for (uint16_t i = 0; i < numSections && i < 96; ++i) {
        const uint8_t* sec = peBuf + sectionOff + static_cast<size_t>(i) * 40;
        char name[9]{};
        std::memcpy(name, sec, 8);
        uint32_t virtualSize = 0;
        uint32_t virtualAddress = 0;
        std::memcpy(&virtualSize, sec + 0x08, sizeof(virtualSize));
        std::memcpy(&virtualAddress, sec + 0x0C, sizeof(virtualAddress));
        if (std::strncmp(name, ".text", 5) == 0) {
            out.textRva = virtualAddress;
            out.textSize = virtualSize;
        } else if (std::strncmp(name, ".rdata", 6) == 0) {
            out.rdataRva = virtualAddress;
            out.rdataSize = virtualSize;
        }
    }

    if (out.sizeOfImage < 0x1000 || out.sizeOfImage > kMaxImageSize)
        return false;

    out.peValid = true;
    return true;
}

struct CacheRange {
    uint64_t startRva = 0;
    uint32_t size = 0;
};

CacheRange ChooseCacheRange(const PeLayout& pe, uint32_t reportedSize)
{
    CacheRange range;
    if (pe.peValid && pe.textSize) {
        range.startRva = pe.textRva;
        range.size = pe.textSize;
        if (pe.rdataSize) {
            const uint32_t textEnd = pe.textRva + pe.textSize;
            if (pe.rdataRva >= textEnd && pe.rdataRva - textEnd < 0x1000000u) {
                const uint32_t gap = pe.rdataRva - textEnd;
                range.size = pe.textSize + gap + pe.rdataSize;
            } else if (pe.rdataRva < pe.textRva) {
                range.startRva = pe.rdataRva;
                range.size = (pe.textRva + pe.textSize) - pe.rdataRva;
            }
        }
    } else if (reportedSize > 0x2000) {
        range.startRva = 0x1000;
        range.size = (std::min)(static_cast<uint32_t>(64u * 1024u * 1024u), reportedSize - 0x1000);
    }

    if (!range.size)
        range.size = reportedSize;
    if (range.size > kMaxImageSize)
        range.size = kMaxImageSize;
    return range;
}

} // namespace

bool ModuleImageCache::Load(uint64_t moduleBase, uint32_t imageSize, DumperState& state)
{
    data_.clear();
    baseVa_ = moduleBase;

    if (!moduleBase) {
        state.AppendLog("Module cache: invalid module base");
        return false;
    }

    g_mem.RefreshModuleInfo();
    if (g_mem.TryImproveModuleBaseForDumper())
        state.AppendLog("Module cache: base corrected via DTB probe");
    moduleBase = g_mem.GetBase();
    baseVa_ = moduleBase;
    PCIMemory::FullRefresh();

    const uint32_t reported = imageSize ? imageSize : g_mem.GetImageSize();

    PeLayout pe{};
    const bool peOk = ReadPeLayout(moduleBase, pe);
    if (peOk) {
        std::ostringstream oss;
        oss << "Module cache: PE ok SizeOfImage=0x" << std::hex << pe.sizeOfImage
            << " .text=0x" << pe.textRva << "+0x" << pe.textSize;
        if (pe.rdataSize)
            oss << " .rdata=0x" << pe.rdataRva << "+0x" << pe.rdataSize;
        state.AppendLog(oss.str());
    } else {
        state.AppendLog("Module cache: PE header unreadable after DTB probe");
    }

    const CacheRange range = ChooseCacheRange(pe, reported);
    if (!range.size) {
        state.AppendLog("Module cache: could not determine cache size");
        return false;
    }

    {
        std::ostringstream oss;
        oss << "Module cache: caching RVA 0x" << std::hex << range.startRva
            << " size 0x" << range.size << std::dec;
        state.AppendLog(oss.str());
    }

    data_.assign(range.size, 0);

    auto* h = PCIMemory::GetVmmHandle();
    const DWORD pid = g_mem.GetPID();
    if (!h || !pid) {
        state.AppendLog("Module cache: VMM not ready");
        return false;
    }

    size_t bytesRead = 0;
    size_t pageFaults = 0;

    for (size_t offset = 0; offset < range.size; offset += kPageSize) {
        if (state.IsCancelRequested())
            return false;

        const size_t pageLen = (std::min)(kPageSize, static_cast<size_t>(range.size) - offset);
        DWORD cbRead = 0;
        const BOOL ok = VMMDLL_MemReadEx(
            h,
            pid,
            moduleBase + range.startRva + offset,
            data_.data() + offset,
            static_cast<DWORD>(pageLen),
            &cbRead,
            VMMDLL_FLAG_NOCACHE);

        if (ok && cbRead > 0) {
            bytesRead += cbRead;
            if (cbRead < pageLen)
                std::memset(data_.data() + offset + cbRead, 0, pageLen - cbRead);
        } else {
            ++pageFaults;
            std::memset(data_.data() + offset, 0, pageLen);
        }

        if ((offset / kPageSize) % 256 == 0 || offset + kPageSize >= range.size) {
            const int pct = static_cast<int>((offset + pageLen) * 20 / range.size);
            state.SetProgress(pct);
        }
    }

    const double pctReadable = range.size
        ? (100.0 * static_cast<double>(bytesRead) / static_cast<double>(range.size)) : 0.0;

    std::ostringstream summary;
    summary << "Module cache: read " << bytesRead << " / " << range.size
            << " bytes (" << pageFaults << " bad pages, "
            << static_cast<int>(pctReadable) << "% readable)";
    state.AppendLog(summary.str());

    if (bytesRead < kMinBytesForSigScan) {
        state.AppendLog("Module cache: too few bytes read for sig scan");
        return false;
    }
    if (peOk && bytesRead < kMinReadableBytes && pctReadable < 80.0 && range.startRva != 0) {
        state.AppendLog("Module cache: readable ratio below 80% threshold");
        return false;
    }

    baseVa_ = moduleBase + range.startRva;
    state.AppendLog("Module image cached");
    return true;
}

} // namespace Dumper
