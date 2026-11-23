#pragma once
#include "core/Common.h"
#include <atomic>
#include "core/JsonValue.h"
#include "core/Vfs.h"
#include "ue/Package.h"
#include "app/Settings.h"
#include "ue/Localization.h"
#include "app/RefIndex.h"

// Tool layer shared by the command line (--cli) and the MCP server (--mcp): every tool takes JSON arguments and
// returns JSON text (plus optional images), so any AI agent or script can browse, inspect and export game data.
struct ApiImage {
    std::string mime;   // image/png
    Bytes data;
    std::string note;
};

struct ApiResult {
    bool ok = true;
    std::string json;   // result document (or {"error": ...})
    std::vector<ApiImage> images;
    static ApiResult Error(const std::string& msg);
};

class Api;
void RegisterMeshTools(Api& api);   // ApiModels.cpp: mesh / animation / material tools
void RegisterWorldTools(Api& api);  // ApiWorld.cpp: map / scene tools
void RegisterAudioTools(Api& api);  // ApiAudio.cpp: Wwise audio tools
void RegisterExportTools(Api& api); // ApiExport.cpp: map / animation set glTF exports
void RegisterLocalizationTools(Api& api);   // ApiLocalization.cpp
void RegisterSearchTools(Api& api);
struct WorldOptions;
struct JsonValue;
void SeaOptionsFrom(const JsonValue& args, WorldOptions& o);   // ApiWorld.cpp: islands / proxies / region         // ApiSearch.cpp: content search, references
void RegisterShaderTools(Api& api);   // ApiShaders.cpp: compiled material / global shaders
void RegisterParticleTools(Api& api);   // ApiParticles.cpp: Cascade particle systems
void RegisterGameTools(Api& api);   // ApiGame.cpp: physical surfaces, behavior trees
// Several AnimSequence packages of one skeleton -> one .glb (ApiExport.cpp; also used by the UI).
bool ExportAnimationSet(PackageCache& cache, const std::vector<std::shared_ptr<Package>>& animPkgs, const std::string& meshPath, Bytes& glb, std::string& note, std::string* error);
// The notify sounds of an AnimSequence / AnimMontage package as .wav files in <base>_sounds\ plus <base>.notifies.json with every
// notify's time (sounds, effects, held items) - to rebuild the timing in Blender. False when it has no notifies.
bool ExportAnimationSounds(Api& a, const Package& pkg, const std::wstring& base, std::vector<std::string>& written, std::string* error = nullptr);
struct ApiTool {
    std::string name;
    std::string description;
    std::string schema;        // JSON schema of the arguments
    std::string primaryArg;    // CLI: a bare argument maps to this key
    std::function<ApiResult(Api&, const JsonValue&)> run;
};

class Api {
public:
    explicit Api(Settings& settings);
    bool EnsureMounted(std::string* error = nullptr);
    bool Mounted() const { return m_mounted; }
    void Unmount();   // callers must make sure no other thread is using vfs/cache
    const std::vector<ApiTool>& Tools() const { return m_tools; }
    const ApiTool* FindTool(std::string_view name) const;
    ApiResult Call(std::string_view tool, const JsonValue& args);

    Settings& settings;
    Vfs vfs;
    std::unique_ptr<PackageCache> cache;
    // localised strings of settings.language (FText "LocalizedString" in JSON); replaced, never freed while in use
    std::shared_ptr<Localization> localization;
    bool SetLanguage(const std::string& culture, std::string* error = nullptr);
    // package imports index (find references); null until loaded from cache or built
    std::shared_ptr<ReferenceIndex> references;
    // needTextKeys: an index (cached by an older version) without text keys is rebuilt when buildIfMissing
    ReferenceIndex* EnsureReferences(bool buildIfMissing, std::string* error = nullptr, bool needTextKeys = false);
    std::unordered_set<u32> TextKeyHashes() const;   // ReferenceIndex::TextKeyHash of every localisation key (empty: none loaded)

    // helpers for tools
    i64 ResolveFile(const std::string& path) const;   // game path, /Game path or object path
    std::shared_ptr<Package> LoadPackage(const std::string& path, std::string* error);

private:
public:
    void Register(ApiTool t) { m_tools.push_back(std::move(t)); }

private:
    void RegisterCoreTools();
    void RegisterAssetTools();
    std::vector<ApiTool> m_tools;
    std::atomic<bool> m_mounted{false};
    std::mutex m_mountMutex;
    friend void RegisterMeshTools(Api&);
};

// Content search (search_content tool and the UI's Content tab): every given filter must match.
struct ContentQuery {
    std::string cls, uses, prop, value, name, folder;   // class / imported asset / property path / value / path substring / folder
    std::string textKey;   // localisation key: packages holding an FText with it (reference index text keys)
    bool exact = false;
    int limit = 100, scanLimit = 20000;
};
struct ContentHit {
    u32 entry = 0;
    std::string cls;
    std::vector<std::string> matches;
};
struct ContentSearchResult {
    std::vector<ContentHit> hits;
    size_t candidates = 0;
    int scanned = 0;
    bool truncated = false, cancelled = false;
};
// progress/total count candidates; cancel stops early. False + error on bad filters (the index must exist for 'uses' when !buildIndex).
bool SearchContent(Api& a, const ContentQuery& q, ContentSearchResult& out, std::string* error, const std::atomic<bool>* cancel = nullptr,
                   std::atomic<int>* progress = nullptr, std::atomic<int>* total = nullptr, bool buildIndex = true);

std::string Base64(const u8* data, size_t size);
bool WildcardMatch(std::string_view pattern, std::string_view text);   // case-insensitive, * and ?
