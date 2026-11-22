#include "ui/App.h"
#include "ui/UiCommon.h"
#include "ue/AssetJson.h"
#include "ue/Blueprint.h"
#include "ue/Collision.h"
#include "ue/WorldLayout.h"
#include "render/Offscreen.h"
#include "app/WwiseEvents.h"
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

// ---------------------------------------------------------------- app

App::App() { m_settings.Load(); }

App::~App() {
    m_exportCancel = true;
    m_shutdown = true;   // stops the reference indexer
    if (m_contentJob) m_contentJob->cancel = true;
    for (auto& t : m_refThreads) t.join();
    m_classes.Stop();   // saves Classes.bin while the archives are still mounted
    m_jobs.reset();   // joins workers before the renderer / api go away
    m_docs.clear();
    m_assets.reset();
}

bool App::Busy() const {
    if (m_player.Active() && !m_player.Paused()) return true;   // playhead
    if (m_busyFrames > 0 || m_mountState == MountState::Mounting || m_exporting || m_refBuilding) return true;
    if (m_jobs && m_jobs->Pending() > 0) return true;
    if (m_classes.Pending() > 0) return true;   // explorer icons still resolving
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
    MeshLod sphere = MakePreviewSphere(50, 64, 32);
    m_sphere = m_renderer->CreateMesh(sphere, {-50, -50, -50}, {50, 50, 50});
    LOGI("settings: %s", Narrow(Settings::Path()).c_str());
    // FModel: start on the Archives tab, mount only when Load is pressed (command-line automation mounts right away)
    m_leftTab = startTab >= 0 ? startTab : 0;
    if (!openOnStart.empty() || !startFolder.empty() || startTab > 0) StartMount();
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
            m_classes.Start(m_api->vfs);
            m_mountSeconds = secs;
            m_readyAt = ImGui::GetTime();
            m_assets = std::make_unique<GpuAssets>(*m_renderer, *m_api->cache);
            m_assets->SetMaxTextureSize(std::min(m_settings.previewMaxTexture, 1024));
            m_folder = 0;
            if (startTab < 0) m_leftTab = 1;   // loaded: show the folders (FModel)
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
                if (p0.rfind("content:", 0) == 0) {   // --content "uses=path;class=X;...": the search window's Content tab
                    std::string q = p0.substr(8);
                    for (size_t s = 0; s <= q.size();) {
                        size_t e = std::min(q.find(';', s), q.size()), eq = q.find('=', s);
                        if (eq < e) {
                            std::string k = q.substr(s, eq - s), v = q.substr(eq + 1, e - eq - 1);
                            char* dst = k == "class" ? m_csClass : k == "uses" ? m_csUses : k == "property" ? m_csProp : k == "value" ? m_csValue
                                        : k == "name" ? m_csName : k == "folder" ? m_csFolder : nullptr;
                            size_t cap = k == "class" || k == "property" ? 128 : k == "value" || k == "name" ? 256 : 512;
                            if (dst) snprintf(dst, cap, "%s", v.c_str());
                        }
                        s = e + 1;
                    }
                    m_showSearch = true;
                    m_searchTab = 3;
                    StartContentSearch();
                    continue;
                }
                bool shaders = p0.rfind("shaders:", 0) == 0;   // --shaders <path>: open its compiled shaders
                bool hlsl = p0.rfind("hlsl:", 0) == 0;         // --material-hlsl <path>: its graph as HLSL
                bool refs = p0.rfind("refs:", 0) == 0;         // --references <path>: its dependency graph
                std::string p = shaders ? p0.substr(8) : (hlsl || refs) ? p0.substr(5) : p0;
                i64 e = m_api->ResolveFile(p);
                if (e >= 0 && refs) FindReferences((u32)e);
                else if (e >= 0 && shaders) DecompileShadersTab((u32)e);
                else if (e >= 0 && hlsl) MaterialHlslTab((u32)e);
                else if (e >= 0) OpenFile((u32)e);
                else LOGW("not found: %s", p.c_str());
            }
            openOnStart.clear();
            if (popoutOnStart) m_settings.viewerWindow = true;   // --popout: 3D views in the separate window for this run
        });
    });
}

ExportOptions App::MakeExportOptions() const {
    ExportOptions o;
    o.outputDir = m_settings.outputDir;
    o.meshFormat = m_settings.meshFormat;
    o.textureFormat = m_settings.textureFormat;
    o.keepDirectories = m_settings.exportKeepDirectories;
    o.animationSounds = m_settings.exportAnimSounds;
    o.json = false;
    return o;
}

void App::ReloadArchives() {
    if (m_mountState == MountState::Mounting) return;
    m_exportCancel = true;
    CloseDocument(0);   // cancels map loads so WaitIdle does not block on them
    m_shutdown = true;  // the reference indexer reads the archives too
    if (m_contentJob) m_contentJob->cancel = true;
    for (auto& t : m_refThreads) t.join();
    m_refThreads.clear();
    m_contentJob.reset();   // its entries index the old archives
    m_shutdown = false;
    m_api->references.reset();
    m_refReady = m_refBuilding = false;
    m_jobs->WaitIdle();
    m_assets.reset();
    m_classes.Stop();
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
    std::vector<MeshSocket> sockets;
    ReadMeshSockets(*m_api->cache, *mp, mp->MainExport() - 1, *mesh, sockets);
    d.sockets = std::move(sockets);
    d.attachSocket = -1;
    d.mesh = mesh;
    d.poser = poser;
    d.gpuMesh = gm;
    d.lod = lod;
    d.animMesh = meshPath;
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
        } else if (main && (d->className == "AnimMontage" || IsBlendSpaceClass(d->className))) {
            auto anim = std::make_shared<AnimSequenceData>();
            std::vector<std::string> paths;
            if (d->className == "AnimMontage") {
                auto m = std::make_shared<MontageInfo>();
                ReadMontage(*pkg, main - 1, *m, &err);
                anim->skeletonPath = m->skeleton;
                anim->length = m->length;
                anim->encoding = "montage";
                if (!m->slots.empty())
                    for (auto& g : m->slots[0].segments) paths.push_back(g.anim);
                d->montage = m;
            } else {
                auto b = std::make_shared<BlendSpaceInfo>();
                if (ReadBlendSpace(*pkg, main - 1, *b, &err)) {
                    anim->skeletonPath = b->skeleton;
                    anim->encoding = b->dims == 1 ? "blend space 1D" : "blend space";
                    for (auto& s : b->samples) paths.push_back(s.anim);
                    d->bsX = (b->axes[0].min + b->axes[0].max) / 2;
                    d->bsY = (b->axes[1].min + b->axes[1].max) / 2;
                    d->blendSpace = b;
                } else d->error = "blend space: " + err;
            }
            std::unordered_map<std::string, std::shared_ptr<AnimSequenceData>> loaded;
            for (auto& path : paths) {
                auto& sa = loaded[path];
                if (!sa) {
                    auto ap = m_api->cache->LoadPath(path);
                    i32 ae = ap ? ap->MainExport() : 0;
                    auto a = std::make_shared<AnimSequenceData>();
                    if (ae && IsAnimSequenceClass(ap->ExportClass(ae - 1)) && ReadAnimSequence(*ap, ae - 1, *a)) sa = a;
                }
                d->subAnims.push_back(sa);
                if (sa && d->blendSpace) anim->length = std::max(anim->length, sa->length);
                if (sa && anim->skeletonPath.empty()) anim->skeletonPath = sa->skeletonPath;
            }
            anim->name = d->name;
            anim->numFrames = std::max(2, (int)lroundf(anim->length * 30) + 1);
            if (!anim->skeletonPath.empty() && (d->montage || d->blendSpace)) {
                d->anim = anim;
                d->preferred = DocView::Animation;
                std::string mesh = FindMeshForSkeleton(*m_api->cache, anim->skeletonPath);
                if (!mesh.empty()) LoadAnimationMeshNow(*d, mesh);
                else d->error = "no skeletal mesh found for " + anim->skeletonPath + " (enter one below)";
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
                    auto sm = std::make_shared<MeshData>();   // the model view needs the mesh data too
                    sm->name = "sphere";
                    sm->lods.push_back(MakePreviewSphere(50, 64, 32));
                    MeshMaterialSlot slot;
                    slot.name = d->name.substr(0, d->name.rfind(".uasset"));
                    slot.materialPath = objPath;
                    sm->materials.push_back(slot);
                    sm->ComputeBounds();
                    d->mesh = sm;
                    d->hasMesh = true;
                    d->draw.graphMaterials = true;
                    if (auto g = m_assets->GraphShader(objPath)) {
                        auto mat = s->materials[0];
                        Post([mat, g] { if (mat && !mat->graph) mat->graph = g; });
                    }
                    d->camera.Frame(s->boundsMin, s->boundsMax);
                    d->camera.yaw = 180;
                    d->camera.pitch = -10;
                    d->draw.grid = false;
                }
            } else {
                d->error = "material: " + err;
            }
        } else if (main && d->className == "ParticleSystem") {
            auto ps = std::make_shared<ParticleSystemInfo>();
            if (ReadParticleSystem(*m_api->cache, *pkg, main - 1, *ps, &err)) {
                for (auto& e : ps->emitters) {
                    Image img;
                    d->psTextures.push_back(LoadParticleTexture(*m_api->cache, e.material, 1024, img) ? m_renderer->CreateTexture(img, true, false) : nullptr);
                    d->psLength = std::max(d->psLength, e.delay + e.duration);
                }
                d->psLength = std::min(d->psLength, 10.f);
                d->psVisible.assign(ps->emitters.size(), 1);
                ParticleFrame(*ps, d->psLength, d->psCentre, d->psDist);
                d->camera.target = d->psCentre;
                d->camera.distance = d->psDist;
                d->camera.yaw = d->psYaw;
                d->camera.pitch = d->psPitch;
                d->camera.nearZ = std::max(0.5f, d->psDist * 0.002f);
                auto sim = std::make_shared<ParticleSim>();
                sim->Init(*ps);
                d->particles = ps;
                d->psim = sim;
            } else d->error = "particles: " + err;
        } else if (main && d->className == "BlueprintGeneratedClass") {   // an actor class: assembled from its components
            d->isWorld = true;
            d->worldBlueprint = true;
            d->preferred = DocView::World;
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
        } else if (main && d->className == "WwiseEvent") {   // the sounds it plays, through its bank's hierarchy
            WwiseEventInfo ev;
            if (LoadWwiseEvent(*m_api, d->gamePath, ev, &err)) FillEventDocument(*d, ev);
            else d->audioError = "event: " + err;
        }
        if (!d->anim && !d->isWorld)
            for (size_t i = 0; i < pkg->Exports().size(); i++)
                if (pkg->ExportClass(i) == "AnimBlueprintGeneratedClass") {
                    auto bp = std::make_shared<AnimBlueprintInfo>();
                    if (ReadAnimBlueprint(*pkg, *bp, &err)) d->animBp = bp;
                    break;
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
        bool ok = d->worldBlueprint ? loader.AddBlueprint(GamePathToPackage(d->gamePath), Mat4::Identity(), opt, *world, &err)
                                    : loader.Load(d->gamePath, opt, *world, &err);
        if (ok && d->worldBlueprint && world->InstanceCount() == 0) ok = false;   // logic-only class: no 3D view
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
            if (!ok && d->worldBlueprint) {   // nothing to show: an ordinary tab
                d->isWorld = false;
                LOGI("%s: %s", d->name.c_str(), err.empty() ? "no mesh components" : err.c_str());
                return;
            }
            if (!ok) { d->error = "world: " + err; return; }
            d->world = world;
            d->worldScene = scene;
            if (scene && d->worldBlueprint) {   // one actor: frame all of it, viewer lighting, no sea
                d->camera.Frame(scene->BoundsMin(), scene->BoundsMax());
                d->camera.flySpeed = std::max(100.f, Length(scene->BoundsMax() - scene->BoundsMin()));
                d->worldDraw.water = d->worldDraw.fog = d->worldDraw.lights = false;
            } else if (scene) {
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
                if (x->id == id && gm) {
                    x->gpuMesh = gm;
                    x->morphDirty = true;   // morph weights are re-applied on the new LOD
                }
        });
    });
}

// Space left for a 3D viewport, but never shorter than a usable minimum: in small windows the tab scrolls instead of the viewport vanishing.
static ImVec2 MinViewportAvail() {
    ImVec2 a = ImGui::GetContentRegionAvail();
    a.y = std::max(a.y, ImGui::GetFontSize() * 14);
    return a;
}

// World-space ray through a point of the viewport (uv 0..1, y down), matching Renderer::BeginFrame's camera.
static void ViewRay(const Camera& cam, ImVec2 size, ImVec2 uv, Vec3& origin, Vec3& dir) {
    Vec3 eye = cam.Eye(), f = Normalize(cam.target - eye), s = Normalize(Cross({0, 0, 1}, f)), u = Cross(f, s);
    float th = tanf(cam.fovY * kPi / 360), aspect = size.x / std::max(1.f, size.y);
    origin = eye;
    dir = Normalize(f + s * ((uv.x * 2 - 1) * th * aspect) + u * ((1 - uv.y * 2) * th));
}

// Camera input over an image-sized button (LMB orbit, RMB look + WASD/QE fly, MMB pan, wheel zoom, F calls `frame`),
// then renders `scene` into the viewport. `onClick` (optional) gets the viewport uv of a left click that did not drag.
void DrawViewportScene(Renderer& r, Camera& cam, const DrawOptions& draw, ImVec2 size, bool allowFly, const std::function<void()>& frame,
                       const std::function<void()>& scene, const std::function<void(ImVec2)>& onClick) {
    if (size.x < 8 || size.y < 8) return;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);   // the wheel zooms; it must not also scroll the tab when that has a scrollbar
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (onClick && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && io.MouseDragMaxDistanceSqr[0] < 16)
        onClick(ImVec2((io.MousePos.x - p0.x) / size.x, (io.MousePos.y - p0.y) / size.y));
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
        for (auto& at : d.attachments)
            if (at.gpu && at.placed) r.DrawMesh(*at.gpu, &at.world, 1);
        for (auto& h : d.heldItems)
            if (h.visible && h.placed && h.parts)
                for (auto& p : *h.parts) {
                    Mat4 m = p.local * h.world;
                    r.DrawMesh(*p.mesh, &m, 1);
                }
        if (d.showCollision && !d.collisionLines.empty()) r.DrawLines(d.collisionLines, 0xff60ff40u);
        std::vector<ParticleSprite> sprites;
        for (auto* list : {&d.attachments, &d.notifyFx})
            for (auto& at : *list)
                if (at.sim && at.placed) {
                    sprites.clear();
                    at.sim->Sprites(sprites);
                    DrawParticleSprites(r, d.camera, sprites, at.looks, at.world);
                }
        if (d.showSockets) {
            static const u32 axis[3] = {0xff4040ffu, 0xff40ff40u, 0xffff6040u};
            for (int k = 0; k < 3; k++)
                if (!d.socketLines[k].empty()) r.DrawLines(d.socketLines[k], axis[k]);
        }
    });
}

bool App::Has3dView(const Document& d) {
    return !d.loading && ((d.hasMesh && (!d.hasMaterial || d.materialSphere)) || d.anim || (d.isWorld && !(d.worldBlueprint && d.worldLoading)));
}

// The 3D viewer (toolbar, viewport, side list) lives in its own OS window (Gui.cpp hosts it) while Settings::viewerWindow
// is on: "Back to tab" switches that off, "Separate window" on again.
void App::SetViewerWindow(bool on, Document* d) {
    m_settings.viewerWindow = on;
    m_settings.Save();
    m_popoutDismissed = nullptr;
    m_popoutDoc = on && d ? d->id : 0;
    m_popoutFocus = on;
}

void App::ClosePopout() {
    for (auto& d : m_docs)
        if (d->id == m_popoutDoc) m_popoutDismissed = d.get();
    m_popoutDoc = 0;
}

void App::SyncPopoutDoc() {
    if (m_popoutDoc && std::none_of(m_docs.begin(), m_docs.end(), [&](auto& dp) { return dp->id == m_popoutDoc; })) m_popoutDoc = 0;   // tab closed
    if (!m_settings.viewerWindow) return;
    Document* a = ActiveDoc();
    if (a && Has3dView(*a) && a != m_popoutDismissed && a->id != m_popoutDoc) {
        m_popoutDoc = a->id;
        m_popoutRaise = true;   // above the main window, without taking the focus
    }
}

void App::PopoutButton(Document& d, bool flow) {
    const char* label = m_inPopout ? "Back to tab" : "Separate window";
    if (flow ? FlowButton(label) : ImGui::Button(label)) SetViewerWindow(!m_inPopout, &d);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(m_inPopout ? "Show 3D views inside their tabs again" : "Show 3D views in their own window (e.g. on a second monitor)");
}

std::string App::PopoutTitle() const {
    for (auto& d : m_docs)
        if (d->id == m_popoutDoc) return "SOTModel - " + d->name;
    return "SOTModel - 3D view";
}

// One frame of the separate viewer window (its own ImGui context, set current by Gui.cpp).
void App::FramePopout() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() ||
        io.InputQueueCharacters.Size)
        m_busyFrames = 3;
    Document* d = nullptr;
    for (auto& x : m_docs)
        if (x->id == m_popoutDoc) d = x.get();
    float u = ImGui::GetFontSize() / 16.f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * u, 6 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5 * u, 5 * u));
    ImGui::Begin("##popout", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (d && Has3dView(*d)) {
        m_inPopout = true;
        if (d->isWorld) DrawWorld(*d);
        else if (d->anim) DrawAnimation(*d);
        else DrawModel(*d);
        m_inPopout = false;
    } else {
        ImGui::TextDisabled(d && d->loading ? "Loading %s..." : "%s has no 3D view.", d ? d->name.c_str() : "The tab");
        if (d && !d->loading) {
            m_inPopout = true;
            PopoutButton(*d, false);
            m_inPopout = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// Advances an attached particle system by the frame time (looping like the particle view).
static void StepAttachedParticles(Document::Attachment& at, float dt) {
    if (!at.sim) return;
    at.psTime += dt;
    if (at.psTime > at.psLength * 1.5f && at.sim->Count() == 0) {
        at.sim->Reset();
        at.psTime = 0;
    }
    at.sim->AdvanceTo(at.psTime);
}

// Socket -> mesh space of every attachment: the posed bone (boneWorld = mesh-space bone matrices of an animation pose) or the
// reference pose the sockets were read in.
void App::PlaceAttachments(Document& d, const std::vector<Mat4>* boneWorld) {
    for (auto& at : d.attachments) {
        if (at.sim) {
            StepAttachedParticles(at, ImGui::GetIO().DeltaTime);
            m_busyFrames = 3;
        }
        at.placed = false;
        for (auto& so : d.sockets) {
            if (!IEquals(so.name, at.socket)) continue;
            at.world = so.transform;
            if (boneWorld && d.mesh && !so.bone.empty())
                for (size_t b = 0; b < d.mesh->skeleton.size() && b < boneWorld->size(); b++)
                    if (IEquals(d.mesh->skeleton[b].name, so.bone)) {
                        at.world = Mat4::FromTRS(so.location, RotatorToQuat(so.rotation.x, so.rotation.y, so.rotation.z), so.scale) * (*boneWorld)[b];
                        break;
                    }
            at.placed = true;            break;
        }
    }
}

// Worker thread: a particle system with its emitter textures and a simulation, ready to draw at a socket.
bool App::LoadParticleEffect(const Package& pkg, Document::Attachment& at, std::string* error) {
    auto ps = std::make_shared<ParticleSystemInfo>();
    if (!ReadParticleSystem(*m_api->cache, pkg, pkg.MainExport() - 1, *ps, error)) return false;
    std::vector<std::shared_ptr<GpuTexture>> tex;
    at.psLength = 1;
    for (auto& e : ps->emitters) {
        Image img;
        tex.push_back(LoadParticleTexture(*m_api->cache, e.material, 512, img) ? m_renderer->CreateTexture(img, true, false) : nullptr);
        at.psLength = std::max(at.psLength, e.delay + e.duration);
    }
    at.psLength = std::min(at.psLength, 10.f);
    at.looks = ParticleLooks(*ps, tex);
    at.sim = std::make_shared<ParticleSim>();
    at.sim->Init(*ps);
    at.ps = ps;
    return true;
}

void App::AttachMesh(std::shared_ptr<Document> sp, std::string path, std::string socket) {
    sp->attachLoading = true;
    sp->attachError.clear();
    m_jobs->Push([this, sp, path, socket] {
        std::string err;
        i64 e = m_api->ResolveFile(path);
        auto pkg = e >= 0 ? m_api->cache->Load((u32)e, &err) : nullptr;
        auto mesh = std::make_shared<MeshData>();
        auto at = std::make_shared<Document::Attachment>();
        at->mesh = path;
        at->socket = socket;
        i32 main = pkg ? pkg->MainExport() : 0;
        if (!pkg) err = "not found: " + path;
        else if (main && pkg->ExportClass(main - 1) == "ParticleSystem") {
            if (!LoadParticleEffect(*pkg, *at, &err)) err = path + ": " + err;
        } else if (!main || !ReadMesh(*pkg, main - 1, *mesh, &err)) err = path + ": " + (err.empty() ? "not a mesh or particle system" : err);
        else if (!(at->gpu = m_assets->Mesh(*mesh, 0))) err = path + ": no geometry";
        else at->data = mesh;
        Post([sp, at, err] {
            sp->attachLoading = false;
            sp->attachError = err;
            if (!err.empty()) LOGW("attach: %s", err.c_str());
            if (!at->gpu && !at->sim) return;
            sp->attachments.push_back(std::move(*at));
            sp->attachEdit[0] = 0;
        });
    });
}

void App::DrawAttachControls(Document& d) {
    if (d.sockets.empty()) return;
    if (d.attachSocket < 0) {   // default: a hand prop socket
        d.attachSocket = 0;
        int best = 0;
        for (int i = 0; i < (int)d.sockets.size(); i++) {
            std::string n = ToLower(d.sockets[i].name);
            int score = n == "righthandprop" ? 3 : n.find("hand") != std::string::npos && n.find("prop") != std::string::npos ? 2 : n.find("hand") != std::string::npos ? 1 : 0;
            if (score > best) best = score, d.attachSocket = i;
        }
    }
    d.attachSocket = std::clamp(d.attachSocket, 0, (int)d.sockets.size() - 1);
    if (!attachOnStart.empty()) {   // --attach mesh@socket (no socket: the default one)
        for (auto& sp : m_docs)
            if (sp.get() == &d)
                for (auto& a : attachOnStart) {
                    size_t at = a.rfind('@');
                    std::string mesh = a.substr(0, at), socket = at == std::string::npos ? "" : a.substr(at + 1);
                    AttachMesh(sp, mesh, socket.empty() ? d.sockets[d.attachSocket].name : socket);
                }
        attachOnStart.clear();
    }
    ImGui::PushID("attach");
    float u = ImGui::GetFontSize() / 16.f;
    bool narrow = ImGui::GetContentRegionAvail().x < 420 * u;   // side panel: socket and path on separate lines
    ImGui::SetNextItemWidth(narrow ? -1 : 180 * u);
    if (ImGui::BeginCombo("##sock", d.sockets[d.attachSocket].name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < (int)d.sockets.size(); i++) {
            auto& so = d.sockets[i];
            std::string label = so.bone.empty() ? so.name : so.name + "  @ " + so.bone;
            if (ImGui::Selectable(label.c_str(), i == d.attachSocket)) d.attachSocket = i;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Socket to attach to");
    if (!narrow) ImGui::SameLine();
    float btnW = ImGui::CalcTextSize("Attach").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(std::max(60 * u, ImGui::GetContentRegionAvail().x - btnW - ImGui::GetStyle().ItemSpacing.x));
    bool go = ImGui::InputTextWithHint("##am", narrow ? "mesh path" : "mesh to attach (/Game/... path; empty = the explorer's selection)", d.attachEdit,
                                       sizeof(d.attachEdit), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Static or skeletal mesh path; leave empty to attach the mesh selected in the explorer");
    ImGui::SameLine();
    ImGui::BeginDisabled(d.attachLoading);
    go |= ImGui::Button("Attach");
    ImGui::EndDisabled();
    if (go && !d.attachLoading) {
        std::string path = d.attachEdit;
        if (path.empty() && m_selected >= 0) path = std::string(m_api->vfs.Path((u32)m_selected));
        std::shared_ptr<Document> sp;
        for (auto& x : m_docs)
            if (x.get() == &d) sp = x;
        if (path.empty()) d.attachError = "type a mesh path or select a mesh in the explorer";
        else if (sp) AttachMesh(sp, path, d.sockets[d.attachSocket].name);
    }
    if (!d.attachError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", d.attachError.c_str());
    for (size_t i = 0; i < d.attachments.size(); i++) {
        auto& at = d.attachments[i];
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("x")) {
            d.attachments.erase(d.attachments.begin() + i);
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Detach");
        ImGui::SameLine();
        std::string name = at.mesh.substr(at.mesh.find_last_of('/') + 1);
        if (size_t dot = name.find('.'); dot != std::string::npos) name.resize(dot);
        if (at.placed) ImGui::Text("%s", name.c_str());
        else ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(at.placed ? "@ %s" : "@ %s (no such socket on this mesh)", at.socket.c_str());
        ImGui::PopID();
    }
    ImGui::PopID();
}

// Live editing of a material's scalar / vector parameters (its compiled graph shader reads them from a constant buffer).
// The shader is shared by every view of the material, so all of them change.
void App::DrawGraphParams(GpuMaterial& mat) {
    auto g = mat.graph;
    if (mat.path.empty()) return;
    if (!g) {   // RequestGraphShaders posts it when compiled
        ImGui::TextDisabled("compiling graph shader...");
        m_busyFrames = 3;
        return;
    }
    if (!g->ps) {
        ImGui::TextDisabled("graph shader failed");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g->error.c_str());
        return;
    }
    if (graphOnStart) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (g->params.empty() || !ImGui::TreeNode("##params", "Parameters (%zu)", g->params.size())) return;
    bool changed = false;
    if (ImGui::SmallButton("Reset")) {
        for (size_t i = 0; i < g->params.size(); i++) memcpy(&g->values[i * 4], g->params[i].value, sizeof(float) * 4);
        changed = true;
    }
    for (size_t i = 0; i < g->params.size(); i++) {
        auto& p = g->params[i];
        float* v = &g->values[i * 4];
        ImGui::PushID((int)i);
        bool edited = memcmp(v, p.value, sizeof(float) * p.components) != 0;
        if (edited) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.35f, 1));
        bool unnamed = p.name.empty() || p.name == "None";   // cooked expressions may keep no parameter name
        if (unnamed && !edited) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(unnamed ? "(unnamed parameter)" : p.name.c_str());
        if (edited || unnamed) ImGui::PopStyleColor();
        if (edited && ImGui::IsItemHovered()) ImGui::SetTooltip("changed; material value %g %g %g %g (right-click to restore)", p.value[0], p.value[1], p.value[2], p.value[3]);
        if (ImGui::IsItemClicked(1)) {
            memcpy(v, p.value, sizeof(float) * 4);
            changed = true;
        }
        ImGui::SetNextItemWidth(-1);
        std::string ln = ToLower(p.name);
        bool color = p.components >= 3 && (ln.find("col") != std::string::npos || ln.find("tint") != std::string::npos || ln.find("albedo") != std::string::npos);
        if (color)
            changed |= p.components == 4 ? ImGui::ColorEdit4("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                                         : ImGui::ColorEdit3("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        else {
            float speed = std::max(0.001f, fabsf(p.value[0]) * 0.01f);
            changed |= ImGui::DragScalarN("##v", ImGuiDataType_Float, v, p.components, speed, nullptr, nullptr, "%.3f");
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
    if (changed) {
        g->version++;
        m_busyFrames = 3;
    }
}

void App::DrawModel(Document& d) {
    auto& m = *d.mesh;
    if (graphOnStart && !d.draw.graphMaterials) {   // --graph
        d.draw.graphMaterials = true;
        RequestGraphShaders(d);
    }
    if (!d.meshExtrasLoaded) {
        d.meshExtrasLoaded = true;
        if (auto pkg = m_api->cache->LoadPath(d.gamePath); pkg && pkg->MainExport()) {
            if (ReadMeshSockets(*m_api->cache, *pkg, pkg->MainExport() - 1, m, d.sockets)) {
                float size = std::max(2.f, Length(m.boundsMax - m.boundsMin) * 0.05f);
                SocketAxes(d.sockets, size, d.socketLines[0], d.socketLines[1], d.socketLines[2]);
            }
            if (m.skeletal && ReadMorphTargets(*pkg, pkg->MainExport() - 1, d.morphs)) d.morphWeights.assign(d.morphs.size(), 0.f);
        }
    }
    if (d.morphDirty && !d.morphs.empty()) {   // morphs need the dynamic (CPU written) vertex buffer
        d.morphDirty = false;
        bool any = false;
        for (float w : d.morphWeights) any |= w != 0;
        if (any || (d.gpuMesh && !d.gpuMesh->cpuVertices.empty())) {
            int lod = std::clamp(d.lod, 0, (int)m.lods.size() - 1);
            if (!d.gpuMesh || d.gpuMesh->cpuVertices.empty()) d.gpuMesh = m_assets->Mesh(m, lod, true, true);
            std::vector<Vec3> pos, nrm;
            ApplyMorphs(m.lods[lod], lod, d.morphs, d.morphWeights, pos, nrm);
            if (d.gpuMesh) m_renderer->UpdateMeshVertices(*d.gpuMesh, pos, nrm);
        }
    }
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
    if (!d.sockets.empty()) {
        FlowToggle("Sockets", &d.showSockets);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Socket axes: X red, Y green, Z blue (%zu sockets)", d.sockets.size());
    }
    if (FlowToggle("Graph shader", &d.draw.graphMaterials) && d.draw.graphMaterials) RequestGraphShaders(d);
    if (ImGui::IsItemHovered()) {
        std::string tip = "Draw each material with its translated node graph compiled as a pixel shader\n(textures, parameters, panners, blend modes; engine inputs are stubbed)";
        if (d.gpuMesh)
            for (auto& mat : d.gpuMesh->materials)
                if (mat && mat->graph)
                    tip += "\n" + mat->path.substr(mat->path.rfind('/') + 1) + ": " +
                           (mat->graph->ps ? Fmt("ok, %.0f ms", mat->graph->compileMs) : mat->graph->error.substr(0, 200));
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if (d.hasMaterial && FlowButton("Texture pages")) d.materialSphere = false;
    if (FlowButton("Frame")) d.camera.Frame(d.gpuMesh->boundsMin, d.gpuMesh->boundsMax);
    PopoutButton(d, true);
    EndToolbar();

    const MeshLod& l = m.lods[std::clamp(d.lod, 0, (int)m.lods.size() - 1)];
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%u vertices, %zu triangles, %d UV set(s), %zu section(s) | %.1f x %.1f x %.1f cm", l.VertexCount(), l.indices.size() / 3, l.numUVs,
                        l.sections.size(), m.boundsMax.x - m.boundsMin.x, m.boundsMax.y - m.boundsMin.y, m.boundsMax.z - m.boundsMin.z);
    ImGui::PopTextWrapPos();
    PlaceAttachments(d, nullptr);
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
        if (d.draw.graphMaterials && d.gpuMesh && i < d.gpuMesh->materials.size() && d.gpuMesh->materials[i]) DrawGraphParams(*d.gpuMesh->materials[i]);
        ImGui::PopID();
    }
    if (!d.draw.graphMaterials && !m.materials.empty()) {
        if (ImGui::SmallButton("Edit parameters")) {
            d.draw.graphMaterials = true;
            RequestGraphShaders(d);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns on Graph shader: scalar / vector parameters can then be changed live");
    }
    if (m.skeletal) ImGui::TextDisabled("%zu bones", m.skeleton.size());
    if (!d.sockets.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Attach a mesh to a socket");
        DrawAttachControls(d);
        ImGui::Spacing();
        ImGui::TextDisabled("Sockets (%zu)", d.sockets.size());
        for (auto& so : d.sockets) {
            ImGui::TextUnformatted(so.name.c_str());
            if (!so.bone.empty()) { ImGui::SameLine(); ImGui::TextDisabled("@ %s", so.bone.c_str()); }
            if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
                Vec3 t = so.transform.Translation();
                ImGui::SetTooltip("location %.2f %.2f %.2f\nrotation P %.1f Y %.1f R %.1f\nscale %.2f %.2f %.2f\nmesh space %.2f %.2f %.2f%s", so.location.x,
                                  so.location.y, so.location.z, so.rotation.x, so.rotation.y, so.rotation.z, so.scale.x, so.scale.y, so.scale.z, t.x, t.y, t.z,
                                  so.source == "skeleton" ? "\n(from the skeleton)" : "");
            }
        }
    }
    if (!d.morphs.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Morph targets (%zu)", d.morphs.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            std::fill(d.morphWeights.begin(), d.morphWeights.end(), 0.f);
            d.morphDirty = true;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##mf", "filter morphs", d.morphFilter, sizeof(d.morphFilter));
        std::string f = ToLower(d.morphFilter);
        for (size_t i = 0; i < d.morphs.size(); i++) {
            if (!f.empty() && ToLower(d.morphs[i].name).find(f) == std::string::npos) continue;
            ImGui::PushID((int)i + 1000);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##w", &d.morphWeights[i], 0, 1, (d.morphs[i].name + " %.2f").c_str())) d.morphDirty = true;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void App::DrawAnimation(Document& d) {
    auto& a = *d.anim;
    float fps = a.FrameRate();
    if (!d.notifiesLoaded) LoadNotifies(d);
    // toolbar: transport + options
    if (ImGui::Button(d.animPlaying ? "Pause" : "Play")) d.animPlaying = !d.animPlaying;
    ImGui::SameLine();
    float frame = d.animTime * fps;
    ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - 520 - ImGui::CalcTextSize("Separate window").x - ImGui::GetStyle().FramePadding.x * 2));
    if (ImGui::SliderFloat("##frame", &frame, 0, (float)std::max(0, a.numFrames - 1), "frame %.1f")) {
        d.animTime = frame / fps;
        d.animPlaying = false;
    }
    ImVec2 sliderMin = ImGui::GetItemRectMin(), sliderMax = ImGui::GetItemRectMax();
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
    DrawNotifyToggle(d);
    ImGui::SameLine();
    PopoutButton(d, false);
    DrawNotifyStrip(d, sliderMin.x, sliderMax.x);
    if (d.montage || d.blendSpace) DrawAnimAssetHeader(d);
    else
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
    DrawAttachControls(d);
    // advance time and pose the mesh (CPU skinning into the dynamic vertex buffer)
    if (d.animPlaying && a.length > 0) {
        float prev = d.animTime;
        d.animTime += ImGui::GetIO().DeltaTime * d.animSpeed;
        if (d.animTime > a.length) d.animTime = d.animLoop ? fmodf(d.animTime, a.length) : a.length;
        if (!d.animLoop && d.animTime >= a.length) d.animPlaying = false;
        // notifies passed this frame (a wrap fires the end of the clip, then its start)
        float from = d.notifyPrevTime <= prev ? d.notifyPrevTime : prev;
        if (d.animTime >= prev) FireNotifies(d, from, d.animTime);
        else {
            FireNotifies(d, from, a.length + 1);
            d.heldItems.clear();   // a new pass of the clip starts empty-handed
            FireNotifies(d, -1, d.animTime);
        }
        m_busyFrames = 3;
    } else if (d.animTime != d.notifyPrevTime) {   // scrubbed: the held items as of the new time, without sounds / effects
        d.heldItems.clear();
        FireNotifies(d, -1, d.animTime, true);
    }
    d.notifyPrevTime = d.animTime;
    std::vector<Bone> pose;
    if (!AnimAssetPose(d, pose)) d.poser->MeshPose(a, d.animTime * fps, pose);
    std::vector<Vec3> pos, nrm;
    const MeshLod& lod = d.mesh->lods[std::clamp(d.lod, 0, (int)d.mesh->lods.size() - 1)];
    d.poser->Skin(lod, pose, pos, nrm);
    m_renderer->UpdateMeshVertices(*d.gpuMesh, pos, nrm);
    d.boneLines.clear();
    std::vector<Mat4> world;
    if (d.showBones || !d.attachments.empty() || !d.notifyFx.empty() || !d.heldItems.empty()) AnimPoser::WorldMatrices(pose, world);
    PlaceAttachments(d, &world);
    UpdateNotifyFx(d, &world);
    if (d.showBones) {
        for (size_t i = 0; i < pose.size(); i++)
            if (pose[i].parent >= 0) {
                d.boneLines.push_back(world[pose[i].parent].Translation());
                d.boneLines.push_back(world[i].Translation());
            }
    }
    DrawViewport(*m_renderer, d, MinViewportAvail(), true);
}

// "Export glTF" in the world toolbar: the assembled Blueprint, the whole loaded map or the part around the view target
void App::DrawWorldExport(Document& d) {
    if (FlowButton("Export glTF")) ImGui::OpenPopup("##wexp");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export the assembled scene (or the part around the view) as one .glb for Blender");
    if (!ImGui::BeginPopup("##wexp")) return;
    const WorldData& w = *d.world;
    ImGui::TextUnformatted(d.worldBlueprint ? "Export the assembled Blueprint" : "Export the map");
    ImGui::Separator();
    if (!d.worldBlueprint) {
        if (ImGui::RadioButton("Everything loaded", !d.expPart)) d.expPart = false;
        if (ImGui::RadioButton("Around the view target", d.expPart)) d.expPart = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!d.expPart);
        ImGui::SetNextItemWidth(170);
        ImGui::SliderFloat("##exr", &d.expRadius, 5, 5000, "radius %.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
    }
    bool part = d.expPart && !d.worldBlueprint, filtered = d.expFiltered && d.worldFilter[0];
    std::string filter = ToLower(d.worldFilter);
    Vec3 c = d.camera.target;
    float r2 = d.expRadius * 100 * d.expRadius * 100;
    size_t inst = 0, meshes = 0;
    for (auto& g : w.groups) {   // what the export will hold
        if (filtered && ToLower(g.mesh).find(filter) == std::string::npos) continue;
        size_t n = 0;
        for (auto& i : g.instances) {
            Vec3 p = i.transform.Translation() - c;
            if (!part || Dot(p, p) <= r2) n++;
        }
        inst += n;
        meshes += n > 0;
    }
    for (auto& s : w.splines) {   // bent meshes: one each
        Vec3 p = s.transform.Translation() - c;
        if ((filtered && ToLower(s.mesh).find(filter) == std::string::npos) || (part && Dot(p, p) > r2)) continue;
        inst++;
        meshes++;
    }
    ImGui::TextDisabled("%zu instances of %zu meshes", inst, meshes);
    if (d.worldFilter[0]) ImGui::Checkbox(Fmt("Only meshes matching \"%s\"", d.worldFilter).c_str(), &d.expFiltered);
    if (!d.worldBlueprint) {
        ImGui::Checkbox(part ? "Move the view target to the origin" : "Move the centre to the origin", &d.expRecenter);
        if (!w.landscapes.empty()) ImGui::Checkbox("Landscape", &d.expLandscape);
        ImGui::Checkbox("Lights, decal boxes, fog", &d.expLights);
    }
    ImGui::Checkbox("Textures", &d.expTextures);
    ImGui::SameLine();
    static const char* sizes[] = {"512", "1024", "2048", "full size"};
    ImGui::SetNextItemWidth(100);
    ImGui::BeginDisabled(!d.expTextures);
    ImGui::Combo("##exts", &d.expTexSize, sizes, 4);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(m_exporting || inst == 0);
    if (ImGui::Button("Export", ImVec2(120, 0))) {
        WorldExportOptions wo;
        wo.textures = d.expTextures;
        wo.maxTextureSize = d.expTexSize < 3 ? 512 << d.expTexSize : 0;
        wo.landscape = !d.worldBlueprint && d.expLandscape;
        wo.lights = !d.worldBlueprint && d.expLights;
        wo.clip = part;
        wo.clipCenter = c;
        wo.clipRadius = d.expRadius * 100;
        wo.recenter = !d.worldBlueprint && d.expRecenter;
        if (filtered) wo.meshFilter = d.worldFilter;
        for (auto& sp : m_docs)
            if (sp.get() == &d) ExportWorldDoc(sp, wo);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("to %s", Narrow(m_settings.outputDir).c_str());
    ImGui::EndPopup();
}

void App::ExportWorldDoc(std::shared_ptr<Document> d, WorldExportOptions wo) {
    if (m_mountState != MountState::Ready || !d->world) return;
    if (m_exporting) {
        LOGW("an export is already running");
        return;
    }
    m_exporting = true;
    m_exportCancel = false;
    m_exportDone = m_exportFailed = 0;
    m_exportTotal = 1;
    {
        std::lock_guard lk(m_exportMutex);
        m_exportLast = d->gamePath;
    }
    // the viewer drops the landscape meshes once they are on the GPU: reload the map for them
    bool reload = wo.landscape && !d->worldBlueprint && !d->world->landscapes.empty();
    std::shared_ptr<const WorldData> world = d->world;
    std::string gp = d->gamePath, islands = d->worldIslands;
    bool sea = d->worldSea, proxies = d->worldProxies;
    ExportOptions eo = MakeExportOptions();
    m_jobs->Push([this, wo, world, gp, islands, sea, proxies, reload, eo] {
        std::shared_ptr<const WorldData> src = world;
        std::string err;
        bool ok = true;
        if (reload) {
            auto fresh = std::make_shared<WorldData>();
            WorldLoader loader(*m_api->cache);
            WorldOptions opt;
            opt.landscapeStep = 1;
            if (sea) {
                opt.proxies = proxies;
                opt.islands = islands;
            }
            opt.cancel = [this] { return m_exportCancel.load(); };
            ok = loader.Load(gp, opt, *fresh, &err);
            src = fresh;
        }
        Bytes glb;
        if (ok && !m_exportCancel)
            ok = ExportWorldGlb(*m_api->cache, *src, wo, glb, &err, [this](int done, int total) {
                m_exportDone = done;
                m_exportTotal = total;
            });
        if (ok && !m_exportCancel) {
            std::string rel = gp.substr(0, gp.rfind('.'));
            if (wo.clip) rel += Fmt("_part_%.0fm", wo.clipRadius / 100);
            if (!eo.keepDirectories) rel = rel.substr(rel.rfind('/') + 1);
            std::wstring out = eo.outputDir + L"\\" + Widen(rel) + L".glb";
            for (auto& ch : out) if (ch == L'/') ch = L'\\';
            CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
            if (WriteWholeFile(out, glb.data(), glb.size())) LOGI("exported %s (%s) -> %s", gp.c_str(), HumanSize(glb.size()).c_str(), Narrow(out).c_str());
            else { ok = false; err = "could not write " + Narrow(out); }
        }
        if (!ok) {
            m_exportFailed++;
            LOGW("export %s: %s", gp.c_str(), err.c_str());
        }
        m_exporting = false;
        Post([] {});
    });
}
void App::DrawTimeOfDayControls(Document& d) {
    if (timeOnStart >= 0) {   // --time
        d.todHour = std::clamp(timeOnStart, 0.f, 24.f);
        d.todEnabled = true;
        timeOnStart = -1;
    }
    if (d.todEnabled && !d.tod && !d.todLoading) {   // the curves of the map's region (or the one picked), in the background
        std::shared_ptr<Document> sp;
        for (auto& x : m_docs)
            if (x.get() == &d) sp = x;
        if (sp) {
            d.todLoading = true;
            std::string mapPath = d.world ? d.world->root : std::string(), want = d.todRegion;
            m_jobs->Push([this, sp, mapPath, want] {
                auto regions = TimeOfDayRegions(*m_api->cache);
                std::string region = want.empty() ? TimeOfDayRegionFor(mapPath, regions) : want, err;
                auto c = std::make_shared<TimeOfDayCurves>();
                bool ok = !region.empty() && LoadTimeOfDay(*m_api->cache, region, *c, &err);
                Post([this, sp, regions, region, c, ok, err] {
                    m_todRegions = regions;
                    sp->todLoading = false;
                    sp->todRegion = region;
                    if (ok) sp->tod = c;
                    else {
                        sp->todEnabled = false;
                        LOGW("day cycle: %s", region.empty() ? "no lighting curves in the game files" : err.c_str());
                    }
                });
            });
        }
    }
    FlowToggle("Day cycle", &d.todEnabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lights the sea like the game's day cycle: sun or moon, sky light, sky dome and fog colours from the "
                          "lighting curves of the region (Curves/Lighting/%s)", d.todRegion.empty() ? "..." : d.todRegion.c_str());
    if (!d.todEnabled) return;
    FlowNext(130);
    ImGui::SetNextItemWidth(130);
    int hh = (int)d.todHour % 24, mm = (int)((d.todHour - (int)d.todHour) * 60);
    ImGui::SliderFloat("##tod", &d.todHour, 0, 24, Fmt("time %02d:%02d", hh, mm).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hour of the day cycle (in game hours)");
    if (m_todRegions.size() > 1) {
        FlowNext(120);
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##todr", d.todRegion.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (auto& r : m_todRegions)
                if (ImGui::Selectable(r.c_str(), r == d.todRegion) && r != d.todRegion && !d.todLoading) {
                    d.todRegion = r;
                    d.tod = nullptr;   // reloads next frame
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lighting curve set (the map's sea is picked by default)");
    }
}

// Map effects: the nearest emitters within kFxRange of the camera are simulated (their systems load in the background).
static constexpr float kFxRange = 12000;   // cm
static constexpr size_t kFxMax = 48;

void App::UpdateWorldParticles(Document& d) {
    if (!d.world || !d.worldParticles || d.world->emitters.empty()) {
        d.fxLive.clear();
        return;
    }
    const auto& em = d.world->emitters;
    Vec3 eye = d.camera.Eye();
    std::vector<std::pair<float, size_t>> closest;
    for (size_t i = 0; i < em.size(); i++) {
        float dist = Length(em[i].transform.Translation() - eye);
        if (dist < kFxRange) closest.push_back({dist, i});
    }
    if (closest.size() > kFxMax) {
        std::partial_sort(closest.begin(), closest.begin() + kFxMax, closest.end());
        closest.resize(kFxMax);
    }
    std::unordered_set<size_t> keep;
    float dt = ImGui::GetIO().DeltaTime;
    std::shared_ptr<Document> sp;
    for (auto& [dist, i] : closest) {
        keep.insert(i);
        auto [tit, added] = d.fxTemplates.try_emplace(em[i].system);
        Document::FxTemplate& t = tit->second;
        if (added) {   // load the system once per path
            if (!sp)
                for (auto& x : m_docs)
                    if (x.get() == &d) sp = x;
            std::string path = em[i].system;
            m_jobs->Push([this, sp, path] {
                auto fx = std::make_shared<Document::Attachment>();
                std::string err;
                i64 e = m_api->ResolveFile(path);
                auto pkg = e >= 0 ? m_api->cache->Load((u32)e, &err) : nullptr;
                bool ok = pkg && pkg->MainExport() && LoadParticleEffect(*pkg, *fx, &err);
                Post([sp, path, fx, ok] {
                    auto& tt = sp->fxTemplates[path];
                    tt.state = ok ? 1 : 2;
                    if (ok) tt.fx = fx;
                });
            });
        }
        if (t.state != 1) continue;
        auto& live = d.fxLive[i];
        if (!live.sim) {
            live.sim = std::make_shared<ParticleSim>();
            live.sim->Init(*t.fx->ps, (u32)i * 2654435761u + 1);
        }
        live.time += dt;
        if (live.time > t.fx->psLength * 1.5f && live.sim->Count() == 0) {
            live.sim->Reset();
            live.time = 0;
        }
        live.sim->AdvanceTo(live.time);
    }
    for (auto it = d.fxLive.begin(); it != d.fxLive.end();)
        it = keep.count(it->first) ? std::next(it) : d.fxLive.erase(it);
    if (!closest.empty()) m_busyFrames = 3;
}

void App::DrawWorldParticles(Document& d) {
    d.fxDrawn = 0;
    if (!d.world || !d.worldParticles) return;
    std::vector<ParticleSprite> sprites;
    for (auto& [i, live] : d.fxLive) {
        if (!live.sim || i >= d.world->emitters.size()) continue;
        auto& t = d.fxTemplates[d.world->emitters[i].system];
        if (!t.fx) continue;
        live.sim->Sprites(sprites);
        d.fxDrawn += DrawParticleSprites(*m_renderer, d.camera, sprites, t.fx->looks, d.world->emitters[i].transform);
    }
}

void App::DrawWorld(Document& d) {
    auto reload = [&] {
        for (auto& sp : m_docs)
            if (sp.get() == &d) {
                d.worldScene.reset();
                d.world.reset();
                d.worldSel = {};
                d.fxLive.clear();   // emitter indices change
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
    if (!d.worldBlueprint) {   // map-only switches
        FlowToggle("Landscape", &d.worldDraw.landscape);
        FlowToggle("Water", &d.worldDraw.water);
        FlowToggle("Lights", &d.worldDraw.lights);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The map's sun, sky light and the %d point / spot lights nearest the camera", kMaxSceneLights);
        FlowToggle("Fog", &d.worldDraw.fog);
        FlowToggle("Gizmos", &d.worldDraw.gizmos);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Light ranges / spot cones (yellow) and decal boxes (blue) within 300 m");
        DrawTimeOfDayControls(d);
    }
    if (!w.emitters.empty()) {
        FlowToggle("Particles", &d.worldParticles);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Simulates the %zu nearest of the %zu particle emitters within %.0f m of the camera", kFxMax, w.emitters.size(), kFxRange / 100);
    }
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
    DrawWorldExport(d);
    PopoutButton(d, true);
    EndToolbar();
    seaOptions();
    const auto& st = sc.Stats();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%zu levels | %zu instances of %zu meshes, %zu landscape patches, %zu lights, %zu decals, %zu emitters | drawn: %u instances, %u meshes, "
                        "%u patches, %.2fM tris, %d particles | %s GPU",
                        w.levels.size(), w.InstanceCount(), w.groups.size(), w.landscapes.size(), w.lights.size(), w.decals.size(), w.emitters.size(), st.instancesDrawn,
                        st.meshesDrawn, st.landscapesDrawn, m_renderer->DrawnTriangles() / 1e6, d.fxDrawn, HumanSize(sc.GpuBytes()).c_str());
    ImGui::PopTextWrapPos();

    ImVec2 avail = MinViewportAvail();
    float side = avail.x > 640 ? std::min(360.f, avail.x * 0.3f) : 0.f;   // the outliner is dropped in narrow windows
    Camera& cam = d.camera;
    UpdateWorldParticles(d);
    d.worldDraw.timeOfDay = nullptr;
    if (d.todEnabled && d.tod && !d.worldBlueprint) {
        d.todLight = EvaluateTimeOfDay(*d.tod, d.todHour);
        d.worldDraw.timeOfDay = &d.todLight;
    }
    sc.Lighting(cam, d.worldDraw, d.worldLighting);
    d.draw.lighting = d.worldBlueprint ? nullptr : &d.worldLighting;   // a lone actor: the viewer's key light
    ImVec2 vp0 = ImGui::GetCursorScreenPos(), vsize(side > 0 ? avail.x - side - ImGui::GetStyle().ItemSpacing.x : avail.x, avail.y);
    DrawViewportScene(
        *m_renderer, cam, d.draw, vsize, true, frameWorld,
        [&] {
            sc.Draw(cam, avail.y, d.worldDraw);
            DrawWorldParticles(d);
            sc.DrawOverlays(cam, d.worldDraw);
            if (d.worldSel.kind != WorldScene::Pick::None) {   // selection: its mesh bounds as a box
                const Vec3 &a = d.worldSel.boundsMin, &b = d.worldSel.boundsMax;
                Vec3 c[8];
                for (int k = 0; k < 8; k++) c[k] = d.worldSel.transform.TransformPoint({k & 1 ? b.x : a.x, k & 2 ? b.y : a.y, k & 4 ? b.z : a.z});
                static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                std::vector<Vec3> lines;
                for (auto& ed : e) { lines.push_back(c[ed[0]]); lines.push_back(c[ed[1]]); }
                m_renderer->DrawLines(lines, 0xff30d0ffu);
            }
        },
        [&](ImVec2 uv) {
            Vec3 o, dir;
            ViewRay(cam, vsize, uv, o, dir);
            d.worldSel = sc.Raycast(o, dir, d.worldDraw);
        });
    if (pickU >= 0 && vsize.x >= 8) {   // --pick: one automated click
        Vec3 o, dir;
        ViewRay(cam, vsize, ImVec2(pickU, pickV), o, dir);
        d.worldSel = sc.Raycast(o, dir, d.worldDraw);
        LOGI("pick %.3f,%.3f: kind %d index %zu instance %u at %.1f m", pickU, pickV, (int)d.worldSel.kind, d.worldSel.index, d.worldSel.instance, d.worldSel.distance / 100);
        pickU = pickV = -1;
    }
    if (side <= 0) {
        DrawWorldSelection(d, vp0, vsize);
        return;
    }
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
    DrawWorldSelection(d, vp0, vsize);
}

// What a click in the map picked (WorldScene::Raycast), as a panel over the viewport's top-left corner.
void App::DrawWorldSelection(Document& d, ImVec2 vp0, ImVec2 vsize) {
    const WorldScene::Pick& p = d.worldSel;
    if (p.kind == WorldScene::Pick::None || !d.world) return;
    const WorldData& w = *d.world;
    std::string title, mesh, levelPath, blueprint, actorLine;
    u32 level = 0, actor = UINT32_MAX;
    if (p.kind == WorldScene::Pick::Instance && p.index < w.groups.size() && p.instance < w.groups[p.index].instances.size()) {
        const auto& in = w.groups[p.index].instances[p.instance];
        mesh = w.groups[p.index].mesh;
        level = in.level;
        actor = in.actor;
    } else if (p.kind == WorldScene::Pick::Spline && p.index < w.splines.size()) {
        mesh = w.splines[p.index].mesh;
        level = w.splines[p.index].level;
        actor = w.splines[p.index].actor;
    } else if (p.kind == WorldScene::Pick::Landscape && p.index < w.landscapes.size()) {
        title = "Landscape " + w.landscapes[p.index].name;
        level = w.landscapes[p.index].level;
    } else {
        d.worldSel = {};
        return;
    }
    if (!mesh.empty()) title = mesh.substr(mesh.find_last_of('.') + 1);
    if (level < w.levels.size()) levelPath = w.levels[level].path;
    if (actor < w.actors.size()) {
        const WorldActor& a = w.actors[actor];
        actorLine = a.name + "  (" + a.cls + ")";
        blueprint = a.blueprint;
    }
    float u = ImGui::GetFontSize() / 16.f;
    ImGui::SetCursorScreenPos(ImVec2(vp0.x + 8 * u, vp0.y + 8 * u));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 21, 28, 225));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * u, 6 * u));
    ImGui::BeginChild("##worldsel", ImVec2(std::min(vsize.x - 16 * u, 420 * u), 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(title.c_str());
    if (!mesh.empty()) ImGui::TextDisabled("%s", mesh.c_str());
    if (!actorLine.empty()) ImGui::TextDisabled("actor %s", actorLine.c_str());
    if (!levelPath.empty()) ImGui::TextDisabled("level %s", levelPath.substr(levelPath.find_last_of('/') + 1).c_str());
    Vec3 t = p.transform.Translation();
    ImGui::TextDisabled("at %.0f %.0f %.0f, hit %.1f m away", t.x, t.y, t.z, p.distance / 100);
    ImGui::PopTextWrapPos();
    auto open = [&](const std::string& path) {
        i64 e = m_api->ResolveFile(path);
        if (e >= 0) OpenFile((u32)e, true);
        else LOGW("not found: %s", path.c_str());
    };
    if (!mesh.empty() && ImGui::SmallButton("Open mesh")) open(mesh);
    if (!blueprint.empty()) {
        if (!mesh.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("Open Blueprint")) open(blueprint);
    }
    if (!levelPath.empty()) {
        if (!mesh.empty() || !blueprint.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("Open level")) open(levelPath);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Focus")) {
        Vec3 c = p.transform.TransformPoint((p.boundsMin + p.boundsMax) * 0.5f);
        float r = Length(p.boundsMax - p.boundsMin) * 0.5f * p.transform.MaxScale();
        d.camera.target = c;
        d.camera.distance = std::max(r * 2.5f, d.camera.nearZ * 4);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy path")) CopyToClipboard(!mesh.empty() ? mesh : title);
    ImGui::SameLine();
    if (ImGui::SmallButton("X")) d.worldSel = {};
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
                if (opt.animationSounds)   // each clip's notify sounds + timeline next to its own name
                    for (auto& p : pkgs) {
                        std::vector<std::string> w;
                        ExportAnimationSounds(*m_api, *p, ExportPathFor(opt, *p, L""), w);
                    }
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
                    if (ok && mode == 0 && o.animationSounds && pkg->MainExport()) {   // animations: notify sounds + timeline
                        std::string cls = pkg->ExportClass(pkg->MainExport() - 1);
                        if (IsAnimSequenceClass(cls) || cls == "AnimMontage") ExportAnimationSounds(*m_api, *pkg, ExportPathFor(o, *pkg, L""), written);
                    }
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
    ImGui::Checkbox("Animations: also export notify sounds (.wav) and their timeline (.notifies.json)", &m_settings.exportAnimSounds);
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
        m_pakListed = false;   // the game folder may have changed: list its archives again
        // not loaded yet (Idle): mounting waits for Load in the Archives tab
        if (m_mountState != MountState::Idle && (apply || changed || m_mountState != MountState::Ready) && m_mountState != MountState::Mounting) {
            m_exportCancel = true;
            m_jobs->WaitIdle();
            m_docs.clear();
            m_assets.reset();
            m_classes.Stop();
    m_api->Unmount();
            m_searchResults.clear();
            m_lastSearch.clear();
            StartMount();
        }
    }
    ImGui::EndPopup();
}

void App::RequestGraphShaders(Document& d) {
    if (!d.gpuMesh) return;
    std::vector<std::shared_ptr<GpuMaterial>> mats;
    for (auto& m : d.gpuMesh->materials)
        if (m && !m->graph && !m->path.empty()) mats.push_back(m);
    if (mats.empty()) return;
    m_jobs->Push([this, mats] {
        for (auto& m : mats) {
            auto g = m_assets->GraphShader(m->path);
            Post([m, g] { if (!m->graph) m->graph = g; });
        }
    });
}
