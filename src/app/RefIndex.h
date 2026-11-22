#pragma once
#include "core/Vfs.h"
#include <unordered_map>
#include <unordered_set>
#include <atomic>

// Which packages import which: built once by reading every package header (a few minutes for the whole game),
// then cached next to the settings (References.bin) for the same set of archives.
class ReferenceIndex {
public:
    static std::wstring CacheFile();
    bool LoadCache(const Vfs& vfs);
    // Blocking; `threads` workers. done / total report progress, cancel stops early (nothing is cached then).
    // textKeys (TextKeyHash of the localisation keys) also records which packages hold FText values with those keys;
    // without it 32 hex digit keys are recorded.
    bool Build(const Vfs& vfs, int threads, const std::atomic<bool>& cancel, std::atomic<int>* done = nullptr, std::atomic<int>* total = nullptr,
               const std::unordered_set<u32>* textKeys = nullptr);
    bool SaveCache(const Vfs& vfs) const;
    bool Ready() const { return m_ready; }
    const std::vector<u32>* Referencers(u32 entry) const;   // packages importing `entry`
    size_t Size() const { return m_map.size(); }
    // packages with an FText of this localisation key (the key's FString found in their bytes); false when the index
    // predates text keys (a cache of an older version: rebuild it)
    bool HasTextKeys() const { return m_hasTextKeys; }
    const std::vector<u32>* TextUsers(std::string_view key) const;
    static u32 TextKeyHash(std::string_view key);   // FNV-1a of the lower-case key

private:
    static u64 Signature(const Vfs& vfs);
    std::unordered_map<u32, std::vector<u32>> m_map;
    std::unordered_map<u32, std::vector<u32>> m_text;   // TextKeyHash -> packages
    bool m_ready = false, m_hasTextKeys = false;
};
