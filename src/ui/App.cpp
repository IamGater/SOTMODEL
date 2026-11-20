#include "ui/App.h"
#include "ue/AssetJson.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <array>
#include <chrono>

// ---------------------------------------------------------------- jobs

JobPool::JobPool(int threads) {
    for (int i = 0; i < threads; i++)
        m_threads.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lk(m_mutex);
                    m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
                    if (m_stop && m_queue.empty()) return;
                    job = std::move(m_queue.front());
                    m_queue.pop_front();
                }
                try {
                    job();
                } catch (const std::exception& e) {
                    LOGE("job failed: %s", e.what());
                }
                if (--m_pending == 0) {
                    std::lock_guard lk(m_mutex);
                    m_idle.notify_all();
                }
            }
        });
}

JobPool::~JobPool() {
    {
        std::lock_guard lk(m_mutex);
        m_stop = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    for (auto& t : m_threads) t.join();
}

void JobPool::Push(std::function<void()> job) {
    m_pending++;
    {
        std::lock_guard lk(m_mutex);
        m_queue.push_back(std::move(job));
    }
    m_cv.notify_one();
}

void JobPool::WaitIdle() {
    std::unique_lock lk(m_mutex);
    m_idle.wait(lk, [&] { return m_pending == 0; });
}

// ---------------------------------------------------------------- helpers

static std::string HumanSize(u64 b) {
    if (b < 1024) return Fmt("%llu B", (unsigned long long)b);
    if (b < (1ull << 20)) return Fmt("%.1f KB", b / 1024.0);
    if (b < (1ull << 30)) return Fmt("%.1f MB", b / 1048576.0);
    return Fmt("%.2f GB", b / 1073741824.0);
}

static void CopyToClipboard(const std::string& s) { ImGui::SetClipboardText(s.c_str()); }

static void DrawFolderIcon(ImDrawList* dl, ImVec2 c) {
    float u = ImGui::GetFontSize() / 15.f;
    ImU32 col = IM_COL32(232, 190, 90, 255);
    dl->AddRectFilled(ImVec2(c.x, c.y - 5 * u), ImVec2(c.x + 6 * u, c.y - 2 * u), col, 1.5f * u);
    dl->AddRectFilled(ImVec2(c.x, c.y - 3.5f * u), ImVec2(c.x + 14 * u, c.y + 5 * u), col, 1.5f * u);
}

// Toolbar flow layout: continues on the same line when the next item (width w) still fits, otherwise wraps (keeps toolbars usable in narrow windows)
static void FlowNext(float w) {
    float x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (x + w <= right) ImGui::SameLine();
}
// Compact viewer toolbar: tight frames, no borders. Wrap the toolbar in Begin/EndToolbar.
static void BeginToolbar() {
    float u = ImGui::GetFontSize() / 16.f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * u, 3 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * u, 4 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
}
static void EndToolbar() { ImGui::PopStyleVar(3); }

// On/off chip: a small button that is filled with the accent colour while enabled (replaces bulky checkboxes in toolbars)
static bool FlowToggle(const char* label, bool* v, bool first = false) {
    float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
    if (!first) FlowNext(w);
    bool on = *v;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.12f, 0.42f, 0.84f, 0.85f) : ImVec4(0.18f, 0.19f, 0.26f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.16f, 0.48f, 0.92f, 1) : ImVec4(0.23f, 0.24f, 0.32f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    if (pressed) *v = !*v;
    return pressed;
}
static bool FlowButton(const char* label) {
    FlowNext(ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2);
    return ImGui::Button(label);
}

static std::string GamePathToPackage(std::string_view p) {
    // Athena/Content/X/y.uasset -> /Game/X/y
    std::string s(p);
    if (IStartsWith(s, "Athena/Content/")) s = "/Game/" + s.substr(15);
    size_t dot = s.rfind('.');
    if (dot != std::string::npos && s.find('/', dot) == std::string::npos) s.resize(dot);
    return s;
}

static bool IsPackageFile(std::string_view p) { return IEndsWith(p, ".uasset") || IEndsWith(p, ".umap"); }

// UV sphere used to preview materials
static MeshLod MakeSphere(float radius, int seg, int rings) {
    MeshLod l;
    l.numUVs = 1;
    for (int r = 0; r <= rings; r++) {
        float v = (float)r / rings, th = v * kPi;
        for (int s = 0; s <= seg; s++) {
            float u = (float)s / seg, ph = u * 2 * kPi;
            Vec3 n{sinf(th) * cosf(ph), sinf(th) * sinf(ph), cosf(th)};
            l.positions.push_back(n * radius);
            l.normals.push_back(n);
            l.tangents.push_back({-sinf(ph), cosf(ph), 0, 1});
            l.uvs.push_back({u * 2, v});
        }
    }
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < seg; s++) {
            u32 a = r * (seg + 1) + s, b = a + seg + 1;
            l.indices.insert(l.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    l.sections.push_back({0, 0, (u32)(l.indices.size() / 3), 0, (u32)l.positions.size() - 1, true, 0});
    return l;
}

// ---------------------------------------------------------------- app

App::App() { m_settings.Load(); }

App::~App() {
    m_exportCancel = true;
    m_jobs.reset();   // joins workers before the renderer / api go away
    m_docs.clear();
    m_assets.reset();
}

bool App::Busy() const {
    if (m_busyFrames > 0 || m_mountState == MountState::Mounting || m_exporting) return true;
    if (m_jobs && m_jobs->Pending() > 0) return true;
    for (auto& d : m_docs) if (d->loading) return true;
    return false;
}

bool App::Loading() const {
    if (m_mountState == MountState::Mounting || (m_jobs && m_jobs->Pending() > 0)) return true;
    for (auto& d : m_docs) if (d->loading) return true;
    return false;
}

void App::Post(std::function<void()> fn) {
    {
        std::lock_guard lk(m_postMutex);
        m_posted.push_back(std::move(fn));
    }
    if (m_hwnd) PostMessageW((HWND)m_hwnd, WM_NULL, 0, 0);
}

bool App::Init(void* hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    m_hwnd = hwnd;
    m_api = std::make_unique<Api>(m_settings);
    m_jobs = std::make_unique<JobPool>(3);
    m_renderer = std::make_unique<Renderer>();
    std::string err;
    if (!m_renderer->Init(dev, ctx, &err)) {
        LOGE("renderer: %s", err.c_str());
        return false;
    }
    MeshLod sphere = MakeSphere(50, 64, 32);
    m_sphere = m_renderer->CreateMesh(sphere, {-50, -50, -50}, {50, 50, 50});
    // sea level plane for map views (unit quad, scaled when drawn)
    MeshLod quad;
    quad.positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    quad.normals.assign(4, Vec3{0, 0, 1});
    quad.indices = {0, 2, 1, 0, 3, 2};
    m_water = m_renderer->CreateMesh(quad, {-1, -1, 0}, {1, 1, 0});
    if (m_water) {
        auto wm = std::make_shared<GpuMaterial>();
        wm->color[0] = 0.02f; wm->color[1] = 0.11f; wm->color[2] = 0.16f;
        wm->twoSided = true;
        m_water->materials = {wm};
    }
    LOGI("settings: %s", Narrow(Settings::Path()).c_str());
    if (startTab >= 0) m_leftTab = startTab;
    StartMount();
    return true;
}

void App::StartMount() {
    m_mountState = MountState::Mounting;
    m_mountError.clear();
    m_jobs->Push([this] {
        auto t0 = std::chrono::steady_clock::now();
        std::string err;
        bool ok = m_api->EnsureMounted(&err);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Post([this, ok, err, secs] {
            if (!ok) {
                m_mountState = MountState::Failed;
                m_mountError = err;
                LOGE("mount failed: %s", err.c_str());
                m_showSettings = true;
                return;
            }
            m_mountState = MountState::Ready;
            m_mountSeconds = secs;
            m_assets = std::make_unique<GpuAssets>(*m_renderer, *m_api->cache);
            m_assets->SetMaxTextureSize(std::min(m_settings.previewMaxTexture, 1024));
            m_folder = 0;
            if (!startFolder.empty()) {   // walk the tree along the path components
                auto& folders = m_api->vfs.Folders();
                std::string_view p = startFolder;
                size_t pos = 0;
                while (pos < p.size()) {
                    size_t sl = std::min(p.find('/', pos), p.size());
                    std::string_view part = p.substr(pos, sl - pos);
                    u32 next = UINT32_MAX;
                    for (u32 c : folders[m_folder].folders) if (IEquals(folders[c].name, part)) { next = c; break; }
                    if (next == UINT32_MAX) break;
                    m_folder = next;
                    pos = sl + 1;
                }
            }
            for (auto& p : openOnStart) {
                i64 e = m_api->ResolveFile(p);
                if (e >= 0) OpenFile((u32)e);
                else LOGW("not found: %s", p.c_str());
            }
            openOnStart.clear();
        });
    });
}

ExportOptions App::MakeExportOptions() const {
    ExportOptions o;
    o.outputDir = m_settings.outputDir;
    o.meshFormat = m_settings.meshFormat;
    o.textureFormat = m_settings.textureFormat;
    o.keepDirectories = m_settings.exportKeepDirectories;
    o.json = false;
    return o;
}

void App::Frame() {
    {
        std::vector<std::function<void()>> posted;
        {
            std::lock_guard lk(m_postMutex);
            posted.swap(m_posted);
        }
        for (auto& f : posted) f();
        if (!posted.empty()) m_busyFrames = 3;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() ||
        io.InputQueueCharacters.Size)
        m_busyFrames = 3;
    else if (m_busyFrames > 0)
        m_busyFrames--;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) { m_focusSearch = true; m_leftTab = 2; }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W) && m_activeDoc) CloseDocument(m_activeDoc);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    DrawMenu();

    float statusH = ImGui::GetFrameHeight() + 2;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float mainH = avail.y - statusH - ImGui::GetStyle().ItemSpacing.y;   // the status bar ends flush with the window bottom
    float cellH = mainH - ImGui::GetStyle().CellPadding.y * 2;

    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, mainH))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.37f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.63f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        DrawLeftPanel(cellH);
        ImGui::TableNextColumn();
        float split = std::round(ImGui::GetFontSize() * 0.9f);   // FModel-style strip between the tabs and the log
        if (m_logFrac <= 0) m_logFrac = 0.2f;
        float logH = m_showLog ? std::clamp(m_logFrac * cellH, 48.f, std::max(48.f, cellH * 0.6f)) : 0;
        // the splitter and the log are two more items: account for the spacing before each so the column does not overflow
        float logBlock = m_showLog ? logH + split + ImGui::GetStyle().ItemSpacing.y * 2 : 0;
        ImGui::BeginChild("##docs", ImVec2(0, std::max(1.f, cellH - logBlock)), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawDocuments();
        ImGui::EndChild();
        if (m_showLog) {
            ImGui::InvisibleButton("##logsplit", ImVec2(-FLT_MIN, split));
            if (ImGui::IsItemActive()) m_logFrac = std::clamp((logH - ImGui::GetIO().MouseDelta.y) / std::max(1.f, cellH), 0.08f, 0.6f);
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            sdl->AddRectFilled(a, b, hot ? IM_COL32(52, 54, 66, 255) : IM_COL32(42, 43, 52, 255));
            // up / down arrows: the strip resizes the log
            float k = (b.y - a.y) * 0.32f;
            ImU32 ac = IM_COL32(200, 201, 212, 255);
            sdl->AddTriangleFilled(ImVec2(c.x - 4 - k * 0.6f, c.y - 1), ImVec2(c.x - 4 + k * 0.6f, c.y - 1), ImVec2(c.x - 4, c.y - 1 - k), ac);
            sdl->AddTriangleFilled(ImVec2(c.x + 4 - k * 0.6f, c.y + 1), ImVec2(c.x + 4 + k * 0.6f, c.y + 1), ImVec2(c.x + 4, c.y + 1 + k), ac);
            DrawLog(logH);
        }
        ImGui::PopStyleVar();
        ImGui::EndTable();
    }
    DrawStatusBar();
    ImGui::End();

    if (m_showSettings) DrawSettings();
    DrawExportProgress();
}

void App::ReloadArchives() {
    if (m_mountState == MountState::Mounting) return;
    m_exportCancel = true;
    CloseDocument(0);   // cancels map loads so WaitIdle does not block on them
    m_jobs->WaitIdle();
    m_assets.reset();
    m_api->Unmount();
    m_searchResults.clear();
    m_lastSearch.clear();
    m_pakSizes.clear();
    m_selPak = -1;
    m_folder = 0;
    StartMount();
}

void App::DrawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("Directory")) {
        if (ImGui::MenuItem("Reload archives", nullptr, false, m_mountState != MountState::Mounting)) ReloadArchives();
        if (ImGui::MenuItem("Open output folder")) {
            CreateDirectories(m_settings.outputDir);
            ShellExecuteW(nullptr, L"open", m_settings.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Packages")) {
        bool ready = m_mountState == MountState::Ready;
        if (ImGui::MenuItem("Export current folder (models / textures)", nullptr, false, ready)) ExportFiles(FolderFilesRecursive(m_folder), 0);
        if (ImGui::MenuItem("Save current folder properties (.json)", nullptr, false, ready)) ExportFiles(FolderFilesRecursive(m_folder), 2);
        if (ImGui::MenuItem("Search", "Ctrl+F", false, ready)) { m_focusSearch = true; m_leftTab = 2; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Views")) {
        ImGui::MenuItem("Log", nullptr, &m_showLog);
        if (ImGui::MenuItem("Close all tabs")) CloseDocument(0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("General...")) m_showSettings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("AI / automation: run SOTModel.exe --mcp (MCP server) or --cli tools", nullptr, false, false);
        ImGui::MenuItem("Ctrl+F search, Ctrl+W close tab, F frame model", nullptr, false, false);
        ImGui::MenuItem("Viewport: LMB orbit, RMB look + WASD/QE fly, MMB pan, wheel zoom", nullptr, false, false);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

std::vector<u32> App::FolderFilesRecursive(u32 folder) const {
    std::vector<u32> out;
    auto& f = m_api->vfs.Folders();
    std::vector<u32> stack{folder};
    while (!stack.empty()) {
        u32 i = stack.back();
        stack.pop_back();
        out.insert(out.end(), f[i].files.begin(), f[i].files.end());
        stack.insert(stack.end(), f[i].folders.rbegin(), f[i].folders.rend());
    }
    return out;
}

void App::DrawFolderTree(u32 idx) {
    auto& folders = m_api->vfs.Folders();
    const VfsFolder& f = folders[idx];
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth;
    if (f.folders.empty()) fl |= ImGuiTreeNodeFlags_Leaf;
    if (m_folder == idx) fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID((int)idx);
    // reveal the current folder when it was changed elsewhere (explorer, breadcrumb, "Show in folder"): open its ancestors
    bool reveal = m_treeFolder != m_folder;
    if (reveal && idx != m_folder) {
        for (u32 p = folders[m_folder].parent; p != UINT32_MAX && p != 0; p = folders[p].parent)
            if (p == idx) { ImGui::SetNextItemOpen(true); break; }
    }
    // the item rect spans the full row (SpanFullWidth): take the indented x from the cursor before the node
    float nodeX = ImGui::GetCursorScreenPos().x;
    bool open = ImGui::TreeNodeEx("##n", fl, "      %s", f.name.c_str());
    {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        DrawFolderIcon(ImGui::GetWindowDrawList(), ImVec2(nodeX + ImGui::GetTreeNodeToLabelSpacing() - 2, (mn.y + mx.y) * 0.5f));
    }
    if (reveal && idx == m_folder && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.3f);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_folder = idx;
        m_search[0] = 0;
        m_selectExplorer = true;   // show the folder's contents even when a document tab is in front
    }
    if (ImGui::BeginPopupContextItem()) {
        ImGui::TextDisabled("%s (%u files)", f.name.c_str(), f.totalFiles);
        ImGui::Separator();
        if (ImGui::MenuItem("Export folder (models / textures)")) ExportFiles(FolderFilesRecursive(idx), 0);
        if (ImGui::MenuItem("Save folder properties (.json)")) ExportFiles(FolderFilesRecursive(idx), 2);
        if (ImGui::MenuItem("Export folder raw (.uasset)")) ExportFiles(FolderFilesRecursive(idx), 1);
        ImGui::EndPopup();
    }
    if (open) {
        for (u32 c : f.folders) DrawFolderTree(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

static void SeparatorLabel(const char* label) {
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.97f, 1));
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// value on the left, caption right-aligned (FModel's INFORMATION block)
static void InfoRow(const std::string& value, const char* caption) {
    float w = ImGui::GetContentRegionAvail().x;
    float x0 = ImGui::GetCursorPosX();
    ImVec2 cs = ImGui::CalcTextSize(caption);
    ImGui::TextUnformatted(value.c_str());
    ImGui::SameLine(x0 + std::max(w - cs.x, ImGui::CalcTextSize(value.c_str()).x + 12));
    ImGui::TextUnformatted(caption);
}

void App::DrawArchives() {
    if (m_mountState != MountState::Ready) {
        ImGui::TextDisabled(m_mountState == MountState::Mounting ? "Loading archives..." : "No archives loaded");
        return;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Loading Mode");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##loadmode", "All")) {
        ImGui::Selectable("All", true);
        ImGui::EndCombo();
    }
    if (ImGui::Button("Load", ImVec2(-FLT_MIN, 0))) ReloadArchives();
    SeparatorLabel("GAME ARCHIVES");
    auto& paks = m_api->vfs.Paks();
    if (m_pakSizes.size() != paks.size()) m_pakSizes.assign(paks.size(), UINT64_MAX);
    if (ImGui::BeginChild("##paklist")) {
        ImGuiListClipper clip;
        clip.Begin((int)paks.size(), ImGui::GetTextLineHeightWithSpacing() + 4);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                if (m_pakSizes[i] == UINT64_MAX) {
                    WIN32_FILE_ATTRIBUTE_DATA fad = {};
                    m_pakSizes[i] = GetFileAttributesExW(paks[i]->path.c_str(), GetFileExInfoStandard, &fad) ? ((u64)fad.nFileSizeHigh << 32 | fad.nFileSizeLow) : 0;
                }
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float lh = ImGui::GetTextLineHeight();
                if (ImGui::Selectable("##pak", m_selPak == i, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, lh + 4))) m_selPak = i;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                // small green "file" icon
                dl->AddRectFilled(ImVec2(p.x + 6, p.y + 2), ImVec2(p.x + 6 + lh * 0.8f, p.y + 2 + lh), IM_COL32(90, 190, 90, 255), 2);
                dl->AddRectFilled(ImVec2(p.x + 6 + lh * 0.2f, p.y + 2 + lh * 0.3f), ImVec2(p.x + 6 + lh * 0.6f, p.y + 2 + lh * 0.7f), IM_COL32(230, 245, 230, 255));
                float rowW = ImGui::GetContentRegionAvail().x, szW = ImGui::CalcTextSize("0000.0 MB").x;
                dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + rowW - szW - 12, p.y + lh + 4), true);
                dl->AddText(ImVec2(p.x + 8 + lh, p.y + 2), ImGui::GetColorU32(ImGuiCol_Text), paks[i]->name.c_str());
                dl->PopClipRect();
                std::string sz = HumanSize(m_pakSizes[i]);
                dl->AddText(ImVec2(p.x + rowW - ImGui::CalcTextSize(sz.c_str()).x - 4, p.y + 2), ImGui::GetColorU32(ImGuiCol_Text), sz.c_str());
                ImGui::PopID();
            }
    }
    ImGui::EndChild();
}

void App::DrawLeftPanel(float height) {
    ImGui::BeginChild("##left", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    const VfsFolder* cur = nullptr;
    size_t packages = 0;
    if (m_mountState == MountState::Ready) {
        cur = &m_api->vfs.Folders()[m_folder];
        packages = cur->files.size();
    }
    // three equal tabs
    {
        std::string labels[3] = {"Archives", "Folders", Fmt("%zu Packages", packages)};
        float w = ImGui::GetContentRegionAvail().x / 3.f, h = ImGui::GetFrameHeight() + 6;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGuiStyle& st = ImGui::GetStyle();
        for (int i = 0; i < 3; i++) {
            ImVec2 p(p0.x + w * i, p0.y);
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##t", ImVec2(w, h))) m_leftTab = i;
            ImGui::PopID();
            bool sel = m_leftTab == i;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), sel ? IM_COL32(59, 61, 74, 255) : ImGui::IsItemHovered() ? IM_COL32(48, 49, 60, 255) : IM_COL32(38, 38, 48, 255));
            ImVec2 ts = ImGui::CalcTextSize(labels[i].c_str());
            dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), labels[i].c_str());
        }
        // the strip under the tab row continues the selected tab's colour (FModel's tab control border)
        dl->AddRectFilled(ImVec2(p0.x, p0.y + h), ImVec2(p0.x + w * 3, p0.y + h + 2), IM_COL32(59, 61, 74, 255));
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 2 + st.ItemSpacing.y));
    }
    bool showInfo = height > ImGui::GetFontSize() * 28;
    float infoH = showInfo ? ImGui::GetTextLineHeightWithSpacing() * 5 + ImGui::GetFrameHeight() + 12 : 0;
    ImGui::BeginChild("##leftbody", ImVec2(0, ImGui::GetContentRegionAvail().y - infoH));
    if (m_leftTab == 0) {
        DrawArchives();
    } else if (m_leftTab == 1) {
        if (m_mountState == MountState::Ready) {
            auto& folders = m_api->vfs.Folders();
            if (!folders.empty()) {
                // header: summary of the current folder, "locate" and "collapse all" on the right
                float u = ImGui::GetFontSize() / 16.f, bs = ImGui::GetFrameHeight();
                ImDrawList* hdl = ImGui::GetWindowDrawList();
                ImVec2 hp = ImGui::GetCursorScreenPos();
                float hw = ImGui::GetContentRegionAvail().x;
                DrawFolderIcon(hdl, ImVec2(hp.x + 2 * u, hp.y + bs * 0.5f));
                ImGui::SetCursorScreenPos(ImVec2(hp.x + 22 * u, hp.y));
                ImGui::AlignTextToFramePadding();
                ImGui::PushClipRect(ImVec2(hp.x, hp.y), ImVec2(hp.x + hw - bs * 2 - 8 * u, hp.y + bs), true);
                ImGui::Text("'%s' has %zu folders and %zu packages", m_folder ? cur->name.c_str() : "Root", cur->folders.size(), cur->files.size());
                ImGui::PopClipRect();
                auto iconButton = [&](const char* id, float x, const char* tip) {
                    ImGui::SetCursorScreenPos(ImVec2(x, hp.y));
                    bool r = ImGui::InvisibleButton(id, ImVec2(bs, bs));
                    if (ImGui::IsItemHovered()) {
                        hdl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(59, 61, 74, 255), 3 * u);
                        ImGui::SetTooltip("%s", tip);
                    }
                    return r;
                };
                ImU32 ic = IM_COL32(220, 221, 230, 255);
                float bx = hp.x + hw - bs * 2 - 2 * u;
                if (iconButton("##locate", bx, "Show the current folder")) m_treeFolder = UINT32_MAX;
                {   // crosshair
                    ImVec2 c(bx + bs * 0.5f, hp.y + bs * 0.5f);
                    hdl->AddCircle(c, bs * 0.22f, ic, 0, 1.5f * u);
                    hdl->AddCircleFilled(c, 1.8f * u, ic);
                    for (int k = 0; k < 4; k++) {
                        ImVec2 d(k == 0 ? 1.f : k == 1 ? -1.f : 0.f, k == 2 ? 1.f : k == 3 ? -1.f : 0.f);
                        hdl->AddLine(ImVec2(c.x + d.x * bs * 0.22f, c.y + d.y * bs * 0.22f), ImVec2(c.x + d.x * bs * 0.36f, c.y + d.y * bs * 0.36f), ic, 1.5f * u);
                    }
                }
                if (iconButton("##collapse", bx + bs, "Collapse all")) m_collapseTree = true;
                {   // two chevrons pointing at each other
                    ImVec2 c(bx + bs * 1.5f, hp.y + bs * 0.5f);
                    float a = bs * 0.16f;
                    hdl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - a, c.y - a * 1.9f), ImVec2(c.x, c.y - a * 0.7f), ImVec2(c.x + a, c.y - a * 1.9f)}.data(), 3, ic, 0, 1.5f * u);
                    hdl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - a, c.y + a * 1.9f), ImVec2(c.x, c.y + a * 0.7f), ImVec2(c.x + a, c.y + a * 1.9f)}.data(), 3, ic, 0, 1.5f * u);
                }
                ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + bs + ImGui::GetStyle().ItemSpacing.y));
                ImGui::Separator();
                ImGui::BeginChild("##tree");
                if (m_collapseTree) {   // forget every node's open state
                    ImGui::GetStateStorage()->Clear();
                    m_collapseTree = false;
                }
                for (u32 c : folders[0].folders) DrawFolderTree(c);
                m_treeFolder = m_folder;
                ImGui::EndChild();
            }
        } else if (m_mountState == MountState::Mounting) {
            ImGui::TextDisabled("Loading archives...");
        } else {
            ImGui::TextWrapped("Archives are not loaded. Open Settings to choose the game folder and AES keys.");
            if (!m_mountError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", m_mountError.c_str());
            if (ImGui::Button("Settings...")) m_showSettings = true;
        }
    } else {
        DrawFileList();
    }
    ImGui::EndChild();
    if (showInfo) SeparatorLabel("INFORMATION");
    if (showInfo && m_mountState == MountState::Ready) {
        auto& paks = m_api->vfs.Paks();
        if (m_leftTab == 0 && m_selPak >= 0 && m_selPak < (int)paks.size()) {
            auto& pk = *paks[m_selPak];
            InfoRow(pk.mountPoint.empty() ? "/" : pk.mountPoint, "Mount Point");
            InfoRow(Fmt("%u", pk.fileCount), "File Count");
            InfoRow(pk.keyIndex >= 0 ? "True" : "False", "Is Encrypted");
            InfoRow(Fmt("%u", pk.version), "Archive Version");
        } else {
            InfoRow(Fmt("%zu", cur->files.size()), "Packages Count");
            InfoRow(Fmt("%zu", cur->folders.size()), "Folders Count");
            InfoRow(Fmt("%u", cur->totalFiles), "Total Files");
            InfoRow(Fmt("%zu", paks.size()), "Archives");
        }
    }
    ImGui::EndChild();
}

void App::UpdateSearch() {
    std::string q = m_search;
    if (q == m_lastSearch) return;
    m_lastSearch = q;
    m_searchResults.clear();
    m_searchTruncated = false;
    if (q.empty() || m_mountState != MountState::Ready) return;
    // space separated terms, all must match (case-insensitive); '*' / '?' switch to wildcard mode
    std::vector<std::string> terms;
    bool wildcard = q.find_first_of("*?") != std::string::npos;
    if (!wildcard) {
        size_t p = 0;
        std::string lq = ToLower(q);
        while (p < lq.size()) {
            size_t e = lq.find(' ', p);
            if (e == std::string::npos) e = lq.size();
            if (e > p) terms.push_back(lq.substr(p, e - p));
            p = e + 1;
        }
    }
    const Vfs& vfs = m_api->vfs;
    const size_t limit = 20000;
    std::string lower;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string_view path = vfs.Path(i);
        bool ok;
        if (wildcard) ok = WildcardMatch(q, path) || WildcardMatch(q, vfs.FileName(i));
        else {
            lower.assign(path);
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            ok = true;
            for (auto& t : terms) if (lower.find(t) == std::string::npos) { ok = false; break; }
        }
        if (ok) {
            if (m_searchResults.size() >= limit) { m_searchTruncated = true; break; }
            m_searchResults.push_back(i);
        }
    }
    std::sort(m_searchResults.begin(), m_searchResults.end(), [&](u32 a, u32 b) { return vfs.Path(a) < vfs.Path(b); });
}

void App::DrawFileMenu(u32 e, bool searching) {
    const Vfs& vfs = m_api->vfs;
    auto& folders = vfs.Folders();
    bool pkg = IsPackageFile(vfs.Path(e));
    m_selected = e;
    if (ImGui::MenuItem("Open", nullptr, false, pkg)) OpenFile(e);
    if (ImGui::MenuItem("Export (model / texture)", nullptr, false, pkg)) ExportFiles({e}, 0);
    if (ImGui::MenuItem("Save properties (.json)", nullptr, false, pkg)) ExportFiles({e}, 2);
    if (ImGui::MenuItem("Export raw")) ExportFiles({e}, 1);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy path")) CopyToClipboard(std::string(vfs.Path(e)));
    if (ImGui::MenuItem("Copy /Game path")) CopyToClipboard(GamePathToPackage(vfs.Path(e)));
    if (searching && ImGui::MenuItem("Show in folder")) {
        // walk the tree along the path components
        std::string_view p = vfs.Path(e);
        u32 cur = 0;
        size_t pos = 0;
        for (;;) {
            size_t sl = p.find('/', pos);
            if (sl == std::string_view::npos) break;
            std::string_view part = p.substr(pos, sl - pos);
            u32 next = UINT32_MAX;
            for (u32 c : folders[cur].folders) if (IEquals(folders[c].name, part)) { next = c; break; }
            if (next == UINT32_MAX) break;
            cur = next;
            pos = sl + 1;
        }
        m_folder = cur;
        m_search[0] = 0;
        m_selectExplorer = true;
    }
}

// Explorer in the style of FModel's tiled explorer: folder cards and package cards (own vector icons, no FModel artwork)
static ImU32 FileTint(std::string_view name) {
    if (IEndsWith(name, ".umap")) return IM_COL32(255, 160, 40, 255);
    if (IEndsWith(name, ".uasset")) return IM_COL32(40, 150, 255, 255);
    if (IEndsWith(name, ".ubulk") || IEndsWith(name, ".uexp")) return IM_COL32(160, 120, 235, 255);
    return IM_COL32(245, 215, 40, 255);
}

static ImU32 Shade(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x * k, v.y * k, v.z * k, v.w));
}

// Folder: back panel with a tab, lighter front panel, thin darker outline
static void DrawBigFolder(ImDrawList* dl, ImVec2 c, float size) {
    float w = size, h = size * 0.74f, r = size * 0.07f;
    ImVec2 p(c.x - w * 0.5f, c.y - h * 0.5f);
    ImU32 back = IM_COL32(222, 172, 96, 255), front = IM_COL32(242, 204, 140, 255), line = IM_COL32(200, 150, 80, 255);
    dl->AddRectFilled(p, ImVec2(p.x + w * 0.4f, p.y + h * 0.25f), back, r);
    dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.12f), ImVec2(p.x + w, p.y + h), back, r);
    dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.3f), ImVec2(p.x + w, p.y + h), front, r);
    dl->AddRect(ImVec2(p.x, p.y + h * 0.3f), ImVec2(p.x + w, p.y + h), line, r, 0, std::max(1.f, size / 64));
}

// Per type glyphs: map (.umap), cube (.uasset), sheet with lines (anything else)
static void DrawFileGlyph(ImDrawList* dl, ImVec2 c, float size, std::string_view name, ImU32 col, ImU32 bg) {
    float s = size;
    if (IEndsWith(name, ".umap")) {   // folded map: three slanted panels
        float w = s * 0.9f, h = s * 0.7f, x0 = c.x - w * 0.5f, y0 = c.y - h * 0.5f, pw = w / 3, k = h * 0.1f;
        for (int i = 0; i < 3; i++) {
            float xa = x0 + pw * i, xb = xa + pw;
            float ya = (i % 2) ? k : 0, yb = (i % 2) ? 0 : k;
            ImU32 pc = (i % 2) ? Shade(col, 0.8f) : col;
            dl->AddQuadFilled(ImVec2(xa, y0 + ya), ImVec2(xb, y0 + yb), ImVec2(xb, y0 + h + yb - k), ImVec2(xa, y0 + h + ya - k), pc);
        }
        dl->AddCircleFilled(ImVec2(x0 + pw * 1.5f, y0 + h * 0.42f), s * 0.08f, bg);
        return;
    }
    if (IEndsWith(name, ".uasset")) {   // isometric cube, three shaded faces
        float r = s * 0.45f;
        ImVec2 top(c.x, c.y - r), ul(c.x - r * 0.87f, c.y - r * 0.5f), ur(c.x + r * 0.87f, c.y - r * 0.5f), mid = c;
        ImVec2 bl(c.x - r * 0.87f, c.y + r * 0.5f), br(c.x + r * 0.87f, c.y + r * 0.5f), bot(c.x, c.y + r);
        dl->AddQuadFilled(top, ur, mid, ul, Shade(col, 1.15f));
        dl->AddQuadFilled(ul, mid, bot, bl, col);
        dl->AddQuadFilled(mid, ur, br, bot, Shade(col, 0.72f));
        return;
    }
    float w = s * 0.62f, h = s * 0.8f, fold = w * 0.3f;   // sheet with a folded corner and text lines
    ImVec2 p(c.x - w * 0.5f, c.y - h * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + w - fold, p.y + h), col, s * 0.04f);
    dl->AddRectFilled(ImVec2(p.x + w - fold - 1, p.y + fold), ImVec2(p.x + w, p.y + h), col, s * 0.04f);
    dl->AddTriangleFilled(ImVec2(p.x + w - fold, p.y), ImVec2(p.x + w, p.y + fold), ImVec2(p.x + w - fold, p.y + fold), Shade(col, 0.7f));
    for (int i = 0; i < 3; i++) {
        float ly = p.y + h * (0.45f + i * 0.15f);
        dl->AddRectFilled(ImVec2(p.x + w * 0.18f, ly), ImVec2(p.x + w * (i == 2 ? 0.55f : 0.82f), ly + s * 0.05f), bg);
    }
}

// Text helpers for cards (font + size pushed by the caller)
static void CardText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* s) { dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col, s); }
static void CardTextCentered(ImDrawList* dl, float x0, float x1, float y, ImU32 col, const std::string& s) {
    float w = ImGui::CalcTextSize(s.c_str()).x;
    if (w <= x1 - x0) { CardText(dl, ImVec2(x0 + (x1 - x0 - w) * 0.5f, y), col, s.c_str()); return; }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, ImVec2(x0, y), ImVec2(x1, y + ImGui::GetFontSize()), x1, s.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
}
// Up to two lines, broken anywhere (asset names rarely have spaces); the second line ends with "..." when it still does not fit
static void CardTextTwoLines(ImDrawList* dl, ImVec2 pos, float width, ImU32 col, const std::string& s) {
    size_t n = 0;
    while (n < s.size() && ImGui::CalcTextSize(s.c_str(), s.c_str() + n + 1).x <= width) n++;
    CardText(dl, pos, col, s.substr(0, n).c_str());
    if (n >= s.size()) return;
    std::string rest = s.substr(n);
    float y = pos.y + ImGui::GetFontSize() * 1.15f;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, ImVec2(pos.x, y), ImVec2(pos.x + width, y + ImGui::GetFontSize()), pos.x + width, rest.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
}

void App::DrawExplorer() {
    if (m_mountState != MountState::Ready) {
        ImGui::TextDisabled("%s", m_mountState == MountState::Mounting ? "Loading archives..." : "Archives are not loaded");
        return;
    }
    const Vfs& vfs = m_api->vfs;
    auto& folders = vfs.Folders();
    float u = ImGui::GetFontSize() / 16.f;
    float base = ImGui::GetStyle().FontSizeBase;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // search bar: magnifier inside on the left, clear button inside on the right, type filter next to it
    {
        float availX = ImGui::GetContentRegionAvail().x;
        float rightW = std::min(140 * u, availX * 0.3f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28 * u, 5 * u));
        ImGui::SetNextItemWidth(std::max(80 * u, availX - rightW - 8 * u));
        ImGui::SetNextItemAllowOverlap();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InputTextWithHint("##xsearch", "Search by name...", m_search, sizeof(m_search));
        ImGui::PopStyleVar();
        ImVec2 sz = ImGui::GetItemRectSize();
        float fh = sz.y;
        ImVec2 mc(p0.x + 13 * u, p0.y + fh * 0.5f - 1.5f * u);
        ImU32 ic = IM_COL32(225, 226, 235, 255);
        dl->AddCircle(mc, 5 * u, ic, 0, 1.6f * u);
        dl->AddLine(ImVec2(mc.x + 3.6f * u, mc.y + 3.6f * u), ImVec2(mc.x + 7.5f * u, mc.y + 7.5f * u), ic, 1.8f * u);
        ImVec2 after = ImGui::GetCursorScreenPos();
        if (m_search[0]) {   // clear: a small "x" in a rounded box at the right end of the field
            ImVec2 cp(p0.x + sz.x - fh, p0.y);
            ImGui::SetCursorScreenPos(cp);
            if (ImGui::InvisibleButton("##xclear", ImVec2(fh, fh))) m_search[0] = 0;
            ImU32 xc = ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 202, 215, 255);
            ImVec2 cc(cp.x + fh * 0.5f, cp.y + fh * 0.5f);
            float r = fh * 0.2f;
            dl->AddLine(ImVec2(cc.x - r, cc.y - r), ImVec2(cc.x + r, cc.y + r), xc, 1.6f * u);
            dl->AddLine(ImVec2(cc.x - r, cc.y + r), ImVec2(cc.x + r, cc.y - r), xc, 1.6f * u);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + sz.x + 8 * u, p0.y));
        ImGui::SetNextItemWidth(rightW);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * u, 5 * u));
        const char* kinds[] = {"All", "Packages", "Other files"};
        ImGui::Combo("##xfilter", &m_explorerFilter, kinds, 3);
        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(after.x, std::max(after.y, ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y)));
    }
    UpdateSearch();
    bool searching = m_search[0] != 0;
    const std::vector<u32>& allFiles = searching ? m_searchResults : folders[m_folder].files;

    // location line: the path as plain clickable segments ("Athena / Content / Models")
    ImGui::Dummy(ImVec2(0, 2 * u));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8 * u);
    if (searching) {
        ImGui::Text("%zu result(s)%s", m_searchResults.size(), m_searchTruncated ? " (truncated)" : "");
    } else {
        std::vector<u32> chain;
        for (u32 f = m_folder; f != UINT32_MAX && f != 0; f = folders[f].parent) chain.push_back(f);
        auto link = [&](const char* label, u32 target, bool last) {
            ImVec2 ts = ImGui::CalcTextSize(label);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##crumb", ts);
            bool hov = ImGui::IsItemHovered();
            ImU32 col = last ? IM_COL32(240, 240, 240, 255) : hov ? IM_COL32(110, 165, 255, 255) : IM_COL32(160, 162, 178, 255);
            dl->AddText(p, col, label);
            if (hov && !last) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (clicked && !last) m_folder = target;
        };
        ImGui::PushID("crumbs");
        link("Root", 0, chain.empty());
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            ImGui::SameLine(0, 6 * u);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, 6 * u);
            ImGui::PushID((int)*it);
            link(folders[*it].name.c_str(), *it, it + 1 == chain.rend());
            ImGui::PopID();
        }
        ImGui::PopID();
    }

    // items: folders first, then files filtered by type
    std::vector<u32> subfolders;
    if (!searching && m_explorerFilter != 2) subfolders = folders[m_folder].folders;
    std::vector<u32> files;
    files.reserve(allFiles.size());
    for (u32 e : allFiles) {
        bool pkg = IsPackageFile(vfs.Path(e));
        if (m_explorerFilter == 1 && !pkg) continue;
        if (m_explorerFilter == 2 && pkg) continue;
        files.push_back(e);
    }
    int total = (int)(subfolders.size() + files.size());
    ImGui::BeginChild("##tiles", ImVec2(0, 0), ImGuiChildFlags_None);
    dl = ImGui::GetWindowDrawList();
    // back to the top when another folder (or another search) is shown
    std::string viewKey = searching ? std::string(m_search) : Fmt("#%u", m_folder);
    if (viewKey != m_explorerView) {
        m_explorerView = viewKey;
        ImGui::SetScrollY(0);
    }
    if (total == 0) {
        const char* msg = searching ? "No packages match the search" : "No folders or packages found";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((ImGui::GetContentRegionAvail().x - ts.x) * 0.5f, 24 * u));
        ImGui::TextColored(ImVec4(1, 0.42f, 0.42f, 1), "%s", msg);
        ImGui::EndChild();
        return;
    }
    // fixed-size cards (2:3 like FModel's), the spare width spread evenly: the same gap at both edges and between cards
    float tw = 132 * u, th = 198 * u, minGap = 10 * u, gy = 12 * u;
    float availW = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((availW - minGap) / (tw + minGap)));
    float gx = std::max(minGap, (availW - cols * tw) / (cols + 1));
    int rows = (total + cols - 1) / cols;
    float startX = ImGui::GetCursorPosX() + gx;
    float y0 = ImGui::GetCursorPosY() + 6 * u;
    ImGuiListClipper clip;
    clip.Begin(rows, th + gy);
    const ImU32 textCol = IM_COL32(240, 240, 240, 255), subCol = IM_COL32(240, 240, 240, 200), dimCol = IM_COL32(150, 151, 165, 255);
    const ImU32 accent = IM_COL32(32, 107, 212, 255);
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++)
            for (int cI = 0; cI < cols; cI++) {
                int idx = row * cols + cI;
                if (idx >= total) break;
                ImGui::SetCursorPos(ImVec2(startX + cI * (tw + gx), y0 + row * (th + gy)));
                ImVec2 mn = ImGui::GetCursorScreenPos(), mx(mn.x + tw, mn.y + th);
                ImGui::PushID(idx);
                bool isFolder = idx < (int)subfolders.size();
                u32 id = isFolder ? subfolders[idx] : files[idx - subfolders.size()];
                ImGui::InvisibleButton("##tile", ImVec2(tw, th));
                bool hov = ImGui::IsItemHovered();
                // buttons report clicks on release, so a double click has to be read from the press itself
                bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                bool dblClicked = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                float pad = 6 * u;
                if (isFolder) {
                    bool selected = m_selFolder == id;
                    dl->AddRectFilled(mn, mx, hov ? IM_COL32(60, 61, 76, 255) : IM_COL32(50, 50, 63, 255));
                    const VfsFolder& fo = folders[id];
                    // icon + name + two count lines, the whole block centred vertically
                    float px = ImGui::GetFontSize() / base;   // pushed sizes are unscaled: convert to pixels
                    float iconS = tw * 0.62f, nameH = base * 0.95f * px, subH = base * 0.75f * px;
                    float blockH = iconS * 0.74f + 14 * u + nameH + 6 * u + subH * 2 + 2 * u;
                    float y = mn.y + (th - blockH) * 0.5f;
                    DrawBigFolder(dl, ImVec2((mn.x + mx.x) * 0.5f, y + iconS * 0.37f), iconS);
                    y += iconS * 0.74f + 14 * u;
                    ImGui::PushFont(boldFont, base * 0.95f);
                    CardTextCentered(dl, mn.x + pad, mx.x - pad, y, textCol, fo.name);
                    ImGui::PopFont();
                    y += nameH + 6 * u;
                    ImGui::PushFont(nullptr, base * 0.75f);
                    CardTextCentered(dl, mn.x + pad, mx.x - pad, y, subCol, Fmt("Subfolders: %zu", fo.folders.size()));
                    CardTextCentered(dl, mn.x + pad, mx.x - pad, y + subH + 2 * u, subCol, Fmt("Assets: %zu", fo.files.size()));
                    ImGui::PopFont();
                    if (selected) dl->AddRect(mn, mx, accent, 0, 0, 1.5f * u);
                    if (hov) ImGui::SetTooltip("%s", fo.name.c_str());
                    if (clicked) m_selFolder = id;
                    if (dblClicked) { m_folder = id; m_search[0] = 0; }
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Open")) { m_folder = id; m_search[0] = 0; }
                        if (ImGui::MenuItem("Export folder (models / textures)")) ExportFiles(FolderFilesRecursive(id), 0);
                        if (ImGui::MenuItem("Save folder properties (.json)")) ExportFiles(FolderFilesRecursive(id), 2);
                        ImGui::EndPopup();
                    }
                } else {
                    bool selected = m_selected == (i64)id;
                    std::string_view path = vfs.Path(id);
                    std::string_view fn = vfs.FileName(id);
                    size_t dot = fn.rfind('.');
                    std::string name(dot == std::string_view::npos ? fn : fn.substr(0, dot));
                    std::string ext(dot == std::string_view::npos ? std::string_view() : fn.substr(dot + 1));
                    ImU32 tint = FileTint(fn);
                    const ImU32 previewBg = IM_COL32(31, 32, 41, 255);
                    float topH = tw * 0.92f;   // square-ish preview area, then a 2px type-coloured line
                    dl->AddRectFilled(mn, mx, hov ? IM_COL32(60, 62, 78, 255) : IM_COL32(50, 51, 65, 255));
                    dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + topH), previewBg);
                    DrawFileGlyph(dl, ImVec2((mn.x + mx.x) * 0.5f, mn.y + topH * 0.5f), tw * 0.46f, fn, tint, previewBg);
                    dl->AddRectFilled(ImVec2(mn.x, mn.y + topH), ImVec2(mx.x, mn.y + topH + 2 * u), tint);
                    ImGui::PushFont(boldFont, base * 0.9f);
                    CardTextTwoLines(dl, ImVec2(mn.x + pad, mn.y + topH + 2 * u + pad), tw - pad * 2, textCol, name);
                    ImGui::PopFont();
                    ImGui::PushFont(nullptr, base * 0.68f);
                    std::string sz = HumanSize(vfs.Entry(id).usize);
                    float by = mx.y - pad - ImGui::GetFontSize();
                    CardText(dl, ImVec2(mn.x + pad, by), dimCol, ext.c_str());
                    CardText(dl, ImVec2(mx.x - pad - ImGui::CalcTextSize(sz.c_str()).x, by), dimCol, sz.c_str());
                    ImGui::PopFont();
                    if (selected) dl->AddRect(mn, mx, accent, 0, 0, 1.5f * u);
                    if (clicked) m_selected = id;
                    if (dblClicked) OpenFile(id);
                    if (hov) ImGui::SetTooltip("%s", std::string(path).c_str());
                    if (ImGui::BeginPopupContextItem()) {
                        DrawFileMenu(id, searching);
                        ImGui::EndPopup();
                    }
                }
                ImGui::PopID();
            }
    ImGui::EndChild();
}

void App::DrawFileList() {
    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search all files (terms or wildcards, Ctrl+F)", m_search, sizeof(m_search));
    if (m_mountState != MountState::Ready) return;
    UpdateSearch();
    const Vfs& vfs = m_api->vfs;
    auto& folders = vfs.Folders();
    bool searching = m_search[0] != 0;
    const std::vector<u32>& files = searching ? m_searchResults : folders[m_folder].files;

    // breadcrumb
    if (searching) {
        ImGui::TextDisabled("%zu result(s)%s", m_searchResults.size(), m_searchTruncated ? " (truncated)" : "");
    } else {
        std::vector<u32> chain;
        for (u32 f = m_folder; f != UINT32_MAX && f != 0; f = folders[f].parent) chain.push_back(f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
        if (ImGui::SmallButton("/")) m_folder = 0;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            ImGui::SameLine();
            ImGui::PushID((int)*it);
            if (ImGui::SmallButton(folders[*it].name.c_str())) m_folder = *it;
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        if (!folders[m_folder].folders.empty() || !files.empty()) ImGui::TextDisabled("%zu folder(s), %zu file(s)", folders[m_folder].folders.size(), files.size());
    }

    if (!ImGui::BeginTable("##filelist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 55);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableHeadersRow();

    std::vector<u32> subfolders;
    if (!searching) subfolders = folders[m_folder].folders;
    int rows = (int)(subfolders.size() + files.size());
    ImGuiListClipper clip;
    clip.Begin(rows);
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (row < (int)subfolders.size()) {
                u32 fi = subfolders[row];
                ImGui::PushID(-(int)fi - 1);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.45f, 1));
                DrawFolderIcon(ImGui::GetWindowDrawList(), ImVec2(ImGui::GetCursorScreenPos().x + 2, ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f + 1));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 1.3f);
                if (ImGui::Selectable(folders[fi].name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(0))
                    m_folder = fi;
                ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Open")) m_folder = fi;
                    if (ImGui::MenuItem("Export folder (models / textures)")) ExportFiles(FolderFilesRecursive(fi), 0);
                    if (ImGui::MenuItem("Save folder properties (.json)")) ExportFiles(FolderFilesRecursive(fi), 2);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("folder");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%u", folders[fi].totalFiles);
                ImGui::PopID();
                continue;
            }
            u32 e = files[row - subfolders.size()];
            std::string_view name = searching ? vfs.Path(e) : vfs.FileName(e);
            std::string label(name);
            ImGui::PushID((int)e);
            bool pkg = IsPackageFile(name);
            if (!pkg) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label.c_str(), m_selected == e, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selected = e;
                if (ImGui::IsMouseDoubleClicked(0)) OpenFile(e);
            }
            if (!pkg) ImGui::PopStyleColor();
            if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter)) OpenFile(e);
            if (ImGui::BeginPopupContextItem()) {
                DrawFileMenu(e, searching);
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            size_t dot = name.rfind('.');
            ImGui::TextDisabled("%s", dot == std::string_view::npos ? "" : std::string(name.substr(dot + 1)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", HumanSize(vfs.Entry(e).usize).c_str());
            ImGui::PopID();
        }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- documents

void App::OpenFile(u32 entry) {
    if (m_mountState != MountState::Ready) return;
    for (auto& d : m_docs)
        if (d->entry == entry) {
            m_selectDoc = d->id;
            return;
        }
    if (!IsPackageFile(m_api->vfs.Path(entry))) return;
    auto d = std::make_shared<Document>();
    d->id = m_nextDocId++;
    d->entry = entry;
    d->gamePath = std::string(m_api->vfs.Path(entry));
    d->name = std::string(m_api->vfs.FileName(entry));
    m_docs.push_back(d);
    m_selectDoc = d->id;
    LoadDocument(d);
}

static void SplitLines(Document& d) {
    d.lines.clear();
    d.lines.push_back(0);
    for (u32 i = 0; i < (u32)d.json.size(); i++)
        if (d.json[i] == '\n') d.lines.push_back(i + 1);
}

// Worker thread: skeleton + mesh + poser for an animation document. Returns false (and sets d.error) on failure.
bool App::LoadAnimationMeshNow(Document& d, const std::string& meshPath) {
    std::string err;
    std::vector<Bone> skeleton;
    auto sp = m_api->cache->LoadPath(d.anim->skeletonPath, &err);
    i32 se = sp ? sp->MainExport() : 0;
    if (!se || !ReadSkeleton(*sp, se - 1, skeleton, &err)) { d.error = "skeleton: " + err; return false; }
    i64 entry = m_api->ResolveFile(meshPath);
    auto mp = entry >= 0 ? m_api->cache->Load((u32)entry, &err) : nullptr;
    auto mesh = std::make_shared<MeshData>();
    if (!mp || !mp->MainExport() || !ReadMesh(*mp, mp->MainExport() - 1, *mesh, &err) || !mesh->skeletal) {
        d.error = "mesh " + meshPath + ": " + (err.empty() ? "not a skeletal mesh" : err);
        return false;
    }
    auto poser = std::make_shared<AnimPoser>();
    if (!poser->Init(*mesh, skeleton, &err)) { d.error = err; return false; }
    int lod = 0;
    auto gm = m_assets->Mesh(*mesh, lod, true, true);
    if (!gm) { d.error = "mesh has no geometry"; return false; }
    d.mesh = mesh;
    d.poser = poser;
    d.gpuMesh = gm;
    d.lod = lod;
    d.animMesh = mesh->name.empty() ? meshPath : meshPath;
    snprintf(d.animMeshEdit, sizeof(d.animMeshEdit), "%s", meshPath.c_str());
    d.camera.Frame(mesh->boundsMin, mesh->boundsMax);
    d.camera.yaw = -135;
    d.draw.grid = m_settings.showGrid;
    d.error.clear();
    return true;
}

void App::LoadAnimationMesh(std::shared_ptr<Document> d, std::string meshPath) {
    d->loading = true;
    m_jobs->Push([this, d, meshPath] {
        LoadAnimationMeshNow(*d, meshPath);
        d->loading = false;
        Post([] {});
    });
}

void App::LoadDocument(std::shared_ptr<Document> d) {
    m_jobs->Push([this, d] {
        std::string err;
        auto pkg = m_api->cache->Load(d->entry, &err);
        if (!pkg) {
            d->error = err.empty() ? "could not load package" : err;
            d->loading = false;
            return;
        }
        d->pkg = pkg;
        try {
            d->json = PackageToJson(*pkg);
        } catch (const std::exception& e) {
            d->json = std::string("// failed to serialise: ") + e.what();
        }
        SplitLines(*d);
        i32 main = pkg->MainExport();
        d->className = main ? pkg->ExportClass(main - 1) : "";
        if (main && IsTextureClass(d->className)) {
            if (ReadTextureInfo(*pkg, main - 1, d->tex, &err)) {
                Image img;
                if (DecodeTexture(*pkg, d->tex, -1, m_settings.previewMaxTexture, img, &err, &d->texMip)) {
                    d->texture = m_renderer->CreateTexture(img, d->tex.srgb, true);
                    d->preferred = DocView::Texture;
                }
            }
            if (!d->texture) d->error = "texture: " + err;
        } else if (main && (IsStaticMeshClass(d->className) || IsSkeletalMeshClass(d->className))) {
            auto mesh = std::make_shared<MeshData>();
            if (ReadMesh(*pkg, main - 1, *mesh, &err)) {
                d->mesh = mesh;
                d->gpuMesh = m_assets->Mesh(*mesh, 0);
                d->camera.Frame(mesh->boundsMin, mesh->boundsMax);
                d->draw.grid = m_settings.showGrid;
                d->hasMesh = d->gpuMesh != nullptr;
                if (d->hasMesh) d->preferred = DocView::Model;
            } else {
                d->error = "mesh: " + err;
            }
        } else if (main && IsAnimSequenceClass(d->className)) {
            auto anim = std::make_shared<AnimSequenceData>();
            if (ReadAnimSequence(*pkg, main - 1, *anim, &err)) {
                d->anim = anim;
                d->preferred = DocView::Animation;
                std::string mesh = FindMeshForSkeleton(*m_api->cache, anim->skeletonPath);
                if (!mesh.empty()) LoadAnimationMeshNow(*d, mesh);
                else d->error = "no skeletal mesh found for " + anim->skeletonPath + " (enter one below)";
            } else {
                d->error = "animation: " + err;
            }
        } else if (main && IsMaterialClass(d->className) && d->className != "MaterialFunction") {
            std::string objPath = pkg->FullPath(main);
            if (ReadMaterial(*m_api->cache, objPath, d->material, &err)) {
                d->hasMaterial = true;
                d->preferred = DocView::Material;
                for (auto& t : d->material.textures) {
                    if (d->materialTextures.size() >= 16) break;
                    d->materialTextures.push_back({t.name, m_assets->Texture(t.texturePath)});
                }
                if (m_sphere) {
                    auto s = std::make_shared<GpuMesh>(*m_sphere);
                    s->materials = {m_assets->Material(objPath)};
                    d->gpuMesh = s;
                    d->camera.Frame(s->boundsMin, s->boundsMax);
                    d->camera.yaw = 180;
                    d->camera.pitch = -10;
                    d->draw.grid = false;
                }
            } else {
                d->error = "material: " + err;
            }
        } else if (main && IsWorldClass(d->className)) {
            d->isWorld = true;
            d->preferred = DocView::World;
        }
        d->loading = false;
        if (d->isWorld) LoadWorld(d);
        Post([] {});
    });
}

// Worker: the map with its sublevels -> WorldData -> GPU scene. Progress and the result are posted to the UI thread.
void App::LoadWorld(std::shared_ptr<Document> d) {
    d->worldLoading = true;
    d->worldDone = d->worldTotal = 0;
    d->worldStage = "levels";
    m_jobs->Push([this, d] {
        auto world = std::make_shared<WorldData>();
        WorldLoader loader(*m_api->cache);
        WorldOptions opt;
        opt.landscapeStep = 1;
        opt.cancel = [d] { return d->cancel.load(); };
        opt.progress = [this, d](int done, int total, const std::string& level) {
            std::string stage = level.empty() ? "levels" : level.substr(level.find_last_of('/') + 1);
            Post([d, done, total, stage] { d->worldDone = done; d->worldTotal = total; d->worldStage = "level " + stage; });
        };
        std::string err;
        bool ok = loader.Load(d->gamePath, opt, *world, &err);
        std::shared_ptr<WorldScene> scene;
        if (ok && !d->cancel) {
            scene = std::make_shared<WorldScene>(*m_renderer, *m_assets, *m_api->cache);
            scene->Build(*world, [d] { return d->cancel.load(); }, [this, d](int done, int total) {
                Post([d, done, total] { d->worldDone = done; d->worldTotal = total; d->worldStage = "meshes"; });
            });
            for (auto& l : world->landscapes) {   // uploaded: the CPU copy is no longer needed
                l.mesh = MeshLod{};
            }
        }
        Post([this, d, world, scene, ok, err] {
            d->worldLoading = false;
            if (!ok) { d->error = "world: " + err; return; }
            d->world = world;
            d->worldScene = scene;
            if (scene) {
                d->camera.Frame(scene->FocusMin(), scene->FocusMax());
                d->camera.yaw = -135;
                d->camera.pitch = -35;
                d->camera.nearZ = 10;
                d->camera.flySpeed = 3000;
            }
            d->draw.grid = false;
            LOGI("world %s: %zu levels, %zu mesh instances, %zu landscape components, %u meshes missing", d->name.c_str(), world->levels.size(),
                 world->InstanceCount(), world->landscapes.size(), scene ? scene->MissingMeshes() : 0);
        });
    });
}

void App::RebuildMeshLod(Document& d) {
    if (!d.mesh) return;
    auto mesh = d.mesh;
    int lod = d.lod;
    u64 id = d.id;
    m_jobs->Push([this, mesh, lod, id] {
        auto gm = m_assets->Mesh(*mesh, lod);
        Post([this, gm, id] {
            for (auto& x : m_docs)
                if (x->id == id && gm) x->gpuMesh = gm;
        });
    });
}

void App::DrawDocuments() {
    if (!ImGui::BeginTabBar("##docs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) return;
    if (ImGui::BeginTabItem("Explorer###explorer", nullptr, ImGuiTabItemFlags_NoCloseButton | (m_selectExplorer ? ImGuiTabItemFlags_SetSelected : 0))) {
        m_activeDoc = 0;
        DrawExplorer();
        ImGui::EndTabItem();
    }
    m_selectExplorer = false;
    u64 close = 0;
    for (auto& dp : m_docs) {
        Document& d = *dp;
        bool open = true;
        ImGuiTabItemFlags fl = m_selectDoc == d.id ? ImGuiTabItemFlags_SetSelected : 0;
        std::string label = d.name + "###doc" + std::to_string(d.id);
        if (ImGui::BeginTabItem(label.c_str(), &open, fl)) {
            m_activeDoc = d.id;
            DrawDocument(d);
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.gamePath.c_str());
        if (!open) close = d.id;
    }
    m_selectDoc = 0;
    ImGui::EndTabBar();
    if (close) CloseDocument(close);
}

// Closes one tab (id) or all of them (0); background loads of the closed documents (maps) are cancelled.
void App::CloseDocument(u64 id) {
    for (auto& dp : m_docs)
        if (!id || dp->id == id) dp->cancel = true;
    m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(), [&](auto& d) { return !id || d->id == id; }), m_docs.end());
    if (!id || m_activeDoc == id) m_activeDoc = 0;
    if (m_jobs->Pending() == 0 && m_assets) m_assets->Trim();
}

void App::DrawDocument(Document& d) {
    if (d.loading) {
        ImGui::TextDisabled("Loading %s ...", d.gamePath.c_str());
        return;
    }
    // header, one line: class, the path (clipped, full path in the tooltip), actions on the right
    BeginToolbar();
    ImGuiStyle& st = ImGui::GetStyle();
    float bw = ImGui::CalcTextSize("Export").x + ImGui::CalcTextSize("Save JSON").x + ImGui::CalcTextSize("Copy path").x + st.FramePadding.x * 6 + st.ItemSpacing.x * 2;
    float x0 = ImGui::GetCursorPosX(), right = ImGui::GetContentRegionMax().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1, 1), "%s", d.className.empty() ? "?" : d.className.c_str());
    ImGui::SameLine(0, 10 * ImGui::GetFontSize() / 16.f);
    float pathW = right - bw - st.ItemSpacing.x * 2 - ImGui::GetCursorPosX();
    if (pathW > 40) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float th = ImGui::GetFrameHeight();
        ImGui::Dummy(ImVec2(pathW, th));
        ImGui::PushStyleColor(ImGuiCol_Text, st.Colors[ImGuiCol_TextDisabled]);
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + st.FramePadding.y), ImVec2(p.x + pathW, p.y + th), p.x + pathW,
                                  d.gamePath.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.gamePath.c_str());
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), std::max(x0, right - bw)));
    if (ImGui::Button("Export")) ExportFiles({d.entry}, 0);
    ImGui::SameLine();
    if (ImGui::Button("Save JSON")) ExportFiles({d.entry}, 2);
    ImGui::SameLine();
    if (ImGui::Button("Copy path")) CopyToClipboard(GamePathToPackage(d.gamePath));
    EndToolbar();
    if (!d.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.error.c_str());

    if (!ImGui::BeginTabBar("##views")) return;
    auto tab = [&](const char* name, DocView v) {
        ImGuiTabItemFlags fl = (d.selectPreferred && d.preferred == v) ? ImGuiTabItemFlags_SetSelected : 0;
        return ImGui::BeginTabItem(name, nullptr, fl);
    };
    if (d.texture && tab("Texture", DocView::Texture)) { DrawTexture(d); ImGui::EndTabItem(); }
    if (d.hasMesh && tab("Model", DocView::Model)) { DrawModel(d); ImGui::EndTabItem(); }
    if (d.isWorld && tab("World", DocView::World)) { DrawWorld(d); ImGui::EndTabItem(); }
    if (d.anim && tab("Animation", DocView::Animation)) { DrawAnimation(d); ImGui::EndTabItem(); }
    if (d.hasMaterial && tab("Material", DocView::Material)) { DrawMaterial(d); ImGui::EndTabItem(); }
    if (tab("Properties", DocView::Properties)) { DrawProperties(d); ImGui::EndTabItem(); }
    d.selectPreferred = false;
    ImGui::EndTabBar();
}

// JSON viewer: virtualised lines with light syntax colouring
void App::DrawProperties(Document& d) {
    ImGui::SetNextItemWidth(260);
    bool enter = ImGui::InputTextWithHint("##find", "Find in properties", d.find, sizeof(d.find), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool next = ImGui::Button("Next") || enter;
    ImGui::SameLine();
    bool prev = ImGui::Button("Prev");
    ImGui::SameLine();
    if (ImGui::Button("Copy all")) CopyToClipboard(d.json);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines, %s", d.lines.size(), HumanSize(d.json.size()).c_str());
    if ((next || prev) && d.find[0]) {
        std::string needle = ToLower(d.find);
        int n = (int)d.lines.size();
        for (int k = 1; k <= n; k++) {
            int i = ((d.findLine < 0 ? (prev ? 0 : -1) : d.findLine) + (prev ? -k : k) + n * 2) % n;
            u32 s = d.lines[i], e = i + 1 < n ? d.lines[i + 1] : (u32)d.json.size();
            std::string line = ToLower(std::string_view(d.json).substr(s, e - s));
            if (line.find(needle) != std::string::npos) {
                d.findLine = i;
                d.scrollToLine = i;
                break;
            }
        }
    }

    ImGui::PushFont(monoFont, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.1f, 0.12f, 1));
    ImGui::BeginChild("##json", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    float lh = ImGui::GetTextLineHeightWithSpacing();
    if (d.scrollToLine >= 0) {
        ImGui::SetScrollY(std::max(0.f, d.scrollToLine * lh - ImGui::GetWindowHeight() * 0.4f));
        d.scrollToLine = -1;
    }
    const ImVec4 cKey(0.55f, 0.78f, 1.0f, 1), cStr(0.92f, 0.68f, 0.45f, 1), cNum(0.65f, 0.88f, 0.6f, 1), cLit(0.78f, 0.6f, 0.95f, 1), cPun(0.6f, 0.63f, 0.7f, 1);
    ImGuiListClipper clip;
    clip.Begin((int)d.lines.size(), lh);
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            u32 s = d.lines[i], e = i + 1 < (int)d.lines.size() ? d.lines[i + 1] - 1 : (u32)d.json.size();
            std::string_view line = std::string_view(d.json).substr(s, e - s);
            if (i == d.findLine) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetWindowWidth() + ImGui::GetScrollX(), p.y + lh), IM_COL32(70, 90, 40, 160));
            }
            size_t ind = line.find_first_not_of(' ');
            if (ind == std::string_view::npos) { ImGui::TextUnformatted(""); continue; }
            auto seg = [&](std::string_view t, const ImVec4& c, bool first) {
                if (!first) ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, c);
                ImGui::TextUnformatted(t.data(), t.data() + t.size());
                ImGui::PopStyleColor();
            };
            std::string_view rest = line.substr(ind);
            bool first = true;
            std::string_view pad = line.substr(0, ind);
            seg(pad.empty() ? std::string_view("") : pad, cPun, true);
            first = false;
            if (rest[0] == '"') {
                size_t q = 1;
                while (q < rest.size() && rest[q] != '"') q += rest[q] == '\\' ? 2 : 1;
                q = std::min(q + 1, rest.size());
                if (rest.substr(q, 2) == ": ") {
                    seg(rest.substr(0, q), cKey, first);
                    seg(": ", cPun, false);
                    rest = rest.substr(q + 2);
                }
            }
            if (rest.empty()) continue;
            std::string_view val = rest, tail;
            if (val.back() == ',') { tail = val.substr(val.size() - 1); val = val.substr(0, val.size() - 1); }
            const ImVec4* col = &cPun;
            if (!val.empty()) {
                char c0 = val[0];
                if (c0 == '"') col = &cStr;
                else if (c0 == '-' || (c0 >= '0' && c0 <= '9')) col = &cNum;
                else if (val == "true" || val == "false" || val == "null") col = &cLit;
            }
            seg(val, *col, false);
            if (!tail.empty()) seg(tail, cPun, false);
        }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void App::DrawTexture(Document& d) {
    auto& t = d.tex;
    ImGui::Text("%s  %dx%d", t.pixelFormat.c_str(), t.sizeX, t.sizeY);
    ImGui::SameLine();
    ImGui::TextDisabled("| %zu mips | %s | preview mip %d (%dx%d) | %s", t.mips.size(), t.srgb ? "sRGB" : "linear", d.texMip, d.texture->width, d.texture->height,
                        t.compressionSettings.c_str());
    if (ImGui::Button("Fit")) { d.texZoom = 0; d.texPanX = d.texPanY = 0; }
    ImGui::SameLine();
    if (ImGui::Button("1:1")) { d.texZoom = 1; d.texPanX = d.texPanY = 0; }
    ImGui::SameLine();
    ImGui::Checkbox("Checkerboard", &d.texCheckerboard);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 8 || avail.y < 8) return;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO& io = ImGui::GetIO();
    float tw = (float)d.texture->width, th = (float)d.texture->height;
    // zoom is relative to the full-size texture so 1:1 means one texel of the original per pixel
    float scaleFull = (float)t.sizeX / tw;
    float fit = std::min(avail.x / tw, avail.y / th);
    float scale = d.texZoom <= 0 ? fit : d.texZoom * scaleFull;
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
        float ns = std::clamp(scale * powf(1.2f, io.MouseWheel), 0.02f, 64.f);
        // keep the texel under the cursor fixed
        ImVec2 c(p0.x + avail.x * 0.5f + d.texPanX, p0.y + avail.y * 0.5f + d.texPanY);
        float mx = io.MousePos.x - c.x, my = io.MousePos.y - c.y;
        d.texPanX -= mx * (ns / scale - 1);
        d.texPanY -= my * (ns / scale - 1);
        scale = ns;
        d.texZoom = ns / scaleFull;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(2))) {
        d.texPanX += io.MouseDelta.x;
        d.texPanY += io.MouseDelta.y;
        if (d.texZoom <= 0) d.texZoom = scale / scaleFull;
    }
    float w = tw * scale, h = th * scale;
    ImVec2 a(p0.x + (avail.x - w) * 0.5f + d.texPanX, p0.y + (avail.y - h) * 0.5f + d.texPanY), b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), true);
    if (d.texCheckerboard) {
        const float cs = 12;
        ImVec2 ca(std::max(a.x, p0.x), std::max(a.y, p0.y)), cb(std::min(b.x, p0.x + avail.x), std::min(b.y, p0.y + avail.y));
        dl->AddRectFilled(ca, cb, IM_COL32(60, 60, 64, 255));
        for (float y = ca.y - fmodf(ca.y - a.y, cs * 2); y < cb.y; y += cs)
            for (float x = ca.x - fmodf(ca.x - a.x, cs * 2) + (fmodf((y - a.y) / cs, 2.f) >= 1 ? cs : 0); x < cb.x; x += cs * 2)
                dl->AddRectFilled(ImVec2(std::max(x, ca.x), std::max(y, ca.y)), ImVec2(std::min(x + cs, cb.x), std::min(y + cs, cb.y)), IM_COL32(90, 90, 96, 255));
    }
    dl->AddImage((ImTextureID)(intptr_t)d.texture->srvRaw.Get(), a, b);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 30));
    dl->PopClipRect();
    if (ImGui::IsItemHovered()) {
        float u = (io.MousePos.x - a.x) / w, v = (io.MousePos.y - a.y) / h;
        if (u >= 0 && u < 1 && v >= 0 && v < 1)
            ImGui::SetTooltip("%d, %d  (zoom %.0f%%)", (int)(u * t.sizeX), (int)(v * t.sizeY), scale / scaleFull * 100);
    }
}

// Space left for a 3D viewport, but never shorter than a usable minimum: in small windows the tab scrolls instead of the viewport vanishing.
static ImVec2 MinViewportAvail() {
    ImVec2 a = ImGui::GetContentRegionAvail();
    a.y = std::max(a.y, ImGui::GetFontSize() * 14);
    return a;
}

// Camera input over an image-sized button (LMB orbit, RMB look + WASD/QE fly, MMB pan, wheel zoom, F calls `frame`),
// then renders `scene` into the viewport.
static void DrawViewportScene(Renderer& r, Camera& cam, const DrawOptions& draw, ImVec2 size, bool allowFly, const std::function<void()>& frame,
                              const std::function<void()>& scene) {
    if (size.x < 8 || size.y < 8) return;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);   // the wheel zooms; it must not also scroll the tab when that has a scrollbar
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered && io.MouseWheel != 0) cam.distance = std::max(cam.nearZ * 2, cam.distance * powf(0.85f, io.MouseWheel));
    Vec3 fwd = cam.Forward();
    Vec3 right = Normalize(Cross({0, 0, 1}, fwd));
    Vec3 up = Cross(fwd, right);
    if (active && ImGui::IsMouseDown(0)) {
        cam.yaw += io.MouseDelta.x * 0.3f;
        cam.pitch = std::clamp(cam.pitch - io.MouseDelta.y * 0.3f, -89.f, 89.f);
    }
    if (active && ImGui::IsMouseDown(2)) {
        float k = cam.distance * 0.0015f;
        cam.target = cam.target - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
    }
    if (allowFly && active && ImGui::IsMouseDown(1)) {
        // first-person look around the eye + WASD/QE movement
        Vec3 eye = cam.Eye();
        cam.yaw += io.MouseDelta.x * 0.25f;
        cam.pitch = std::clamp(cam.pitch - io.MouseDelta.y * 0.25f, -89.f, 89.f);
        fwd = cam.Forward();
        right = Normalize(Cross({0, 0, 1}, fwd));
        Vec3 mv;
        if (ImGui::IsKeyDown(ImGuiKey_W)) mv = mv + fwd;
        if (ImGui::IsKeyDown(ImGuiKey_S)) mv = mv - fwd;
        if (ImGui::IsKeyDown(ImGuiKey_D)) mv = mv + right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) mv = mv - right;
        if (ImGui::IsKeyDown(ImGuiKey_E)) mv = mv + Vec3{0, 0, 1};
        if (ImGui::IsKeyDown(ImGuiKey_Q)) mv = mv - Vec3{0, 0, 1};
        float speed = cam.flySpeed * (io.KeyShift ? 4.f : 1.f) * io.DeltaTime;
        eye = eye + mv * speed;
        cam.target = eye + fwd * cam.distance;
    }
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame();

    r.BeginFrame((int)size.x, (int)size.y, cam, draw);
    scene();
    r.EndFrame();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(intptr_t)r.Output(), p0, ImVec2(p0.x + size.x, p0.y + size.y));
}

static void DrawViewport(Renderer& r, Document& d, ImVec2 size, bool allowFly) {
    if (!d.gpuMesh) return;
    DrawViewportScene(r, d.camera, d.draw, size, allowFly, [&] { d.camera.Frame(d.gpuMesh->boundsMin, d.gpuMesh->boundsMax); }, [&] {
        if (d.draw.grid) {
            float rad = std::max(1.f, d.gpuMesh->radius);
            float cell = powf(10.f, floorf(log10f(rad)));
            r.DrawGrid(cell / 10, 50);
        }
        Mat4 id = Mat4::Identity();
        r.DrawMesh(*d.gpuMesh, &id, 1);
        if (!d.boneLines.empty()) r.DrawLines(d.boneLines, 0xff40d0ffu);
    });
}

void App::DrawModel(Document& d) {
    auto& m = *d.mesh;
    BeginToolbar();
    if (m.lods.size() > 1) {
        ImGui::SetNextItemWidth(80 * ImGui::GetFontSize() / 16.f);
        std::string cur = Fmt("LOD %d", d.lod);
        if (ImGui::BeginCombo("##lod", cur.c_str())) {
            for (int i = 0; i < (int)m.lods.size(); i++)
                if (ImGui::Selectable(Fmt("LOD %d  (%zu tris)", i, m.lods[i].indices.size() / 3).c_str(), d.lod == i) && d.lod != i) {
                    d.lod = i;
                    RebuildMeshLod(d);
                }
            ImGui::EndCombo();
        }
    }
    FlowToggle("Textures", &d.draw.textures, m.lods.size() <= 1);   // FlowNext decides whether it fits next to the LOD combo
    FlowToggle("Normal maps", &d.draw.normalMaps);
    FlowToggle("Vertex colors", &d.draw.vertexColors);
    FlowToggle("Wireframe", &d.draw.wireframe);
    FlowToggle("Grid", &d.draw.grid);
    FlowToggle("Cull", &d.draw.backfaceCulling);
    if (FlowButton("Frame")) d.camera.Frame(d.gpuMesh->boundsMin, d.gpuMesh->boundsMax);
    EndToolbar();

    const MeshLod& l = m.lods[std::clamp(d.lod, 0, (int)m.lods.size() - 1)];
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%u vertices, %zu triangles, %d UV set(s), %zu section(s) | %.1f x %.1f x %.1f cm", l.VertexCount(), l.indices.size() / 3, l.numUVs,
                        l.sections.size(), m.boundsMax.x - m.boundsMin.x, m.boundsMax.y - m.boundsMin.y, m.boundsMax.z - m.boundsMin.z);
    ImGui::PopTextWrapPos();
    ImVec2 avail = MinViewportAvail();
    float matW = avail.x > 560 ? std::min(320.f, avail.x * 0.3f) : 0.f;   // the material list is dropped in narrow windows
    DrawViewport(*m_renderer, d, ImVec2(matW > 0 ? avail.x - matW - ImGui::GetStyle().ItemSpacing.x : avail.x, avail.y), true);
    if (matW <= 0) return;
    ImGui::SameLine();
    ImGui::BeginChild("##mats", ImVec2(matW, avail.y));
    ImGui::TextDisabled("Materials");
    for (size_t i = 0; i < m.materials.size(); i++) {
        auto& s = m.materials[i];
        ImGui::PushID((int)i);
        ImGui::Text("[%zu] %s", i, s.name.c_str());
        if (!s.materialPath.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", s.materialPath.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::IsItemClicked()) {
                i64 e = m_api->ResolveFile(s.materialPath);
                if (e >= 0) OpenFile((u32)e);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open the material");
        }
        ImGui::PopID();
    }
    if (m.skeletal) ImGui::TextDisabled("%zu bones", m.skeleton.size());
    ImGui::EndChild();
}

void App::DrawAnimation(Document& d) {
    auto& a = *d.anim;
    float fps = a.FrameRate();
    // toolbar: transport + options
    if (ImGui::Button(d.animPlaying ? "Pause" : "Play")) d.animPlaying = !d.animPlaying;
    ImGui::SameLine();
    float frame = d.animTime * fps;
    ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - 520));
    if (ImGui::SliderFloat("##frame", &frame, 0, (float)std::max(0, a.numFrames - 1), "frame %.1f")) {
        d.animTime = frame / fps;
        d.animPlaying = false;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderFloat("##speed", &d.animSpeed, 0.05f, 2.f, "speed %.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &d.animLoop);
    ImGui::SameLine();
    ImGui::Checkbox("Bones", &d.showBones);
    ImGui::SameLine();
    ImGui::Checkbox("Textures", &d.draw.textures);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &d.draw.grid);
    ImGui::TextDisabled("%d frames, %.2f s, %.1f fps | %s | %zu tracks%s | skeleton %s", a.numFrames, a.length, fps, a.encoding.c_str(), a.tracks.size(),
                        a.additiveType != "AAT_None" ? (" | additive " + a.additiveType).c_str() : "", a.skeletonPath.c_str());
    ImGui::SetNextItemWidth(-140);
    bool apply = ImGui::InputTextWithHint("##animmesh", "skeletal mesh to preview on (/Game/... path)", d.animMeshEdit, sizeof(d.animMeshEdit), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    apply |= ImGui::Button("Use mesh");
    if (apply && d.animMeshEdit[0]) {
        for (auto& sp : m_docs)
            if (sp.get() == &d) { LoadAnimationMesh(sp, d.animMeshEdit); break; }
        return;
    }
    if (!d.poser || !d.gpuMesh || !d.mesh) {
        ImGui::TextDisabled("No mesh to show the animation on.");
        return;
    }
    // advance time and pose the mesh (CPU skinning into the dynamic vertex buffer)
    if (d.animPlaying && a.length > 0) {
        d.animTime += ImGui::GetIO().DeltaTime * d.animSpeed;
        if (d.animTime > a.length) d.animTime = d.animLoop ? fmodf(d.animTime, a.length) : a.length;
        if (!d.animLoop && d.animTime >= a.length) d.animPlaying = false;
        m_busyFrames = 3;
    }
    std::vector<Bone> pose;
    d.poser->MeshPose(a, d.animTime * fps, pose);
    std::vector<Vec3> pos, nrm;
    const MeshLod& lod = d.mesh->lods[std::clamp(d.lod, 0, (int)d.mesh->lods.size() - 1)];
    d.poser->Skin(lod, pose, pos, nrm);
    m_renderer->UpdateMeshVertices(*d.gpuMesh, pos, nrm);
    d.boneLines.clear();
    if (d.showBones) {
        std::vector<Mat4> world;
        AnimPoser::WorldMatrices(pose, world);
        for (size_t i = 0; i < pose.size(); i++)
            if (pose[i].parent >= 0) {
                d.boneLines.push_back(world[pose[i].parent].Translation());
                d.boneLines.push_back(world[i].Translation());
            }
    }
    DrawViewport(*m_renderer, d, MinViewportAvail(), true);
}

void App::DrawWorld(Document& d) {
    if (d.worldLoading || !d.worldScene) {
        if (d.worldLoading) {
            float f = d.worldTotal > 0 ? (float)d.worldDone / d.worldTotal : 0.f;
            ImGui::TextDisabled("Loading map: %s", d.worldStage.c_str());
            ImGui::ProgressBar(f, ImVec2(std::min(480.f, ImGui::GetContentRegionAvail().x), 0), Fmt("%d / %d", d.worldDone, d.worldTotal).c_str());
            m_busyFrames = 3;
        } else if (ImGui::Button("Load scene")) {
            for (auto& sp : m_docs)
                if (sp.get() == &d) { d.cancel = false; LoadWorld(sp); break; }
        }
        return;
    }
    WorldData& w = *d.world;
    WorldScene& sc = *d.worldScene;
    // toolbar
    BeginToolbar();
    FlowToggle("Meshes", &d.worldDraw.meshes, true);
    FlowToggle("Landscape", &d.worldDraw.landscape);
    FlowToggle("Water", &d.worldWater);
    FlowToggle("Textures", &d.draw.textures);
    FlowToggle("Wireframe", &d.draw.wireframe);
    FlowNext(130);
    ImGui::SetNextItemWidth(130);
    ImGui::SliderFloat("##dd", &d.worldDraw.drawDistanceScale, 0, 4, d.worldDraw.drawDistanceScale > 0 ? "draw dist x%.2f" : "draw dist: all");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scales the game's per-mesh draw distances (0 = draw everything)");
    FlowNext(120);
    ImGui::SetNextItemWidth(120);
    float cullPx = d.worldDraw.minScreenSize * 1000;
    if (ImGui::SliderFloat("##cull", &cullPx, 0, 10, "small cull %.1f")) d.worldDraw.minScreenSize = cullPx / 1000;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Skips instances smaller than this (per mille of the view height)");
    FlowNext(120);
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##fly", &d.camera.flySpeed, 200, 50000, "fly %.0f cm/s", ImGuiSliderFlags_Logarithmic);
    // Camera::Frame derives the near plane from the bounds (metres for a whole map), which would clip everything close by
    auto frameWorld = [&] {
        d.camera.Frame(sc.FocusMin(), sc.FocusMax());
        d.camera.nearZ = 10;
        d.camera.flySpeed = 3000;
    };
    if (FlowButton("Frame")) frameWorld();
    EndToolbar();
    const auto& st = sc.Stats();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%zu levels | %zu instances of %zu meshes, %zu landscape patches | drawn: %u instances, %u meshes, %u patches, %.2fM tris | %s GPU",
                        w.levels.size(), w.InstanceCount(), w.groups.size(), w.landscapes.size(), st.instancesDrawn, st.meshesDrawn, st.landscapesDrawn,
                        m_renderer->DrawnTriangles() / 1e6, HumanSize(sc.GpuBytes()).c_str());
    ImGui::PopTextWrapPos();

    ImVec2 avail = MinViewportAvail();
    float side = avail.x > 640 ? std::min(360.f, avail.x * 0.3f) : 0.f;   // the outliner is dropped in narrow windows
    Camera& cam = d.camera;
    DrawViewportScene(*m_renderer, cam, d.draw, ImVec2(side > 0 ? avail.x - side - ImGui::GetStyle().ItemSpacing.x : avail.x, avail.y), true, frameWorld, [&] {
        sc.Draw(cam, avail.y, d.worldDraw);
        if (d.worldWater && m_water) {
            Vec3 c = cam.Eye();
            float half = std::max(Length(sc.BoundsMax() - sc.BoundsMin()), 2e6f);
            Mat4 m = Mat4::FromTRS({c.x, c.y, 0}, {0, 0, 0, 1}, {half, half, 1});
            m_renderer->DrawMesh(*m_water, &m, 1);
        }
    });
    if (side <= 0) return;
    ImGui::SameLine();
    ImGui::BeginChild("##outliner", ImVec2(side, avail.y));
    if (ImGui::BeginTabBar("##outl")) {
        if (ImGui::BeginTabItem("Meshes")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##wf", "filter meshes", d.worldFilter, sizeof(d.worldFilter));
            std::string f = ToLower(d.worldFilter);
            if (ImGui::BeginTable("##wm", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Mesh");
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48);
                ImGui::TableHeadersRow();
                for (size_t gi = 0; gi < w.groups.size(); gi++) {
                    auto& g = w.groups[gi];
                    if (!f.empty() && ToLower(g.mesh).find(f) == std::string::npos) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)gi);
                    std::string name = g.mesh.substr(g.mesh.find_last_of('.') + 1);
                    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(0) && !g.instances.empty()) {   // fly to the first instance
                            Vec3 t = g.instances[0].transform.Translation();
                            cam.target = t;
                            cam.distance = 1500;
                        }
                    }
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Open mesh")) {
                            i64 e = m_api->ResolveFile(g.mesh);
                            if (e >= 0) OpenFile((u32)e);
                        }
                        if (ImGui::MenuItem("Copy path")) CopyToClipboard(g.mesh);
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s%s\nDouble-click: fly to it, right-click: open", g.mesh.c_str(), g.materials.empty() ? "" : "\n(material overrides)");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%zu", g.instances.size());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Levels")) {
            for (auto& l : w.levels) {
                ImGui::TextUnformatted(l.path.substr(l.path.find_last_of('/') + 1).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.path.c_str());
                ImGui::TextDisabled("   %u actors, %u instances%s%s", l.actors, l.instances,
                                    l.landscapeComponents ? Fmt(", %u landscape", l.landscapeComponents).c_str() : "",
                                    l.unresolved ? Fmt(", %u unresolved", l.unresolved).c_str() : "");
            }
            if (!w.unresolved.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Unresolved components (%zu)", w.unresolved.size());
                for (size_t i = 0; i < w.unresolved.size() && i < 200; i++) ImGui::TextDisabled("  %s", w.unresolved[i].c_str());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void App::DrawMaterial(Document& d) {
    auto& m = d.material;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float viewW = std::min(avail.x * 0.5f, avail.y);
    ImGui::BeginChild("##matinfo", ImVec2(avail.x - viewW - ImGui::GetStyle().ItemSpacing.x, avail.y));
    ImGui::TextDisabled("Blend");
    ImGui::SameLine(110);
    ImGui::Text("%s%s", m.blendMode.c_str(), m.twoSided ? ", two sided" : "");
    if (!m.shadingModel.empty()) { ImGui::TextDisabled("Shading"); ImGui::SameLine(110); ImGui::Text("%s", m.shadingModel.c_str()); }
    ImGui::TextDisabled("Chain");
    for (size_t i = 0; i < m.chain.size(); i++) {
        if (i == 0) ImGui::SameLine(110);
        else ImGui::SetCursorPosX(110);
        ImGui::TextUnformatted(m.chain[i].c_str());
    }
    ImGui::Separator();
    ImGui::TextDisabled("Textures");
    float thumb = 72;
    for (size_t i = 0; i < m.textures.size(); i++) {
        auto& t = m.textures[i];
        ImGui::PushID((int)i);
        std::shared_ptr<GpuTexture> gt;
        for (auto& [n, tex] : d.materialTextures) if (n == t.name) { gt = tex; break; }
        if (gt) {
            ImGui::Image((ImTextureID)(intptr_t)gt->srvRaw.Get(), ImVec2(thumb, thumb));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Image((ImTextureID)(intptr_t)gt->srvRaw.Get(), ImVec2(320, 320));
                ImGui::EndTooltip();
            }
        } else {
            ImGui::Dummy(ImVec2(thumb, thumb));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s", t.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", ClassifyTexture(t.name == "<referenced>" ? "" : t.name, t.texturePath).c_str());
        ImGui::TextDisabled("%s", t.texturePath.c_str());
        if (ImGui::SmallButton("Open")) {
            i64 e = m_api->ResolveFile(t.texturePath);
            if (e >= 0) OpenFile((u32)e);
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (!m.scalars.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Scalars");
        for (auto& s : m.scalars) ImGui::Text("%s = %g", s.name.c_str(), s.value);
    }
    if (!m.vectors.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Vectors");
        for (auto& v : m.vectors) {
            ImGui::ColorButton(v.name.c_str(), ImVec4(v.r, v.g, v.b, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
            ImGui::SameLine();
            ImGui::Text("%s = (%.3f, %.3f, %.3f, %.3f)", v.name.c_str(), v.r, v.g, v.b, v.a);
        }
    }
    if (!m.staticSwitches.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Static switches");
        for (auto& s : m.staticSwitches) ImGui::TextUnformatted(s.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    DrawViewport(*m_renderer, d, ImVec2(viewW, viewW), false);
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- export

void App::ExportFiles(std::vector<u32> entries, int mode) {
    if (m_mountState != MountState::Ready || entries.empty()) return;
    if (m_exporting) {
        LOGW("an export is already running");
        return;
    }
    m_exporting = true;
    m_exportCancel = false;
    m_exportDone = 0;
    m_exportFailed = 0;
    m_exportTotal = (int)entries.size();
    ExportOptions opt = MakeExportOptions();
    m_jobs->Push([this, entries = std::move(entries), mode, opt] {
        auto t0 = std::chrono::steady_clock::now();
        size_t files = 0;
        std::string lastWritten;
        for (u32 e : entries) {
            if (m_exportCancel) break;
            std::string path(m_api->vfs.Path(e));
            {
                std::lock_guard lk(m_exportMutex);
                m_exportLast = path;
            }
            std::string err;
            std::vector<std::string> written;
            bool ok = true;
            if (mode == 1) {
                Bytes data;
                ok = m_api->vfs.Read(e, data, &err);
                if (ok) {
                    std::string rel = opt.keepDirectories ? path : path.substr(path.rfind('/') + 1);
                    std::wstring out = opt.outputDir + L"\\" + Widen(rel);
                    for (auto& c : out) if (c == L'/') c = L'\\';
                    CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
                    ok = WriteWholeFile(out, data.data(), data.size());
                    if (ok) written.push_back(Narrow(out));
                }
            } else if (IsPackageFile(path)) {
                auto pkg = m_api->cache->Load(e, &err);
                if (!pkg) ok = false;
                else {
                    ExportOptions o = opt;
                    o.json = mode == 2;
                    ok = ExportPackage(*m_api->cache, *pkg, o, written, &err);
                    if (ok && written.empty() && mode == 0 && entries.size() == 1) {
                        // nothing visual to export for this type: save its properties instead
                        o.json = true;
                        ok = ExportPackage(*m_api->cache, *pkg, o, written, &err);
                    }
                }
            }
            if (!ok) {
                m_exportFailed++;
                LOGW("export %s: %s", path.c_str(), err.c_str());
            }
            files += written.size();
            if (!written.empty()) lastWritten = written.back();
            m_exportDone++;
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        LOGI("exported %zu file(s) from %d asset(s) in %.1f s%s%s", files, m_exportDone.load(), secs, m_exportFailed ? Fmt(", %d failed", m_exportFailed.load()).c_str() : "",
             lastWritten.empty() ? "" : (" -> " + lastWritten).c_str());
        m_exporting = false;
        Post([] {});
    });
}

void App::DrawExportProgress() {
    if (!m_exporting || m_exportTotal <= 1) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20, vp->WorkPos.y + vp->WorkSize.y - 50), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(420, 0));
    ImGui::Begin("Exporting", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    int done = m_exportDone, total = m_exportTotal;
    ImGui::ProgressBar(total ? (float)done / total : 0, ImVec2(-FLT_MIN, 0), Fmt("%d / %d", done, total).c_str());
    {
        std::lock_guard lk(m_exportMutex);
        ImGui::TextDisabled("%s", m_exportLast.c_str());
    }
    if (m_exportFailed) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%d failed (see log)", m_exportFailed.load());
    if (ImGui::Button("Cancel")) m_exportCancel = true;
    ImGui::End();
}

// ---------------------------------------------------------------- log / status / settings

void App::DrawLog(float height) {
    LogSnapshot(m_log, m_logSeen);
    if (m_log.size() > 5000) m_log.erase(m_log.begin(), m_log.begin() + (m_log.size() - 4000));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(38, 38, 48, 255));
    ImGui::BeginChild("##log", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(monoFont, monoFont->LegacySize);   // size 0 would keep the UI size, which makes Consolas look oversized
    ImGuiListClipper clip;
    clip.Begin((int)m_log.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            auto& l = m_log[i];
            ImVec4 tag = l.level == LogLevel::Error ? ImVec4(1, 0.4f, 0.4f, 1) : l.level == LogLevel::Warn ? ImVec4(1, 0.75f, 0.3f, 1) : ImVec4(0.45f, 0.62f, 1, 1);
            ImGui::TextColored(tag, "[%s]", l.level == LogLevel::Error ? "ERR" : l.level == LogLevel::Warn ? "WRN" : "INF");
            ImGui::SameLine();
            ImGui::TextUnformatted(l.text.c_str());
        }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    {   // bottom-right corner of the visible area: open the output folder, clear the log
        // placed inside the visible content rect (scroll + padding aware) so they never grow the scrollable area
        float u = ImGui::GetFontSize() / 16.f, bs = ImGui::GetFrameHeight();
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        ImVec2 pad = ImGui::GetStyle().WindowPadding;
        float x = win->InnerRect.Max.x - pad.x - bs;
        float y = win->InnerRect.Max.y - pad.y - bs * 2 - 2 * u;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 ic = IM_COL32(225, 226, 235, 255);
        auto button = [&](const char* id, float by, const char* tip) {
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            bool r = ImGui::InvisibleButton(id, ImVec2(bs, bs));
            if (ImGui::IsItemHovered()) {
                dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(59, 61, 74, 255), 3 * u);
                ImGui::SetTooltip("%s", tip);
            }
            return r;
        };
        if (button("##openout", y, "Open the output folder")) {
            CreateDirectories(m_settings.outputDir);
            ShellExecuteW(nullptr, L"open", m_settings.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        {   // folder with an arrow
            ImVec2 c(x + bs * 0.5f, y + bs * 0.5f);
            float w = bs * 0.5f, h = bs * 0.38f;
            dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f - 2 * u), ImVec2(c.x - w * 0.1f, c.y - h * 0.5f), ic, 1.5f * u);
            dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), ic, 1.5f * u);
            ImU32 bg = IM_COL32(38, 38, 48, 255);
            dl->AddLine(ImVec2(c.x - w * 0.18f, c.y + h * 0.05f), ImVec2(c.x + w * 0.22f, c.y + h * 0.05f), bg, 1.6f * u);
            dl->AddTriangleFilled(ImVec2(c.x + w * 0.3f, c.y + h * 0.05f), ImVec2(c.x + w * 0.1f, c.y - h * 0.22f), ImVec2(c.x + w * 0.1f, c.y + h * 0.32f), bg);
        }
        float y2 = y + bs + 2 * u;
        if (button("##clearlog", y2, "Clear the log")) m_log.clear();
        {   // trash can
            ImVec2 c(x + bs * 0.5f, y2 + bs * 0.5f);
            float w = bs * 0.34f, h = bs * 0.4f;
            dl->AddRectFilled(ImVec2(c.x - w * 0.65f, c.y - h * 0.62f), ImVec2(c.x + w * 0.65f, c.y - h * 0.48f), ic, 1 * u);
            dl->AddRectFilled(ImVec2(c.x - w * 0.2f, c.y - h * 0.75f), ImVec2(c.x + w * 0.2f, c.y - h * 0.55f), ic, 1 * u);
            dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.38f), ImVec2(c.x + w * 0.5f, c.y + h * 0.6f), ic, 1.5f * u);
            ImU32 bg = IM_COL32(38, 38, 48, 255);
            for (int k = -1; k <= 1; k++) dl->AddLine(ImVec2(c.x + k * w * 0.22f, c.y - h * 0.2f), ImVec2(c.x + k * w * 0.22f, c.y + h * 0.42f), bg, 1.2f * u);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::DrawStatusBar() {
    // FModel-style bar: blue when idle, orange while busy, red on failure
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight() + 2, w = ImGui::GetContentRegionAvail().x;
    bool busy = m_mountState == MountState::Mounting || m_jobs->Pending() > 0;
    ImU32 col = m_mountState == MountState::Failed ? IM_COL32(190, 55, 50, 255) : busy ? IM_COL32(200, 115, 20, 255) : IM_COL32(31, 107, 214, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col);
    std::string left = m_mountState == MountState::Mounting ? "Loading archives..." : m_mountState == MountState::Failed ? "Not loaded: " + m_mountError : busy ? "Working..." : "Completed";
    std::string right;
    if (m_mountState == MountState::Ready) right = Fmt("%zu archives | %zu files | ", m_api->vfs.Paks().size(), m_api->vfs.FileCount());
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) right += "RAM " + HumanSize(pmc.PrivateUsage);
    if (m_assets) right += " | textures " + HumanSize(m_assets->TextureBytes());
    float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
    float rightX = p.x + w - ImGui::CalcTextSize(right.c_str()).x - 10;
    float leftW = ImGui::CalcTextSize(left.c_str()).x;
    if (rightX < p.x + 8 + leftW + 20) right.clear();   // narrow window: the status (e.g. a mount error) wins over the stats
    dl->PushClipRect(p, ImVec2(right.empty() ? p.x + w - 4 : rightX - 12, p.y + h), true);
    dl->AddText(ImVec2(p.x + 8, ty), IM_COL32(255, 255, 255, 240), left.c_str());
    dl->PopClipRect();
    if (!right.empty()) dl->AddText(ImVec2(rightX, ty), IM_COL32(255, 255, 255, 240), right.c_str());
    if (m_mountState == MountState::Failed && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + h))) ImGui::SetTooltip("%s", m_mountError.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

static bool BrowseFolder(char* buf, size_t size, const wchar_t* title) {
    BROWSEINFOW bi = {};
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH];
    bool ok = SHGetPathFromIDListW(pidl, path) != 0;
    CoTaskMemFree(pidl);
    if (ok) snprintf(buf, size, "%s", Narrow(path).c_str());
    return ok;
}

void App::DrawSettings() {
    static bool initialised = false;
    static char keyBuf[8192];
    if (!ImGui::IsPopupOpen("Settings")) {
        ImGui::OpenPopup("Settings");
        initialised = false;
    }
    if (!initialised) {
        snprintf(m_editGameDir, sizeof(m_editGameDir), "%s", Narrow(m_settings.gameDir).c_str());
        snprintf(m_editOutputDir, sizeof(m_editOutputDir), "%s", Narrow(m_settings.outputDir).c_str());
        snprintf(m_editOodle, sizeof(m_editOodle), "%s", Narrow(m_settings.oodlePath).c_str());
        m_editKeys.clear();
        for (auto& k : m_settings.aesKeys) m_editKeys += k + "\n";
        snprintf(keyBuf, sizeof(keyBuf), "%s", m_editKeys.c_str());
        m_settingsMessage.clear();
        initialised = true;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Settings", &open)) {
        if (!open) m_showSettings = false;
        return;
    }
    float lw = 150;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(lw);
    };
    row("Game directory");
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##game", m_editGameDir, sizeof(m_editGameDir));
    ImGui::SameLine();
    if (ImGui::Button("Browse##g")) BrowseFolder(m_editGameDir, sizeof(m_editGameDir), L"Sea of Thieves folder (contains Athena\\Content\\Paks)");
    row("Output directory");
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##out", m_editOutputDir, sizeof(m_editOutputDir));
    ImGui::SameLine();
    if (ImGui::Button("Browse##o")) BrowseFolder(m_editOutputDir, sizeof(m_editOutputDir), L"Export folder");
    row("Oodle DLL");
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##oodle", m_editOodle, sizeof(m_editOodle));
    row("AES keys");
    ImGui::InputTextMultiline("##keysedit", keyBuf, sizeof(keyBuf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5));
    ImGui::SetCursorPosX(lw);
    ImGui::TextDisabled("one 0x... key per line (all keys are tried per archive)");
    ImGui::Separator();
    row("Mesh format");
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##mf", &m_settings.meshFormat, "glTF binary (.glb)\0Wavefront (.obj)\0");
    row("Texture format");
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##tf", &m_settings.textureFormat, "PNG\0DDS (original blocks)\0");
    row("Preview textures");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("##pt", &m_settings.previewMaxTexture, 256, 8192, "max %d px", ImGuiSliderFlags_Logarithmic);
    row("");
    ImGui::Checkbox("Keep folder structure when exporting", &m_settings.exportKeepDirectories);
    row("");
    ImGui::Checkbox("Show grid in model viewer", &m_settings.showGrid);
    ImGui::Separator();
    if (ImGui::Button("Import from FModel")) {
        Settings tmp = m_settings;
        tmp.gameDir.clear();
        tmp.oodlePath.clear();
        tmp.aesKeys.clear();
        std::string r = tmp.ImportFromFModel();
        if (r.empty()) m_settingsMessage = "FModel settings not found (%APPDATA%\\FModel\\AppSettings.json)";
        else {
            m_settingsMessage = r;
            if (!tmp.gameDir.empty()) snprintf(m_editGameDir, sizeof(m_editGameDir), "%s", Narrow(tmp.gameDir).c_str());
            if (!tmp.oodlePath.empty()) snprintf(m_editOodle, sizeof(m_editOodle), "%s", Narrow(tmp.oodlePath).c_str());
            std::string keys;
            for (auto& k : tmp.aesKeys) keys += k + "\n";
            if (!keys.empty()) snprintf(keyBuf, sizeof(keyBuf), "%s", keys.c_str());
        }
    }
    ImGui::SameLine();
    bool apply = ImGui::Button("Save && reload archives");
    ImGui::SameLine();
    bool save = ImGui::Button("Save");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_settings.Load();
        ImGui::CloseCurrentPopup();
        m_showSettings = false;
    }
    if (!m_settingsMessage.empty()) ImGui::TextWrapped("%s", m_settingsMessage.c_str());
    if (apply || save) {
        bool changed = Widen(m_editGameDir) != m_settings.gameDir || Widen(m_editOodle) != m_settings.oodlePath;
        m_settings.gameDir = Widen(m_editGameDir);
        m_settings.outputDir = Widen(m_editOutputDir);
        m_settings.oodlePath = Widen(m_editOodle);
        std::vector<std::string> keys;
        std::string kb = keyBuf;
        size_t p = 0;
        while (p < kb.size()) {
            size_t e = kb.find_first_of("\r\n", p);
            if (e == std::string::npos) e = kb.size();
            std::string k = kb.substr(p, e - p);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            if (!k.empty()) keys.push_back(k);
            p = e + 1;
        }
        changed |= keys != m_settings.aesKeys;
        m_settings.aesKeys = keys;
        m_settings.Save();
        if (m_assets) m_assets->SetMaxTextureSize(std::min(m_settings.previewMaxTexture, 1024));
        ImGui::CloseCurrentPopup();
        m_showSettings = false;
        if ((apply || changed || m_mountState != MountState::Ready) && m_mountState != MountState::Mounting) {
            m_exportCancel = true;
            m_jobs->WaitIdle();
            m_docs.clear();
            m_assets.reset();
            m_api->Unmount();
            m_searchResults.clear();
            m_lastSearch.clear();
            StartMount();
        }
    }
    ImGui::EndPopup();
}
