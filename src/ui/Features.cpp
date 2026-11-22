// FModel features on top of the main window: package metadata, archives info, find references (background index of
// package imports, cached on disk), favourite directories, search options, explorer texture thumbnails, copy image and
// backups (file list snapshots compared against the current archives).
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "core/Json.h"
#include "ue/Texture.h"
#include "ue/Blueprint.h"
#include "render/ShaderTools.h"
#include "ue/Material.h"
#include "app/WwiseEvents.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <regex>
#include <ctime>

// ------------------------------------------------------------------ documents holding plain text

void App::PlaceDocument(std::shared_ptr<Document> d, bool newTab) {
    auto it = std::find_if(m_docs.begin(), m_docs.end(), [&](auto& x) { return x->id == m_activeDoc; });
    if (!newTab && it != m_docs.end()) {
        d->id = (*it)->id;
        (*it)->cancel = true;
        *it = d;
    } else if (newTab && it != m_docs.end() && (*it)->newTab) {
        d->id = (*it)->id;
        *it = d;
    } else {
        if (m_docs.size() >= 25) return;
        d->id = m_nextDocId++;
        m_docs.push_back(d);
    }
    m_activeDoc = d->id;
    m_explorerVisible = false;
}

void App::OpenTextTab(const std::string& title, std::string text, u32 entry, bool newTab, int syntax) {
    auto d = std::make_shared<Document>();
    d->entry = entry;
    d->name = title;
    d->gamePath = entry != UINT32_MAX && entry < m_api->vfs.FileCount() ? std::string(m_api->vfs.Path(entry)) : title;
    d->json = std::move(text);
    d->syntax = syntax;
    d->lines.push_back(0);
    for (u32 i = 0; i < (u32)d->json.size(); i++) if (d->json[i] == '\n') d->lines.push_back(i + 1);
    d->loading = false;
    PlaceDocument(d, newTab);
}

static std::string Hex(u64 v) { return Fmt("0x%llX", (unsigned long long)v); }

// Show Metadata: the package header (summary, name / import / export maps) instead of the properties
void App::ShowMetadata(u32 entry, bool newTab) {
    if (m_mountState != MountState::Ready) return;
    std::string title = std::string(m_api->vfs.FileName(entry)) + " (Metadata)";
    m_jobs->Push([this, entry, newTab, title] {
        std::string err;
        auto pkg = m_api->cache->Load(entry, &err);
        JsonWriter w;
        if (!pkg) {
            w.BeginObject();
            w.KV("Error", err);
            w.EndObject();
        } else {
            const auto& s = pkg->Summary();
            w.BeginObject();
            w.Key("Summary");
            w.BeginObject();
            w.KV("LegacyFileVersion", (i64)s.legacyVersion);
            w.KV("FileVersionUE3", (i64)s.ue3Version);
            w.KV("FileVersionUE4", (i64)s.ue4Version);
            w.KV("FileVersionLicenseeUE4", (i64)s.licenseeVersion);
            w.KV("TotalHeaderSize", (i64)s.totalHeaderSize);
            w.KV("PackageFlags", Hex(s.packageFlags));
            w.KV("NameCount", (i64)s.nameCount);
            w.KV("NameOffset", (i64)s.nameOffset);
            w.KV("ImportCount", (i64)s.importCount);
            w.KV("ImportOffset", (i64)s.importOffset);
            w.KV("ExportCount", (i64)s.exportCount);
            w.KV("ExportOffset", (i64)s.exportOffset);
            w.KV("DependsOffset", (i64)s.dependsOffset);
            w.KV("BulkDataStartOffset", (i64)s.bulkDataStartOffset);
            w.KV("Guid", Fmt("%08X%08X%08X%08X", s.guid.a, s.guid.b, s.guid.c, s.guid.d));
            w.KV("PackageName", pkg->PackageName());
            w.KV("Size", (i64)pkg->Data().size());
            w.EndObject();
            w.Key("NameMap");
            w.BeginArray();
            for (auto& n : pkg->Names()) w.Str(n);
            w.EndArray();
            w.Key("ImportMap");
            w.BeginArray();
            for (size_t i = 0; i < pkg->Imports().size(); i++) {
                const auto& im = pkg->Imports()[i];
                w.BeginObject();
                w.KV("Index", (i64)(-(i64)i - 1));
                w.KV("ObjectName", im.objectName);
                w.KV("ClassPackage", im.classPackage);
                w.KV("ClassName", im.className);
                if (im.outerIndex) w.KV("Outer", pkg->FullPath(im.outerIndex));
                w.KV("FullPath", pkg->FullPath(-(i32)i - 1));
                w.EndObject();
            }
            w.EndArray();
            w.Key("ExportMap");
            w.BeginArray();
            for (size_t i = 0; i < pkg->Exports().size(); i++) {
                const auto& ex = pkg->Exports()[i];
                w.BeginObject();
                w.KV("Index", (i64)i + 1);
                w.KV("ObjectName", ex.objectName);
                w.KV("Class", pkg->ExportClass(i));
                if (ex.superIndex) w.KV("Super", pkg->FullPath(ex.superIndex));
                if (ex.outerIndex) w.KV("Outer", pkg->FullPath(ex.outerIndex));
                w.KV("ObjectFlags", Hex(ex.objectFlags));
                w.KV("SerialSize", (i64)ex.serialSize);
                w.KV("SerialOffset", (i64)ex.serialOffset);
                w.EndObject();
            }
            w.EndArray();
            w.EndObject();
        }
        std::string text = w.Take();
        Post([this, entry, newTab, title, text] { OpenTextTab(title, text, entry, newTab); });
    });
}

// Directory > Archives Info: every mounted archive as JSON
void App::ShowArchivesInfo() {
    if (m_mountState != MountState::Ready) return;
    JsonWriter w;
    w.BeginArray();
    for (auto& p : m_api->vfs.Paks()) {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        u64 size = GetFileAttributesExW(p->path.c_str(), GetFileExInfoStandard, &fad) ? ((u64)fad.nFileSizeHigh << 32 | fad.nFileSizeLow) : 0;
        w.BeginObject();
        w.KV("Name", p->name);
        w.KV("Path", Narrow(p->path));
        w.KV("MountPoint", p->mountPoint);
        w.KV("Version", (i64)p->version);
        w.KV("FileCount", (i64)p->fileCount);
        w.KV("PatchLevel", (i64)p->patchLevel);
        w.KV("IsEncrypted", p->keyIndex >= 0);
        w.KV("Size", HumanSize(size));
        w.EndObject();
    }
    w.EndArray();
    OpenTextTab("Archives Info", w.Take(), UINT32_MAX, true);
}

// ------------------------------------------------------------------ folders / favourites

u32 App::FolderByPath(std::string_view p) const {
    auto& folders = m_api->vfs.Folders();
    u32 cur = 0;
    size_t pos = 0;
    while (pos < p.size()) {
        size_t sl = std::min(p.find('/', pos), p.size());
        std::string_view part = p.substr(pos, sl - pos);
        u32 next = UINT32_MAX;
        for (u32 c : folders[cur].folders) if (IEquals(folders[c].name, part)) { next = c; break; }
        if (next == UINT32_MAX) break;
        cur = next;
        pos = sl + 1;
    }
    return cur;
}

static std::string PathOfFolder(const std::vector<VfsFolder>& folders, u32 f) {
    std::string out;
    for (; f != UINT32_MAX && f != 0; f = folders[f].parent) out = folders[f].name + (out.empty() ? "" : "/") + out;
    return out;
}

bool App::IsFavorite(u32 folder) const {
    if (m_mountState != MountState::Ready || !folder) return false;
    std::string p = PathOfFolder(m_api->vfs.Folders(), folder);
    for (auto& f : m_settings.favoriteDirs) if (IEquals(f, p)) return true;
    return false;
}

void App::ToggleFavorite(u32 folder) {
    if (m_mountState != MountState::Ready || !folder) return;
    std::string p = PathOfFolder(m_api->vfs.Folders(), folder);
    auto& v = m_settings.favoriteDirs;
    auto it = std::find_if(v.begin(), v.end(), [&](auto& f) { return IEquals(f, p); });
    if (it != v.end()) v.erase(it);
    else v.push_back(p);
    m_settings.Save();
}

// ------------------------------------------------------------------ search (Packages > Search)

void App::UpdateSearch() {
    std::string key = Fmt("%s|%d|%d", m_search, (int)m_searchMatchCase, (int)m_searchRegex);
    if (key == m_lastSearch) return;
    m_lastSearch = key;
    m_searchResults.clear();
    m_searchTruncated = false;
    m_searchError.clear();
    std::string q = m_search;
    if (q.empty() || m_mountState != MountState::Ready) return;
    const Vfs& vfs = m_api->vfs;
    const size_t limit = 20000;
    auto add = [&](u32 i) {
        if (m_searchResults.size() >= limit) { m_searchTruncated = true; return false; }
        m_searchResults.push_back(i);
        return true;
    };
    if (m_searchRegex) {
        std::regex re;
        try {
            re = std::regex(q, m_searchMatchCase ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
        } catch (const std::exception& e) {
            m_searchError = e.what();
            return;
        }
        for (u32 i = 0; i < vfs.FileCount(); i++) {
            std::string_view p = vfs.Path(i);
            if (std::regex_search(p.begin(), p.end(), re) && !add(i)) break;
        }
    } else {
        // space separated terms, all must match; '*' / '?' switch to wildcard mode
        bool wildcard = q.find_first_of("*?") != std::string::npos;
        std::vector<std::string> terms;
        std::string lq = m_searchMatchCase ? q : ToLower(q);
        for (size_t p = 0; p < lq.size();) {
            size_t e = lq.find(' ', p);
            if (e == std::string::npos) e = lq.size();
            if (e > p) terms.push_back(lq.substr(p, e - p));
            p = e + 1;
        }
        std::string buf;
        for (u32 i = 0; i < vfs.FileCount(); i++) {
            std::string_view path = vfs.Path(i);
            bool ok;
            if (wildcard) ok = WildcardMatch(q, path) || WildcardMatch(q, vfs.FileName(i));
            else {
                buf.assign(path);
                if (!m_searchMatchCase) for (auto& c : buf) c = (char)tolower((unsigned char)c);
                ok = true;
                for (auto& t : terms) if (buf.find(t) == std::string::npos) { ok = false; break; }
            }
            if (ok && !add(i)) break;
        }
    }
    std::sort(m_searchResults.begin(), m_searchResults.end(), [&](u32 a, u32 b) { return vfs.Path(a) < vfs.Path(b); });
}

// ------------------------------------------------------------------ find references

static std::wstring DataDir() {
    std::wstring p = Settings::Path();
    return p.substr(0, p.find_last_of(L'\\'));
}

void App::StartReferenceIndex(bool rebuild) {
    if (m_refBuilding || m_mountState != MountState::Ready) return;
    auto idx = std::make_shared<ReferenceIndex>();
    if (!rebuild) {
        if (m_refReady) return;
        if (m_api->references && m_api->references->Ready()) { m_refReady = true; return; }
        if (idx->LoadCache(m_api->vfs)) {
            m_api->references = idx;
            m_refReady = true;
            LOGI("references: loaded the index of %zu packages from cache", idx->Size());
            return;
        }
    }
    m_refBuilding = true;
    m_refDone = 0;
    m_refTotal = 0;
    LOGI(rebuild ? "references: re-indexing every package to record the localised texts they use..."
                 : "references: indexing the imports of every package (once, then cached)...");
    m_refThreads.emplace_back([this, idx] {   // 2 workers: the job pool stays free for documents
        auto keys = m_api->TextKeyHashes();   // also which packages use each localised text
        if (!idx->Build(m_api->vfs, 2, m_shutdown, &m_refDone, &m_refTotal, keys.empty() ? nullptr : &keys)) return;
        idx->SaveCache(m_api->vfs);
        Post([this, idx] {
            if (m_api->references) m_retiredRefs.push_back(m_api->references);
            m_api->references = idx;
            m_refBuilding = false;
            m_refReady = true;
            LOGI("references: index ready (%zu referenced packages)", idx->Size());
        });
    });
}
void App::FindReferences(u32 entry) {
    if (m_mountState != MountState::Ready) return;
    m_refTarget = entry;
    m_showSearch = true;
    m_searchTab = 1;
    StartReferenceIndex();
}

void App::DrawReferencesTab() {
    const Vfs& vfs = m_api->vfs;
    if (m_refTarget == UINT32_MAX || m_refTarget >= vfs.FileCount()) {
        ImGui::TextWrapped("Right-click a package and choose \"Find References\", or select one and press the button below.");
    } else {
        ImGui::TextUnformatted(std::string(vfs.Path(m_refTarget)).c_str());
    }
    if (m_selected >= 0 && (size_t)m_selected < vfs.FileCount() && IsPackageFile(vfs.Path((u32)m_selected)) && (u32)m_selected != m_refTarget) {
        if (ImGui::Button("Use the selected package")) { NavigateReferences((u32)m_selected); StartReferenceIndex(); }
    }
    if (!m_refReady) {
        if (!m_refBuilding) StartReferenceIndex();
        int done = m_refDone, total = std::max(1, m_refTotal.load());
        ImGui::TextDisabled("Indexing package imports (first time only, then cached)...");
        ImGui::ProgressBar((float)done / total, ImVec2(-FLT_MIN, 0), Fmt("%d / %d", done, total).c_str());
        m_busyFrames = 3;
        return;
    }
    if (m_refTarget == UINT32_MAX) return;
    if (ImGui::RadioButton("Graph", m_refGraphView)) m_refGraphView = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("List", !m_refGraphView)) m_refGraphView = false;
    if (m_refGraphView) {
        DrawDependencyGraph();
        return;
    }
    static const std::vector<u32> none;
    const std::vector<u32>* found = m_api->references ? m_api->references->Referencers(m_refTarget) : nullptr;
    const std::vector<u32>& refs = found ? *found : none;
    ImGui::TextDisabled("%zu package(s) reference it", refs.size());
    if (!refs.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Export...")) ImGui::OpenPopup("##refexp");
        if (ImGui::IsItemHovered()) Tip("Export every referencing package (choose what)");
        if (ImGui::BeginPopup("##refexp")) {
            DrawExportResultsMenu(refs);
            ImGui::EndPopup();
        }
    }
    if (ImGui::BeginTable("##refs", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)refs.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                u32 e = refs[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)e);
                std::string path(vfs.Path(e));
                if (ImGui::Selectable(path.c_str(), m_selected == (i64)e, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_selected = e;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        SelectFolder(FolderByPath(path.substr(0, path.rfind('/'))));
                        m_selected = e;
                        OpenFile(e);
                    }
                }
                if (ImGui::BeginPopupContextItem("##rctx")) {
                    DrawFileMenu(e);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", HumanSize(vfs.Entry(e).usize).c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ explorer texture thumbnails

std::shared_ptr<GpuTexture> App::Thumbnail(u32 entry) {
    std::lock_guard lk(m_thumbMutex);
    auto it = m_thumbs.find(entry);
    if (it != m_thumbs.end()) return it->second.state == 2 ? it->second.tex : nullptr;
    if (m_thumbPending >= 6) return nullptr;   // a few at a time: documents keep loading
    m_thumbs[entry].state = 1;
    m_thumbPending++;
    m_jobs->Push([this, entry] {
        std::shared_ptr<GpuTexture> tex;
        std::string err;
        if (auto pkg = m_api->cache->Load(entry, &err)) {
            i32 main = pkg->MainExport();
            TextureInfo info;
            Image img;
            if (main && IsTextureClass(pkg->ExportClass(main - 1)) && ReadTextureInfo(*pkg, main - 1, info, &err) && DecodeTexture(*pkg, info, -1, 160, img, &err))
                tex = m_renderer->CreateTexture(img, info.srgb, false);
        }
        {
            std::lock_guard lk(m_thumbMutex);
            auto& t = m_thumbs[entry];
            t.state = tex ? 2 : 3;
            t.tex = tex;
            m_thumbPending--;
        }
        Post([] {});
    });
    return nullptr;
}

// ------------------------------------------------------------------ copy image (texture inspector)

void App::CopyTextureImage(std::shared_ptr<Document> d) {
    if (!d || !d->pkg) return;
    m_jobs->Push([this, d] {
        Image img;
        std::string err;
        if (!DecodeTexture(*d->pkg, d->tex, -1, 8192, img, &err)) {
            LOGW("copy image: %s", err.c_str());
            return;
        }
        // CF_DIB: bottom-up BGRA
        size_t px = (size_t)img.width * img.height;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + px * 4);
        if (!mem) return;
        auto* hdr = (BITMAPINFOHEADER*)GlobalLock(mem);
        *hdr = {};
        hdr->biSize = sizeof(BITMAPINFOHEADER);
        hdr->biWidth = img.width;
        hdr->biHeight = img.height;
        hdr->biPlanes = 1;
        hdr->biBitCount = 32;
        hdr->biCompression = BI_RGB;
        u8* dst = (u8*)(hdr + 1);
        for (int y = 0; y < img.height; y++) {
            const u8* s = &img.rgba[(size_t)(img.height - 1 - y) * img.width * 4];
            u8* o = dst + (size_t)y * img.width * 4;
            for (int x = 0; x < img.width; x++) {
                o[x * 4 + 0] = s[x * 4 + 2];
                o[x * 4 + 1] = s[x * 4 + 1];
                o[x * 4 + 2] = s[x * 4 + 0];
                o[x * 4 + 3] = s[x * 4 + 3];
            }
        }
        GlobalUnlock(mem);
        bool ok = false;
        if (OpenClipboard((HWND)m_hwnd)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_DIB, mem) != nullptr;
            CloseClipboard();
        }
        if (!ok) GlobalFree(mem);
        LOGI(ok ? "copied %s (%dx%d) to the clipboard" : "could not copy %s", d->name.c_str(), img.width, img.height);
    });
}

// ------------------------------------------------------------------ backups (Directory > Backup)

static std::wstring BackupDir() { return DataDir() + L"\\Backups"; }

void App::DrawBackupWindow() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 22), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Backup Manager", &m_showBackups, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("A backup is a list of every file in the archives with its size. Compare one with the current game files to see "
                       "which packages a patch added, changed or removed.");
    bool ready = m_mountState == MountState::Ready;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Create Backup")) {
        const Vfs& vfs = m_api->vfs;
        std::string out;
        out.reserve(vfs.FileCount() * 80);
        for (u32 i = 0; i < vfs.FileCount(); i++) {
            out.append(vfs.Path(i));
            out += '\t';
            out += std::to_string(vfs.Entry(i).usize);
            out += '\n';
        }
        time_t now = time(nullptr);
        tm lt;
        localtime_s(&lt, &now);
        wchar_t name[64];
        wcsftime(name, 64, L"SOTModel_%Y-%m-%d_%H-%M-%S.sotbkp", &lt);
        CreateDirectories(BackupDir());
        std::wstring path = BackupDir() + L"\\" + name;
        m_backupMessage = WriteWholeFile(path, out.data(), out.size()) ? "Created " + Narrow(name) : "Could not write " + Narrow(path);
        LOGI("%s", m_backupMessage.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open Folder")) {
        CreateDirectories(BackupDir());
        ShellExecuteW(nullptr, L"open", BackupDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (!m_backupMessage.empty()) ImGui::TextDisabled("%s", m_backupMessage.c_str());
    ImGui::Separator();
    // existing backups, newest first
    std::vector<std::pair<std::wstring, u64>> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((BackupDir() + L"\\*.sotbkp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do files.push_back({fd.cFileName, (u64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow});
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (files.empty()) ImGui::TextDisabled("No backups yet");
    if (ImGui::BeginTable("##bk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Backup", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
        for (auto& [name, size] : files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Narrow(name).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", HumanSize(size).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(Narrow(name).c_str());
            ImGui::BeginDisabled(!ready);
            if (ImGui::SmallButton("Compare")) CompareBackup(BackupDir() + L"\\" + name);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void App::CompareBackup(const std::wstring& file) {
    Bytes data;
    if (!ReadWholeFile(file, data)) { LOGW("could not read %s", Narrow(file).c_str()); return; }
    std::unordered_map<std::string, u64> old;
    std::string_view text((const char*)data.data(), data.size());
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        size_t tab = line.find('\t');
        if (tab != std::string_view::npos) old.emplace(ToLower(line.substr(0, tab)), strtoull(std::string(line.substr(tab + 1)).c_str(), nullptr, 10));
        pos = nl + 1;
    }
    const Vfs& vfs = m_api->vfs;
    std::vector<std::string> added, modified, removed;
    std::unordered_map<std::string, bool> seen;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string p(vfs.Path(i)), k = ToLower(p);
        auto it = old.find(k);
        if (it == old.end()) added.push_back(p);
        else {
            if (it->second != vfs.Entry(i).usize) modified.push_back(p);
            seen[k] = true;
        }
    }
    for (auto& [k, sz] : old) if (!seen.count(k)) removed.push_back(k);
    std::sort(added.begin(), added.end());
    std::sort(modified.begin(), modified.end());
    std::sort(removed.begin(), removed.end());
    JsonWriter w;
    w.BeginObject();
    w.KV("Backup", Narrow(file.substr(file.find_last_of(L'\\') + 1)));
    w.KV("NewCount", (i64)added.size());
    w.KV("ModifiedCount", (i64)modified.size());
    w.KV("RemovedCount", (i64)removed.size());
    auto list = [&](const char* k, const std::vector<std::string>& v) {
        w.Key(k);
        w.BeginArray();
        for (auto& s : v) w.Str(s);
        w.EndArray();
    };
    list("New", added);
    list("Modified", modified);
    list("Removed", removed);
    w.EndObject();
    LOGI("backup comparison: %zu new, %zu modified, %zu removed", added.size(), modified.size(), removed.size());
    OpenTextTab("Backup Comparison", w.Take(), UINT32_MAX, true);
}

// ------------------------------------------------------------------ audio (.wem / WwiseBank)

void App::LoadAudioDocument(std::shared_ptr<Document> d) {
    d->loading = true;
    m_jobs->Push([this, d] {
        Bytes data;
        std::string err;
        JsonWriter w;
        w.BeginObject();
        if (!m_api->vfs.Read(d->entry, data, &err)) {
            d->audioError = err;
        } else if (ReadWemInfo(data.data(), data.size(), d->wem, &err)) {
            auto pcm = std::make_shared<AudioPcm>();
            if (DecodeWem(data.data(), data.size(), *pcm, &err)) d->audio = pcm;
            else d->audioError = err;
            w.KV("Codec", d->wem.codecName);
            w.KV("Channels", d->wem.channels);
            w.KV("SampleRate", d->wem.sampleRate);
            if (d->audio) w.KV("Duration", d->audio->Seconds());
            w.KV("Size", HumanSize(data.size()));
        } else {
            d->audioError = err;
        }
        if (!d->audioError.empty()) w.KV("Error", d->audioError);
        w.EndObject();
        d->json = w.Take();
        d->lines.assign(1, 0);
        for (u32 i = 0; i < (u32)d->json.size(); i++) if (d->json[i] == '\n') d->lines.push_back(i + 1);
        d->loading = false;
        Post([] {});
    });
}

void App::FillEventDocument(Document& d, const WwiseEventInfo& ev) {
    d.isBank = d.isEvent = true;
    d.eventName = ev.name.empty() ? std::to_string(ev.id) : ev.name;
    d.eventId = ev.id;
    d.eventBank = ev.bankPath;
    d.bank = BankInfo{};
    d.bank.bankId = ev.bankInfo.bankId;
    d.bank.version = ev.bankInfo.version;
    d.mediaBytes.clear();
    d.mediaWhere.clear();
    d.eventConds.clear();
    d.eventWhen.clear();
    d.eventSwitches.clear();
    for (auto& g : ev.switches) {
        Document::EventSwitch s;
        s.group = g.group;
        s.state = g.state;
        s.name = WwiseNameOrId(*m_api, g.group);
        for (u32 v : g.values) s.values.push_back({v, WwiseNameOrId(*m_api, v)});
        std::sort(s.values.begin(), s.values.end(), [](auto& a, auto& b) { return a.second < b.second; });
        d.eventSwitches.push_back(std::move(s));
    }
    JsonWriter w;
    w.BeginObject();
    w.KV("Event", d.eventName);
    w.KV("WwiseId", (i64)ev.id);
    w.KV("Bank", ev.bankPath);
    w.Key("Sounds");
    w.BeginArray();
    static const char* where[] = {"in another bank", "embedded in the bank", "streamed"};
    for (auto& m : ev.media) {
        Bytes b;
        BankWem bw;
        bw.id = m.id;
        if (m.where != WwiseEventMedia::Missing && ReadEventMedia(*m_api, ev, m, b)) {
            bw.offset = (u32)d.mediaBytes.size();
            bw.size = (u32)b.size();
            d.mediaBytes.insert(d.mediaBytes.end(), b.begin(), b.end());
        }
        d.bank.wems.push_back(bw);
        d.mediaWhere.push_back((u8)m.where);
        d.eventConds.push_back(m.conditions);
        std::string when;
        for (auto& c : m.conditions) {
            when += (when.empty() ? "" : ",  ") + WwiseNameOrId(*m_api, c.group) + " = ";
            for (size_t k = 0; k < c.values.size(); k++) when += (k ? " | " : "") + WwiseNameOrId(*m_api, c.values[k]);
            if (c.values.empty()) when += "(none)";
        }
        d.eventWhen.push_back(when);
        w.BeginObject();
        w.KV("Media", (i64)m.id);
        w.KV("Location", where[m.where]);
        if (!when.empty()) w.KV("When", when);
        if (m.where == WwiseEventMedia::Stream) w.KV("File", std::string(m_api->vfs.Path(m.entry)));
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    if (d.json.empty()) {   // an event of a bank (an event asset keeps its properties)
        d.json = w.Take();
        d.lines.assign(1, 0);
        for (u32 i = 0; i < (u32)d.json.size(); i++) if (d.json[i] == '\n') d.lines.push_back(i + 1);
    }
    d.bankSel = d.bank.wems.empty() ? -1 : 0;
}

void App::OpenBankEvent(Document& bankDoc, u32 eventId) {
    if (!bankDoc.pkg) return;
    auto d = std::make_shared<Document>();
    d->entry = bankDoc.entry;
    std::string name = WwiseEventName(*m_api, eventId);
    d->name = name.empty() ? Fmt("event %u", eventId) : name;
    d->gamePath = bankDoc.gamePath;
    d->className = "WwiseEvent";
    d->loading = true;
    auto pkg = bankDoc.pkg;
    BankInfo info = bankDoc.bank;
    PlaceDocument(d, true);
    m_jobs->Push([this, d, pkg, info, eventId] {
        WwiseEventInfo ev;
        std::string err;
        if (ResolveEventInBank(*m_api, pkg, info, eventId, ev, &err)) FillEventDocument(*d, ev);
        else d->audioError = err;
        d->loading = false;
        Post([] {});
    });
}

bool App::ExportAudioEntry(u32 entry, const ExportOptions& opt, std::vector<std::string>& written, std::string* error) {
    Bytes data;
    if (!m_api->vfs.Read(entry, data, error)) return false;
    std::string rel(m_api->vfs.Path(entry));
    rel = rel.substr(0, rel.rfind('.'));
    if (!opt.keepDirectories) rel = rel.substr(rel.rfind('/') + 1);
    auto write = [&](const AudioPcm& pcm, const std::string& r) {
        Bytes wav = MakeWav(pcm);
        std::wstring out = opt.outputDir + L"\\" + Widen(r + ".wav");
        for (auto& c : out) if (c == L'/') c = L'\\';
        CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
        if (!WriteWholeFile(out, wav.data(), wav.size())) return false;
        written.push_back(Narrow(out));
        return true;
    };
    if (data.size() >= 4 && !memcmp(data.data(), "RIFF", 4)) {
        AudioPcm pcm;
        return DecodeWem(data.data(), data.size(), pcm, error) && write(pcm, rel);
    }
    BankInfo bank;
    if (!ParseBank(data.data(), data.size(), bank, nullptr, error)) return false;
    bool all = true;
    for (auto& m : bank.wems) {
        AudioPcm pcm;
        std::string e;
        if (!DecodeWem(data.data() + m.offset, m.size, pcm, &e) || !write(pcm, rel + "/" + std::to_string(m.id))) {
            all = false;
            if (error) *error = e;
        }
    }
    return all;
}

void App::PlayAudio(Document& d, std::shared_ptr<AudioPcm> pcm, double from) {
    if (!pcm) return;
    if (m_player.Play(pcm, from)) m_playDoc = d.id;
    else LOGW("audio playback failed (no output device?)");
}

// ------------------------------------------------------------------ Decompile Blueprint

void App::DecompileBlueprintTab(u32 entry) {
    if (m_mountState != MountState::Ready) return;
    std::string title = std::string(m_api->vfs.FileName(entry));
    title = title.substr(0, title.rfind('.')) + " (Decompiled)";
    m_jobs->Push([this, entry, title] {
        std::string err, code;
        auto pkg = m_api->cache->Load(entry, &err);
        bool ok = pkg && DecompileBlueprint(*pkg, code, &err);
        Post([this, entry, title, code, ok, err] {
            if (!ok) {
                LOGW("decompile: %s", err.c_str());
                return;
            }
            OpenTextTab(title, code, entry, true, 1);
        });
    });
}
// ------------------------------------------------------------------ Decompile Shaders

void App::DecompileShadersTab(u32 entry) {
    if (m_mountState != MountState::Ready) return;
    std::string path(m_api->vfs.Path(entry)), title(m_api->vfs.FileName(entry));
    title = title.substr(0, title.rfind('.')) + " (Shaders)";
    m_jobs->Push([this, entry, path, title] {
        std::string err;
        auto list = std::make_shared<std::vector<ShaderEntry>>();
        if (IsShaderCacheFile(path) || IEndsWith(path, ".bin")) {
            Bytes data;
            if (m_api->vfs.Read(entry, data, &err)) ScanShaders(data, nullptr, *list);
            if (list->empty() && err.empty()) err = "no shaders found";
        } else if (auto pkg = m_api->cache->Load(entry, &err)) {
            ReadPackageShaders(*pkg, *list, &err);
        }
        Post([this, entry, title, list, err] {
            if (list->empty()) {
                LOGW("shaders: %s", err.c_str());
                return;
            }
            OpenTextTab(title, "", entry, true, 1);
            auto it = std::find_if(m_docs.begin(), m_docs.end(), [&](auto& x) { return x->id == m_activeDoc; });
            if (it == m_docs.end()) return;
            (*it)->shaders = list;
            LOGI("%s: %zu shaders", title.c_str(), list->size());
            ShowShader(*it, 0, 0);
        });
    });
}

void App::ShowShader(std::shared_ptr<Document> d, int index, int mode) {
    if (!d->shaders || index < 0 || index >= (int)d->shaders->size()) return;
    d->shaderSel = index;
    d->shaderMode = mode;
    auto list = d->shaders;
    m_jobs->Push([this, d, list, index, mode] {
        const ShaderEntry& s = (*list)[index];
        std::string text, err;
        bool ok = true;
        if (mode == 2) ok = DisassembleShader(s.dxbc, text, &err);
        else if (mode == 3) {
            ShaderReflectionInfo info;
            ok = ReflectShader(s.dxbc, info, &err);
            if (ok) text = ReflectionToText(info);
        } else ok = DecompileShader(s.dxbc, text, mode == 1, &err);
        std::string head = Fmt("// [%d] %s shader %s\n", index, ShaderFrequencyName(s.frequency), s.type.c_str());
        if (!s.vertexFactory.empty()) head += "// vertex factory: " + s.vertexFactory + "\n";
        if (!s.uniformBuffers.empty()) {
            head += "// uniform buffers:";
            for (auto& u : s.uniformBuffers) head += " " + u;
            head += "\n";
        }
        if (!s.usage.empty()) head += "// " + s.usage + "\n";
        text = head + "\n" + (ok ? text : "// " + err + "\n");
        Post([d, index, mode, text] {
            if (d->shaderSel != index || d->shaderMode != mode) return;   // a newer selection is on its way
            d->json = text;
            d->lines.assign(1, 0);
            for (u32 i = 0; i < (u32)d->json.size(); i++)
                if (d->json[i] == '\n') d->lines.push_back(i + 1);
            d->selA = d->selB = 0;
            d->scrollToLine = 0;
        });
    });
}

// ------------------------------------------------------------------ Material HLSL (graph translated to code + node graph)

void App::MaterialHlslTab(u32 entry) {
    if (m_mountState != MountState::Ready) return;
    std::string path(m_api->vfs.Path(entry)), title(m_api->vfs.FileName(entry));
    title = title.substr(0, title.rfind('.')) + " (HLSL)";
    m_jobs->Push([this, entry, path, title] {
        std::string err, hlsl;
        MaterialGraphStats st;
        bool ok = MaterialToHlsl(*m_api->cache, path, hlsl, &st, &err);
        auto graph = std::make_shared<std::vector<MaterialGraphNode>>();
        if (ok) {
            ReadMaterialGraph(*m_api->cache, path, *graph, nullptr);
            bool hasExpressions = std::any_of(graph->begin(), graph->end(), [](auto& n) { return n.type != "Material"; });
            if (!hasExpressions) {   // an instance: show its master's graph
                MaterialInfo mi;
                if (ReadMaterial(*m_api->cache, path, mi, nullptr) && !mi.baseMaterial.empty())
                    ReadMaterialGraph(*m_api->cache, mi.baseMaterial.substr(0, mi.baseMaterial.find('.')), *graph, nullptr);
            }
        }
        Post([this, entry, title, hlsl, graph, ok, err, st] {
            if (!ok) {
                LOGW("material HLSL: %s", err.c_str());
                return;
            }
            LOGI("%s: %d nodes, %d function calls%s", title.c_str(), st.nodes, st.functions,
                 st.unsupported.empty() ? "" : Fmt(", %zu node types not translated", st.unsupported.size()).c_str());
            OpenTextTab(title, hlsl, entry, true, 1);
            auto it = std::find_if(m_docs.begin(), m_docs.end(), [&](auto& x) { return x->id == m_activeDoc; });
            if (it != m_docs.end() && !graph->empty()) (*it)->matGraph = graph;
        });
    });
}

// ------------------------------------------------------------------ material variants (skins)

// Sibling material instances by name: "wpn_cutlass_pol_01_a" -> wpn_cutlass_pol_01_b / _c and Variation/..._01_a_v01 / _v02.
void App::ListMaterialVariants(Document& d) {
    d.matVariantsListed = true;
    if (!d.mesh) return;
    const Vfs& vfs = m_api->vfs;
    size_t n = d.mesh->materials.size();
    d.matVariants.assign(n, {});
    d.matOverrides.resize(n);
    for (size_t s = 0; s < n; s++) {
        std::string own = d.mesh->materials[s].materialPath;   // /Game/X/name.name
        if (own.empty()) continue;
        std::string stem = ToLower(own.substr(own.rfind('/') + 1));
        stem = stem.substr(0, stem.find('.'));
        std::string prefix = stem + "_";
        static const std::regex lettered("(.*_\\d\\d)_[a-z]");   // name_01_a: every letter of that number
        std::smatch mm;
        if (std::regex_match(stem, mm, lettered)) prefix = mm[1].str() + "_";
        std::vector<std::string> found;
        for (u32 i = 0; i < vfs.FileCount() && found.size() < 64; i++) {
            std::string_view fn = vfs.FileName(i);
            if (fn.size() < prefix.size() || !IStartsWith(fn, prefix) || !IEndsWith(fn, ".uasset")) continue;
            std::string path = ToLower(vfs.Path(i));
            if (path.find("/materials/") == std::string::npos) continue;
            std::string name(fn.substr(0, fn.size() - 7));
            std::string gp = GamePathToPackage(vfs.Path(i));
            found.push_back(gp + "." + name);
        }
        std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return ToLower(a.substr(a.rfind('/'))) < ToLower(b.substr(b.rfind('/'))); });
        if (found.size() > 1 || (found.size() == 1 && !IEquals(found[0], own))) d.matVariants[s] = std::move(found);
    }
}

void App::ApplyMaterialVariants(std::shared_ptr<Document> d) {
    if (!d->gpuMesh || !d->mesh) return;
    auto overrides = d->matOverrides;
    auto base = d->gpuMesh;
    auto mesh = d->mesh;
    m_jobs->Push([this, d, overrides, base, mesh] {
        auto copy = std::make_shared<GpuMesh>(*base);   // the cached mesh stays as the game ships it
        for (size_t s = 0; s < copy->materials.size() && s < mesh->materials.size(); s++) {
            const std::string& p = s < overrides.size() && !overrides[s].empty() ? overrides[s] : mesh->materials[s].materialPath;
            if (!p.empty()) copy->materials[s] = m_assets->Material(p);
        }
        Post([this, d, copy, base] {
            if (d->gpuMesh != base) return;   // the LOD changed meanwhile: that rebuild re-applies them
            d->gpuMesh = copy;
            if (d->draw.graphMaterials) RequestGraphShaders(*d);
        });
    });
}