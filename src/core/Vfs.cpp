#include "core/Vfs.h"
#include "core/Reader.h"
#include <windows.h>
#include <thread>
#include <map>

static constexpr u32 PakMagic = 0x5A6F12E1;
static constexpr u32 PackageMagic = 0x9E2A83C1;

namespace {
struct RawEntry {
    std::string path;
    VfsEntry e;
};

// The FPakEntry record (same layout in the index and in front of the data).
bool ParseEntry(Reader& r, u32 version, VfsEntry& e) {
    size_t start = r.Pos();
    e.offset = r.U64();
    e.size = r.U64();
    e.usize = r.U64();
    u32 method = version >= 8 ? 0 : r.U32();
    if (version < 2) r.Skip(8);   // timestamp
    r.Skip(20);                   // SHA1
    // legacy ECompressionFlags: ZLIB=1, GZIP=2, Custom=4. SoT: 0x408 = Oodle, Custom = LZ4 (CUE4Parse)
    if (method == 0) e.comp = Compression::None;
    else if (method == 0x408) e.comp = Compression::Oodle;
    else if (method & 1) e.comp = Compression::Zlib;
    else if (method & 2) e.comp = Compression::Gzip;
    else if (method & 4) e.comp = Compression::LZ4;
    else e.comp = Compression::Unknown;
    if (version >= 3) {
        if (method != 0) {
            i32 n = r.I32();
            if (n < 0 || n > (1 << 22)) throw ParseError("bad block count");
            r.Skip((i64)n * 16);
        }
        e.encrypted = r.U8() & 1;
        e.blockSize = r.U32();
    }
    e.headerSize = (u16)(r.Pos() - start);
    return true;
}

int PatchLevel(const std::string& name) {
    // "...WindowsClient_P3.pak" -> 3
    size_t p = name.rfind("_P");
    if (p == std::string::npos) return 0;
    int v = 0;
    for (size_t i = p + 2; i < name.size() && isdigit((u8)name[i]); i++) v = v * 10 + (name[i] - '0');
    return v;
}

void FindPaks(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) FindPaks(full, out);
        else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".pak") == 0) out.push_back(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}

Vfs::~Vfs() { Clear(); }

void Vfs::Clear() {
    for (auto& p : m_paks) if (p->handle) CloseHandle(p->handle);
    m_paks.clear();
    m_entries.clear();
    m_pathPool.clear();
    m_lookup.clear();
    m_folders.clear();
}

std::string Vfs::NormalizePath(std::string_view p) {
    std::string s(p);
    for (char& c : s) if (c == '\\') c = '/';
    while (s.rfind("../", 0) == 0) s.erase(0, 3);
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    if (IStartsWith(s, "Game/")) s = "Athena/Content/" + s.substr(5);
    else if (IStartsWith(s, "Engine/") && !IStartsWith(s, "Engine/Content/") && !IStartsWith(s, "Engine/Plugins/")) s = "Engine/Content/" + s.substr(7);
    return s;
}

u64 Vfs::Hash(std::string_view s) {
    u64 h = 1469598103934665603ull;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ (u8)c) * 1099511628211ull;
    }
    return h;
}

std::string_view Vfs::FileName(u32 i) const {
    auto p = Path(i);
    size_t s = p.rfind('/');
    return s == std::string_view::npos ? p : p.substr(s + 1);
}

bool Vfs::Mount(const std::wstring& dir, std::function<void(int, int)> progress) {
    Clear();
    std::vector<std::wstring> files;
    FindPaks(dir, files);
    if (files.empty()) { LOGE("no .pak files under %s", Narrow(dir).c_str()); return false; }

    std::vector<std::unique_ptr<PakArchive>> paks(files.size());
    std::vector<std::vector<RawEntry>> perPak(files.size());
    std::atomic<size_t> next{0};
    std::atomic<int> done{0};
    auto worker = [&]() {
        for (;;) {
            size_t i = next++;
            if (i >= files.size()) break;
            auto pak = std::make_unique<PakArchive>();
            pak->path = files[i];
            std::wstring fn = files[i].substr(files[i].find_last_of(L"\\/") + 1);
            pak->name = Narrow(fn);
            pak->patchLevel = PatchLevel(pak->name);
            HANDLE h = CreateFileW(files[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (h == INVALID_HANDLE_VALUE) { LOGW("cannot open %s", pak->name.c_str()); done++; continue; }
            pak->handle = h;
            try {
                LARGE_INTEGER sz;
                GetFileSizeEx(h, &sz);
                u8 tail[256];
                size_t tn = (size_t)std::min<i64>(sz.QuadPart, sizeof(tail));
                if (!ReadRaw(*pak, sz.QuadPart - tn, tail, tn)) throw ParseError("footer read");
                int at = -1;
                for (int k = (int)tn - 44; k >= 0; k--) if (memcmp(tail + k + 1, &PakMagic, 4) == 0) { at = k; break; }
                if (at < 0) throw ParseError("no pak magic");
                Reader fr(tail, tn, at);
                bool encIndex = fr.U8() != 0;
                fr.U32();
                pak->version = fr.U32();
                u64 indexOffset = fr.U64(), indexSize = fr.U64();
                if (encIndex) throw ParseError("encrypted index is not supported");
                if (indexSize > (1ull << 31) || indexOffset + indexSize > (u64)sz.QuadPart) throw ParseError("bad index");
                Bytes index(indexSize);
                if (!ReadRaw(*pak, indexOffset, index.data(), index.size())) throw ParseError("index read");
                Reader r(index);
                pak->mountPoint = r.FString();
                i32 count = r.Count(40);
                pak->fileCount = count;
                auto& list = perPak[i];
                list.resize(count);
                for (i32 k = 0; k < count; k++) {
                    list[k].path = NormalizePath(pak->mountPoint + r.FString());
                    ParseEntry(r, pak->version, list[k].e);
                }
            } catch (const std::exception& ex) {
                LOGW("pak %s: %s", pak->name.c_str(), ex.what());
                perPak[i].clear();
            }
            paks[i] = std::move(pak);
            int d = ++done;
            if (progress) progress(d, (int)files.size());
        }
    };
    unsigned nt = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < nt; t++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    // mount order: base archives first, then patch levels ascending (later wins)
    std::vector<size_t> order;
    for (size_t i = 0; i < paks.size(); i++) if (paks[i]) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (paks[a]->patchLevel != paks[b]->patchLevel) return paks[a]->patchLevel < paks[b]->patchLevel;
        return paks[a]->name < paks[b]->name;
    });
    size_t total = 0;
    for (auto& l : perPak) total += l.size();
    m_entries.reserve(total);
    m_lookup.reserve(total);
    for (size_t oi : order) {
        u16 pakIndex = (u16)m_paks.size();
        m_paks.push_back(std::move(paks[oi]));
        for (auto& re : perPak[oi]) {
            re.e.pak = pakIndex;
            u64 h = Hash(re.path);
            auto it = m_lookup.find(h);
            if (it != m_lookup.end()) {
                re.e.pathOffset = m_entries[it->second].pathOffset;
                re.e.pathLen = m_entries[it->second].pathLen;
                m_entries[it->second] = re.e;   // patch override
                continue;
            }
            re.e.pathOffset = (u32)m_pathPool.size();
            re.e.pathLen = (u16)re.path.size();
            m_pathPool += re.path;
            m_lookup.emplace(h, (u32)m_entries.size());
            m_entries.push_back(re.e);
        }
        perPak[oi].clear();
        perPak[oi].shrink_to_fit();
    }
    m_pathPool.shrink_to_fit();
    BuildTree();
    LOGI("mounted %zu archives, %zu files", m_paks.size(), m_entries.size());
    return !m_entries.empty();
}

void Vfs::BuildTree() {
    m_folders.clear();
    m_folders.push_back({});
    std::vector<std::map<std::string, u32, std::less<>>> childMaps(1);
    for (u32 i = 0; i < m_entries.size(); i++) {
        std::string_view p = Path(i);
        u32 cur = 0;
        size_t start = 0;
        for (;;) {
            size_t s = p.find('/', start);
            if (s == std::string_view::npos) break;
            std::string_view part = p.substr(start, s - start);
            auto& cm = childMaps[cur];
            auto it = cm.find(part);
            u32 child;
            if (it == cm.end()) {
                child = (u32)m_folders.size();
                VfsFolder f;
                f.name = std::string(part);
                f.parent = cur;
                m_folders.push_back(std::move(f));
                childMaps.emplace_back();
                childMaps[cur].emplace(std::string(part), child);
            } else child = it->second;
            cur = child;
            start = s + 1;
        }
        m_folders[cur].files.push_back(i);
    }
    for (u32 f = 0; f < m_folders.size(); f++) {
        for (auto& [name, idx] : childMaps[f]) m_folders[f].folders.push_back(idx);   // std::map is sorted
        // case-insensitive ordering like FModel
        std::sort(m_folders[f].folders.begin(), m_folders[f].folders.end(), [&](u32 a, u32 b) { return ToLower(m_folders[a].name) < ToLower(m_folders[b].name); });
        std::sort(m_folders[f].files.begin(), m_folders[f].files.end(), [&](u32 a, u32 b) { return ToLower(FileName(a)) < ToLower(FileName(b)); });
    }
    // recursive counts (children always have larger indices than their parents)
    for (u32 f = (u32)m_folders.size(); f-- > 0;) {
        m_folders[f].totalFiles += (u32)m_folders[f].files.size();
        if (m_folders[f].parent != UINT32_MAX) m_folders[m_folders[f].parent].totalFiles += m_folders[f].totalFiles;
    }
}

i64 Vfs::Find(std::string_view path) const {
    std::string n = NormalizePath(path);
    auto it = m_lookup.find(Hash(n));
    if (it == m_lookup.end()) return -1;
    if (!IEquals(Path(it->second), n)) return -1;
    return it->second;
}

i64 Vfs::FindPackage(std::string_view packagePath) const {
    std::string p(packagePath);
    // strip object name: "/Game/A/b.b" or "/Game/A/b.b_C"
    size_t slash = p.rfind('/');
    size_t dot = p.find('.', slash == std::string::npos ? 0 : slash);
    if (dot != std::string::npos) {
        std::string ext = ToLower(p.substr(dot));
        if (ext == ".uasset" || ext == ".umap") return Find(p);
        p.resize(dot);
    }
    i64 r = Find(p + ".uasset");
    if (r < 0) r = Find(p + ".umap");
    return r;
}

bool Vfs::ReadRaw(const PakArchive& pak, u64 offset, void* dst, size_t size) const {
    u8* d = (u8*)dst;
    while (size) {
        DWORD chunk = (DWORD)std::min<size_t>(size, 1u << 30), got = 0;
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        if (!ReadFile(pak.handle, d, chunk, &got, &ov) || got != chunk) return false;
        d += chunk;
        size -= chunk;
        offset += chunk;
    }
    return true;
}

bool Vfs::DecodeEntry(const VfsEntry& e, const PakArchive& pak, int keyIndex, Bytes& out, std::string* error) const {
    auto fail = [&](const std::string& msg) { if (error) *error = msg; return false; };
    const AesKey* key = (e.encrypted && keyIndex >= 0) ? &m_keys[keyIndex] : nullptr;
    if (e.encrypted && !key) return fail("encrypted entry and no AES key");
    u64 dataStart = e.offset + e.headerSize;
    out.resize(e.usize);
    if (e.comp == Compression::None) {
        size_t n = e.encrypted ? (size_t)((e.size + 15) & ~15ull) : (size_t)e.size;
        if (n == e.usize) {
            if (!ReadRaw(pak, dataStart, out.data(), n)) return fail("read failed");
            if (key) AesDecrypt(*key, out.data(), n);
        } else {
            Bytes tmp(n);
            if (!ReadRaw(pak, dataStart, tmp.data(), n)) return fail("read failed");
            if (key) AesDecrypt(*key, tmp.data(), n);
            memcpy(out.data(), tmp.data(), std::min<size_t>(e.usize, n));
        }
        return true;
    }
    // block table lives in the in-data record
    Bytes hdr(e.headerSize);
    if (!ReadRaw(pak, e.offset, hdr.data(), hdr.size())) return fail("header read failed");
    Reader hr(hdr);
    hr.Skip(48);
    i32 nb = hr.I32();
    if (nb <= 0 || (size_t)nb * 16 > hr.Remaining()) return fail("bad block table");
    std::vector<std::pair<u64, u64>> blocks(nb);
    for (auto& b : blocks) { b.first = hr.U64(); b.second = hr.U64(); }
    if (e.comp == Compression::Oodle && !OodleLoaded()) return fail("Oodle DLL not loaded (Settings > Oodle)");
    u64 blockSize = e.blockSize ? e.blockSize : e.usize;
    Bytes buf;
    u64 produced = 0;
    for (auto& [s, en] : blocks) {
        u64 start = s >= e.offset ? s : e.offset + s;   // v3 stores absolute offsets, later versions relative ones
        u64 csize = en - s;
        size_t readSize = key ? (size_t)((csize + 15) & ~15ull) : (size_t)csize;
        buf.resize(readSize);
        if (!ReadRaw(pak, start, buf.data(), readSize)) return fail("block read failed");
        if (key) AesDecrypt(*key, buf.data(), readSize);
        u64 want = std::min<u64>(blockSize, e.usize - produced);
        if (!Decompress(e.comp, buf.data(), (size_t)csize, out.data() + produced, (size_t)want))
            return fail(Fmt("%s block decompression failed", CompressionName(e.comp)));
        produced += want;
        if (produced >= e.usize) break;
    }
    return produced == e.usize ? true : fail("short data");
}

bool Vfs::Read(u32 index, Bytes& out, std::string* error) const {
    if (index >= m_entries.size()) { if (error) *error = "bad index"; return false; }
    const VfsEntry& e = m_entries[index];
    PakArchive& pak = *m_paks[e.pak];
    if (!e.encrypted) return DecodeEntry(e, pak, -1, out, error);
    int k = pak.keyIndex.load();
    if (k >= 0) return DecodeEntry(e, pak, k, out, error);
    // detect the key: the right one decompresses / yields a package tag
    std::string err;
    bool isPackage = IEndsWith(Path(index), ".uasset") || IEndsWith(Path(index), ".umap");
    int unverified = -1;   // uncompressed non-package data cannot be verified: used, but not remembered
    for (int i = 0; i < (int)m_keys.size(); i++) {
        try {
            if (!DecodeEntry(e, pak, i, out, &err)) continue;
        } catch (...) { continue; }
        bool verified;
        if (e.comp != Compression::None) verified = true;   // a wrong key does not decompress
        else if (isPackage) {
            u32 magic = 0;
            if (out.size() >= 4) memcpy(&magic, out.data(), 4);
            verified = magic == PackageMagic;
        } else verified = false;
        if (verified) {
            pak.keyIndex = i;
            return true;
        }
        if (!isPackage && unverified < 0) unverified = i;
    }
    if (unverified >= 0) return DecodeEntry(e, pak, unverified, out, error);
    if (error) *error = m_keys.empty() ? "encrypted entry and no AES key (Settings > AES keys)" : "no AES key decrypts " + pak.name + (err.empty() ? "" : ": " + err);
    return false;
}
