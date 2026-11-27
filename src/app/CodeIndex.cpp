#include "app/CodeIndex.h"
#include "app/RefIndex.h"
#include "app/Settings.h"
#include "ue/Package.h"
#include "ue/Blueprint.h"
#include "ue/MaterialGraph.h"
#include <algorithm>
#include <functional>
#include <mutex>
#include <thread>

namespace {
// bump when the decompiler / translator output changes enough that a cached index should be rebuilt
constexpr u32 kCodeIndexVersion = 1;
const char kMagic[8] = {'S', 'O', 'T', 'C', 'O', 'D', 'E', '1'};

char Low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

std::vector<std::string> Terms(std::string_view q) {
    std::vector<std::string> out;
    for (size_t i = 0; i < q.size();) {
        while (i < q.size() && q[i] == ' ') i++;
        if (i >= q.size()) break;
        size_t e;
        std::string t;
        if (q[i] == '"') {
            e = q.find('"', i + 1);
            if (e == std::string_view::npos) e = q.size();
            t = std::string(q.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            e = q.find(' ', i);
            if (e == std::string_view::npos) e = q.size();
            t = std::string(q.substr(i, e - i));
            i = e;
        }
        for (auto& c : t) c = Low(c);
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

bool LineHas(std::string_view line, const std::string& term) {
    if (term.size() > line.size()) return false;
    return std::search(line.begin(), line.end(), term.begin(), term.end(), [](char a, char b) { return Low(a) == b; }) != line.end();
}
}   // namespace

std::wstring CodeIndex::CacheFile() {
    std::wstring p = Settings::Path();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\CodeIndex.bin";
}

size_t CodeIndex::Bytes() const {
    size_t n = 0;
    for (auto& d : m_docs) n += d.text.size();
    return n;
}

// file: magic, u64 archive signature, u32 version, u32 count, per doc: u8 kind, u32 entry, u32 size, text
bool CodeIndex::LoadCache(const Vfs& vfs) {
    ::Bytes data;
    if (!ReadWholeFile(CacheFile(), data) || data.size() < 24 || memcmp(data.data(), kMagic, 8)) return false;
    u64 sig;
    u32 ver, count;
    memcpy(&sig, data.data() + 8, 8);
    memcpy(&ver, data.data() + 16, 4);
    memcpy(&count, data.data() + 20, 4);
    if (sig != ReferenceIndex::Signature(vfs) || ver != kCodeIndexVersion) return false;
    std::vector<CodeDoc> docs;
    docs.reserve(count);
    size_t p = 24;
    for (u32 i = 0; i < count; i++) {
        if (p + 9 > data.size()) return false;
        CodeDoc d;
        d.kind = data[p];
        u32 size;
        memcpy(&d.entry, data.data() + p + 1, 4);
        memcpy(&size, data.data() + p + 5, 4);
        p += 9;
        if (p + size > data.size() || d.entry >= vfs.FileCount()) return false;
        d.text.assign((const char*)data.data() + p, size);
        p += size;
        docs.push_back(std::move(d));
    }
    m_docs = std::move(docs);
    m_ready = true;
    return true;
}

bool CodeIndex::SaveCache(const Vfs& vfs) const {
    ::Bytes out(24);
    memcpy(out.data(), kMagic, 8);
    u64 sig = ReferenceIndex::Signature(vfs);
    u32 ver = kCodeIndexVersion, count = (u32)m_docs.size();
    memcpy(out.data() + 8, &sig, 8);
    memcpy(out.data() + 16, &ver, 4);
    memcpy(out.data() + 20, &count, 4);
    out.reserve(24 + Bytes() + m_docs.size() * 9);
    for (auto& d : m_docs) {
        u8 h[9];
        h[0] = d.kind;
        u32 size = (u32)d.text.size();
        memcpy(h + 1, &d.entry, 4);
        memcpy(h + 5, &size, 4);
        out.insert(out.end(), h, h + 9);
        out.insert(out.end(), d.text.begin(), d.text.end());
    }
    std::wstring f = CacheFile();
    CreateDirectories(f.substr(0, f.find_last_of(L'\\')));
    return WriteWholeFile(f, out.data(), out.size());
}

bool CodeIndex::Build(const Vfs& vfs, PackageCache& cache, int threads, const std::atomic<bool>& cancel, std::atomic<int>* done,
                      std::atomic<int>* total) {
    // code lives in .uasset packages; audio events / banks, localisation and streamed media hold none
    std::vector<u32> packages;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (!IEndsWith(p, ".uasset")) continue;
        std::string l = ToLower(p);
        if (l.find("/wwiseaudio/") != std::string::npos || l.find("/localization/") != std::string::npos) continue;
        packages.push_back(i);
    }
    if (total) *total = (int)packages.size();
    std::mutex mtx;
    std::vector<CodeDoc> docs;
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    threads = std::max(1, threads);
    for (int w = 0; w < threads; w++)
        workers.emplace_back([&] {
            std::vector<CodeDoc> local;
            for (size_t k; (k = next++) < packages.size() && !cancel;) {
                u32 e = packages[k];
                ::Bytes bytes;
                Package pkg;
                if (vfs.Read(e, bytes) && pkg.Load(std::move(bytes), std::string(vfs.Path(e)))) {
                    i32 m = pkg.MainExport();
                    std::string cls = m ? pkg.ExportClass((size_t)m - 1) : "";
                    CodeDoc d;
                    d.entry = e;
                    std::string err;
                    try {
                        if (IsBlueprintPackage(pkg)) {
                            BlueprintOptions bo;
                            bo.cache = &cache;
                            d.kind = CodeDoc::Blueprint;
                            if (DecompileBlueprint(pkg, d.text, &err, bo)) local.push_back(std::move(d));
                        } else if (cls == "Material" || cls == "MaterialFunction") {
                            d.kind = CodeDoc::Material;
                            if (MaterialToHlsl(cache, std::string(vfs.Path(e)), d.text, nullptr, &err)) local.push_back(std::move(d));
                        }
                    } catch (const std::exception&) {
                    }
                }
                if (done) (*done)++;
            }
            std::lock_guard lk(mtx);
            for (auto& d : local) docs.push_back(std::move(d));
        });
    for (auto& t : workers) t.join();
    if (cancel) return false;
    std::sort(docs.begin(), docs.end(), [&](const CodeDoc& a, const CodeDoc& b) { return vfs.Path(a.entry) < vfs.Path(b.entry); });
    m_docs = std::move(docs);
    m_ready = true;
    return true;
}

std::vector<CodeHit> CodeIndex::Search(std::string_view query, int kinds, const std::string& pathFilter, size_t limit, const Vfs& vfs,
                                       size_t* docsMatched, const std::atomic<bool>* cancel) const {
    std::vector<CodeHit> hits;
    if (docsMatched) *docsMatched = 0;
    auto terms = Terms(query);
    if (terms.empty()) return hits;
    // the longest term finds candidate lines, the others are checked on that line
    std::sort(terms.begin(), terms.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    const std::string& first = terms[0];
    std::string filter = ToLower(pathFilter);
    for (u32 di = 0; di < (u32)m_docs.size(); di++) {
        if (cancel && *cancel) break;
        const CodeDoc& d = m_docs[di];
        if (!(kinds & (1 << d.kind))) continue;
        if (!filter.empty() && ToLower(vfs.Path(d.entry)).find(filter) == std::string::npos) continue;
        std::string_view text = d.text;
        bool any = false;
        size_t lineNo = 1, counted = 0;
        auto it = text.begin();
        while (true) {
            it = std::search(it, text.end(), first.begin(), first.end(), [](char a, char b) { return Low(a) == b; });
            if (it == text.end()) break;
            size_t pos = (size_t)(it - text.begin());
            size_t ls = text.rfind('\n', pos);
            ls = ls == std::string_view::npos ? 0 : ls + 1;
            size_t le = text.find('\n', pos);
            if (le == std::string_view::npos) le = text.size();
            std::string_view line = text.substr(ls, le - ls);
            bool all = true;
            for (size_t t = 1; t < terms.size() && all; t++) all = LineHas(line, terms[t]);
            if (all) {
                lineNo += (size_t)std::count(text.begin() + counted, text.begin() + ls, '\n');
                counted = ls;
                any = true;
                if (hits.size() < limit) {
                    size_t a = 0;
                    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) a++;
                    std::string s(line.substr(a, 300));
                    if (!s.empty() && s.back() == '\r') s.pop_back();
                    hits.push_back({di, (u32)lineNo, std::move(s)});
                }
            }
            if (le >= text.size()) break;
            it = text.begin() + le + 1;
        }
        if (any && docsMatched) (*docsMatched)++;
        if (hits.size() >= limit && !docsMatched) break;
    }
    return hits;
}
