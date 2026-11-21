#include "app/RefIndex.h"
#include "app/Settings.h"
#include "ue/Package.h"
#include <thread>
#include <mutex>

std::wstring ReferenceIndex::CacheFile() {
    std::wstring p = Settings::Path();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\References.bin";
}

u64 ReferenceIndex::Signature(const Vfs& vfs) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const u8*)p)[i]; h *= 1099511628211ull; } };
    u64 count = vfs.FileCount();
    mix(&count, sizeof(count));
    for (auto& p : vfs.Paks()) {
        mix(p->name.data(), p->name.size());
        mix(&p->fileCount, sizeof(p->fileCount));
    }
    return h;
}

// file: "SOTREF1\0" u64 signature, u32 targets, per target: u32 entry, u32 count, u32 referencers[count]
bool ReferenceIndex::LoadCache(const Vfs& vfs) {
    Bytes data;
    if (!ReadWholeFile(CacheFile(), data) || data.size() < 20 || memcmp(data.data(), "SOTREF1", 7) != 0) return false;
    u64 sig;
    memcpy(&sig, data.data() + 8, 8);
    if (sig != Signature(vfs)) return false;
    m_map.clear();
    size_t pos = 16;
    u32 n;
    memcpy(&n, data.data() + pos, 4);
    pos += 4;
    for (u32 i = 0; i < n && pos + 8 <= data.size(); i++) {
        u32 target, cnt;
        memcpy(&target, data.data() + pos, 4);
        memcpy(&cnt, data.data() + pos + 4, 4);
        pos += 8;
        if (pos + cnt * 4ull > data.size()) break;
        auto& v = m_map[target];
        v.resize(cnt);
        memcpy(v.data(), data.data() + pos, cnt * 4ull);
        pos += cnt * 4ull;
    }
    m_ready = true;
    return true;
}

bool ReferenceIndex::SaveCache(const Vfs& vfs) const {
    Bytes out(16);
    memcpy(out.data(), "SOTREF1", 8);
    u64 sig = Signature(vfs);
    memcpy(out.data() + 8, &sig, 8);
    u32 n = (u32)m_map.size();
    out.insert(out.end(), (u8*)&n, (u8*)&n + 4);
    for (auto& [t, v] : m_map) {
        u32 cnt = (u32)v.size();
        out.insert(out.end(), (const u8*)&t, (const u8*)&t + 4);
        out.insert(out.end(), (u8*)&cnt, (u8*)&cnt + 4);
        out.insert(out.end(), (const u8*)v.data(), (const u8*)(v.data() + v.size()));
    }
    std::wstring f = CacheFile();
    CreateDirectories(f.substr(0, f.find_last_of(L'\\')));
    return WriteWholeFile(f, out.data(), out.size());
}

bool ReferenceIndex::Build(const Vfs& vfs, int threads, const std::atomic<bool>& cancel, std::atomic<int>* done, std::atomic<int>* total) {
    std::vector<u32> packages;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (IEndsWith(p, ".uasset") || IEndsWith(p, ".umap")) packages.push_back(i);
    }
    if (total) *total = (int)packages.size();
    std::mutex mtx;
    std::unordered_map<u32, std::vector<u32>> result;
    std::vector<std::thread> workers;
    threads = std::max(1, threads);
    for (int w = 0; w < threads; w++)
        workers.emplace_back([&, w] {
            std::unordered_map<u32, std::vector<u32>> local;
            for (size_t k = w; k < packages.size() && !cancel; k += threads) {
                u32 e = packages[k];
                Bytes bytes;
                Package pkg;
                if (vfs.Read(e, bytes) && pkg.Load(std::move(bytes), std::string(vfs.Path(e)))) {
                    for (auto& im : pkg.Imports()) {
                        if (im.outerIndex != 0 || im.className != "Package" || !IStartsWith(im.objectName, "/Game/")) continue;
                        i64 t = vfs.FindPackage(im.objectName);
                        if (t >= 0 && (u32)t != e) local[(u32)t].push_back(e);
                    }
                }
                if (done) (*done)++;
            }
            std::lock_guard lk(mtx);
            for (auto& [t, v] : local) {
                auto& dst = result[t];
                dst.insert(dst.end(), v.begin(), v.end());
            }
        });
    for (auto& t : workers) t.join();
    if (cancel) return false;
    for (auto& [t, v] : result) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    m_map = std::move(result);
    m_ready = true;
    return true;
}

const std::vector<u32>* ReferenceIndex::Referencers(u32 entry) const {
    auto it = m_map.find(entry);
    return it == m_map.end() ? nullptr : &it->second;
}
