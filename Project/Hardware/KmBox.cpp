#define WIN32_LEAN_AND_MEAN
#include "KmBox.h"
#include "KmboxNet.hpp"
#include "Makcu/MyMakcu.h"
#include "../Interface/OverlayHost.h"
#include "../Interface/Overlay/MenuLayout.h"
#include "../Interface/Overlay/Menu.h"
#include "../Interface/Utils/AutoConfig.h"
#include "../Core/StringUtils.hpp"
#include "../Core/RunReport.hpp"
#include "../ThirdParty/ImGui/imgui.h"

#include <Windows.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

template<typename... Ts>
void KmboxLogLine(const char* level, Ts&&... parts)
{
    std::cout << level;
    ((std::cout << parts), ...);
    std::cout << '\n';
}

#define LOG_INFO(...)  KmboxLogLine("[INFO] ", __VA_ARGS__)
#define LOG_WARN(...)  KmboxLogLine("[WARN] ", __VA_ARGS__)
#define LOG_ERROR(...) KmboxLogLine("[ERROR] ", __VA_ARGS__)

std::string ExeDirectory()
{
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
        return {};
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos)
        dir.resize(slash + 1);
    return dir;
}

static uint64_t GetNowMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000;
}

} // namespace

std::string KmboxController::GetConfigFilePath() const
{
    return ExeDirectory() + CONFIG_FILE;
}

void KmboxController::LogStatus() const
{
    std::cout << "[KmBox] type=" << kmboxConfig.type;
    if (kmboxConfig.type == "MAKCU")
        std::cout << " port=" << kmboxConfig.comPort;
    else if (kmboxConfig.type == "Net")
        std::cout << " endpoint=" << kmboxConfig.ip << ":" << kmboxConfig.port;
    std::cout << " initialized=" << (kmboxConfig.initialized ? "yes" : "no") << '\n';
}

void KmboxController::InitializeFirst() {
    const bool fromSaved = AutoConfig_HadFileOnLoad();
    if (fromSaved)
        LOG_INFO("[", kmboxConfig.type, "] Loaded saved config (auto_config.ini)");

    if (kmboxConfig.type == "MAKCU") {
        const std::string detected = MyMakcu::AutoDetectComPort();
        if (!detected.empty()) {
            kmboxConfig.comPort = detected;
            LOG_INFO("[MAKCU] Auto-detected device on ", detected);
            AutoConfig_MarkDirty();
        } else if (fromSaved && !kmboxConfig.comPort.empty()) {
            LOG_INFO("[MAKCU] Using saved port: ", kmboxConfig.comPort);
        } else {
            LOG_WARN("[MAKCU] No MAKCU device found automatically; using default/manual port ",
                      kmboxConfig.comPort);
        }
        Initialize();
    } else if (kmboxConfig.type == "Net") {
        if (!fromSaved)
            LOG_INFO("[Net] Using default IP/Port settings");
        else
            LOG_INFO("[Net] ", kmboxConfig.ip, ":", kmboxConfig.port);
        Initialize();

        if (!kmboxConfig.initialized) {
            LOG_ERROR("[Net] Initialization failed; falling back to MAKCU auto-detect...");
            kmboxConfig.type = "MAKCU";
            const std::string detected = MyMakcu::AutoDetectComPort();
            if (!detected.empty()) {
                kmboxConfig.comPort = detected;
                LOG_INFO("[MAKCU] Auto-detected device on ", detected);
                Initialize();
            } else {
                LOG_ERROR("[MAKCU] No MAKCU device found either.");
            }
        }
    } else {
        LOG_WARN("[", kmboxConfig.type, "] Unknown type, initializing anyway");
        Initialize();
    }
}

bool KmboxController::EnsureReady() {
    if (kmboxConfig.initialized)
        return true;

    // Fix slow ticks: Throttle re-initialization attempts to every 500ms.
    const uint64_t now = GetNowMs();
    if (lastReadyAttemptMs != 0 && now - lastReadyAttemptMs < 500)
        return false;
    lastReadyAttemptMs = now;

    // First attempt: Try the configured port if we have one.
    if (!kmboxConfig.comPort.empty()) {
        kmboxConfig.type = "MAKCU";
        Initialize();
        if (kmboxConfig.initialized) return true;
    }

    // Second attempt: Auto-detect MAKCU (slow, scans COM ports).
    const std::string detected = MyMakcu::AutoDetectComPort();
    if (!detected.empty()) {
        kmboxConfig.type = "MAKCU";
        kmboxConfig.comPort = detected;
        Initialize();
        if (kmboxConfig.initialized) return true;
    }

    // Third attempt: Try Net (KmBox B+ Pro).
    kmboxConfig.type = "Net";
    Initialize();
    if (kmboxConfig.initialized) return true;

    return false;
}

void KmboxController::Initialize() {
    if (kmboxConfig.type == "MAKCU") {
        LOG_INFO("[#] Init MAKCU on COM", kmboxConfig.comPort, "...");

        MyMakcu::SetComPort(kmboxConfig.comPort);
        if (!MyMakcu::Initialize()) {
            LOG_ERROR("[MAKCU] MAKCU initialization failed - port may be in use by another application");
            LOG_WARN("[MAKCU] Close all programs using COM ports (Arduino IDE, PuTTY, etc.)");
            LOG_WARN("[MAKCU] Try running as administrator");
            kmboxConfig.initialized = false;
            return;
        }

        LOG_INFO("[MAKCU] MAKCU initialized successfully on ", kmboxConfig.comPort);
        kmboxConfig.initialized = true;
    } else if (kmboxConfig.type == "Net") {
        LOG_INFO("[#] Init KmBoxNet on ", kmboxConfig.ip, ":", kmboxConfig.port, "...");

        char ipBuf[64], portBuf[16], uuidBuf[16];
        strcpy_s(ipBuf, kmboxConfig.ip.c_str());
        strcpy_s(portBuf, kmboxConfig.port.c_str());
        if (kmboxConfig.uuid.size() >= sizeof(uuidBuf)) {
            LOG_ERROR("[KmBoxNet] UUID too long (max 15 chars)");
            return;
        }
        strcpy_s(uuidBuf, kmboxConfig.uuid.c_str());

        int result = kmNet_init(ipBuf, portBuf, uuidBuf);
        if (result != 0) {
            LOG_ERROR("[KmBoxNet] Initialization failed (error: ", result, ")");
            LOG_ERROR("[KmBoxNet] Check IP, port, and UUID. Ensure device is powered on.");
            kmboxConfig.initialized = false;
            return;
        }

        LOG_INFO("[KmBoxNet] Initialized successfully on ", kmboxConfig.ip, ":", kmboxConfig.port);
        kmboxConfig.initialized = true;
        kmNet_monitor(1);
    } else {
        LOG_WARN("[KmBox] Unsupported type \"", kmboxConfig.type,
                 "\" — only MAKCU and Net are supported; remapping to MAKCU");
        kmboxConfig.type = "MAKCU";
        MyMakcu::SetComPort(kmboxConfig.comPort);
        if (!MyMakcu::Initialize()) {
            kmboxConfig.initialized = false;
            return;
        }
        kmboxConfig.initialized = true;
    }

    AutoConfig_MarkDirty();
}

void KmboxController::renderKmboxSettings() {
    ArcMenuLayout::HoverableText("KMBox Device");
    ArcMenuHoverTooltip("Hardware mouse device settings.");
    ImGui::Separator();
    ImGui::Spacing();

    const int monitorCount = OverlayDisplay_GetMonitorCount();
    if (monitorCount <= 0) {
        ImGui::TextUnformatted("No active displays found.");
        ArcMenuHoverTooltip("No monitor found for the overlay - check display connections.");
    } else {
        static std::vector<std::string> monitorLabels;
        static std::vector<const char*> monitorItems;
        monitorLabels.clear();
        monitorItems.clear();
        monitorLabels.reserve(static_cast<size_t>(monitorCount));
        monitorItems.reserve(static_cast<size_t>(monitorCount));
        for (int i = 0; i < monitorCount; ++i) {
            monitorLabels.push_back(OverlayDisplay_GetMonitorLabel(i));
            monitorItems.push_back(monitorLabels.back().c_str());
        }

        int selected = OverlayDisplay_GetSelectedMonitor();
        if (selected < 0)
            selected = 0;
        if (selected >= monitorCount)
            selected = monitorCount - 1;

        if (ArcMenuLayout::Combo(
                "Overlay monitor", "##kmbox_overlay_monitor", &selected, monitorItems.data(), monitorCount)) {
            OverlayDisplay_SetSelectedMonitor(selected);
            kmboxConfig.monitorIndex = selected;
            if (OverlayDisplay_ApplySelectedMonitor())
                AutoConfig_MarkDirty();
        }
        ArcMenuHoverTooltip("Move the ESP/menu overlay to this monitor.");

        if (ImGui::Button("Apply monitor now")) {
            kmboxConfig.monitorIndex = OverlayDisplay_GetSelectedMonitor();
            OverlayDisplay_ApplySelectedMonitor();
            AutoConfig_MarkDirty();
        }
        ArcMenuHoverTooltip("Re-apply overlay position/size on the selected monitor.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char* types[] = { "MAKCU (Serial)", "Net (UDP)" };
    int currentType = (kmboxConfig.type == "Net") ? 1 : 0;
    if (ArcMenuLayout::Combo("Device Type", "##kmbox_device_type", &currentType, types, IM_ARRAYSIZE(types))) {
        kmboxConfig.type = (currentType == 1) ? "Net" : "MAKCU";
    }
    ArcMenuHoverTooltip("MAKCU uses USB serial on this PC. Net uses UDP to a KmBox network device.");

    ImGui::Spacing();

    if (kmboxConfig.type == "MAKCU") {
        ArcMenuLayout::HoverableText("MAKCU (Serial) Settings");
    ArcMenuHoverTooltip("Serial settings for the MAKCU device.");
        ImGui::Separator();

        char comBuf[64] = {};
        strcpy_s(comBuf, sizeof(comBuf), kmboxConfig.comPort.c_str());
        ArcMenuLayout::HoverableText("COM port");
        ArcMenuHoverTooltip("Serial port for MAKCU, e.g. COM3. Must match the port shown in Device Manager.");
        ImGui::PushItemWidth(-1.0f);
        if (ImGui::InputText("##kmbox_com_port", comBuf, sizeof(comBuf)))
            kmboxConfig.comPort = comBuf;
        ImGui::PopItemWidth();
        ArcMenuHoverTooltip("Serial port for MAKCU, e.g. COM3. Must match the port shown in Device Manager.");
        if (ImGui::Button("Auto-detect")) {
            const std::string detected = MyMakcu::AutoDetectComPort();
            if (!detected.empty()) {
                kmboxConfig.comPort = detected;
                LOG_INFO("[MAKCU] Auto-detected ", detected);
            }
        }
        ArcMenuHoverTooltip("Scan COM ports and pick the first MAKCU device found.");
    } else {
        ArcMenuLayout::HoverableText("KmBoxNet (UDP) Settings");
    ArcMenuHoverTooltip("UDP settings for the KmBox Net device.");
        ImGui::Separator();

        char ipBuf[64] = {};
        strcpy_s(ipBuf, sizeof(ipBuf), kmboxConfig.ip.c_str());
        if (ArcMenuLayout::InputText("IP", "##kmbox_ip", ipBuf, static_cast<int>(sizeof(ipBuf))))
            kmboxConfig.ip = ipBuf;
        ArcMenuHoverTooltip("KmBoxNet device IP address on your LAN.");

        char portBuf[16] = {};
        strcpy_s(portBuf, sizeof(portBuf), kmboxConfig.port.c_str());
        ArcMenuLayout::HoverableText("Port");
        ArcMenuHoverTooltip("UDP port configured on the KmBox Net device.");
        ImGui::PushItemWidth(-1.0f);
        if (ImGui::InputText("##kmbox_port", portBuf, sizeof(portBuf)))
            kmboxConfig.port = portBuf;
        ImGui::PopItemWidth();
        ArcMenuHoverTooltip("UDP port configured on the KmBox Net device.");

        char uuidBuf[16] = {};
        strcpy_s(uuidBuf, sizeof(uuidBuf), kmboxConfig.uuid.c_str());
        if (ArcMenuLayout::InputText("UUID (MAC)", "##kmbox_uuid", uuidBuf, static_cast<int>(sizeof(uuidBuf))))
            kmboxConfig.uuid = uuidBuf;
        ArcMenuHoverTooltip("Device UUID / MAC string required by kmNet_init.");

        ImGui::Spacing();
        ArcMenuLayout::HoverableText("KmBoxNet Advanced");
    ArcMenuHoverTooltip("Raw device commands for troubleshooting.");
        ImGui::Separator();

        if (ImGui::Button("Enable monitor"))
            kmNet_monitor(1);
        ArcMenuHoverTooltip("Send the monitor-enable command to the KmBox Net device.");
        ImGui::SameLine();
        if (ImGui::Button("Disable monitor"))
            kmNet_monitor(0);
        ArcMenuHoverTooltip("Send the monitor-disable command to the KmBox Net device.");

        if (ImGui::Button("Mask LMB"))
            kmNet_mask_mouse_left(1);
        ArcMenuHoverTooltip("Block the physical left mouse button from reaching the target PC.");
        ImGui::SameLine();
        if (ImGui::Button("Unmask all"))
            kmNet_unmask_all();
        ArcMenuHoverTooltip("Unblock all masked mouse buttons.");

        if (ImGui::Button("LCD clear"))
            kmNet_lcd_color(0x0000);
        ArcMenuHoverTooltip("Clear the device LCD display.");
        ImGui::SameLine();
        if (ImGui::Button("Reboot device"))
            kmNet_reboot();
        ArcMenuHoverTooltip("Reboot the KmBox Net device.");
    }

    ImGui::Spacing();

    ImGui::TextDisabled("Settings auto-save to auto_config.ini");
    ArcMenuHoverTooltip("Settings auto-save to this file.");

    ImGui::Spacing();

    ImGui::SliderInt("Move delay (ms)", &kmboxConfig.minDelay, 0, 50);
    ArcMenuHoverTooltip(
        "Sleep after Move/MoveBlocking/LeftClick. MoveAim (aim hot path) ignores this.");
    if (ImGui::IsItemDeactivatedAfterEdit())
        AutoConfig_MarkDirty();

    ImGui::Spacing();

    if (ImGui::Button("Initialize"))
        Initialize();
    ArcMenuHoverTooltip("Open the device and set initialized status. Required for hardware aim.");

    ImGui::SameLine();
    if (ImGui::Button("Move test"))
        moveTest();
    ArcMenuHoverTooltip("Small mouse move to verify KMBox or MAKCU is responding.");

    ImGui::SameLine();
    if (ImGui::Button("Test fire"))
        fireTest();
    ArcMenuHoverTooltip("Manual hardware left-click test. This is not used by aimbot.");

    if (kmboxConfig.initialized)
        ArcMenuLayout::HoverableTextColoredF(ImVec4(0, 1, 0, 1.f), "Status: connected");
    else
        ArcMenuLayout::HoverableTextColoredF(ImVec4(1, 0, 0, 1.f), "Status: not initialized");
    ArcMenuHoverTooltip("Whether the hardware mouse connection is active.");
}

bool KmboxController::LoadKmboxConfig() {
    std::ifstream file(GetConfigFilePath());
    if (!file.is_open())
        return false;

    std::string line;
    while (std::getline(file, line)) {
        StringUtils::TrimInPlace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        StringUtils::TrimInPlace(key);
        StringUtils::TrimInPlace(val);

        if (key == "typeName") {
            // Legacy BPro was never implemented — treat as MAKCU.
            if (val == "BPro")
                kmboxConfig.type = "MAKCU";
            else if (val == "Net" || val == "MAKCU")
                kmboxConfig.type = val;
        } else if (key == "type") {
            // Legacy: 0=BPro(unimplemented)→MAKCU, 1=Net, 2=MAKCU.
            const int type = std::atoi(val.c_str());
            if (type == 1)
                kmboxConfig.type = "Net";
            else
                kmboxConfig.type = "MAKCU";
        } else if (key == "comPort") {
            kmboxConfig.comPort = val;
        } else if (key == "baudRate") {
            kmboxConfig.baudRate = val;
        } else if (key == "ip") {
            kmboxConfig.ip = val;
        } else if (key == "port") {
            kmboxConfig.port = val;
        } else if (key == "uuid") {
            kmboxConfig.uuid = val;
        } else if (key == "minDelay") {
            kmboxConfig.minDelay = std::atoi(val.c_str());
        } else if (key == "remember") {
            rememberKmboxInfo = (std::atoi(val.c_str()) != 0);
        } else if (key == "monitorIndex") {
            kmboxConfig.monitorIndex = std::atoi(val.c_str());
        }
    }

    if (kmboxConfig.monitorIndex < 0)
        kmboxConfig.monitorIndex = 0;
    OverlayDisplay_SetSelectedMonitor(kmboxConfig.monitorIndex);

    return true;
}

void KmboxController::Move(int x, int y) {
    if (!kmboxConfig.initialized)
        return;
    RunReport::NoteInputMoveSubmitted();
    if (kmboxConfig.type == "MAKCU")
        MyMakcu::Move(x, y);
    else if (kmboxConfig.type == "Net")
        kmNet_mouse_move_fire_and_forget(static_cast<short>(x), static_cast<short>(y));
    if (kmboxConfig.minDelay > 0)
        Sleep(static_cast<DWORD>(kmboxConfig.minDelay));
}

void KmboxController::MoveAim(int x, int y) {
    if (!kmboxConfig.initialized)
        return;
    // Run report: counted where the command reaches the device, not where it is
    // decided, so the number is submissions rather than intent.
    RunReport::NoteInputMoveSubmitted();
    if (kmboxConfig.type == "MAKCU")
        MyMakcu::Move(x, y);
    else if (kmboxConfig.type == "Net")
        kmNet_mouse_move_fire_and_forget(static_cast<short>(x), static_cast<short>(y));
}

void KmboxController::MoveBlocking(int x, int y) {
    if (!kmboxConfig.initialized)
        return;
    if (kmboxConfig.type == "MAKCU")
        MyMakcu::Move(x, y);
    else if (kmboxConfig.type == "Net")
        kmNet_mouse_move(static_cast<short>(x), static_cast<short>(y));
    if (kmboxConfig.minDelay > 0)
        Sleep(static_cast<DWORD>(kmboxConfig.minDelay));
}

void KmboxController::moveTest() {
    if (!kmboxConfig.initialized) {
        LOG_WARN("[", kmboxConfig.type, "] Move Test: device not initialized");
        return;
    }
    if (kmboxConfig.type == "Net")
        kmNet_mouse_move(50, 50);
    else
        MoveBlocking(50, 50);
    LOG_INFO("[", kmboxConfig.type, "] Move Test executed");
}

void KmboxController::fireTest() {
    if (!kmboxConfig.initialized) {
        LOG_WARN("[", kmboxConfig.type, "] Fire Test: device not initialized");
        return;
    }
    LeftClick();
    LOG_INFO("[", kmboxConfig.type, "] Fire Test executed");
}

void KmboxController::LeftClick() {
    if (!kmboxConfig.initialized)
        return;
    RunReport::NoteInputPressSubmitted();
    if (kmboxConfig.type == "MAKCU") {
        MyMakcu::LeftClick();
    } else if (kmboxConfig.type == "Net") {
        kmNet_mouse_left(1);
        Sleep(22);
        kmNet_mouse_left(0);
    }
    if (kmboxConfig.minDelay > 0)
        Sleep(static_cast<DWORD>(kmboxConfig.minDelay));
}

void KmboxController::Click(int holdMs) {
    if (!kmboxConfig.initialized)
        return;
    // T3.2: a click is a press+release, so it must not interleave with an
    // outstanding release either.
    if (!m_pressState.BeginPress()) {
        RunReport::NoteInputRejected();
        return;
    }
    RunReport::NoteInputPressSubmitted();
    if (holdMs < 1)
        holdMs = 1;
    if (kmboxConfig.type == "MAKCU") {
        MyMakcu::MouseDown();
        Sleep(static_cast<DWORD>(holdMs));
        MyMakcu::MouseUp();
    } else if (kmboxConfig.type == "Net") {
        kmNet_mouse_left(1);
        Sleep(static_cast<DWORD>(holdMs));
        kmNet_mouse_left(0);
    }
    // The release went out inside this call, so nothing stays outstanding.
    m_pressState.BeginRelease();
}

void KmboxController::HoldStart() {
    if (!kmboxConfig.initialized)
        return;
    // T3.2: refuse a new press while a release is outstanding. Pressing on top of
    // an undelivered release is how a button ends up held on the target machine.
    if (!m_pressState.BeginPress()) {
        RunReport::NoteInputRejected();
        return;
    }
    RunReport::NoteInputPressSubmitted();
    RunReport::PressOutstanding().store(true, std::memory_order_relaxed);
    if (kmboxConfig.type == "MAKCU") {
        MyMakcu::MouseDown();
    } else if (kmboxConfig.type == "Net") {
        kmNet_mouse_left(1);
    }
}

void KmboxController::HoldEnd() {
    // T3.2: this is the only release path, and it owns the state. A release with
    // nothing outstanding is a no-op rather than a stray mouse-up.
    const int attempts = m_pressState.BeginRelease();
    if (attempts == 0)
        return;

    if (!kmboxConfig.initialized) {
        // The device cannot take the release. Keep the outstanding flag set: the
        // button may still be down on the target machine, and clearing the flag
        // would hide that rather than report it. The run report shows it, and the
        // next press is refused until a release gets through.
        m_pressState.NoteReleaseUndeliverable();
        RunReport::PressOutstanding().store(true, std::memory_order_relaxed);
        RunReport::NoteInputRejected();
        return;
    }

    RunReport::NoteInputReleaseSubmitted();
    // Bounded retry: the device has no ACK, so the best available guarantee is
    // that the release is issued more than once rather than assumed.
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (kmboxConfig.type == "MAKCU")
            MyMakcu::MouseUp();
        else if (kmboxConfig.type == "Net")
            kmNet_mouse_left(0);
    }
    RunReport::PressOutstanding().store(false, std::memory_order_relaxed);
}

KmboxController g_kmbox;
