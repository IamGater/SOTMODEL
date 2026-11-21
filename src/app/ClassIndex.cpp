#include "app/ClassIndex.h"
#include "app/Settings.h"
#include "ue/Package.h"

static constexpr u16 kQueued = 0xFFFF;

std::wstring ClassIndex::CacheFile() {
    std::wstring p = Settings::Path();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\Classes.bin";
}

u64 ClassIndex::Signature(const Vfs& vfs) {
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

void ClassIndex::Start(const Vfs& vfs) {
    Stop();
    m_vfs = &vfs;
    m_stop = false;
    LoadCache();
    m_thread = std::thread([this] { Worker(); });
}

void ClassIndex::Stop() {
    {
        std::lock_guard lk(m_mutex);
        m_stop = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_vfs && m_dirty) SaveCache();
    std::lock_guard lk(m_mutex);
    m_class.clear();
    m_names.clear();
    m_nameIds.clear();
    m_dirty = false;
    m_vfs = nullptr;
}

std::string ClassIndex::Get(u32 entry, bool request) {
    std::lock_guard lk(m_mutex);
    if (!m_vfs) return {};
    auto it = m_class.find(entry);
    if (it != m_class.end()) return it->second == kQueued ? std::string() : m_names[it->second];
    std::string_view path = m_vfs->Path(entry);
    if (IEndsWith(path, ".umap")) return "World";
    if (!IEndsWith(path, ".uasset") || !request) return {};
    m_class[entry] = kQueued;
    m_queue.push_back(entry);
    m_cv.notify_one();
    return {};
}

void ClassIndex::Request(const std::vector<u32>& entries) {
    for (u32 e : entries) Get(e, true);
}

int ClassIndex::Pending() const {
    std::lock_guard lk(m_mutex);
    return (int)m_queue.size() + m_inflight;
}

void ClassIndex::Worker() {
    for (;;) {
        u32 e;
        {
            std::unique_lock lk(m_mutex);
            m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            e = m_queue.back();   // newest first: what the explorer shows right now
            m_queue.pop_back();
            m_inflight++;
        }
        std::string cls = "?";
        Bytes bytes;
        Package pkg;
        if (m_vfs->Read(e, bytes) && pkg.Load(std::move(bytes), std::string(m_vfs->Path(e)))) {
            i32 main = pkg.MainExport();
            if (main) cls = pkg.ExportClass(main - 1);
            else if (!pkg.Exports().empty()) cls = pkg.ExportClass(0);
        }
        std::lock_guard lk(m_mutex);
        auto [it, added] = m_nameIds.emplace(cls, (u16)m_names.size());
        if (added) m_names.push_back(cls);
        m_class[e] = it->second;
        m_dirty = true;
        m_inflight--;
    }
}

// file: "SOTCLS1\0" u64 signature, u32 names, per name: u16 length + bytes, u32 entries, per entry: u32 entry, u16 name
void ClassIndex::LoadCache() {
    Bytes d;
    if (!ReadWholeFile(CacheFile(), d) || d.size() < 20 || memcmp(d.data(), "SOTCLS1", 7) != 0) return;
    u64 sig;
    memcpy(&sig, d.data() + 8, 8);
    if (sig != Signature(*m_vfs)) return;
    size_t pos = 16;
    auto rd = [&](void* dst, size_t n) { if (pos + n > d.size()) return false; memcpy(dst, d.data() + pos, n); pos += n; return true; };
    u32 names = 0, entries = 0;
    if (!rd(&names, 4)) return;
    std::lock_guard lk(m_mutex);
    for (u32 i = 0; i < names; i++) {
        u16 len;
        if (!rd(&len, 2) || pos + len > d.size()) return;
        std::string s((const char*)d.data() + pos, len);
        pos += len;
        m_nameIds[s] = (u16)m_names.size();
        m_names.push_back(s);
    }
    if (!rd(&entries, 4)) return;
    for (u32 i = 0; i < entries; i++) {
        u32 e;
        u16 n;
        if (!rd(&e, 4) || !rd(&n, 2)) break;
        if (n < m_names.size()) m_class[e] = n;
    }
}

void ClassIndex::SaveCache() {
    Bytes out(16);
    memcpy(out.data(), "SOTCLS1", 8);
    u64 sig = Signature(*m_vfs);
    memcpy(out.data() + 8, &sig, 8);
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const u8*)p, (const u8*)p + n); };
    std::lock_guard lk(m_mutex);
    u32 names = (u32)m_names.size();
    put(&names, 4);
    for (auto& s : m_names) {
        u16 len = (u16)s.size();
        put(&len, 2);
        put(s.data(), len);
    }
    u32 count = 0;
    for (auto& [e, n] : m_class) count += n != kQueued;
    put(&count, 4);
    for (auto& [e, n] : m_class)
        if (n != kQueued) {
            put(&e, 4);
            put(&n, 2);
        }
    std::wstring f = CacheFile();
    CreateDirectories(f.substr(0, f.find_last_of(L'\\')));
    WriteWholeFile(f, out.data(), out.size());
    m_dirty = false;
}
