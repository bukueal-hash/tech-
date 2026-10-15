#pragma once
#include <string>
#include <cstdint>

#include "../Core/FeaturePolicy.hpp"

class KmboxController {
public:
    KmboxController() = default;

    bool rememberKmboxInfo = false;
    const std::string CONFIG_FILE = "kmbox_config.ini";
    bool LoadKmboxConfig();
    void SetRememberConfig(bool remember) { rememberKmboxInfo = remember; }

    struct KMBoxConfig {
        std::string type = "MAKCU";
        std::string comPort = "COM3";
        std::string baudRate = "115200";
        std::string ip = "192.168.2.188";
        std::string port = "9742";
        std::string uuid;
        int minDelay{ 1 };
        int monitorIndex{ 0 };
        bool initialized{ false };
    } kmboxConfig;

    uint64_t lastReadyAttemptMs = 0;
    void InitializeFirst();
    void Initialize();
    /** (Re)open MAKCU/Net if aim needs hardware mouse. Tries MAKCU serial first, then Net. */
    bool EnsureReady();
    void renderKmboxSettings();

    /** Hot-path move (Net: fire-and-forget UDP). Use from AimAssistence hardware aim path. */
    void Move(int x, int y);
    /** Aim hot path — no minDelay sleep between chunks. */
    void MoveAim(int x, int y);
    /** Blocking move with device ACK (init / UI test). */
    void MoveBlocking(int x, int y);
    void LeftClick();
    /** Fire click with configurable hold duration in ms. For triggerbot semi-auto mode. */
    void Click(int holdMs);
    /** Press left mouse and keep held. For triggerbot auto-hold mode. */
    void HoldStart();
    /** Release left mouse. For triggerbot auto-hold mode. */
    void HoldEnd();
    void moveTest();
    void fireTest();

    /** Console summary of type / port or IP / initialized flag. */
    void LogStatus() const;
    std::string GetConfigFilePath() const;

    /** T3.2: true while a press was sent and its release is still outstanding. */
    bool PressOutstanding() const { return m_pressState.PressOutstanding(); }

private:
    // T3.2 -- single owner of press/release. The run report reads the outstanding
    // flag; this machine decides whether a new press may start.
    FeaturePolicy::PressReleaseState m_pressState;
};

extern KmboxController g_kmbox;
