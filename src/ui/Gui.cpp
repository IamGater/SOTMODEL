// Win32 window + D3D11 swap chain + Dear ImGui host for the FModel-style UI.
#include "ui/App.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "export/Png.h"
#include "ui/Icons.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <functional>
#include <dxgi1_2.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ImFont* g_iconFont = nullptr;
ImFont* g_iconFillFont = nullptr;

namespace {
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain1> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
UINT g_resizeW = 0, g_resizeH = 0;
bool g_minimized = false;
std::function<void(bool)> g_frame;   // renders one frame; called from WM_SIZE so resizing redraws live instead of stretching
bool g_inFrame = false;

void CreateRtv() {
    ComPtr<ID3D11Texture2D> back;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
}

void ResizeBuffers() {
    if (!g_swap || !g_resizeW || !g_resizeH) return;
    g_rtv.Reset();
    g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
    g_resizeW = g_resizeH = 0;
    CreateRtv();
}

bool CreateSwapChain(HWND hwnd, ComPtr<IDXGISwapChain1>& swap) {
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
    if (FAILED(factory->CreateSwapChainForHwnd(g_dev.Get(), hwnd, &sd, nullptr, nullptr, &swap))) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        if (FAILED(factory->CreateSwapChainForHwnd(g_dev.Get(), hwnd, &sd, nullptr, nullptr, &swap))) return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool CreateDevice(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) return false;
    if (!CreateSwapChain(hwnd, g_swap)) return false;
    CreateRtv();
    return true;
}

// Separate 3D viewer window (App::PopoutDoc): own swap chain and ImGui context sharing the main font atlas, rendered after the
// main window each frame on the same device.
struct Popout {
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ImGuiContext* ctx = nullptr;
    UINT resizeW = 0, resizeH = 0;
    bool minimized = false, closed = false;
    std::string title;
    RECT rect = {};   // last placement, reused when the window is opened again
} g_pop;
ImGuiContext* g_mainCtx = nullptr;

void PopoutResize() {
    if (!g_pop.swap || !g_pop.resizeW || !g_pop.resizeH) return;
    g_pop.rtv.Reset();
    g_pop.swap->ResizeBuffers(0, g_pop.resizeW, g_pop.resizeH, DXGI_FORMAT_UNKNOWN, 0);
    g_pop.resizeW = g_pop.resizeH = 0;
    ComPtr<ID3D11Texture2D> back;
    g_pop.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back.Get(), nullptr, &g_pop.rtv);
}

LRESULT WINAPI PopoutProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_pop.ctx) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_pop.ctx);
        LRESULT r = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        ImGui::SetCurrentContext(prev);
        if (r) return r;
    }
    switch (msg) {
    case WM_SIZE:
        g_pop.minimized = wParam == SIZE_MINIMIZED;
        if (!g_pop.minimized) {
            g_pop.resizeW = LOWORD(lParam);
            g_pop.resizeH = HIWORD(lParam);
            if (g_frame && !g_inFrame && g_pop.ctx) g_frame(true);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lParam)->ptMinTrackSize = {360, 260};
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        g_pop.closed = true;   // the main loop tells the App and destroys the window
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ApplyTheme(float scale);

void DestroyPopout() {
    if (!g_pop.hwnd) return;
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (GetWindowPlacement(g_pop.hwnd, &wp)) g_pop.rect = wp.rcNormalPosition;
    ImGui::SetCurrentContext(g_pop.ctx);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_pop.ctx);
    ImGui::SetCurrentContext(g_mainCtx);
    g_pop.ctx = nullptr;
    g_pop.rtv.Reset();
    g_pop.swap.Reset();
    DestroyWindow(g_pop.hwnd);
    g_pop.hwnd = nullptr;
    g_pop.minimized = g_pop.closed = false;
}

bool CreatePopout(HINSTANCE inst, HICON icon, float scale, const std::string& title) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, PopoutProc, 0, 0, inst, icon, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"SOTModelViewer", icon};
        registered = RegisterClassExW(&wc) != 0;
    }
    RECT r = g_pop.rect;
    bool placed = r.right > r.left && r.bottom > r.top;
    g_pop.hwnd = CreateWindowExW(0, L"SOTModelViewer", Widen(title).c_str(), WS_OVERLAPPEDWINDOW, placed ? r.left : CW_USEDEFAULT, placed ? r.top : CW_USEDEFAULT,
                                 placed ? r.right - r.left : (int)(1280 * scale), placed ? r.bottom - r.top : (int)(820 * scale), nullptr, nullptr, inst, nullptr);
    if (!g_pop.hwnd) return false;
    g_pop.title = title;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_pop.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (!CreateSwapChain(g_pop.hwnd, g_pop.swap)) {
        DestroyWindow(g_pop.hwnd);
        g_pop.hwnd = nullptr;
        return false;
    }
    RECT cr;
    GetClientRect(g_pop.hwnd, &cr);
    g_pop.resizeW = cr.right;
    g_pop.resizeH = cr.bottom;
    PopoutResize();
    g_pop.ctx = ImGui::CreateContext(ImGui::GetIO().Fonts);
    ImGui::SetCurrentContext(g_pop.ctx);
    ImGui::GetIO().IniFilename = nullptr;
    ApplyTheme(scale);
    ImGui_ImplWin32_Init(g_pop.hwnd);
    ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());
    ImGui::SetCurrentContext(g_mainCtx);
    ShowWindow(g_pop.hwnd, SW_SHOWNORMAL);
    return true;
}

// Opens / closes / retitles the separate viewer window to match the App.
void SyncPopout(App& app, HINSTANCE inst, HICON icon, float scale) {
    if (g_pop.closed) {
        app.ClosePopout();
        g_pop.closed = false;
    }
    if (!app.PopoutDoc()) {
        DestroyPopout();
        return;
    }
    std::string title = app.PopoutTitle();
    if (!g_pop.hwnd && !CreatePopout(inst, icon, scale, title)) {
        app.ClosePopout();
        return;
    }
    if (title != g_pop.title) {
        g_pop.title = title;
        SetWindowTextW(g_pop.hwnd, Widen(title).c_str());
    }
    if (app.TakePopoutRaise()) {   // another tab's view: bring the window up without stealing the keyboard focus
        if (IsIconic(g_pop.hwnd)) ShowWindow(g_pop.hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_pop.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (app.TakePopoutFocus()) {
        if (IsIconic(g_pop.hwnd)) ShowWindow(g_pop.hwnd, SW_RESTORE);
        SetForegroundWindow(g_pop.hwnd);
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        g_minimized = wParam == SIZE_MINIMIZED;
        if (!g_minimized) {
            g_resizeW = LOWORD(lParam);
            g_resizeH = HIWORD(lParam);
            if (g_frame && !g_inFrame) {
                ResizeBuffers();
                g_frame(true);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lParam)->ptMinTrackSize = {640, 420};
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
    s.FrameRounding = 0;
    s.PopupRounding = 3;
    s.ScrollbarRounding = 0;
    s.GrabRounding = 2;
    s.GrabMinSize = 5;   // a thin handle: slider captions ("speed 1.00x") stay readable under it
    s.TabRounding = 0;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(7, 5);
    s.ScrollbarSize = 10;
    s.IndentSpacing = 16;
    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    // FModel-like dark theme (AdonisUI dark layers): flat grey-violet panels, #206BD4 accent
    c[ImGuiCol_Text] = rgb(240, 240, 240);
    c[ImGuiCol_TextDisabled] = rgb(150, 151, 165);
    c[ImGuiCol_WindowBg] = rgb(38, 38, 48);
    c[ImGuiCol_ChildBg] = rgb(38, 38, 48);
    c[ImGuiCol_PopupBg] = rgb(46, 47, 58, 0.99f);
    c[ImGuiCol_Border] = rgb(74, 74, 94);
    c[ImGuiCol_FrameBg] = rgb(50, 51, 65);
    c[ImGuiCol_FrameBgHovered] = rgb(59, 61, 76);
    c[ImGuiCol_FrameBgActive] = rgb(64, 66, 82);
    c[ImGuiCol_TitleBg] = rgb(42, 43, 52);
    c[ImGuiCol_TitleBgActive] = rgb(42, 43, 52);
    c[ImGuiCol_MenuBarBg] = rgb(42, 43, 52);
    c[ImGuiCol_ScrollbarBg] = rgb(38, 38, 48, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(74, 75, 92);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(92, 93, 112);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(32, 107, 212);
    c[ImGuiCol_CheckMark] = rgb(32, 107, 212);
    c[ImGuiCol_SliderGrab] = rgb(32, 107, 212);
    c[ImGuiCol_SliderGrabActive] = rgb(60, 135, 240);
    c[ImGuiCol_Button] = rgb(50, 51, 65);
    c[ImGuiCol_ButtonHovered] = rgb(62, 64, 80);
    c[ImGuiCol_ButtonActive] = rgb(32, 107, 212);
    c[ImGuiCol_Header] = rgb(32, 107, 212);
    c[ImGuiCol_HeaderHovered] = rgb(54, 56, 70);
    c[ImGuiCol_HeaderActive] = rgb(32, 107, 212);
    c[ImGuiCol_Separator] = rgb(74, 74, 94);
    c[ImGuiCol_SeparatorHovered] = rgb(32, 107, 212);
    c[ImGuiCol_SeparatorActive] = rgb(32, 107, 212);
    c[ImGuiCol_Tab] = rgb(38, 38, 48);
    c[ImGuiCol_TabHovered] = rgb(54, 56, 70);
    c[ImGuiCol_TabSelected] = rgb(59, 61, 74);
    c[ImGuiCol_TabSelectedOverline] = rgb(32, 107, 212);
    c[ImGuiCol_TabDimmed] = rgb(38, 38, 48);
    c[ImGuiCol_TabDimmedSelected] = rgb(52, 54, 66);
    c[ImGuiCol_TableHeaderBg] = rgb(42, 43, 52);
    c[ImGuiCol_TableBorderStrong] = rgb(42, 43, 52);
    c[ImGuiCol_TableBorderLight] = rgb(50, 51, 62);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.02f);
    c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg] = rgb(32, 107, 212, 0.45f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.5f);
    s.ScaleAllSizes(scale);
}
}

// Saves the current back buffer (after ImGui rendered) as PNG: used by --screenshot for automated UI checks.
static bool SaveBackBuffer(IDXGISwapChain1* swap, const std::wstring& path) {
    ComPtr<ID3D11Texture2D> back, staging;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
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
    int forceW = 0, forceH = 0, startTab = -1;
    std::string startFolder;
    bool popout = false;
    float pickU = -1, pickV = -1;
    std::vector<std::string> attach;
    bool graph = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--screenshot" && i + 1 < args.size()) screenshot = Widen(args[++i]);
        else if (args[i] == "--size" && i + 1 < args.size()) sscanf(args[++i].c_str(), "%dx%d", &forceW, &forceH);
        else if (args[i] == "--tab" && i + 1 < args.size()) {   // a number or archives / folders / packages
            const std::string& t = args[++i];
            startTab = t == "archives" ? 0 : t == "folders" ? 1 : t == "packages" ? 2 : atoi(t.c_str());
        }
        else if (args[i] == "--folder" && i + 1 < args.size()) startFolder = args[++i];
        else if (args[i] == "--delay" && i + 1 < args.size()) delay = atof(args[++i].c_str());
        else if (args[i] == "--popout") popout = true;
        else if (args[i] == "--graph") graph = true;
        else if (args[i] == "--attach" && i + 1 < args.size()) attach.push_back(args[++i]);
        else if (args[i] == "--pick" && i + 1 < args.size()) sscanf(args[++i].c_str(), "%f,%f", &pickU, &pickV);
        else if (args[i] == "--shaders" && i + 1 < args.size()) open.push_back("shaders:" + args[++i]);
        else if (args[i] == "--material-hlsl" && i + 1 < args.size()) open.push_back("hlsl:" + args[++i]);
        else if (args[i] == "--references" && i + 1 < args.size()) open.push_back("refs:" + args[++i]);
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
    if (forceW > 0) {
        SetWindowPos(hwnd, nullptr, 20, 20, forceW, forceH, SWP_NOZORDER);
        ShowWindow(hwnd, SW_SHOWNORMAL);
    } else {
        ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    }
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    g_mainCtx = ImGui::CreateContext();
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
    // Chinese / Korean game text (localisation): merged fallbacks, glyphs are baked on demand
    for (const char* f : {"msyh.ttc", "malgun.ttf"}) {
        if (GetFileAttributesA((fonts + f).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), 16.f * scale, &merge);
    }
    ImFont* mono = io.Fonts->AddFontFromFileTTF((fonts + "consola.ttf").c_str(), 13.f * scale, &cfg);
    ImFont* bold = io.Fonts->AddFontFromFileTTF((fonts + "seguisb.ttf").c_str(), 16.f * scale, &cfg);   // Segoe UI Semibold: card titles
    // Phosphor icon fonts, embedded (Resources.rc); the atlas reads the resource memory in place
    for (int id : {101, 102}) {
        HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
        HGLOBAL rg = rs ? LoadResource(nullptr, rs) : nullptr;
        void* data = rg ? LockResource(rg) : nullptr;
        if (!data) continue;
        ImFontConfig ic;
        ic.FontDataOwnedByAtlas = false;
        ImFont* f = io.Fonts->AddFontFromMemoryTTF(data, (int)SizeofResource(nullptr, rs), 16.f * scale, &ic);
        (id == 101 ? g_iconFont : g_iconFillFont) = f;
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());

    {
        App app;
        app.monoFont = mono ? mono : io.Fonts->Fonts[0];
        app.boldFont = bold ? bold : io.Fonts->Fonts[0];
        app.openOnStart = open;
        app.popoutOnStart = popout;
        app.pickU = pickU;
        app.pickV = pickV;
        app.attachOnStart = attach;
        app.graphOnStart = graph;
        app.startTab = startTab;
        app.startFolder = startFolder;
        if (!app.Init(hwnd, g_dev.Get(), g_ctx.Get())) {
            MessageBoxW(hwnd, L"Renderer initialisation failed (see log).", L"SOTModel", MB_ICONERROR);
        }
        bool done = false;
        int settled = 0;
        g_frame =[&](bool present) {
            if (g_inFrame) return;
            g_inFrame = true;
            ImGui::SetCurrentContext(g_mainCtx);
            ResizeBuffers();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.Frame();
            ImGui::Render();
            const float clear[4] = {0.145f, 0.153f, 0.2f, 1};
            ID3D11RenderTargetView* rtv = g_rtv.Get();
            g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            g_ctx->ClearRenderTargetView(rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (present) g_swap->Present(0, 0);
            // the separate viewer window: after the main window, so the viewer's offscreen target is free to reuse
            if (g_pop.ctx && !g_pop.minimized) {
                PopoutResize();
                ImGui::SetCurrentContext(g_pop.ctx);
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                app.FramePopout();
                ImGui::Render();
                rtv = g_pop.rtv.Get();
                g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
                g_ctx->ClearRenderTargetView(rtv, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                ImGui::SetCurrentContext(g_mainCtx);
                g_pop.swap->Present(g_minimized ? 1 : 0, 0);   // the main window's present paces frames unless it is minimized
            }
            g_inFrame = false;
        };
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done || app.WantsQuit()) break;
            SyncPopout(app, inst, wc.hIcon, scale);
            if (g_minimized && !g_pop.hwnd) { Sleep(50); continue; }
            // idle: sleep until input or a worker posts a result (keeps CPU/GPU use near zero)
            if (!app.Busy() && screenshot.empty()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
            g_frame(false);
            // a few settled frames first: tab bars lay out their tabs one frame after they appear
            settled = app.Loading() ? 0 : settled + 1;
            if (!screenshot.empty() && (GetTickCount64() - start) / 1000.0 >= delay && settled >= 3) {
                SaveBackBuffer(g_swap.Get(), screenshot);
                if (g_pop.swap) {   // the separate viewer window next to it: <name>.viewer.png
                    size_t dot = screenshot.find_last_of(L'.');
                    SaveBackBuffer(g_pop.swap.Get(), (dot == std::wstring::npos ? screenshot : screenshot.substr(0, dot)) + L".viewer.png");
                }
                break;
            }
            if (g_minimized) continue;
            HRESULT hr = g_swap->Present(1, 0);
            if (hr == DXGI_STATUS_OCCLUDED && !g_pop.hwnd) Sleep(16);
        }
        g_frame = nullptr;   // it references `app`: WM_SIZE during shutdown must not render with a destroyed App
        DestroyPopout();
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
