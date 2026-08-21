#pragma once

#include <chrono>
#include <cstdint>



#define XINPUT_DPAD_UP_BTN    0x0001

#define XINPUT_DPAD_DOWN_BTN  0x0002

#define XINPUT_DPAD_LEFT_BTN  0x0004

#define XINPUT_DPAD_RIGHT_BTN 0x0008

#define XINPUT_START_BTN      0x0010

#define XINPUT_SHARE_BTN      0x0020

#define XINPUT_L3_BTN         0x0040

#define XINPUT_R3_BTN         0x0080

#define XINPUT_LB_BTN         0x0100

#define XINPUT_RB_BTN         0x0200

#define XINPUT_XBOX_BTN       0x0400

#define XINPUT_A_BTN          0x1000

#define XINPUT_B_BTN          0x2000

#define XINPUT_X_BTN          0x4000

#define XINPUT_Y_BTN          0x8000

#define XINPUT_LT_BTN         0x10000

#define XINPUT_RT_BTN         0x20000



struct XInputState {

    uint16_t buttons;

    uint8_t  lt;

    uint8_t  rt;

    int16_t  lx;

    int16_t  ly;

    int16_t  rx;

    int16_t  ry;

};



class Controller {

public:

    bool InitController();

    void UpdateState();

    void RefreshState();

    const XInputState& GetState() const { return state; }

    bool IsButtonDown(uint32_t button);

    bool IsReady() const { return xInputControllerDevice >= 0xFFFF000000000000ull; }

private:

    bool readKernelPadState(XInputState& out) const;

    uint32_t winlogonPID = 0;

    int kernelPID = 0;

    uint64_t xInputControllerDevice = 0;

    XInputState state{};

    XInputState previousState{};

    std::chrono::system_clock::time_point lastDown{};

    std::chrono::system_clock::time_point lastPressed{};

};



extern Controller g_controller;



bool ReinitController();

