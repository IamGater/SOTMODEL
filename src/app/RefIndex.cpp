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

u32 ReferenceIndex::TextKeyHash(std::string_view key) {
    u32 h = 2166136261u;
    for (char c : key) {
        h ^= (u8)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        h *= 16777619u;
    }
    return h;
}

const std::vector<u32>* ReferenceIndex::TextUsers(std::string_view key) const {
    auto it = m_text.find(TextKeyHash(key));
    return it == m_text.end() ? nullptr : &it->second;
}

// file: "SOTREF2\0" u64 signature, then two maps (imports, then text keys), each: u32 count, per item: u32 key, u32 n, u32 entries[n].
// "SOTREF1" files have the imports map only.
static bool ReadMap(const Bytes& data, size_t& pos, std::unordered_map<u32, std::vector<u32>>& map) {
    map.clear();
    if (pos + 4 > data.size()) return false;
    u32 n;
    memcpy(&n, data.data() + pos, 4);
    pos += 4;
    for (u32 i = 0; i < n; i++) {
        if (pos + 8 > data.size()) return false;
        u32 key, cnt;
        memcpy(&key, data.data() + pos, 4);
        memcpy(&cnt, data.data() + pos + 4, 4);
        pos += 8;
        if (pos + cnt * 4ull > data.size()) return false;
        auto& v = map[key];
        v.resize(cnt);
        memcpy(v.data(), data.data() + pos, cnt * 4ull);
        pos += cnt * 4ull;
    }
    return true;
}

static void WriteMap(Bytes& out, const std::unordered_map<u32, std::vector<u32>>& map) {
    u32 n = (u32)map.size();
    out.insert(out.end(), (u8*)&n, (u8*)&n + 4);
    for (auto& [k, v] : map) {
        u32 cnt = (u32)v.size();
        out.insert(out.end(), (const u8*)&k, (const u8*)&k + 4);
        out.insert(out.end(), (u8*)&cnt, (u8*)&cnt + 4);
        out.insert(out.end(), (const u8*)v.data(), (const u8*)(v.data() + v.size()));
    }
}

bool ReferenceIndex::LoadCache(const Vfs& vfs) {
    Bytes data;
    if (!ReadWholeFile(CacheFile(), data) || data.size() < 20) return false;
    bool v1 = !memcmp(data.data(), "SOTREF1", 7), v2 = !memcmp(data.data(), "SOTREF2", 7);
    if (!v1 && !v2) return false;
    u64 sig;
    memcpy(&sig, data.data() + 8, 8);
    if (sig != Signature(vfs)) return false;
    size_t pos = 16;
    ReadMap(data, pos, m_map);
    m_hasTextKeys = v2 && ReadMap(data, pos, m_text);
    m_ready = true;
    return true;
}

bool ReferenceIndex::SaveCache(const Vfs& vfs) const {
    Bytes out(16);
    memcpy(out.data(), m_hasTextKeys ? "SOTREF2" : "SOTREF1", 8);
    u64 sig = Signature(vfs);
    memcpy(out.data() + 8, &sig, 8);
    WriteMap(out, m_map);
    if (m_hasTextKeys) WriteMap(out, m_text);
    std::wstring f = CacheFile();
    CreateDirectories(f.substr(0, f.find_last_of(L'\\')));
    return WriteWholeFile(f, out.data(), out.size());
}

// FText keys in package bytes: FStrings (i32 length with the terminator, ASCII, 0) that are localisation keys
static void ScanTextKeys(const Bytes& b, u32 entry, const std::unordered_set<u32>* keys, std::unordered_map<u32, std::vector<u32>>& out) {
    std::unordered_set<u32> found;
    const u8* d = b.data();
    size_t n = b.size();
    for (size_t p = 0; p + 8 <= n; p++) {
        u32 len;
        memcpy(&len, d + p, 4);
        if (len < 2 || len > 130 || (!keys && len != 33)) continue;
        size_t s = p + 4, e = s + len - 1;
        if (e >= n || d[e] != 0) continue;
        bool ok = true;
        for (size_t k = s; k < e && ok; k++) {
            u8 c = d[k];
            ok = keys ? c >= 0x20 && c < 0x7f : (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        }
        if (!ok) continue;
        u32 h = ReferenceIndex::TextKeyHash(std::string_view((const char*)d + s, e - s));
        if (keys && !keys->count(h)) continue;
        if (found.insert(h).second) out[h].push_back(entry);
        p = e;
    }
}

bool ReferenceIndex::Build(const Vfs& vfs, int threads, const std::atomic<bool>& cancel, std::atomic<int>* done, std::atomic<int>* total,
                           const std::unordered_set<u32>* textKeys) {
    std::vector<u32> packages;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (IEndsWith(p, ".uasset") || IEndsWith(p, ".umap")) packages.push_back(i);
    }
    if (total) *total = (int)packages.size();
    std::mutex mtx;
    std::unordered_map<u32, std::vector<u32>> result, text;
    std::vector<std::thread> workers;
    threads = std::max(1, threads);
    for (int w = 0; w < threads; w++)
        workers.emplace_back([&, w] {
            std::unordered_map<u32, std::vector<u32>> local, localText;
            for (size_t k = w; k < packages.size() && !cancel; k += threads) {
                u32 e = packages[k];
                Bytes bytes;
                Package pkg;
                if (vfs.Read(e, bytes)) ScanTextKeys(bytes, e, textKeys, localText);
                if (!bytes.empty() && pkg.Load(std::move(bytes), std::string(vfs.Path(e)))) {
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
            for (auto& [t, v] : localText) {
                auto& dst = text[t];
                dst.insert(dst.end(), v.begin(), v.end());
            }
        });
    for (auto& t : workers) t.join();
    if (cancel) return false;
    for (auto& [t, v] : result) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    for (auto& [t, v] : text) std::sort(v.begin(), v.end());
    m_map = std::move(result);
    m_text = std::move(text);
    m_hasTextKeys = true;
    m_ready = true;
    return true;
}

const std::vector<u32>* ReferenceIndex::Referencers(u32 entry) const {
    auto it = m_map.find(entry);
    return it == m_map.end() ? nullptr : &it->second;
}
