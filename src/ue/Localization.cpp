#include "ue/Localization.h"
#include "core/Reader.h"
#include <set>

// lower case for ASCII, Latin-1, Cyrillic (UTF-8 in, UTF-8 out): enough for searching the shipped languages
static std::string Fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        u8 c = (u8)s[i];
        u32 cp = c;
        int n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) { cp = (c & 7) << 18 | ((u8)s[i + 1] & 63) << 12 | ((u8)s[i + 2] & 63) << 6 | ((u8)s[i + 3] & 63); n = 4; }
        else if (c >= 0xE0 && i + 2 < s.size()) { cp = (c & 15) << 12 | ((u8)s[i + 1] & 63) << 6 | ((u8)s[i + 2] & 63); n = 3; }
        else if (c >= 0xC0 && i + 1 < s.size()) { cp = (c & 31) << 6 | ((u8)s[i + 1] & 63); n = 2; }
        if (cp < 0x80) cp = (u32)tolower((int)cp);
        else if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x410 && cp <= 0x42F)) cp += 0x20;
        else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 63)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        i += n;
    }
    return out;
}

std::vector<std::string> Localization::Cultures(const Vfs& vfs) {
    std::set<std::string> out;
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (!IEndsWith(p, ".locres")) continue;
        size_t e = p.rfind('/'), s = e == std::string_view::npos ? std::string_view::npos : p.rfind('/', e - 1);
        if (s != std::string_view::npos) out.insert(std::string(p.substr(s + 1, e - s - 1)));
    }
    return {out.begin(), out.end()};
}

void Localization::Clear() {
    m_entries.clear();
    m_index.clear();
    m_culture.clear();
}

bool Localization::ParseLocres(const Bytes& data, std::string* error) {
    static const u8 magic[16] = {0x0E, 0x14, 0x74, 0x75, 0x67, 0x4A, 0x03, 0xFC, 0x4A, 0x15, 0x90, 0x9D, 0xC3, 0x37, 0x7F, 0x1B};
    try {
        Reader r(data);
        int version = 0;
        std::vector<std::string> strings;
        if (data.size() >= 17 && memcmp(data.data(), magic, 16) == 0) {
            r.Skip(16);
            version = r.U8();
            if (version >= 1) {   // localized strings live in a shared array
                i64 arrayOffset = r.I64();
                size_t back = r.Pos();
                r.Seek((size_t)arrayOffset);
                i32 n = r.I32();
                if (n < 0 || n > (1 << 24)) throw ParseError("bad string array");
                strings.reserve(n);
                for (i32 i = 0; i < n; i++) {
                    strings.push_back(r.FString());
                    if (version >= 2) r.I32();   // ref count
                }
                r.Seek(back);
                if (version >= 2) r.U32();   // entry count
            }
        }
        u32 nsCount = r.U32();
        for (u32 n = 0; n < nsCount; n++) {
            if (version >= 2) r.U32();   // namespace hash
            std::string ns = r.FString();
            u32 keys = r.U32();
            for (u32 k = 0; k < keys; k++) {
                if (version >= 2) r.U32();   // key hash
                std::string key = r.FString();
                r.U32();   // source string hash
                std::string text;
                if (version >= 1) {
                    i32 idx = r.I32();
                    if (idx >= 0 && idx < (i32)strings.size()) text = strings[idx];
                } else {
                    text = r.FString();
                }
                std::string id = ns + '\x1f' + key;
                auto it = m_index.find(id);
                if (it != m_index.end()) m_entries[it->second].text = text;
                else {
                    m_index.emplace(std::move(id), m_entries.size());
                    m_entries.push_back({ns, std::move(key), std::move(text)});   // ns is shared by the namespace's other keys: copy
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool Localization::Load(const Vfs& vfs, const std::string& culture, std::string* error) {
    Clear();
    m_culture = culture;
    int files = 0;
    std::string seg = "/" + ToLower(culture) + "/";
    for (u32 i = 0; i < vfs.FileCount(); i++) {
        std::string p = ToLower(vfs.Path(i));
        if (!IEndsWith(p, ".locres") || p.find("/localisation/") == std::string::npos && p.find("/localization/") == std::string::npos) continue;
        if (p.find(seg) == std::string::npos) continue;
        Bytes data;
        std::string err;
        if (!vfs.Read(i, data, &err) || !ParseLocres(data, &err)) {
            LOGW("locres %s: %s", p.c_str(), err.c_str());
            continue;
        }
        files++;
    }
    if (!files) {
        if (error) *error = "no .locres for culture " + culture;
        return false;
    }
    return true;
}

const std::string* Localization::Find(std::string_view ns, std::string_view key) const {
    std::string id(ns);
    id += '\x1f';
    id += key;
    auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_entries[it->second].text;
}

std::vector<const LocEntry*> Localization::Search(std::string_view query, size_t limit, bool inKeys) const {
    std::vector<std::string> terms;
    std::string q = Fold(query);
    for (size_t p = 0; p < q.size();) {
        size_t e = q.find(' ', p);
        if (e == std::string::npos) e = q.size();
        if (e > p) terms.push_back(q.substr(p, e - p));
        p = e + 1;
    }
    std::vector<const LocEntry*> out;
    if (terms.empty()) return out;
    std::string buf;
    for (auto& e : m_entries) {
        buf = Fold(e.text);
        if (inKeys) buf += '\x1f' + ToLower(e.key) + '\x1f' + ToLower(e.ns);
        bool ok = true;
        for (auto& t : terms) if (buf.find(t) == std::string::npos) { ok = false; break; }
        if (!ok) continue;
        out.push_back(&e);
        if (out.size() >= limit) break;
    }
    return out;
}
