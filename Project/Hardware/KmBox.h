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
    /** (Re)open the selected MAKCU/Net device. Tries MAKCU serial first, then Net. */
    bool EnsureReady();
    void renderKmboxSettings();

    /** Low-level relative move command; retained for future device integrations. */
    void Move(int x, int y);
    /** Aim hot path — no minDelay sleep between chunks. */
    /** Returns true if the device reported the move as sent. */
    bool MoveAim(int x, int y);
    /** Blocking move with device ACK (init / UI test). */
    void MoveBlocking(int x, int y);
    void LeftClick();
    /** Send a device-level click with a configurable hold duration in ms. */
    void Click(int holdMs);
    /** Press left mouse and keep it held until released. */
    void HoldStart();
    /** Release the left mouse button. */
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
