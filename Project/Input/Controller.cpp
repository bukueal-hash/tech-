#include "../DMA/Memory.h"
#include "Controller.h"
#include "DmaGamepad.h"
#include "../Core/Engine.h"

#include <cstdio>
#include <cstring>

extern Engine engine;

Controller g_controller;

bool ReinitController() {
	return g_controller.InitController();
}

static constexpr uint64_t kKernelPtrMin = 0xFFFF000000000000ull;

static void MapPadRawToState(const DmaPadRaw& raw, XInputState& out) {
	out.buttons = raw.buttons;
	out.lt = raw.lt;
	out.rt = raw.rt;
	out.lx = raw.lx;
	out.ly = raw.ly;
	out.rx = raw.rx;
	out.ry = raw.ry;
}

bool Controller::readKernelPadState(XInputState& out) const {
	if (xInputControllerDevice < kKernelPtrMin)
		return false;

	if (DmaGamepad::IsReady()) {
		DmaPadRaw raw{};
		if (!DmaGamepad::ReadRaw(raw))
			return false;
		MapPadRawToState(raw, out);
		return true;
	}

	if (!g_mem.IsInitialized())
		return false;
	DmaMem dma(g_mem);
	return dma.Read(xInputControllerDevice, &out, sizeof(XInputState), kernelPID);
}

bool Controller::InitController() {
	if (!g_mem.IsInitialized()) {
		printf("[Controller] DMA not initialized\n");
		return false;
	}

	DmaMem dma(g_mem);

	if (!DmaGamepad::TryInit(dma)) {
		printf("[Controller] failed: %s\n", DmaGamepad::GetLastStatusMessage());
		return false;
	}

	uint64_t sharedVa{};
	int sharedKpid{};
	uint32_t sharedHost{};
	if (!DmaGamepad::TryGetControllerReadTarget(&sharedVa, &sharedKpid, &sharedHost)) {
		printf("[Controller] failed: %s\n", DmaGamepad::GetLastStatusMessage());
		return false;
	}

	xInputControllerDevice = sharedVa;
	kernelPID = sharedKpid;
	winlogonPID = sharedHost;
	printf("[Controller] OK device=0x%llX (%s)\n", static_cast<unsigned long long>(sharedVa),
		DmaGamepad::GetLastStatusMessage());
	return true;
}

void Controller::RefreshState() {
	if (xInputControllerDevice < kKernelPtrMin)
		return;
	if (!readKernelPadState(state))
		return;
}

void Controller::UpdateState() {
	if (xInputControllerDevice < kKernelPtrMin)
		return;
	previousState = state;
	(void)readKernelPadState(state);
}

void Controller::UpdatePressedState() {
	if (xInputControllerDevice < kKernelPtrMin)
		return;
	previousState = state;
	(void)readKernelPadState(state);
}

bool Controller::IsButtonDown(uint32_t button) {
	if (xInputControllerDevice < kKernelPtrMin)
		return false;

	RefreshState();

	if (button == XINPUT_LT_BTN)
		return state.lt > 128;

	if (button == XINPUT_RT_BTN)
		return state.rt > 128;

	return (state.buttons & static_cast<uint16_t>(button)) != 0;
}

bool Controller::IsButtonPressed(uint32_t button) {
	if (xInputControllerDevice < kKernelPtrMin)
		return false;

	UpdatePressedState();

	if (button == XINPUT_LT_BTN) {
		const bool cur = state.lt > 128;
		const bool prev = previousState.lt > 128;
		if (cur)
			previousState.lt = state.lt;
		else
			previousState.lt = 0;
		return cur && !prev;
	}

	if (button == XINPUT_RT_BTN) {
		const bool cur = state.rt > 128;
		const bool prev = previousState.rt > 128;
		if (cur)
			previousState.rt = state.rt;
		else
			previousState.rt = 0;
		return cur && !prev;
	}

	const uint16_t m = static_cast<uint16_t>(button);
	const bool current = (state.buttons & m) != 0;
	const bool previous = (previousState.buttons & m) != 0;
	const bool isPressed = current && !previous;
	if (current)
		previousState.buttons |= m;
	else
		previousState.buttons &= static_cast<uint16_t>(~m);

	return isPressed;
}
