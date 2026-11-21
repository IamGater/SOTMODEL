#pragma once
#include "core/Vfs.h"
#include <unordered_map>
#include <atomic>

// Which packages import which: built once by reading every package header (a few minutes for the whole game),
// then cached next to the settings (References.bin) for the same set of archives.
class ReferenceIndex {
public:
    static std::wstring CacheFile();
    bool LoadCache(const Vfs& vfs);
    // Blocking; `threads` workers. done / total report progress, cancel stops early (nothing is cached then).
    bool Build(const Vfs& vfs, int threads, const std::atomic<bool>& cancel, std::atomic<int>* done = nullptr, std::atomic<int>* total = nullptr);
    bool SaveCache(const Vfs& vfs) const;
    bool Ready() const { return m_ready; }
    const std::vector<u32>* Referencers(u32 entry) const;   // packages importing `entry`
    size_t Size() const { return m_map.size(); }

private:
    static u64 Signature(const Vfs& vfs);
    std::unordered_map<u32, std::vector<u32>> m_map;
    bool m_ready = false;
};
