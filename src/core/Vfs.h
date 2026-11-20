#pragma once
#include "core/Common.h"
#include "core/Compression.h"
#include "core/Crypto.h"
#include <unordered_map>
#include <mutex>
#include <atomic>

// Virtual file system over the game's .pak archives (UE4 pak v3 with SoT's per-entry AES + Oodle/LZ4/zlib).
struct PakArchive {
    std::wstring path;
    std::string name;
    void* handle = nullptr;
    std::string mountPoint;
    u32 version = 0;
    u32 fileCount = 0;
    int patchLevel = 0;              // _P1.._Pn: later patches override earlier archives
    std::atomic<int> keyIndex{-1};   // detected on first encrypted read
};

struct VfsEntry {
    u64 offset = 0;        // of the in-data FPakEntry record
    u64 size = 0;          // stored size
    u64 usize = 0;         // uncompressed size
    u32 pathOffset = 0;    // into the path pool
    u16 pathLen = 0;
    u16 pak = 0;
    Compression comp = Compression::None;
    u8 encrypted = 0;
    u16 headerSize = 0;    // size of the in-data FPakEntry record (data follows it)
    u32 blockSize = 0;
};

struct VfsFolder {
    std::string name;
    u32 parent = UINT32_MAX;
    std::vector<u32> folders;   // child folder indices, sorted by name
    std::vector<u32> files;     // entry indices, sorted by name
    u32 totalFiles = 0;         // recursive
};

class Vfs {
public:
    ~Vfs();
    void SetKeys(std::vector<AesKey> keys) { m_keys = std::move(keys); }
    const std::vector<AesKey>& Keys() const { return m_keys; }

    // Mounts every .pak under dir (recursively). progress(done, total) may be called from worker threads.
    bool Mount(const std::wstring& dir, std::function<void(int, int)> progress = {});
    void Clear();

    size_t FileCount() const { return m_entries.size(); }
    const std::vector<std::unique_ptr<PakArchive>>& Paks() const { return m_paks; }
    const VfsEntry& Entry(u32 i) const { return m_entries[i]; }
    std::string_view Path(u32 i) const { return std::string_view(m_pathPool).substr(m_entries[i].pathOffset, m_entries[i].pathLen); }
    std::string_view FileName(u32 i) const;

    // Lookup by game path: "Athena/Content/X/y.uasset", "/Game/X/y" (package path, .uasset/.umap appended) or any case.
    i64 Find(std::string_view path) const;
    i64 FindPackage(std::string_view packagePath) const;   // "/Game/Models/x" or "/Game/Models/x.x" -> .uasset or .umap

    bool Read(u32 index, Bytes& out, std::string* error = nullptr) const;

    const std::vector<VfsFolder>& Folders() const { return m_folders; }   // [0] = root

    static std::string NormalizePath(std::string_view p);   // -> "Athena/Content/..." (no leading ../../../ or /)

private:
    bool ReadRaw(const PakArchive& pak, u64 offset, void* dst, size_t size) const;
    bool DecodeEntry(const VfsEntry& e, const PakArchive& pak, int keyIndex, Bytes& out, std::string* error) const;
    void BuildTree();
    static u64 Hash(std::string_view lowerPath);

    std::vector<AesKey> m_keys;
    std::vector<std::unique_ptr<PakArchive>> m_paks;
    std::vector<VfsEntry> m_entries;
    std::string m_pathPool;
    std::unordered_map<u64, u32> m_lookup;
    std::vector<VfsFolder> m_folders;
};
