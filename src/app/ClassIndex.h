#pragma once
#include "core/Vfs.h"
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

// Main asset class of each package (StaticMesh, Texture2D, ...), resolved lazily: the explorer requests what it shows,
// a background thread reads those package headers (newest requests first). Cached next to the settings (Classes.bin)
// for the same set of archives. .umap files are "World" without reading them.
class ClassIndex {
public:
    ~ClassIndex() { Stop(); }
    void Start(const Vfs& vfs);   // loads the cache, starts the worker (after mounting)
    void Stop();                  // joins the worker and saves new results (before unmounting)
    // Class name of a package, or empty while unknown (queues it). `request` false only looks.
    std::string Get(u32 entry, bool request = true);
    void Request(const std::vector<u32>& entries);
    int Pending() const;

private:
    void Worker();
    static std::wstring CacheFile();
    static u64 Signature(const Vfs& vfs);
    void LoadCache();
    void SaveCache();

    const Vfs* m_vfs = nullptr;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<u32> m_queue;
    std::unordered_map<u32, u16> m_class;   // entry -> index into m_names (0xFFFF = queued)
    std::vector<std::string> m_names;
    std::unordered_map<std::string, u16> m_nameIds;
    bool m_stop = false, m_dirty = false;
    int m_inflight = 0;   // popped, being read
};
