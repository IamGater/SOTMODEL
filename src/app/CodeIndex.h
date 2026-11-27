#pragma once
#include "core/Vfs.h"
#include <atomic>

class PackageCache;

// Global code search: the decompiled C++ of every Blueprint and the translated HLSL of every material / material function,
// generated once (a background job, several minutes for the whole game) and cached next to the settings (CodeIndex.bin) for
// the same set of archives and the same decompiler version.
struct CodeDoc {
    enum Kind : u8 { Blueprint = 0, Material = 1 };
    u8 kind = Blueprint;
    u32 entry = 0;       // vfs entry
    std::string text;
};
struct CodeHit {
    u32 doc = 0;         // index into Docs()
    u32 line = 0;        // 1-based
    std::string text;    // the line (trimmed, at most 300 chars)
};

class CodeIndex {
public:
    static std::wstring CacheFile();
    bool LoadCache(const Vfs& vfs);
    // Blocking; done / total count packages. cancel stops early (nothing is kept then).
    bool Build(const Vfs& vfs, PackageCache& cache, int threads, const std::atomic<bool>& cancel, std::atomic<int>* done = nullptr,
               std::atomic<int>* total = nullptr);
    bool SaveCache(const Vfs& vfs) const;
    bool Ready() const { return m_ready; }
    const std::vector<CodeDoc>& Docs() const { return m_docs; }
    size_t Bytes() const;
    // Lines containing every space-separated term (case-insensitive; a term in quotes may hold spaces). kinds: bit 0 Blueprints,
    // bit 1 materials. pathFilter: substring of the package path. docsMatched counts documents with a hit.
    std::vector<CodeHit> Search(std::string_view query, int kinds, const std::string& pathFilter, size_t limit, const Vfs& vfs,
                                size_t* docsMatched = nullptr, const std::atomic<bool>* cancel = nullptr) const;

private:
    std::vector<CodeDoc> m_docs;
    bool m_ready = false;
};
