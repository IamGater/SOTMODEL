#pragma once
#include "app/Api.h"
#include "render/GpuAssets.h"
#include "ue/Material.h"
#include "ue/Mesh.h"
#include "ue/AnimPose.h"
#include "export/Exporter.h"
#include <thread>
#include <condition_variable>
#include <deque>
#include <atomic>

struct ImFont;

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

enum class DocView { Properties, Texture, Model, Material, Animation };

struct Document {
    u64 id = 0;
    u32 entry = 0;
    std::string gamePath, name, className;
    std::atomic<bool> loading{true};
    std::string error;
    std::shared_ptr<Package> pkg;

    // properties
    std::string json;
    std::vector<u32> lines;   // line start offsets
    char find[128] = {};
    int findLine = -1, scrollToLine = -1;

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
    std::vector<std::string> openOnStart;   // asset paths from the command line

private:
    void StartMount();
    void DrawMenu();
    void DrawFolderTree(u32 folder);
    void DrawArchives();
    void DrawFileList();
    void DrawDocuments();
    void DrawDocument(Document& d);
    void DrawProperties(Document& d);
    void DrawTexture(Document& d);
    void DrawModel(Document& d);
    void DrawMaterial(Document& d);
    void DrawAnimation(Document& d);
    void LoadAnimationMesh(std::shared_ptr<Document> d, std::string meshPath);
    bool LoadAnimationMeshNow(Document& d, const std::string& meshPath);
    void DrawLog();
    void DrawSettings();
    void DrawStatusBar();
    void DrawExportProgress();

    void OpenFile(u32 entry);
    void LoadDocument(std::shared_ptr<Document> d);
    void RebuildMeshLod(Document& d);
    void ExportFiles(std::vector<u32> entries, int mode);   // 0 by type, 1 raw, 2 json
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

    u32 m_folder = 0;
    i64 m_selected = -1;
    char m_search[256] = {};
    std::string m_lastSearch;
    std::vector<u32> m_searchResults;
    bool m_searchTruncated = false;
    bool m_focusSearch = false;

    std::vector<std::shared_ptr<Document>> m_docs;
    u64 m_nextDocId = 1, m_activeDoc = 0, m_selectDoc = 0;

    int m_busyFrames = 3;
    bool m_showSettings = false, m_showLog = true, m_quit = false;
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
