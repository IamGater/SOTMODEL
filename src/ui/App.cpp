#include "ui/App.h"
#include "ui/UiCommon.h"
#include "ue/AssetJson.h"
#include "ue/Blueprint.h"
#include "ue/Collision.h"
#include "ue/WorldLayout.h"
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

std::string HumanSize(u64 b) {
    if (b < 1024) return Fmt("%llu B", (unsigned long long)b);
    if (b < (1ull << 20)) return Fmt("%.1f KB", b / 1024.0);
    if (b < (1ull << 30)) return Fmt("%.1f MB", b / 1048576.0);
    return Fmt("%.2f GB", b / 1073741824.0);
}

void CopyToClipboard(const std::string& s) { ImGui::SetClipboardText(s.c_str()); }

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

std::string GamePathToPackage(std::string_view p) {
    // Athena/Content/X/y.uasset -> /Game/X/y
    std::string s(p);
    if (IStartsWith(s, "Athena/Content/")) s = "/Game/" + s.substr(15);
    size_t dot = s.rfind('.');
    if (dot != std::string::npos && s.find('/', dot) == std::string::npos) s.resize(dot);
    return s;
}

bool IsPackageFile(std::string_view p) { return IEndsWith(p, ".uasset") || IEndsWith(p, ".umap"); }

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
    m_shutdown = true;   // stops the reference indexer
    for (auto& t : m_refThreads) t.join();
    m_jobs.reset();   // joins workers before the renderer / api go away
    m_docs.clear();
    m_assets.reset();
}

bool App::Busy() const {
    if (m_player.Active() && !m_player.Paused()) return true;   // playhead
    if (m_busyFrames > 0 || m_mountState == MountState::Mounting || m_exporting || m_refBuilding) return true;
    if (m_jobs && m_jobs->Pending() > 0) return true;
    for (auto& d : m_docs) if (d->loading) return true;
    return false;
}

bool App::Loading() const {
    if (m_mountState == MountState::Mounting || m_refBuilding || (m_jobs && m_jobs->Pending() > 0)) return true;
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
            m_readyAt = ImGui::GetTime();
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
            if (m_folder) SelectFolder(m_folder);
            for (auto& p0 : openOnStart) {
                bool shaders = p0.rfind("shaders:", 0) == 0;   // --shaders <path>: open its compiled shaders
                bool hlsl = p0.rfind("hlsl:", 0) == 0;         // --material-hlsl <path>: its graph as HLSL
                std::string p = shaders ? p0.substr(8) : hlsl ? p0.substr(5) : p0;
                i64 e = m_api->ResolveFile(p);
                if (e >= 0 && shaders) DecompileShadersTab((u32)e);
                else if (e >= 0 && hlsl) MaterialHlslTab((u32)e);
                else if (e >= 0) OpenFile((u32)e);
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

void App::ReloadArchives() {
    if (m_mountState == MountState::Mounting) return;
    m_exportCancel = true;
    CloseDocument(0);   // cancels map loads so WaitIdle does not block on them
    m_shutdown = true;  // the reference indexer reads the archives too
    for (auto& t : m_refThreads) t.join();
    m_refThreads.clear();
    m_shutdown = false;
    m_api->references.reset();
    m_refReady = m_refBuilding = false;
    m_jobs->WaitIdle();
    m_assets.reset();
    m_api->Unmount();
    m_searchResults.clear();
    m_lastSearch.clear();
    m_pakSizes.clear();
    m_selPak = -1;
    m_folder = 0;
    m_expanded.clear();
    m_folderFilters.clear();
    m_filterFolder = UINT32_MAX;
    m_filterBuf[0] = 0;
    m_selected = -1;
    m_selFolder = UINT32_MAX;
    {
        std::lock_guard lk(m_thumbMutex);
        m_thumbs.clear();
        m_thumbPending = 0;
    }
    StartMount();
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
            d->worldSea = IsWorldLayoutMap(d->gamePath);
            d->preferred = DocView::World;
        } else if (main && IsTableClass(d->className)) {
            auto t = std::make_shared<TableData>();
            if (ReadTable(*pkg, main - 1, *t, &err)) d->table = t;
        } else if (main && d->className == "WwiseBank") {   // the bank bytes follow the properties: list its embedded media
            d->isBank = ParseBank(pkg->Data().data(), pkg->Data().size(), d->bank, nullptr, &err);
            if (!d->isBank) d->audioError = "bank: " + err;
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
        if (d->worldSea) {
            opt.proxies = d->worldProxies;
            opt.islands = d->worldIslands;
            if (opt.proxies) opt.landscape = false;   // the proxies include the terrain
        }
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
        if (d.showCollision && !d.collisionLines.empty()) r.DrawLines(d.collisionLines, 0xff60ff40u);
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
    if (FlowToggle("Collision", &d.showCollision) && d.showCollision && !d.collision) {
        d.collision = std::make_shared<CollisionData>();
        std::string err;
        auto pkg = m_api->cache->LoadPath(d.gamePath, &err);
        if (pkg && pkg->MainExport() && ReadMeshCollision(*m_api->cache, *pkg, pkg->MainExport() - 1, m, *d.collision, &err)) {
            CollisionLines(*d.collision, d.collisionLines);
            d.collisionNote = Fmt("%d boxes, %d spheres, %d capsules, %d convex", d.collision->Count(CollisionShape::Box), d.collision->Count(CollisionShape::Sphere),
                                  d.collision->Count(CollisionShape::Capsule), d.collision->Count(CollisionShape::Convex));
        } else d.collisionNote = "no simple collision: " + err;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Simple collision (BodySetup / PhysicsAsset) as green wireframe%s%s", d.collisionNote.empty() ? "" : "\n", d.collisionNote.c_str());
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
    auto reload = [&] {
        for (auto& sp : m_docs)
            if (sp.get() == &d) {
                d.worldScene.reset();
                d.world.reset();
                d.cancel = false;
                LoadWorld(sp);
                break;
            }
    };
    // whole sea: what to load
    auto seaOptions = [&] {
        if (!d.worldSea) return;
        BeginToolbar();
        FlowToggle("Island proxies", &d.worldProxies, true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each island as its merged LOD proxy mesh (fast). Off: every mesh and the landscapes (filter islands first)");
        FlowNext(260);
        ImGui::SetNextItemWidth(260);
        bool enter = ImGui::InputTextWithHint("##isl", "islands: name / sea terms, -excluded", d.worldIslands, sizeof(d.worldIslands), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((FlowButton("Reload sea") || enter) && !d.worldLoading) reload();
        EndToolbar();
    };
    if (d.worldLoading || !d.worldScene) {
        seaOptions();
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
    FlowToggle("Water", &d.worldDraw.water);
    FlowToggle("Lights", &d.worldDraw.lights);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The map's sun, sky light and the %d point / spot lights nearest the camera", kMaxSceneLights);
    FlowToggle("Fog", &d.worldDraw.fog);
    FlowToggle("Gizmos", &d.worldDraw.gizmos);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Light ranges / spot cones (yellow) and decal boxes (blue) within 300 m");
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
    seaOptions();
    const auto& st = sc.Stats();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%zu levels | %zu instances of %zu meshes, %zu landscape patches, %zu lights, %zu decals | drawn: %u instances, %u meshes, %u patches, %.2fM tris | %s GPU",
                        w.levels.size(), w.InstanceCount(), w.groups.size(), w.landscapes.size(), w.lights.size(), w.decals.size(), st.instancesDrawn, st.meshesDrawn,
                        st.landscapesDrawn, m_renderer->DrawnTriangles() / 1e6, HumanSize(sc.GpuBytes()).c_str());
    ImGui::PopTextWrapPos();

    ImVec2 avail = MinViewportAvail();
    float side = avail.x > 640 ? std::min(360.f, avail.x * 0.3f) : 0.f;   // the outliner is dropped in narrow windows
    Camera& cam = d.camera;
    sc.Lighting(cam, d.worldDraw, d.worldLighting);
    d.draw.lighting = &d.worldLighting;
    DrawViewportScene(*m_renderer, cam, d.draw, ImVec2(side > 0 ? avail.x - side - ImGui::GetStyle().ItemSpacing.x : avail.x, avail.y), true, frameWorld, [&] {
        sc.Draw(cam, avail.y, d.worldDraw);
        sc.DrawOverlays(cam, d.worldDraw);
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
        if (d.worldSea && ImGui::BeginTabItem("Islands")) {
            const WorldLayout* L = GetWorldLayout(*m_api->cache);
            std::string f = ToLower(d.worldFilter);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##wf", "filter islands", d.worldFilter, sizeof(d.worldFilter));
            if (L && ImGui::BeginTable("##wi", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Island");
                ImGui::TableSetupColumn("Sea", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableHeadersRow();
                for (size_t ii = 0; ii < L->islands.size(); ii++) {
                    auto& isl = L->islands[ii];
                    if (!f.empty() && ToLower(isl.name + " " + isl.sea).find(f) == std::string::npos) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)ii);
                    if (ImGui::Selectable(isl.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(0)) {
                        cam.target = isl.radius > 0 ? isl.centre : isl.location;
                        cam.distance = std::max(isl.radius * 2.5f, 20000.f);
                    }
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Load in full detail")) {
                            snprintf(d.worldIslands, sizeof(d.worldIslands), "%s", isl.name.c_str());
                            d.worldProxies = false;
                            reload();
                        }
                        if (ImGui::MenuItem("Open island map", nullptr, false, !isl.map.empty())) {
                            i64 e = m_api->ResolveFile(isl.map);
                            if (e >= 0) OpenFile((u32)e);
                        }
                        if (ImGui::MenuItem("Copy folder")) CopyToClipboard(isl.dir);
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\nlocation %.0f %.0f %.0f, yaw %.1f\n%s%sDouble-click: fly to it, right-click: load / open", isl.dir.c_str(),
                                          isl.location.x, isl.location.y, isl.location.z, isl.yaw, isl.type.c_str(), isl.type.empty() ? "" : "\n");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", isl.sea.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Levels")) {
            for (auto& l : w.levels) {
                ImGui::TextUnformatted(l.path.substr(l.path.find_last_of('/') + 1).c_str());
                if (!l.island.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", l.island.c_str()); }
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

// ---------------------------------------------------------------- export

void App::ExportFiles(std::vector<u32> entries, int mode, int kind) {
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
    if (mode == 0 && kind == 4 && entries.size() > 1) {   // several animations: one .glb per skeleton with every clip in it
        m_jobs->Push([this, entries = std::move(entries), opt] {
            std::map<std::string, std::vector<std::shared_ptr<Package>>> bySkeleton;
            for (u32 e : entries) {
                if (m_exportCancel) break;
                m_exportDone++;
                if (!IsPackageFile(m_api->vfs.Path(e))) continue;
                std::string err;
                auto pkg = m_api->cache->Load(e, &err);
                i32 main = pkg ? pkg->MainExport() : 0;
                AnimSequenceData a;
                if (!main || !IsAnimSequenceClass(pkg->ExportClass(main - 1)) || !ReadAnimSequence(*pkg, main - 1, a, &err)) continue;
                bySkeleton[a.skeletonPath].push_back(pkg);
            }
            size_t files = 0;
            for (auto& [skel, pkgs] : bySkeleton) {
                Bytes glb;
                std::string note, err;
                if (!ExportAnimationSet(*m_api->cache, pkgs, "", glb, note, &err)) { m_exportFailed++; LOGW("animations of %s: %s", skel.c_str(), err.c_str()); continue; }
                std::string rel = pkgs[0]->GamePath();
                rel = rel.substr(0, rel.rfind('/') + 1) + "Animations_" + skel.substr(skel.rfind('.') + 1) + ".glb";
                if (!opt.keepDirectories) rel = rel.substr(rel.rfind('/') + 1);
                std::wstring out = opt.outputDir + L"\\" + Widen(rel);
                for (auto& c : out) if (c == L'/') c = L'\\';
                CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
                if (WriteWholeFile(out, glb.data(), glb.size())) { files++; LOGI("%s -> %s", note.c_str(), Narrow(out).c_str()); }
            }
            LOGI("exported %zu animation set(s) from %zu skeleton(s)", files, bySkeleton.size());
            m_exporting = false;
            Post([] {});
        });
        return;
    }
    m_jobs->Push([this, entries = std::move(entries), mode, kind, opt] {
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
            if (mode == 0 && (kind == 0 || kind == 5) && (IEndsWith(path, ".wem") || kind == 5)) {
                ok = ExportAudioEntry(e, opt, written, &err);
            } else if (kind == 6) {   // decompiled Blueprint -> .cpp
                if (IsPackageFile(path)) {
                    auto pkg = m_api->cache->Load(e, &err);
                    std::string code;
                    if (pkg && IsBlueprintPackage(*pkg)) {
                        ok = DecompileBlueprint(*pkg, code, &err);
                        if (ok) {
                            std::string rel = opt.keepDirectories ? path : path.substr(path.rfind('/') + 1);
                            std::wstring out = opt.outputDir + L"\\" + Widen(rel.substr(0, rel.rfind('.')) + ".cpp");
                            for (auto& c : out) if (c == L'/') c = L'\\';
                            CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
                            ok = WriteWholeFile(out, code.data(), code.size());
                            if (ok) written.push_back(Narrow(out));
                        }
                    } else if (!pkg) ok = false;
                }
            } else if (kind == 5) {
                ok = false;
            } else if (mode == 1) {
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
                else if (kind != 0) {   // only packages whose main asset has the requested type
                    i32 main = pkg->MainExport();
                    std::string cls = main ? pkg->ExportClass(main - 1) : "";
                    bool want = kind == 1   ? IsTextureClass(cls)
                                : kind == 2 ? IsStaticMeshClass(cls) || IsSkeletalMeshClass(cls)
                                : kind == 3 ? IsWorldClass(cls)
                                            : IsAnimSequenceClass(cls);
                    if (!want) {
                        m_exportDone++;
                        continue;
                    }
                }
                if (pkg) {
                    ExportOptions o = opt;
                    o.json = mode == 2;
                    o.worlds = kind == 3;   // Export > World: the whole map as .glb
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
    row("Game language");
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##lang", m_settings.language.c_str())) {   // FText values show this translation
        if (m_mountState == MountState::Ready) {
            static std::vector<std::string> cultures;
            if (cultures.empty()) cultures = Localization::Cultures(m_api->vfs);
            for (auto& c : cultures)
                if (ImGui::Selectable(c.c_str(), c == m_settings.language)) {
                    std::string err;
                    if (m_api->SetLanguage(c, &err)) {
                        m_settings.language = c;
                        m_settings.Save();
                    } else {
                        LOGW("language %s: %s", c.c_str(), err.c_str());
                    }
                }
        }
        ImGui::EndCombo();
    }
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
