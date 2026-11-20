#include "ui/App.h"
#include "ue/AssetJson.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>
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
    LOGI("settings: %s", Narrow(Settings::Path()).c_str());
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

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) m_focusSearch = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W) && m_activeDoc) {
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(), [&](auto& d) { return d->id == m_activeDoc; }), m_docs.end());
        m_activeDoc = 0;
        if (m_jobs->Pending() == 0 && m_assets) m_assets->Trim();
    }

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
    float logH = m_showLog ? ImGui::GetTextLineHeightWithSpacing() * 7 : 0;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float mainH = avail.y - statusH - logH;
    float cellH = mainH - ImGui::GetStyle().CellPadding.y * 2;

    if (ImGui::BeginTable("##layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, mainH))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("docs", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGui::BeginChild("##left", ImVec2(0, cellH), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (ImGui::BeginTabBar("##lefttabs")) {
            if (ImGui::BeginTabItem("Folders")) {
                ImGui::BeginChild("##tree");
                if (m_mountState == MountState::Ready) {
                    auto& folders = m_api->vfs.Folders();
                    if (!folders.empty())
                        for (u32 c : folders[0].folders) DrawFolderTree(c);
                } else if (m_mountState == MountState::Mounting) {
                    ImGui::TextDisabled("Loading archives...");
                } else {
                    ImGui::TextWrapped("Archives are not loaded. Open Settings to choose the game folder and AES keys.");
                    if (!m_mountError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", m_mountError.c_str());
                    if (ImGui::Button("Settings...")) m_showSettings = true;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Archives")) {
                DrawArchives();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##files", ImVec2(0, cellH), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawFileList();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##docs", ImVec2(0, cellH), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawDocuments();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndTable();
    }
    if (m_showLog) DrawLog();
    DrawStatusBar();
    ImGui::End();

    if (m_showSettings) DrawSettings();
    DrawExportProgress();
}

void App::DrawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Settings...")) m_showSettings = true;
        if (ImGui::MenuItem("Reload archives", nullptr, false, m_mountState != MountState::Mounting)) {
            m_exportCancel = true;
            m_jobs->WaitIdle();
            m_docs.clear();
            m_assets.reset();
            m_api->Unmount();
            m_searchResults.clear();
            m_lastSearch.clear();
            StartMount();
        }
        if (ImGui::MenuItem("Open output folder")) {
            CreateDirectories(m_settings.outputDir);
            ShellExecuteW(nullptr, L"open", m_settings.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) m_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Log", nullptr, &m_showLog);
        if (ImGui::MenuItem("Close all tabs")) {
            m_docs.clear();
            if (m_jobs->Pending() == 0 && m_assets) m_assets->Trim();
        }
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
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (f.folders.empty()) fl |= ImGuiTreeNodeFlags_Leaf;
    if (m_folder == idx) fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID((int)idx);
    bool open = ImGui::TreeNodeEx("##n", fl, "%s", f.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_folder = idx;
        m_search[0] = 0;
    }
    if (ImGui::BeginPopupContextItem()) {
        ImGui::TextDisabled("%s (%u files)", f.name.c_str(), f.totalFiles);
        ImGui::Separator();
        if (ImGui::MenuItem("Export folder (models / textures)")) ExportFiles(FolderFilesRecursive(idx), 0);
        if (ImGui::MenuItem("Save folder properties (.json)")) ExportFiles(FolderFilesRecursive(idx), 2);
        if (ImGui::MenuItem("Export folder raw (.uasset)")) ExportFiles(FolderFilesRecursive(idx), 1);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u", f.totalFiles);
    if (open) {
        for (u32 c : f.folders) DrawFolderTree(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void App::DrawArchives() {
    if (m_mountState != MountState::Ready) {
        ImGui::TextDisabled("No archives loaded");
        return;
    }
    auto& paks = m_api->vfs.Paks();
    ImGui::TextDisabled("%zu archives, %zu files", paks.size(), m_api->vfs.FileCount());
    if (ImGui::BeginTable("##paks", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Archive");
        ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)paks.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(paks[i]->name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", paks[i]->fileCount);
                ImGui::TableNextColumn();
                int k = paks[i]->keyIndex;
                if (k >= 0) ImGui::Text("#%d", k);
                else ImGui::TextDisabled("-");
            }
        ImGui::EndTable();
    }
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
                }
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
        }
        d->loading = false;
        Post([] {});
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
    if (m_docs.empty()) {
        ImVec2 a = ImGui::GetContentRegionAvail();
        const char* msg = m_mountState == MountState::Ready ? "Double-click a file to open it" : "Loading...";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((a.x - ts.x) * 0.5f, a.y * 0.45f));
        ImGui::TextDisabled("%s", msg);
        return;
    }
    if (!ImGui::BeginTabBar("##docs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) return;
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
    if (close) {
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(), [&](auto& d) { return d->id == close; }), m_docs.end());
        if (m_activeDoc == close) m_activeDoc = 0;
        if (m_jobs->Pending() == 0 && m_assets) m_assets->Trim();
    }
}

void App::DrawDocument(Document& d) {
    if (d.loading) {
        ImGui::TextDisabled("Loading %s ...", d.gamePath.c_str());
        return;
    }
    // header
    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1, 1), "%s", d.className.empty() ? "?" : d.className.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", d.gamePath.c_str());
    float bw = ImGui::CalcTextSize("Export").x + ImGui::CalcTextSize("Save JSON").x + ImGui::CalcTextSize("Copy path").x + ImGui::GetStyle().FramePadding.x * 6 +
               ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - bw));
    if (ImGui::SmallButton("Export")) ExportFiles({d.entry}, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Save JSON")) ExportFiles({d.entry}, 2);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy path")) CopyToClipboard(GamePathToPackage(d.gamePath));
    if (!d.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.error.c_str());

    if (!ImGui::BeginTabBar("##views")) return;
    auto tab = [&](const char* name, DocView v) {
        ImGuiTabItemFlags fl = (d.selectPreferred && d.preferred == v) ? ImGuiTabItemFlags_SetSelected : 0;
        return ImGui::BeginTabItem(name, nullptr, fl);
    };
    if (d.texture && tab("Texture", DocView::Texture)) { DrawTexture(d); ImGui::EndTabItem(); }
    if (d.hasMesh && tab("Model", DocView::Model)) { DrawModel(d); ImGui::EndTabItem(); }
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

static void DrawViewport(Renderer& r, Document& d, ImVec2 size, bool allowFly) {
    if (size.x < 8 || size.y < 8 || !d.gpuMesh) return;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    Camera& cam = d.camera;
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
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) cam.Frame(d.gpuMesh->boundsMin, d.gpuMesh->boundsMax);

    r.BeginFrame((int)size.x, (int)size.y, cam, d.draw);
    if (d.draw.grid) {
        float rad = std::max(1.f, d.gpuMesh->radius);
        float cell = powf(10.f, floorf(log10f(rad)));
        r.DrawGrid(cell / 10, 50);
    }
    Mat4 id = Mat4::Identity();
    r.DrawMesh(*d.gpuMesh, &id, 1);
    if (!d.boneLines.empty()) r.DrawLines(d.boneLines, 0xff40d0ffu);
    r.EndFrame();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(intptr_t)r.Output(), p0, ImVec2(p0.x + size.x, p0.y + size.y));
}

void App::DrawModel(Document& d) {
    auto& m = *d.mesh;
    if (m.lods.size() > 1) {
        ImGui::SetNextItemWidth(90);
        std::string cur = Fmt("LOD %d", d.lod);
        if (ImGui::BeginCombo("##lod", cur.c_str())) {
            for (int i = 0; i < (int)m.lods.size(); i++)
                if (ImGui::Selectable(Fmt("LOD %d  (%zu tris)", i, m.lods[i].indices.size() / 3).c_str(), d.lod == i) && d.lod != i) {
                    d.lod = i;
                    RebuildMeshLod(d);
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::Checkbox("Textures", &d.draw.textures);
    ImGui::SameLine();
    ImGui::Checkbox("Normal maps", &d.draw.normalMaps);
    ImGui::SameLine();
    ImGui::Checkbox("Vertex colors", &d.draw.vertexColors);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &d.draw.wireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &d.draw.grid);
    ImGui::SameLine();
    ImGui::Checkbox("Cull", &d.draw.backfaceCulling);
    ImGui::SameLine();
    if (ImGui::Button("Frame")) d.camera.Frame(d.gpuMesh->boundsMin, d.gpuMesh->boundsMax);

    const MeshLod& l = m.lods[std::clamp(d.lod, 0, (int)m.lods.size() - 1)];
    ImGui::TextDisabled("%u vertices, %zu triangles, %d UV set(s), %zu section(s) | %.1f x %.1f x %.1f cm", l.VertexCount(), l.indices.size() / 3, l.numUVs,
                        l.sections.size(), m.boundsMax.x - m.boundsMin.x, m.boundsMax.y - m.boundsMin.y, m.boundsMax.z - m.boundsMin.z);
    float matW = std::min(320.f, ImGui::GetContentRegionAvail().x * 0.3f);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    DrawViewport(*m_renderer, d, ImVec2(avail.x - matW - ImGui::GetStyle().ItemSpacing.x, avail.y), true);
    ImGui::SameLine();
    ImGui::BeginChild("##mats", ImVec2(matW, avail.y));
    ImGui::TextDisabled("Materials");
    for (size_t i = 0; i < m.materials.size(); i++) {
        auto& s = m.materials[i];
        ImGui::PushID((int)i);
        ImGui::Text("[%zu] %s", i, s.name.c_str());
        if (!s.materialPath.empty()) {
            ImGui::TextDisabled("%s", s.materialPath.c_str());
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
    DrawViewport(*m_renderer, d, ImGui::GetContentRegionAvail(), true);
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

void App::DrawLog() {
    LogSnapshot(m_log, m_logSeen);
    if (m_log.size() > 5000) m_log.erase(m_log.begin(), m_log.begin() + (m_log.size() - 4000));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.1f, 0.12f, 1));
    ImGui::BeginChild("##log", ImVec2(0, ImGui::GetContentRegionAvail().y - (ImGui::GetFrameHeight() + 2 + ImGui::GetStyle().ItemSpacing.y * 2)), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(monoFont, 0);
    ImGuiListClipper clip;
    clip.Begin((int)m_log.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            auto& l = m_log[i];
            ImVec4 c = l.level == LogLevel::Error ? ImVec4(1, 0.45f, 0.4f, 1) : l.level == LogLevel::Warn ? ImVec4(1, 0.8f, 0.4f, 1) : ImVec4(0.75f, 0.78f, 0.84f, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::DrawStatusBar() {
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
    switch (m_mountState) {
    case MountState::Mounting: ImGui::TextColored(ImVec4(0.5f, 0.75f, 1, 1), "Loading archives..."); break;
    case MountState::Failed: ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Not loaded: %s", m_mountError.c_str()); break;
    case MountState::Ready:
        ImGui::TextDisabled("%zu archives | %zu files | mounted in %.2f s", m_api->vfs.Paks().size(), m_api->vfs.FileCount(), m_mountSeconds);
        break;
    default: break;
    }
    ImGui::SameLine();
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        ImGui::TextDisabled("| RAM %s", HumanSize(pmc.PrivateUsage).c_str());
    if (m_assets) {
        ImGui::SameLine();
        ImGui::TextDisabled("| textures %s", HumanSize(m_assets->TextureBytes()).c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| output: %s", Narrow(m_settings.outputDir).c_str());
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
