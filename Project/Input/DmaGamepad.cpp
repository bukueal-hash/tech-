#include "../DMA/Memory.h"
#include "DmaGamepad.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

static DWORD s_winlogonPid = 0;
static int s_kernelPid = 0;
static uint64_t s_controllerVa = 0;
static bool s_initialized = false;
static char s_lastStatus[320] = "Not initialized";

static constexpr uint64_t kKernelPtrMin = 0xFFFF000000000000ull;

static bool KernelPtrOk(uint64_t p) {
	return p >= kKernelPtrMin;
}

static uint64_t ResolveRipRelMov(DmaMem& mem, uint64_t instr, int pid) {
	int32_t disp = mem.Read<int32_t>(instr + 3, pid);
	return instr + 7 + static_cast<int64_t>(disp);
}

static void SetStatus(const char* msg) {
	strncpy_s(s_lastStatus, msg, _TRUNCATE);
}

namespace DmaGamepad {

const char* GetLastStatusMessage() {
	return s_lastStatus;
}

bool IsReady() {
	return s_initialized && s_controllerVa != 0 && KernelPtrOk(s_controllerVa);
}

bool TryGetControllerReadTarget(uint64_t* outStateVa, int* outKernelPid, uint32_t* outHostPid) {
	if (!outStateVa || !outKernelPid || !outHostPid || !IsReady())
		return false;
	*outStateVa = s_controllerVa;
	*outKernelPid = s_kernelPid;
	*outHostPid = static_cast<uint32_t>(s_winlogonPid);
	return true;
}

bool TryInit(DmaMem& mem) {
	s_initialized = false;
	s_controllerVa = 0;
	strcpy_s(s_lastStatus, "Initializing...");

	const char* sysProcs[] = { "winlogon.exe", "lsass.exe", "csrss.exe", "explorer.exe" };
	s_winlogonPid = 0;
	for (const char* proc : sysProcs) {
		s_winlogonPid = mem.GetPidFromName(proc);
		if (s_winlogonPid != 0)
			break;
	}
	if (s_winlogonPid == 0) {
		SetStatus("DmaGamepad: no sysproc found");
		return false;
	}

	s_kernelPid = static_cast<int>(static_cast<DWORD>(s_winlogonPid) | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY);

	PVMMDLL_MAP_MODULEENTRY module_info = nullptr;
	if (!VMMDLL_Map_GetModuleFromNameW(mem.GetVmm(), static_cast<DWORD>(s_kernelPid), L"xusb22.sys", &module_info,
			VMMDLL_MODULE_FLAG_NORMAL) ||
		!module_info) {
		SetStatus("DmaGamepad: no xusb22.sys");
		return false;
	}

	const uint64_t imgBase = module_info->vaBase;
	const uint64_t imgEnd = imgBase + module_info->cbImageSize;

	static const char* kSigPatterns[] = {
		"48 8B 0D ? ? ? ? 8B D3",
		"48 8B 0D ? ? ? ? 8B D1",
		"48 8B 0D ? ? ? ? 8B CB",
		"48 8B 0D ? ? ? ? 8B D8",
	};

	uint64_t sig1 = 0;
	for (const char* sig : kSigPatterns) {
		sig1 = mem.FindSignature(sig, imgBase, imgEnd, s_kernelPid);
		if (sig1)
			break;
	}
	if (!sig1) {
		SetStatus("DmaGamepad: no globals sig matched");
		return false;
	}

	const uint64_t globalsPtrVa = ResolveRipRelMov(mem, sig1, s_kernelPid);
	if (!KernelPtrOk(globalsPtrVa)) {
		SetStatus("DmaGamepad: globalsPtrVa invalid");
		return false;
	}

	const uint64_t driverGlobals = mem.Read<uint64_t>(globalsPtrVa, s_kernelPid);
	if (!KernelPtrOk(driverGlobals)) {
		SetStatus("DmaGamepad: driverGlobals invalid");
		return false;
	}

	static const char* kStateSigPatterns[] = {
		"66 89 43 ? 8A 43",
		"66 89 43 ? 8A 47",
		"66 89 43 ? 88 43",
	};

	uint64_t sig2 = 0;
	for (const char* sig2pat : kStateSigPatterns) {
		sig2 = mem.FindSignature(sig2pat, imgBase, imgEnd, s_kernelPid);
		if (sig2)
			break;
	}
	if (!sig2) {
		SetStatus("DmaGamepad: no state sig matched");
		return false;
	}

	const uint8_t offByte = mem.Read<uint8_t>(sig2 + 0x3, s_kernelPid);

	const uint64_t xenonBusInformation = mem.Read<uint64_t>(driverGlobals + 0x48, s_kernelPid);
	if (!KernelPtrOk(xenonBusInformation)) {
		SetStatus("DmaGamepad: xenonBusInformation invalid");
		return false;
	}

	const uint64_t gamepadInformation = mem.Read<uint64_t>(xenonBusInformation + 0x30, s_kernelPid);
	if (!KernelPtrOk(gamepadInformation)) {
		SetStatus("DmaGamepad: gamepadInformation invalid");
		return false;
	}

	const uint64_t gpi88 = mem.Read<uint64_t>(gamepadInformation + 0x88, s_kernelPid);
	if (!KernelPtrOk(gpi88)) {
		SetStatus("DmaGamepad: gpi88 invalid");
		return false;
	}

	const uint64_t stateVa = gpi88 + static_cast<uint64_t>(offByte);
	if (!KernelPtrOk(stateVa)) {
		SetStatus("DmaGamepad: stateVa invalid");
		return false;
	}

	s_controllerVa = stateVa;
	s_initialized = true;
	sprintf_s(s_lastStatus, "DmaGamepad: OK (xusb22) xenon+0x48 gp+0x30 field+0x88 state+0x%02X",
		static_cast<unsigned>(offByte));
	return true;
}

bool ReadRaw(DmaPadRaw& out) {
	memset(&out, 0, sizeof(out));
	if (!IsReady())
		return false;
	VMM_HANDLE vmm = PCIMemory::GetVmmHandle();
	if (!vmm)
		return false;
	DWORD cbRead = 0;
	return VMMDLL_MemReadEx(
		vmm,
		static_cast<DWORD>(s_kernelPid),
		s_controllerVa,
		reinterpret_cast<PBYTE>(&out),
		static_cast<DWORD>(sizeof(out)),
		&cbRead,
		VMMDLL_FLAG_NOCACHE) && cbRead == sizeof(out);
}

bool ReadState(XINPUT_STATE& out) {
	memset(&out, 0, sizeof(out));
	DmaPadRaw raw{};
	if (!ReadRaw(raw))
		return false;

	++out.dwPacketNumber;
	out.wButtons = raw.buttons;
	out.bLeftTrigger = raw.lt;
	out.bRightTrigger = raw.rt;
	out.sThumbLX = raw.lx;
	out.sThumbLY = raw.ly;
	out.sThumbRX = raw.rx;
	out.sThumbRY = raw.ry;
	return true;
}

} // namespace DmaGamepad
