#pragma once
#include "core/Common.h"
#include "core/Reader.h"
#include "ue/Properties.h"
#include <mutex>
#include <unordered_map>

class Vfs;

struct FGuid { u32 a = 0, b = 0, c = 0, d = 0; };

struct PackageSummary {
    i32 legacyVersion = 0, ue3Version = 0, ue4Version = 0, licenseeVersion = 0;
    i32 totalHeaderSize = 0;
    u32 packageFlags = 0;
    i32 nameCount = 0, nameOffset = 0;
    i32 exportCount = 0, exportOffset = 0;
    i32 importCount = 0, importOffset = 0;
    i32 dependsOffset = 0;
    i64 bulkDataStartOffset = 0;
    i32 worldTileInfoOffset = 0;   // world composition tile info (levels of a NestedWorldComposition)
    FGuid guid;
};

struct ObjectImport {
    std::string classPackage;
    std::string className;
    i32 outerIndex = 0;
    std::string objectName;
};

struct ObjectExport {
    i32 classIndex = 0, superIndex = 0, outerIndex = 0;
    std::string objectName;
    u32 objectFlags = 0;
    i32 serialSize = 0, serialOffset = 0;
};

enum : u32 { RF_Public = 0x1, RF_Standalone = 0x2, RF_Transactional = 0x8, RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20 };

// FWorldTileInfo of a world-composition level, stored after the package header (WorldTileInfoDataOffset).
struct WorldTileInfo {
    i32 x = 0, y = 0;                 // tile position (cm, relative to the parent tile)
    float boundsMin[3]{}, boundsMax[3]{};
    bool boundsValid = false;
    std::string layer;                // world layer name
    i32 streamingDistance = 0;
    bool distanceStreaming = true, hidden = false;
    std::string parent;               // ParentTilePackageName
    i32 zOrder = 0;
    std::vector<float> lodDistances;  // RelativeStreamingDistance of the LOD levels
};

// A legacy (UE4.10 / Athena) package: one .uasset/.umap carrying header, exports and bulk data.
class Package {
public:
    bool Load(Bytes data, std::string gamePath, std::string* error = nullptr);

    const std::string& GamePath() const { return m_gamePath; }      // Athena/Content/X/y.uasset
    const std::string& PackageName() const { return m_packageName; } // /Game/X/y
    std::string ShortName() const;                                    // y
    const PackageSummary& Summary() const { return m_summary; }
    const Bytes& Data() const { return m_data; }

    const std::vector<std::string>& Names() const { return m_names; }
    const std::vector<ObjectImport>& Imports() const { return m_imports; }
    const std::vector<ObjectExport>& Exports() const { return m_exports; }

    std::string Name(i32 index, i32 number = 0) const;
    std::string ReadName(Reader& r) const;   // FName: index + number

    // index: >0 export (1-based), <0 import, 0 null
    std::string ObjectName(i32 index) const;
    std::string ClassName(i32 index) const;   // class of an object reference
    std::string ExportClass(size_t exportIdx) const { return ClassName((i32)exportIdx + 1); }
    std::string FullPath(i32 index) const;    // "/Game/Pkg.Obj" or "/Script/Engine.StaticMesh" or "/Game/Pkg.Outer:Inner"
    std::string ImportPackage(i32 importIndex) const;   // outermost package of an import ("/Game/Models/x")
    i32 FindExport(std::string_view name, std::string_view className = {}) const;   // 1-based, 0 = none
    i32 MainExport() const;   // the asset object (named like the package), 1-based

    // Tagged properties of an export; `end` receives the position after the property block.
    PropertyList ReadProperties(size_t exportIdx, size_t* end = nullptr, std::string* error = nullptr) const;
    PropertyList ReadProperties(Reader& r, size_t limit, std::string* error = nullptr) const;
    Reader ExportReader(size_t exportIdx) const;   // reader limited to the export's serial range (absolute positions)
    bool ReadWorldTileInfo(WorldTileInfo& out) const;   // false when the package has none

private:
    std::string m_gamePath, m_packageName;
    Bytes m_data;
    PackageSummary m_summary;
    std::vector<std::string> m_names;
    std::vector<ObjectImport> m_imports;
    std::vector<ObjectExport> m_exports;
};

// Loads and caches packages from the VFS (LRU, bounded by bytes).
class PackageCache {
public:
    explicit PackageCache(const Vfs& vfs, size_t budgetBytes = 512ull << 20) : m_vfs(vfs), m_budget(budgetBytes) {}
    std::shared_ptr<Package> Load(u32 entry, std::string* error = nullptr);
    std::shared_ptr<Package> LoadPath(std::string_view packageOrObjectPath, std::string* error = nullptr);   // /Game/..., Athena/Content/...
    void Clear();
    const Vfs& FileSystem() const { return m_vfs; }

private:
    struct Item { std::shared_ptr<Package> pkg; u64 tick; size_t bytes; };
    const Vfs& m_vfs;
    size_t m_budget;
    size_t m_used = 0;
    u64 m_tick = 0;
    std::unordered_map<u32, Item> m_items;
    std::mutex m_mutex;
};
