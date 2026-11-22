#pragma once
#include "app/Api.h"
#include "app/DepGraph.h"
#include "app/ClassIndex.h"
#include "render/GpuAssets.h"
#include "render/WorldScene.h"
#include "render/ParticleDraw.h"
#include "ue/Material.h"
#include "ue/Mesh.h"
#include "ue/MeshExtras.h"
#include "ue/AnimPose.h"
#include "ue/AnimAssets.h"
#include "ue/Particles.h"
#include "export/Exporter.h"
#include "export/MeshExport.h"
#include "audio/Player.h"
#include "ue/DataTable.h"
#include "ue/Shaders.h"
#include "ue/MaterialGraph.h"
#include <thread>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

struct ImFont;
struct ImVec2;

// Small worker pool; results that touch UI state are posted back with App::Post (run on the UI thread).
class JobPool {
public:
    explicit JobPool(int threads);
    ~JobPool();
    void Push(std::function<void()> job);
    void WaitIdle();
    int Pending() const { return m_pending; }

private:
    std::vector<std::thread> m_threads;
    std::deque<std::function<void()>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv, m_idle;
    std::atomic<int> m_pending{0};
    bool m_stop = false;
};

enum class DocView { Properties, Texture, Model, Material, Animation, World };

// App.cpp: an interactive 3D viewport (orbit / fly / pan / zoom) rendering `scene`; onClick gets the uv of a click.
void DrawViewportScene(Renderer& r, Camera& cam, const DrawOptions& draw, ImVec2 size, bool allowFly, const std::function<void()>& frame,
                       const std::function<void()>& scene, const std::function<void(ImVec2)>& onClick = {});

struct CollisionData;
struct Document {
    u64 id = 0;
    u32 entry = 0;
    std::string gamePath, name, className;
    std::atomic<bool> loading{true};
    std::string error;
    std::shared_ptr<Package> pkg;

    bool newTab = false;   // FModel "New Tab" placeholder (no asset yet)
    int syntax = 0;        // editor colouring: 0 JSON, 1 C++ (decompiled Blueprints)

    // properties (JSON editor)
    std::string json;
    std::vector<u32> lines;   // line start offsets
    u32 maxCols = 0;          // longest line, for the horizontal scroll range
    u32 selA = 0, selB = 0;   // selection as byte offsets (anchor, caret)
    bool searchOpen = false, focusSearch = false, searchUp = false, wholeWord = false, matchCase = false;
    char find[256] = {};
    int scrollToLine = -1;
    float sideW = 0;          // preview column width (0 = default)
    bool hasSide = false;     // has a preview column (set while drawing)
    bool sideHidden = false;  // preview column closed with the button at the right of the tab strip
    int imagePage = 0;        // material textures shown one at a time

    // texture
    TextureInfo tex;
    int texMip = 0;
    std::shared_ptr<GpuTexture> texture;
    float texZoom = 0;   // 0 = fit
    float texPanX = 0, texPanY = 0;
    bool texCheckerboard = true;

    // model
    bool hasMesh = false;
    std::shared_ptr<MeshData> mesh;
    int lod = 0;
    std::shared_ptr<GpuMesh> gpuMesh;
    Camera camera;
    DrawOptions draw;

    // animation
    std::shared_ptr<AnimSequenceData> anim;
    std::shared_ptr<AnimPoser> poser;
    std::string animMesh;          // mesh the animation is shown on
    char animMeshEdit[512] = {};
    float animTime = 0, animSpeed = 1;
    bool animPlaying = true, animLoop = true, showBones = false;
    std::vector<Vec3> boneLines;   // bone segments for the viewport (world space)
    // montage / blend space: shown through the animation view (d.anim is a stand-in with the skeleton and length)
    std::shared_ptr<MontageInfo> montage;
    std::shared_ptr<BlendSpaceInfo> blendSpace;
    std::vector<std::shared_ptr<AnimSequenceData>> subAnims;   // per slot-0 segment / per blend sample
    float bsX = 0, bsY = 0;
    std::vector<float> bsWeights;
    // animation blueprint: baked state machines (graph in the side panel)
    std::shared_ptr<AnimBlueprintInfo> animBp;
    int smSel = 0, smState = -1;
    // particle system: CPU simulation drawn as sprites in the side panel
    std::shared_ptr<ParticleSystemInfo> particles;
    std::shared_ptr<ParticleSim> psim;
    std::vector<std::shared_ptr<GpuTexture>> psTextures;   // per emitter (material's texture, masks as alpha)
    std::vector<char> psVisible;
    float psTime = 0, psSpeed = 1, psLength = 1, psYaw = -135, psPitch = -15, psDist = 300;
    Vec3 psCentre;
    bool psPlaying = true, psLoop = true, psGrid = true, psSolo = false;
    int psSel = -1, psModule = -1;
    std::shared_ptr<CollisionData> collision;   // loaded when "Collision" is first switched on
    std::string collisionNote;
    bool showCollision = false;
    std::vector<Vec3> collisionLines;
    // mesh sockets / morph targets (read when the model view first shows)
    bool meshExtrasLoaded = false, showSockets = false, morphDirty = false;
    std::vector<MeshSocket> sockets;
    std::vector<Vec3> socketLines[3];   // X / Y / Z axes
    std::vector<MorphTarget> morphs;
    // material variants (skins): sibling material instances by name, per slot; overrides applied to a copy of gpuMesh
    std::vector<std::vector<std::string>> matVariants;   // per slot: candidate material object paths (empty until listed)
    std::vector<std::string> matOverrides;               // per slot: chosen variant ("" = the mesh's own)
    bool matVariantsListed = false;
    std::vector<float> morphWeights;
    char morphFilter[64] = {};
    // meshes attached to sockets (a weapon in a hand); they follow the animation's pose
    struct Attachment {
        std::string mesh, socket;   // mesh: the attached asset (a mesh or a particle system)
        std::shared_ptr<MeshData> data;
        std::shared_ptr<GpuMesh> gpu;
        std::shared_ptr<ParticleSystemInfo> ps;   // particle system attachments: simulated in the viewport
        std::shared_ptr<ParticleSim> sim;
        std::vector<ParticleLook> looks;
        float psTime = 0, psLength = 1;
        Mat4 world = Mat4::Identity();   // this frame's attachment -> mesh space
        bool placed = false;             // false: the socket is not on the current mesh
        Mat4 offset = Mat4::Identity();  // notify effects: location / rotation offset from the socket
        bool follow = true;              // notify effects: false = stays where it was spawned
    };
    std::vector<Attachment> attachments;
    // AnimNotify sounds and particle effects fired as the animation plays over their times
    bool notifiesLoaded = false, playNotifies = true;
    std::vector<AnimNotifyInfo> notifies;
    std::unordered_map<std::string, std::string> notifyParents;   // skeleton asset bone -> parent (lower case): sockets missing on the mesh
    float notifyPrevTime = -1;   // -1: notifies at 0 s fire on the first frame
    std::vector<Attachment> notifyFx;   // spawned effects, dropped once their particles are gone
    struct ItemPart { std::shared_ptr<GpuMesh> mesh; Mat4 local; };   // a cosmetic item: its Blueprint's meshes relative to the actor
    struct HeldItem {
        std::string cls;
        std::shared_ptr<const std::vector<ItemPart>> parts;
        int hand = 0;           // 0 right, 1 left
        bool visible = true, placed = false;
        Mat4 world = Mat4::Identity();
    };
    std::vector<HeldItem> heldItems;   // AnimNotify_SpawnCosmeticItem results, reset when the clip loops
    char attachEdit[512] = {};
    int attachSocket = -1;   // -1: pick a hand socket
    bool attachLoading = false;
    std::string attachError;

    // world (.umap)
    bool isWorld = false;
    std::atomic<bool> cancel{false};          // set when the tab closes: stops background loading
    std::shared_ptr<WorldData> world;
    std::shared_ptr<WorldScene> worldScene;
    WorldDrawOptions worldDraw;
    bool worldLoading = false;
    SceneLighting worldLighting;   // rebuilt every frame from the camera
    WorldScene::Pick worldSel;     // what a click in the viewport picked
    int worldDone = 0, worldTotal = 0;
    std::string worldStage;
    char worldFilter[128] = {};
    int worldOutliner = 0;   // 0 meshes, 1 levels
    bool worldSea = false;        // sot_world_layout_2: the whole open world
    bool worldBlueprint = false;  // a Blueprint actor class assembled from its components (shown through the world view)
    bool bpEnv = false;           // Blueprint: sea plane, day cycle sky and fog around it (on for ships)
    bool worldParticles = true;   // simulate the ParticleSystemComponents near the camera
    // day cycle: the region's lighting curves evaluated at todHour
    bool todEnabled = true;
    float todHour = 12;
    std::string todRegion;                     // empty: picked from the map path
    std::shared_ptr<TimeOfDayCurves> tod;      // loaded curves (null while loading / none)
    bool todLoading = false;
    TimeOfDayLight todLight;
    struct FxTemplate { int state = 0; std::shared_ptr<Attachment> fx; };   // state 0 loading, 1 ready, 2 failed; fx: system + looks
    std::unordered_map<std::string, FxTemplate> fxTemplates;   // by system path
    struct FxLive { std::shared_ptr<ParticleSim> sim; float time = 0; };
    std::unordered_map<size_t, FxLive> fxLive;                 // by WorldData::emitters index
    int fxDrawn = 0;
    bool worldProxies = true;     // whole sea: islands as their merged LOD proxies
    char worldIslands[256] = {};  // whole sea: island filter (comma terms, '-' excludes)
    // "Export glTF" popup of the world view: everything loaded or the part around the view target
    bool expPart = false, expRecenter = true, expFiltered = false, expTextures = true, expLandscape = true, expLights = true;
    float expRadius = 100;   // m
    int expTexSize = 1;      // 512 / 1024 / 2048 / full

    // audio: a .wem, or a sound bank (WwiseBank) with embedded media decoded on demand
    std::shared_ptr<AudioPcm> audio;
    WemInfo wem;
    std::string audioError;
    bool isBank = false;
    BankInfo bank;
    int bankSel = -1;
    // a Wwise event (shown like a bank: bank.wems are the sounds it can play, their bytes copied into mediaBytes)
    bool isEvent = false;
    std::string eventName, eventBank;
    u32 eventId = 0;
    Bytes mediaBytes;
    std::vector<u8> mediaWhere;   // WwiseEventMedia::Where per bank.wems entry
    // the event's switch / state groups (names guessed) and the chosen value (-1 any); the media list shows what plays with it
    struct EventSwitch {
        u32 group = 0;
        bool state = false;
        std::string name;
        std::vector<std::pair<u32, std::string>> values;
        int sel = -1;
    };
    std::vector<EventSwitch> eventSwitches;
    std::vector<std::vector<WwiseMediaCondition>> eventConds;   // per bank.wems entry
    std::vector<std::string> eventWhen;                          // per bank.wems entry: "FootwearType = Boots, surface = Sand | Dirt"
    // bank: its events (HIRC), named from the WwiseEvent assets
    int bankTab = 0;   // 0 media, 1 events
    std::vector<std::pair<u32, std::string>> bankEvents;
    bool bankEventsListed = false;
    char eventEdit[128] = {};
    std::unordered_map<u32, std::shared_ptr<AudioPcm>> bankPcm;
    std::vector<std::pair<float, float>> peaks;   // waveform min/max per column
    const AudioPcm* peaksFor = nullptr;
    int peaksWidth = 0;

    // DataTable / CurveTable rows
    std::shared_ptr<TableData> table;
    char tableFilter[128] = {};
    std::vector<std::array<char, 64>> tableColFilters;   // per column: substring, or >x <x >=x <=x =x !x

    // compiled shaders (Decompile Shaders): the list in the side panel, the selected one as text in the editor
    std::shared_ptr<std::vector<ShaderEntry>> shaders;
    int shaderSel = -1, shaderMode = 0;   // 0 HLSL, 1 bit-exact HLSL, 2 assembly, 3 reflection
    char shaderFilter[128] = {};

    // material graph (Material HLSL): the translated code in the editor, the node graph in the side panel
    std::shared_ptr<std::vector<MaterialGraphNode>> matGraph;
    float graphZoom = 0, graphPanX = 0, graphPanY = 0;   // zoom 0 = fit on the next frame
    int graphSel = -1;

    // material
    bool hasMaterial = false;
    bool materialSphere = true;   // material tabs: the 3D sphere view (else the texture pages)
    MaterialInfo material;
    std::vector<std::pair<std::string, std::shared_ptr<GpuTexture>>> materialTextures;

    DocView preferred = DocView::Properties;
    bool selectPreferred = true;
};

class App {
public:
    App();
    ~App();
    bool Init(void* hwnd, struct ID3D11Device* dev, struct ID3D11DeviceContext* ctx);
    void Frame();   // builds the whole UI for one frame
    void Post(std::function<void()> fn);   // run on the UI thread next frame
    bool WantsQuit() const { return m_quit; }
    bool Busy() const;
    bool Loading() const;   // archives mounting, jobs queued or documents loading   // something animating / loading: keep rendering frames
    // separate 3D viewer window (Gui.cpp creates / destroys it to match PopoutDoc)
    u64 PopoutDoc() const { return m_popoutDoc; }
    void ClosePopout();   // the window was closed: its tab shows the view again until another 3D tab is selected
    bool TakePopoutFocus() { bool f = m_popoutFocus; m_popoutFocus = false; return f; }
    bool TakePopoutRaise() { bool f = m_popoutRaise; m_popoutRaise = false; return f; }
    std::string PopoutTitle() const;
    void FramePopout();   // builds the separate window's UI (its ImGui context is current)

    ImFont* monoFont = nullptr;
    ImFont* boldFont = nullptr;
    int startTab = -1;   // --tab: initial left tab
    std::string startFolder;   // --folder: initial explorer folder (game path)
    std::vector<std::string> openOnStart;   // asset paths from the command line
    float pickU = -1, pickV = -1;           // --pick u,v: click the 3D view there once a map has loaded (automation)
    bool popoutOnStart = false;             // --popout: show the opened tab's 3D view in the separate window
    std::vector<std::string> attachOnStart; // --attach mesh@socket: attached to the opened tab's mesh once loaded (automation)
    bool graphOnStart = false;              // --graph: model tabs open with Graph shader on and the parameters expanded
    std::string variantOnStart;             // --variant name: model tabs switch the first slot having it to that material variant
    float timeOnStart = -1;                 // --time h: maps open at this hour of the day cycle (automation)

private:
    void StartMount();
    void DrawMenu();
    // main window (Shell.cpp): FModel layout
    void DrawLeftPanel(float width, float height);
    void DrawArchivesTab();
    void DrawFoldersTab();
    void DrawPackagesTab();
    void DrawRightPanel(float width, float height);
    void DrawExplorer();
    void DrawDocTabs(float height);
    void DrawDocContent(Document& d);
    void DrawJsonEditor(Document& d, ImVec2 size);
    void DrawImagePanel(Document& d);
    void DrawAudioPanel(Document& d);
    void DrawTablePanel(Document& d);
    void LoadAudioDocument(std::shared_ptr<Document> d);
    void FillEventDocument(Document& d, const struct WwiseEventInfo& ev);   // worker: the event's sounds as a bank-like list
    void OpenBankEvent(Document& bankDoc, u32 eventId);                     // an event of an open bank in a new tab
    bool ExportAudioEntry(u32 entry, const ExportOptions& opt, std::vector<std::string>& written, std::string* error);
    void PlayAudio(Document& d, std::shared_ptr<AudioPcm> pcm, double from = 0);
    void DrawLog(float width, float open);   // open: 0 shut .. 1 open (slides)
    float m_docHeaderH = 0;   // height of the document tab row (+ strip), for the content fade
    void DrawSearchWindow();
    void DrawFolderMenu(u32 folder);
    void Breadcrumb(u32 folder);
    void HandleHotkeys();
    void SelectFolder(u32 folder);   // select + reveal in the tree, show it in the explorer
    void AddTab();
    Document* ActiveDoc();
    std::string& FolderFilter();
    // FModel features (Features.cpp)
    void PlaceDocument(std::shared_ptr<Document> d, bool newTab);   // into the selected tab unless newTab (FModel)
    void OpenTextTab(const std::string& title, std::string text, u32 entry, bool newTab, int syntax = 0);
    void DecompileBlueprintTab(u32 entry);
    void DecompileShadersTab(u32 entry);
    void ShowShader(std::shared_ptr<Document> d, int index, int mode);   // decompiles in the background into the editor
    void DrawShaderPanel(Document& d);
    void MaterialHlslTab(u32 entry);
    void DrawMaterialGraphPanel(Document& d);
    void ShowMetadata(u32 entry, bool newTab = false);
    void ShowArchivesInfo();
    void FindReferences(u32 entry);
    void StartReferenceIndex(bool rebuild = false);   // rebuild: an index without text keys is replaced (kept usable meanwhile)
    void DrawReferencesTab();
    void DrawDependencyGraph();   // DepGraphView.cpp
    void NavigateReferences(u32 entry);   // re-centre the References tab on a package (keeps a back history)
    void DrawLocalizationTab();
    void DrawBackupWindow();
    void CompareBackup(const std::wstring& file);
    std::shared_ptr<GpuTexture> Thumbnail(u32 entry);   // asynchronous; nullptr until decoded (or not a texture)
    void CopyTextureImage(std::shared_ptr<Document> d);
    u32 FolderByPath(std::string_view path) const;      // deepest existing folder along a game path
    bool IsFavorite(u32 folder) const;
    void ToggleFavorite(u32 folder);
    // viewers (App.cpp)
    void DrawModel(Document& d);
    void DrawAnimation(Document& d);
    // AnimPanels.cpp
    bool AnimAssetPose(Document& d, std::vector<Bone>& pose);
    void DrawAnimAssetHeader(Document& d);
    void DrawStateMachinePanel(Document& d);
    void DrawParticlePanel(Document& d);   // ParticlePanel.cpp
    void DrawGraphParams(GpuMaterial& mat);   // live scalar / vector parameter editing (graph shader)
    void DrawAttachControls(Document& d);   // attach a mesh to one of the shown mesh's sockets
    void AttachMesh(std::shared_ptr<Document> d, std::string meshPath, std::string socket);   // loads in the background (mesh or particle system)
    bool LoadParticleEffect(const Package& pkg, Document::Attachment& at, std::string* error);
    void PlaceAttachments(Document& d, const std::vector<Mat4>* boneWorld);   // null: reference pose; steps attached particles
    void RequestGraphShaders(Document& d);   // compiles the graph shaders of the shown mesh's materials in the background
    void DrawWorld(Document& d);
    void PopoutButton(Document& d, bool flow);
    void DrawWorldSelection(Document& d, ImVec2 vp0, ImVec2 vsize);
    void UpdateWorldParticles(Document& d);   // picks the emitters near the camera, loads their systems, steps the simulations
    void DrawTimeOfDayControls(Document& d);  // map toolbar: day cycle toggle, hour, lighting region (loads the curves)
    std::vector<std::string> m_todRegions;    // lighting curve sets in the game (listed on first use)
    void DrawWorldParticles(Document& d);     // inside the viewport scene
    static bool Has3dView(const Document& d);
    void LoadWorld(std::shared_ptr<Document> d);
    void ListMaterialVariants(Document& d);
    void ApplyMaterialVariants(std::shared_ptr<Document> d);   // builds the chosen materials in a job, then swaps them in
    // AnimNotify playback (AnimNotifies.cpp)
    struct NotifySound { int state = 0; std::vector<std::shared_ptr<AudioPcm>> clips; };   // 0 loading, 1 ready, 2 nothing playable
    struct NotifyFxTemplate { int state = 0; std::shared_ptr<Document::Attachment> fx; };
    std::mutex m_notifyMutex;
    std::unordered_map<std::string, NotifySound> m_notifySounds;          // by event path
    std::unordered_map<std::string, NotifyFxTemplate> m_notifyFxTemplates; // by particle system path
    struct ItemTemplate { int state = 0; std::shared_ptr<const std::vector<Document::ItemPart>> parts; };
    std::unordered_map<std::string, ItemTemplate> m_itemTemplates;       // by Blueprint class path
    AudioPlayer m_notifyPlayers[6];                                       // overlapping notify sounds (WinMM mixes the devices)
    void LoadNotifies(Document& d);
    void FireNotifies(Document& d, float from, float to, bool itemsOnly = false);   // notifies with from < time <= to (itemsOnly: held items, silently)
    void UpdateNotifyFx(Document& d, const std::vector<Mat4>* boneWorld);
    void DrawNotifyToggle(Document& d);
    void DrawNotifyStrip(Document& d, float x0, float x1);   // notify markers under the frame slider (x0..x1 = its frame)
    void DrawWorldExport(Document& d);                      // toolbar button + options popup
    void ExportWorldDoc(std::shared_ptr<Document> d, WorldExportOptions wo);
    void LoadAnimationMesh(std::shared_ptr<Document> d, std::string meshPath);
    bool LoadAnimationMeshNow(Document& d, const std::string& meshPath);
    void DrawFileMenu(u32 entry);
    void ReloadArchives();
    void CloseDocument(u64 id);   // 0 = all
    void CloseOtherDocuments(u64 keep);
    void DrawSettings();
    void DrawStatusBar();
    void DrawExportProgress();

    void OpenFile(u32 entry, bool newTab = false);   // FModel: into the selected tab unless newTab
    void LoadDocument(std::shared_ptr<Document> d);
    void RebuildMeshLod(Document& d);
    void ExportFiles(std::vector<u32> entries, int mode, int kind = 0);   // mode 0 by type, 1 raw, 2 json; kind 1 textures, 2 meshes, 3 worlds, 4 animations
    void ExportCurrentFolder();
    void UpdateSearch();
    std::vector<u32> FolderFilesRecursive(u32 folder) const;
    ExportOptions MakeExportOptions() const;

    Settings m_settings;
    std::unique_ptr<Api> m_api;
    std::unique_ptr<JobPool> m_jobs;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<GpuAssets> m_assets;
    std::shared_ptr<GpuMesh> m_sphere;
    void* m_hwnd = nullptr;

    std::mutex m_postMutex;
    std::vector<std::function<void()>> m_posted;

    enum class MountState { Idle, Mounting, Ready, Failed } m_mountState = MountState::Idle;
    std::string m_mountError;
    double m_mountSeconds = 0;

    u32 m_folder = 0;                       // selected folder (0 = root)
    std::unordered_set<u32> m_expanded;     // open folders in the tree
    std::unordered_map<u32, double> m_expandedAt;   // when a folder was opened (its rows fade in)
    u32 m_scrollTreeTo = UINT32_MAX;        // tree row to bring into view next frame
    int m_leftTab = 0;   // 0 archives, 1 folders, 2 packages
    int m_selPak = -1;
    std::vector<u64> m_pakSizes;
    std::vector<std::pair<std::string, u64>> m_pakList;   // before Load: the game folder's .pak files (name, size), not mounted
    bool m_pakListed = false;
    int m_explorerFilter = 0;   // category: 0 all, 1 packages, 2 maps, 3 other files
    bool m_explorerVisible = true;   // explorer instead of the document tabs (FModel "Assets Explorer" toggle)
    std::string m_explorerView;   // folder / filter the explorer last showed (scroll resets when it changes)
    std::unordered_map<u32, std::string> m_folderFilters;   // "Filter assets in current folder..." per folder
    char m_filterBuf[256] = {};
    u32 m_filterFolder = UINT32_MAX;
    i64 m_selected = -1;            // selected package (Packages list / explorer)
    u32 m_selFolder = UINT32_MAX;   // folder card selected in the explorer
    bool m_showSearch = false, m_focusGlobalSearch = false;   // Packages > Search window
    int m_searchTab = 0;                       // 0 search, 1 references, 2 localization, 3 content
    // Content tab (SearchContent): runs on its own thread, the table shows the last finished search
    struct ContentJob {
        ContentQuery query;
        std::atomic<bool> cancel{false}, done{false};
        std::atomic<int> progress{0}, total{0};
        ContentSearchResult result;
        std::string error;
        bool ok = false;
    };
    std::shared_ptr<ContentJob> m_contentJob;
    char m_csClass[128] = {}, m_csUses[512] = {}, m_csProp[128] = {}, m_csValue[256] = {}, m_csName[256] = {}, m_csFolder[512] = {}, m_csTextKey[160] = {};
    bool m_csExact = false, m_csPendingIndex = false;
    int m_csSelected = -1;
    void DrawContentTab();
    void DrawExportResultsMenu(const std::vector<u32>& entries);   // inside a popup: export a result list
    void StartContentSearch();
    char m_locQuery[256] = {};
    std::string m_locLast;
    std::vector<const LocEntry*> m_locResults;
    bool m_locInKeys = false;
    bool m_searchMatchCase = false, m_searchRegex = false;
    std::string m_searchError;
    // reference index (Api::references): built once in the background, cached on disk
    std::atomic<int> m_refDone{0}, m_refTotal{0};
    bool m_refReady = false, m_refBuilding = false;
    u32 m_refTarget = UINT32_MAX;
    // References tab graph view (app/DepGraph.h): built in a job for (target, depth), click a node to walk to it
    bool m_refGraphView = true;
    int m_refDepth = 1;
    std::shared_ptr<DepGraph> m_depGraph;
    u64 m_depGraphKey = 0, m_depGraphPendingKey = 0;
    std::vector<u32> m_refHistory;
    // explorer texture thumbnails
    struct Thumb { int state = 0; std::shared_ptr<GpuTexture> tex; };   // 0 none, 1 pending, 2 ready, 3 not a texture
    std::unordered_map<u32, Thumb> m_thumbs;
    ClassIndex m_classes;   // main class per package for explorer icons / type filters (resolved on demand)
    std::mutex m_thumbMutex;
    int m_thumbPending = 0;
    std::vector<std::thread> m_refThreads;
    std::vector<std::shared_ptr<ReferenceIndex>> m_retiredRefs;   // replaced indexes: a running search may still read them
    std::atomic<bool> m_shutdown{false};
    AudioPlayer m_player;
    u64 m_playDoc = 0, m_playRequest = 0;
    bool m_playLoop = false;
    bool m_showBackups = false;
    std::string m_backupMessage;
    double m_readyAt = 0;            // ImGui time the archives finished mounting ("Last Refresh")
    char m_search[256] = {};
    std::string m_lastSearch;
    std::vector<u32> m_searchResults;
    bool m_searchTruncated = false;
    bool m_focusSearch = false;

    std::vector<std::shared_ptr<Document>> m_docs;
    u64 m_nextDocId = 1, m_activeDoc = 0;
    u64 m_popoutDoc = 0;   // tab whose 3D viewer is shown in the separate window (0 = none)
    bool m_popoutFocus = false, m_popoutRaise = false, m_inPopout = false;
    const Document* m_popoutDismissed = nullptr;   // closed with the window's X: stays in its tab (compared only)
    void SyncPopoutDoc();   // with the setting on, the window follows the active tab's 3D view
    void SetViewerWindow(bool on, Document* d);

    int m_busyFrames = 3;
    bool m_showSettings = false, m_quit = false;
    std::vector<LogLine> m_log;
    size_t m_logSeen = 0;
    bool m_logScroll = true;

    // settings dialog edit buffers
    char m_editGameDir[512] = {}, m_editOutputDir[512] = {}, m_editOodle[512] = {};
    std::string m_editKeys;
    std::string m_settingsMessage;

    // batch export state
    std::atomic<int> m_exportDone{0}, m_exportTotal{0}, m_exportFailed{0};
    std::atomic<bool> m_exportCancel{false}, m_exporting{false};
    std::string m_exportLast;
    std::mutex m_exportMutex;
};
