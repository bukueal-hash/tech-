#pragma once
#include <string>
#include <cstdint>

class KmboxController {
public:
    KmboxController() = default;

    bool rememberKmboxInfo = false;
    const std::string CONFIG_FILE = "kmbox_config.ini";
    bool SaveKmboxConfig();
    bool LoadKmboxConfig();
    bool GetRememberConfig() const { return rememberKmboxInfo; }
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
    void MoveSmooth(int x, int y, uint32_t segments);
    void MoveBezier(int x, int y, uint32_t segments, int32_t ctrl_x, int32_t ctrl_y);
    void LeftClick();
    /** Physical LMB from KmBox/MAKCU monitor path; false if device not ready. */
    bool IsPhysicalLeftDown() const;
    /** True when hardware can report physical mouse buttons. */
    bool FiringProxyAvailable() const;
    void moveTest();
    void fireTest();

    /** Console summary of type / port or IP / initialized flag. */
    void LogStatus() const;
    std::string GetConfigFilePath() const;
};

extern KmboxController g_kmbox;
