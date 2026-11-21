#pragma once
#include "app/Api.h"
#include "render/GpuAssets.h"
#include "render/WorldScene.h"
#include "ue/Material.h"
#include "ue/Mesh.h"
#include "ue/AnimPose.h"
#include "export/Exporter.h"
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

struct Document {
    u64 id = 0;
    u32 entry = 0;
    std::string gamePath, name, className;
    std::atomic<bool> loading{true};
    std::string error;
    std::shared_ptr<Package> pkg;

    bool newTab = false;   // FModel "New Tab" placeholder (no asset yet)

    // properties (JSON editor)
    std::string json;
    std::vector<u32> lines;   // line start offsets
    u32 maxCols = 0;          // longest line, for the horizontal scroll range
    u32 selA = 0, selB = 0;   // selection as byte offsets (anchor, caret)
    bool searchOpen = false, focusSearch = false, searchUp = false, wholeWord = false, matchCase = false;
    char find[256] = {};
    int scrollToLine = -1;
    float sideW = 0;          // preview column width (0 = default)
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

    // world (.umap)
    bool isWorld = false;
    std::atomic<bool> cancel{false};          // set when the tab closes: stops background loading
    std::shared_ptr<WorldData> world;
    std::shared_ptr<WorldScene> worldScene;
    WorldDrawOptions worldDraw;
    bool worldWater = true, worldLoading = false;
    int worldDone = 0, worldTotal = 0;
    std::string worldStage;
    char worldFilter[128] = {};
    int worldOutliner = 0;   // 0 meshes, 1 levels

    // material
    bool hasMaterial = false;
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

    ImFont* monoFont = nullptr;
    ImFont* boldFont = nullptr;
    int startTab = -1;   // --tab: initial left tab
    std::string startFolder;   // --folder: initial explorer folder (game path)
    std::vector<std::string> openOnStart;   // asset paths from the command line

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
    void DrawLog(float width);
    void DrawSearchWindow();
    void DrawFolderMenu(u32 folder);
    void Breadcrumb(u32 folder);
    void HandleHotkeys();
    void SelectFolder(u32 folder);   // select + reveal in the tree, show it in the explorer
    void AddTab();
    Document* ActiveDoc();
    std::string& FolderFilter();
    // viewers (App.cpp)
    void DrawModel(Document& d);
    void DrawAnimation(Document& d);
    void DrawWorld(Document& d);
    void LoadWorld(std::shared_ptr<Document> d);
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
    std::shared_ptr<GpuMesh> m_sphere, m_water;
    void* m_hwnd = nullptr;

    std::mutex m_postMutex;
    std::vector<std::function<void()>> m_posted;

    enum class MountState { Idle, Mounting, Ready, Failed } m_mountState = MountState::Idle;
    std::string m_mountError;
    double m_mountSeconds = 0;

    u32 m_folder = 0;                       // selected folder (0 = root)
    std::unordered_set<u32> m_expanded;     // open folders in the tree
    u32 m_scrollTreeTo = UINT32_MAX;        // tree row to bring into view next frame
    int m_leftTab = 1;   // 0 archives, 1 folders, 2 packages
    int m_selPak = -1;
    std::vector<u64> m_pakSizes;
    int m_explorerFilter = 0;   // category: 0 all, 1 packages, 2 maps, 3 other files
    bool m_explorerVisible = true;   // explorer instead of the document tabs (FModel "Assets Explorer" toggle)
    std::string m_explorerView;   // folder / filter the explorer last showed (scroll resets when it changes)
    std::unordered_map<u32, std::string> m_folderFilters;   // "Filter assets in current folder..." per folder
    char m_filterBuf[256] = {};
    u32 m_filterFolder = UINT32_MAX;
    i64 m_selected = -1;            // selected package (Packages list / explorer)
    u32 m_selFolder = UINT32_MAX;   // folder card selected in the explorer
    bool m_showSearch = false, m_focusGlobalSearch = false;   // Packages > Search window
    double m_readyAt = 0;            // ImGui time the archives finished mounting ("Last Refresh")
    char m_search[256] = {};
    std::string m_lastSearch;
    std::vector<u32> m_searchResults;
    bool m_searchTruncated = false;
    bool m_focusSearch = false;

    std::vector<std::shared_ptr<Document>> m_docs;
    u64 m_nextDocId = 1, m_activeDoc = 0;

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
