#pragma once

#include <cstdint>

class DmaKeyboard {
public:
    bool Init();
    bool IsReady() const { return ready_; }
    bool IsKeyDown(uint32_t virtualKeyCode);

private:
    bool InitKeyboard();
    void UpdateKeys();
    bool QueryWindowsBuild(int& buildOut) const;

    uint64_t gafAsyncKeyStateExport_ = 0;
    uint8_t stateBitmap_[64]{};
    int winlogonPid_ = 0;
    int consecutiveFailures_ = 0;
    bool ready_ = false;
};

extern DmaKeyboard g_dmaKeyboard;
