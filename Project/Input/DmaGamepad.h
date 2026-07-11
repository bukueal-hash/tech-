#pragma once

#include <cstdint>

class DmaMem;

#pragma pack(push, 1)
struct DmaPadRaw {
	uint16_t buttons{};
	uint8_t  lt{};
	uint8_t  rt{};
	int16_t  lx{};
	int16_t  ly{};
	int16_t  rx{};
	int16_t  ry{};
};
#pragma pack(pop)

struct XINPUT_STATE {
	uint32_t dwPacketNumber;
	uint16_t wButtons;
	uint8_t  bLeftTrigger;
	uint8_t  bRightTrigger;
	int16_t  sThumbLX;
	int16_t  sThumbLY;
	int16_t  sThumbRX;
	int16_t  sThumbRY;
};

inline constexpr int kDmaTriggerPressedThreshold = 128;

namespace DmaGamepad {

bool TryInit(DmaMem& mem);
bool ReadState(XINPUT_STATE& out);
bool ReadRaw(DmaPadRaw& out);
bool IsReady();
const char* GetLastStatusMessage();
bool TryGetControllerReadTarget(uint64_t* outStateVa, int* outKernelPid, uint32_t* outHostPid);

} // namespace DmaGamepad
