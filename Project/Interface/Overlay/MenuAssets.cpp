#define WIN32_LEAN_AND_MEAN
#include "MenuTheme.h"

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

#include "../../ThirdParty/ImGui/imgui.h"

namespace {

ArcMenuUi g_theme;

static std::vector<unsigned char> ReadFileBytes(const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return {};
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return {};
    }
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(static_cast<size_t>(sz));
    if (fread(buf.data(), 1, buf.size(), f) != static_cast<size_t>(sz))
        buf.clear();
    fclose(f);
    return buf;
}

static ImFont* LoadFontFromFile(const wchar_t* path, float size, ImGuiIO& io)
{
    const std::vector<unsigned char> data = ReadFileBytes(path);
    if (data.empty())
        return nullptr;
    void* mem = IM_ALLOC(data.size());
    memcpy(mem, data.data(), data.size());
    ImFontConfig cfg{};
    cfg.FontDataOwnedByAtlas = true;
    return io.Fonts->AddFontFromMemoryTTF(mem, static_cast<int>(data.size()), size, &cfg);
}

static ImFont* LoadFontFirstPath(const wchar_t* const* paths, int count, float size, ImGuiIO& io)
{
    for (int i = 0; i < count; ++i) {
        if (ImFont* f = LoadFontFromFile(paths[i], size, io))
            return f;
    }
    return nullptr;
}

static bool CreateTextureFromRgba(ID3D11Device* device, const unsigned char* rgba, int w, int h,
    ID3D11ShaderResourceView** out_srv)
{
    if (!device || !rgba || w <= 0 || h <= 0 || !out_srv)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = rgba;
    sub.SysMemPitch = static_cast<UINT>(w) * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &sub, &tex)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    const HRESULT hr = device->CreateShaderResourceView(tex, &srvDesc, out_srv);
    tex->Release();
    return SUCCEEDED(hr);
}

static bool LoadLogoTextureWic(ID3D11Device* device, const wchar_t* path, ArcMenuUi& ui)
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool coOwned = (coHr == S_OK);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        if (coOwned) CoUninitialize();
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        factory->Release();
        if (coOwned) CoUninitialize();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoder->Release();
        factory->Release();
        if (coOwned) CoUninitialize();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        frame->Release();
        decoder->Release();
        factory->Release();
        if (coOwned) CoUninitialize();
        return false;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0.f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (coOwned) CoUninitialize();
        return false;
    }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (coOwned) CoUninitialize();
        return false;
    }

    const UINT stride = w * 4;
    const UINT bufSize = stride * h;
    std::vector<unsigned char> pixels(bufSize);
    hr = converter->CopyPixels(nullptr, stride, bufSize, pixels.data());

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    if (coOwned) CoUninitialize();

    if (FAILED(hr))
        return false;

    ID3D11ShaderResourceView* srv = nullptr;
    if (!CreateTextureFromRgba(device, pixels.data(), static_cast<int>(w), static_cast<int>(h), &srv))
        return false;

    ui.logoTexture = reinterpret_cast<ImTextureID>(srv);
    ui.logoWidth = static_cast<int>(w);
    ui.logoHeight = static_cast<int>(h);
    return true;
}

static bool LoadLogoTexture(ID3D11Device* device, const wchar_t* path, ArcMenuUi& ui)
{
    return LoadLogoTextureWic(device, path, ui);
}

static std::wstring GetProjectRootFromExe()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return L"";
    std::wstring dir(exePath);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash + 1);
    // Build\ArcRaiders.exe -> solution root is one level up
    for (int i = 0; i < 1; ++i) {
        const size_t s = dir.find_last_of(L"\\/", dir.length() - 2);
        if (s == std::wstring::npos)
            break;
        dir.resize(s + 1);
    }
    return dir;
}

static std::wstring GetExecutableDir()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return L"";
    std::wstring dir(exePath);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash + 1);
    return dir;
}

static bool IsLogoImageExtension(const std::wstring& extLower)
{
    return extLower == L".png" || extLower == L".jpg" || extLower == L".jpeg";
}

static void CollectImagesInDir(const std::wstring& dir, std::vector<std::wstring>& out)
{
    if (dir.empty())
        return;

    std::wstring pattern = dir;
    if (pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';
    pattern += L"*.*";

    WIN32_FIND_DATAW fd{};
    const HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        std::wstring fileName = fd.cFileName;
        const size_t dot = fileName.find_last_of(L'.');
        if (dot == std::wstring::npos)
            continue;

        std::wstring ext = fileName.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(towlower(c));
        });
        if (!IsLogoImageExtension(ext))
            continue;

        out.push_back(pattern.substr(0, pattern.size() - 3) + fileName);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static std::vector<std::wstring> CollectOverlayLogos()
{
    std::vector<std::wstring> paths;
    CollectImagesInDir(GetExecutableDir(), paths);
    CollectImagesInDir(GetProjectRootFromExe() + L"Project\\Interface\\", paths);
    CollectImagesInDir(GetProjectRootFromExe() + L"Project\\Assets\\", paths);

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

static bool LoadRandomLogo(ID3D11Device* device, ArcMenuUi& ui)
{
    const std::vector<std::wstring> logos = CollectOverlayLogos();
    if (logos.empty() || !device)
        return false;

    std::vector<size_t> order(logos.size());
    for (size_t i = 0; i < logos.size(); ++i)
        order[i] = i;

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(order.begin(), order.end(), rng);

    for (size_t idx : order) {
        if (LoadLogoTexture(device, logos[idx].c_str(), ui))
            return true;
    }
    return false;
}

} // namespace

ArcMenuUi& ArcMenuTheme()
{
    return g_theme;
}

void InitArcMenuAssets(ID3D11Device* device)
{
    ImGuiIO& io = ImGui::GetIO();

    static const wchar_t* kLogoFontPaths[] = {
        L"C:\\Windows\\Fonts\\tahomabd.ttf",
        L"C:\\Windows\\Fonts\\tahoma.ttf",
    };
    static const wchar_t* kHeaderFontPaths[] = {
        L"C:\\Windows\\Fonts\\segoeui.ttf",
        L"C:\\Windows\\Fonts\\arial.ttf",
    };
    static const wchar_t* kRegularFontPaths[] = {
        L"C:\\Windows\\Fonts\\verdanai.ttf",
        L"C:\\Windows\\Fonts\\verdana.ttf",
    };

    g_theme.logoFont = LoadFontFirstPath(kLogoFontPaths, 2, 32.0f, io);
    g_theme.headerFont = LoadFontFirstPath(kHeaderFontPaths, 2, 22.0f, io);
    g_theme.regularFont = LoadFontFirstPath(kRegularFontPaths, 2, 19.0f, io);

    if (!g_theme.logoFont) g_theme.logoFont = io.Fonts->AddFontDefault();
    if (!g_theme.headerFont) g_theme.headerFont = io.Fonts->AddFontDefault();
    if (!g_theme.regularFont) g_theme.regularFont = io.Fonts->AddFontDefault();

    g_theme.logoTexture = 0;
    g_theme.logoWidth = 0;
    g_theme.logoHeight = 0;
    if (device)
        (void)LoadRandomLogo(device, g_theme);

    io.Fonts->Build();
}
