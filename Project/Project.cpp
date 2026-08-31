#include "ThirdParty/ImGui/imgui.h"
#include "ThirdParty/ImGui/imgui_impl_win32.h"
#include "ThirdParty/ImGui/imgui_impl_dx11.h"

#include <d3d11.h>
#include <tchar.h>
#include <dwmapi.h>
#include <iostream>
#include <chrono>

#include "Core/Memory.h"
#include "Core/SessionLog.h"
#include "DMA/Memory.h"
#include "DMA/DmaKeyboard.h"
#include "Hardware/KmBox.h"
#include "Input/Controller.h"
#include "Input/DmaGamepad.h"
#include "Interface/Overlay/Menu.h"
#include "Interface/OverlayHost.h"
#include "Interface/Utils/AutoConfig.h"
#include "Interface/Render.h"
#include "Core/AgentLog.h"
#include "Core/AssetNames.h"
#include "Core/CrashHandler.h"
#include "resource.h"
#include "Dumper/Dumper.h"
#include "Dumper/DumperWorker.h"

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void Render(HWND hwnd);

static DWORD g_pid = 0;

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    InstallCrashHandler();  // VEH — log crash context before dying
    SessionLog::Init();
    timeBeginPeriod(1);

    // No AllocConsole — Release is Windows subsystem; keep overlay-only (no black console).

    while (!Memory::InitializeGame()) {
        Sleep(2000);
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

    engine.StartWorkerThreads();
    Dumper::StartDump(Dumper::DumpMode::FullSdk);

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

    RegisterClassExW(&wc);

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
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
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


    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    InitArcMenuAssets(g_pd3dDevice);
    OverlayDisplay_Bind(hwnd, &g_ResizeWidth, &g_ResizeHeight);
    OverlayDisplay_ApplySelectedMonitor();

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

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            AutoConfig_Tick();
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
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

    AutoConfig_SaveNow();
    Dumper::Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    engine.StopWorkerThreads();
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

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
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
