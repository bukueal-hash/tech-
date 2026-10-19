#define NOMINMAX
#include "ThirdParty/ImGui/imgui.h"
#include "ThirdParty/ImGui/imgui_impl_win32.h"
#include "ThirdParty/ImGui/imgui_impl_dx11.h"

#include <d3d11.h>
#include <tchar.h>
#include <dwmapi.h>
#include <iostream>
#include <chrono>
#include <sstream>
#include <string>

#include "Core/Memory.h"
#include "Core/SessionLog.h"
#include "Core/EntityDiagnostics.hpp"
#include "Core/FeaturePolicy.hpp"
#include "Core/RunReport.hpp"
#include "DMA/Memory.h"
#include "DMA/DmaKeyboard.h"
#include "Hardware/KmBox.h"
#include "Input/Controller.h"
#include "Input/DmaGamepad.h"
#include "Interface/Overlay/Menu.h"
#include "Interface/OverlayHost.h"
#include "Interface/Utils/AutoConfig.h"
#include "Interface/Utils/UpgradeOptions.hpp"
#include "Functions/WorldScanCommon.h"
#include "Interface/Render.h"
#include "Core/AgentLog.h"
#include "Core/AssetNames.h"
#include "Core/CrashHandler.h"
#include "resource.h"

#define DOCTEST_CONFIG_IMPLEMENT
#include "ThirdParty/doctest/doctest.h"

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
bool CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void Render(HWND hwnd);

static DWORD g_pid = 0;

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    // Explicit pure diagnostics exit before attach, settings/log writes, graphics
    // and device initialization. Read the process command line directly so this
    // remains reliable for the Windows-subsystem entry point and shell launchers.
    const wchar_t* commandLine = GetCommandLineW();
    if (commandLine && std::wstring(commandLine).find(L"--audit-diagnostics") != std::wstring::npos) {
        doctest::Context context;
        context.setOption("out", "audit-diagnostics.txt");
        context.setOption("no-breaks", true);
        return context.run();
    }

    // Single-instance guard: exactly one overlay may run at a time. A second
    // ArcRaiders.exe exits here, before it reads settings, opens the diagnostics
    // log, attaches the DMA device or creates a window, so two copies cannot
    // fight over the shared config file, the log or the game attach. The handle
    // is deliberately held until the process ends -- that ownership IS the lock;
    // closing it early would let the next launch straight through. Fails open if
    // the mutex cannot be created, matching every other guard in this app.
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\ArcRaiders.SingleInstance");
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance);
        return 0;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    InstallCrashHandler();  // VEH — log crash context before dying
    SessionLog::Init();
    EntityDiagnostics::Init();
    RunReport::Init();
    std::cout << "[diag] run report: "
              << RunReport::GetPath().string() << std::endl;
    std::cout << "[diag] entity log: "
              << EntityDiagnostics::GetPath().string() << std::endl;
    timeBeginPeriod(1);

    // No AllocConsole — Release is Windows subsystem; keep overlay-only (no black console).

    while (!Memory::InitializeGame()) {
        // Local END cancels attach without waiting for a target process forever.
        for (int wait = 0; wait < 20; ++wait) {
            if (GetAsyncKeyState(VK_END) & 0x8000) {
                RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
                timeEndPeriod(1);
                return 0;
            }
            Sleep(100);
        }
    }
    g_pid = static_cast<DWORD>(Memory::getProcessID());

    AutoConfig_Load();
    AssetNamesInit();
    g_kmbox.InitializeFirst();
    g_kmbox.LogStatus();

    std::cout << "[*] DMA keyboard (target PC hotkeys)..." << std::endl;
    if (g_dmaKeyboard.Init())
        std::cout << "[+] DMA keyboard ready" << std::endl;
    else
        std::cout << "[!] DMA keyboard failed — aim hotkey uses overlay PC keys only" << std::endl;

    std::cout << "[*] Controller (xusb22)..." << std::endl;
    g_controller.InitController();
    std::cout << "[+] Controller: " << (g_controller.IsReady() ? "ready" : "not found — plug pad on target PC") << std::endl;

    std::cout << "=========================" << std::endl;
    std::cout << "[*] Starting overlay..." << std::endl;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(IDI_ICON1));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCE(IDI_ICON1));
    wc.lpszClassName = L"UnrealWindow";

    if (!RegisterClassExW(&wc)) {
        RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
        timeEndPeriod(1);
        return 1;
    }

    // Full screen including taskbar
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Opaque overlay (game runs on the DMA target PC — no transparency needed).
    // Deliberately NOT WS_EX_LAYERED and no DWM glass margins: the layered+
    // glass combo lets the DWM ghost-composite a stale copy of the surface
    // ("exact copy of everything flashes off to the side"). A plain topmost
    // opaque popup presents straight with no composition path to ghost.
    const HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"UnrealWindow",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    if (!hwnd) {
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
        timeEndPeriod(1);
        return 1;
    }

    // Push the icon onto the window explicitly: the taskbar/Alt-Tab does not
    // always pick up the window-class icon for WS_POPUP | WS_EX_NOACTIVATE
    // overlay windows, and falls back to a blank white glyph.
    const HICON hAppIcon = LoadIconW(hInst, MAKEINTRESOURCE(IDI_ICON1));
    if (hAppIcon)
    {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        engine.StopWorkerThreads();
        RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
        if (hwnd)
            ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        timeEndPeriod(1);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    style.WindowRounding = 12.f;
    style.FrameRounding = 8.f;
    style.GrabRounding = 8.f;
    style.TabRounding = 10.f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.95f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.32f, 0.65f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.20f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.32f, 0.65f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.32f, 0.65f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.18f, 0.26f, 0.50f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.98f, 1.00f);


    const bool win32Ready = ImGui_ImplWin32_Init(hwnd);
    const bool dx11Ready = win32Ready
        && ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    if (!dx11Ready) {
        if (win32Ready)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
        timeEndPeriod(1);
        return 1;
    }
    InitArcMenuAssets(g_pd3dDevice);
    OverlayDisplay_Bind(hwnd, &g_ResizeWidth, &g_ResizeHeight);
    OverlayDisplay_ApplySelectedMonitor();

    try {
        engine.StartWorkerThreads(); // graphics/UI dependencies are now ready
    } catch (...) {
        // A later thread constructor can fail after earlier workers started.
        engine.StopWorkerThreads();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
        timeEndPeriod(1);
        return 1;
    }

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_SwapChainOccluded) {
            const HRESULT testResult = g_pSwapChain->Present(0, DXGI_PRESENT_TEST);
            if (FAILED(testResult)) {
                std::cerr << "[graphics] Present test failed; shutting down overlay.\n";
                break;
            }
            if (testResult == DXGI_STATUS_OCCLUDED) {
                AutoConfig_Tick();
                ::Sleep(10);
                continue;
            }
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            const HRESULT resizeResult = g_pSwapChain->ResizeBuffers(
                0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            if (FAILED(resizeResult) || !CreateRenderTarget()) {
                std::cerr << "[graphics] Resize/render target failed; shutting down overlay.\n";
                break;
            }
        }

        // Paint-stall diagnostic: time the whole frame iteration (NewFrame →
        // Present) AND which phase ate the time (New/Rend/Draw/Pres). Baseline
        // is ~4ms at 240Hz vsync; anything >= 25ms is a stall that can re-show
        // the previous backbuffer (ghost-copy flash). Recorded in-memory only;
        // the worker drains it (AgentLog.h).
        const auto frameStart = std::chrono::steady_clock::now();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto tAfterNew = std::chrono::steady_clock::now();

        Render(hwnd);
        AutoConfig_Tick();
        // Same idea for the admission lane's first-probe ordering: it is the one
        // check in this build that the tests project cannot run for you.
        WorldScan::EnsureFirstProbeSelfTestRan();
        // Same treatment for the two invariants this build changed and that the
        // tests project cannot run: the read ledger's per-field split (what
        // scan_turn reports) and the legacy auto_config.ini round trip.
        {
            static bool s_ledgerChecked = false;
            if (!s_ledgerChecked)
            {
                s_ledgerChecked = true;
                const ReadBudget::BreakdownSelfTest rb = ReadBudget::RunBreakdownSelfTest();
                const UpgradeOptions::LegacyKeySelfTest legacy =
                    UpgradeOptions::RunLegacyKeySelfTest();
                std::ostringstream f;
                f << "{\"location\":\"ReadBudget.hpp\",\"message\":\"ledger_purpose_selftest\","
                  << "\"timestamp\":" << EntityDiagnostics::NowMs()
                  << ",\"passed\":" << (rb.passed ? "true" : "false")
                  << ",\"cases\":" << rb.cases << ",\"failed\":" << rb.failed
                  << ",\"detail\":\"" << EntityDiagnostics::Sanitize(rb.detail, 200) << "\"}\n";
                EntityDiagnostics::LogRaw(f.str());

                std::ostringstream c;
                c << "{\"location\":\"UpgradeOptions.hpp\",\"message\":\"config_legacy_keys_selftest\","
                  << "\"timestamp\":" << EntityDiagnostics::NowMs()
                  << ",\"passed\":" << (legacy.passed ? "true" : "false")
                  << ",\"cases\":" << legacy.cases << ",\"failed\":" << legacy.failed
                  << ",\"detail\":\"" << EntityDiagnostics::Sanitize(legacy.detail, 200) << "\"}\n";
                EntityDiagnostics::LogRaw(c.str());
            }
        }
        if (GetAsyncKeyState(VK_END) & 0x1)
            SendMessage(hwnd, WM_CLOSE, 0, 0);
        const auto tAfterRender = std::chrono::steady_clock::now();

        ImGui::Render();

        // Always dark grey background (2-PC DMA: game on other PC, no need for transparency)
        ImVec4 clearColor = ImVec4(0.06f, 0.06f, 0.08f, 1.0f);

        const float clear_color_with_alpha[4] = {
            clearColor.x * clearColor.w,
            clearColor.y * clearColor.w,
            clearColor.z * clearColor.w,
            clearColor.w
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const auto tAfterDraw = std::chrono::steady_clock::now();

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
        if (FAILED(hr)) {
            std::cerr << "[graphics] Present failed; shutting down overlay.\n";
            break;
        }

        const auto tEnd = std::chrono::steady_clock::now();
        const float frameMs = std::chrono::duration<float, std::milli>(tEnd - frameStart).count();
        float worstMs = 0.f;
        const char* worstPhase = "-";
        {
            const float mNew = std::chrono::duration<float, std::milli>(tAfterNew - frameStart).count();
            const float mRend = std::chrono::duration<float, std::milli>(tAfterRender - tAfterNew).count();
            const float mDraw = std::chrono::duration<float, std::milli>(tAfterDraw - tAfterRender).count();
            const float mPres = std::chrono::duration<float, std::milli>(tEnd - tAfterDraw).count();
            if (mPres > worstMs) { worstMs = mPres; worstPhase = "Pres"; }
            if (mDraw > worstMs) { worstMs = mDraw; worstPhase = "Draw"; }
            if (mRend > worstMs) { worstMs = mRend; worstPhase = "Rend"; }
            if (mNew > worstMs)  { worstMs = mNew;  worstPhase = "New"; }
        }
        PaintStallNote(frameMs, worstPhase, worstMs);

    }

    engine.StopWorkerThreads();
    AutoConfig_SaveNow();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    RunReport::Shutdown();
        EntityDiagnostics::Shutdown();
                SessionLog::Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    timeEndPeriod(1);

    return 0;
}

// ==========================

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    return CreateRenderTarget();
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

bool CreateRenderTarget()
{
    if (!g_pSwapChain || !g_pd3dDevice)
        return false;
    ID3D11Texture2D* pBackBuffer = nullptr;
    const HRESULT bufferResult =
        g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(bufferResult) || !pBackBuffer) {
        if (pBackBuffer)
            pBackBuffer->Release();
        std::cerr << "[graphics] Failed to acquire swap-chain backbuffer.\n";
        return false;
    }
    const HRESULT viewResult = g_pd3dDevice->CreateRenderTargetView(
        pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(viewResult) || !g_mainRenderTargetView) {
        CleanupRenderTarget();
        std::cerr << "[graphics] Failed to create render target view.\n";
        return false;
    }
    return true;
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
