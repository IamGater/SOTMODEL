// Win32 window + D3D11 swap chain + Dear ImGui host for the FModel-style UI.
#include "ui/App.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "export/Png.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain1> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
UINT g_resizeW = 0, g_resizeH = 0;
bool g_minimized = false;

void CreateRtv() {
    ComPtr<ID3D11Texture2D> back;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
}

bool CreateDevice(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    g_dev.As(&dxgiDev);
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(g_dev.Get(), hwnd, &sd, nullptr, nullptr, &g_swap))) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        if (FAILED(factory->CreateSwapChainForHwnd(g_dev.Get(), hwnd, &sd, nullptr, nullptr, &g_swap))) return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateRtv();
    return true;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        g_minimized = wParam == SIZE_MINIMIZED;
        if (!g_minimized) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lParam)->ptMinTrackSize = {800, 500};
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ApplyTheme(float scale) {
    // FModel-like: dark slate panels with a blue accent
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 3;
    s.PopupRounding = 3;
    s.ScrollbarRounding = 3;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(7, 5);
    s.ScrollbarSize = 13;
    s.IndentSpacing = 16;
    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    c[ImGuiCol_Text] = rgb(225, 228, 235);
    c[ImGuiCol_TextDisabled] = rgb(125, 132, 145);
    c[ImGuiCol_WindowBg] = rgb(30, 33, 40);
    c[ImGuiCol_ChildBg] = rgb(30, 33, 40);
    c[ImGuiCol_PopupBg] = rgb(36, 39, 48, 0.98f);
    c[ImGuiCol_Border] = rgb(52, 56, 68);
    c[ImGuiCol_FrameBg] = rgb(42, 46, 56);
    c[ImGuiCol_FrameBgHovered] = rgb(52, 57, 70);
    c[ImGuiCol_FrameBgActive] = rgb(58, 64, 80);
    c[ImGuiCol_TitleBg] = rgb(24, 26, 32);
    c[ImGuiCol_TitleBgActive] = rgb(24, 26, 32);
    c[ImGuiCol_MenuBarBg] = rgb(24, 26, 32);
    c[ImGuiCol_ScrollbarBg] = rgb(30, 33, 40);
    c[ImGuiCol_ScrollbarGrab] = rgb(62, 67, 80);
    c[ImGuiCol_CheckMark] = rgb(80, 160, 255);
    c[ImGuiCol_SliderGrab] = rgb(70, 140, 230);
    c[ImGuiCol_SliderGrabActive] = rgb(90, 160, 255);
    c[ImGuiCol_Button] = rgb(48, 53, 66);
    c[ImGuiCol_ButtonHovered] = rgb(58, 110, 190);
    c[ImGuiCol_ButtonActive] = rgb(50, 100, 175);
    c[ImGuiCol_Header] = rgb(45, 80, 135);
    c[ImGuiCol_HeaderHovered] = rgb(52, 92, 152);
    c[ImGuiCol_HeaderActive] = rgb(58, 104, 170);
    c[ImGuiCol_Separator] = rgb(52, 56, 68);
    c[ImGuiCol_Tab] = rgb(36, 40, 49);
    c[ImGuiCol_TabHovered] = rgb(58, 104, 170);
    c[ImGuiCol_TabSelected] = rgb(46, 84, 140);
    c[ImGuiCol_TabDimmed] = rgb(32, 35, 43);
    c[ImGuiCol_TabDimmedSelected] = rgb(40, 60, 95);
    c[ImGuiCol_TableHeaderBg] = rgb(36, 40, 49);
    c[ImGuiCol_TableBorderStrong] = rgb(52, 56, 68);
    c[ImGuiCol_TableBorderLight] = rgb(44, 48, 58);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.5f);
    s.ScaleAllSizes(scale);
}
}

// Saves the current back buffer (after ImGui rendered) as PNG: used by --screenshot for automated UI checks.
static bool SaveBackBuffer(const std::wstring& path) {
    ComPtr<ID3D11Texture2D> back, staging;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    g_ctx->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
    std::vector<u8> rgba((size_t)td.Width * td.Height * 4);
    for (UINT y = 0; y < td.Height; y++) memcpy(&rgba[(size_t)y * td.Width * 4], (u8*)ms.pData + (size_t)y * ms.RowPitch, td.Width * 4);
    g_ctx->Unmap(staging.Get(), 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    Bytes png = EncodePng(rgba.data(), td.Width, td.Height);
    return WriteWholeFile(path, png.data(), png.size());
}

int RunGui(const std::vector<std::string>& args) {
    // arguments: asset paths to open, optional --screenshot <file.png> [--delay <seconds>] (captures the window and exits)
    std::vector<std::string> open;
    std::wstring screenshot;
    double delay = 4;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--screenshot" && i + 1 < args.size()) screenshot = Widen(args[++i]);
        else if (args[i] == "--delay" && i + 1 < args.size()) delay = atof(args[++i].c_str());
        else open.push_back(args[i]);
    }
    ULONGLONG start = GetTickCount64();
    ImGui_ImplWin32_EnableDpiAwareness();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"SOTModel", nullptr};
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SOTModel - Sea of Thieves asset explorer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                (int)(1600 * scale), (int)(950 * scale), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (!CreateDevice(hwnd)) {
        MessageBoxW(hwnd, L"Could not create a Direct3D 11 device.", L"SOTModel", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = Narrow(Settings::Path().substr(0, Settings::Path().find_last_of(L'\\'))) + "\\imgui.ini";
    CreateDirectories(Widen(iniPath.substr(0, iniPath.find_last_of('\\'))));
    io.IniFilename = iniPath.c_str();
    ApplyTheme(scale);

    wchar_t winDir[MAX_PATH];
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::string fonts = Narrow(winDir) + "\\Fonts\\";
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    if (!io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 16.f * scale, &cfg)) io.Fonts->AddFontDefault();
    ImFont* mono = io.Fonts->AddFontFromFileTTF((fonts + "consola.ttf").c_str(), 15.f * scale, &cfg);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());

    {
        App app;
        app.monoFont = mono ? mono : io.Fonts->Fonts[0];
        app.openOnStart = open;
        if (!app.Init(hwnd, g_dev.Get(), g_ctx.Get())) {
            MessageBoxW(hwnd, L"Renderer initialisation failed (see log).", L"SOTModel", MB_ICONERROR);
        }
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done || app.WantsQuit()) break;
            if (g_minimized) { Sleep(50); continue; }
            // idle: sleep until input or a worker posts a result (keeps CPU/GPU use near zero)
            if (!app.Busy() && screenshot.empty()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
            if (g_resizeW && g_resizeH) {
                g_rtv.Reset();
                g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeW = g_resizeH = 0;
                CreateRtv();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.Frame();
            ImGui::Render();
            const float clear[4] = {0.118f, 0.129f, 0.157f, 1};
            ID3D11RenderTargetView* rtv = g_rtv.Get();
            g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            g_ctx->ClearRenderTargetView(rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (!screenshot.empty() && (GetTickCount64() - start) / 1000.0 >= delay && !app.Loading()) {
                SaveBackBuffer(screenshot);
                break;
            }
            HRESULT hr = g_swap->Present(1, 0);
            if (hr == DXGI_STATUS_OCCLUDED) Sleep(16);
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_rtv.Reset();
    g_swap.Reset();
    g_ctx.Reset();
    g_dev.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
