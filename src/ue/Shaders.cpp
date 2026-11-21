#include "ue/Shaders.h"

const char* ShaderFrequencyName(int f) {
    static const char* n[] = {"vertex", "hull", "domain", "pixel", "geometry", "compute"};
    return f >= 0 && f < 6 ? n[f] : "unknown";
}
const char* ShaderStagePrefix(int f) {
    static const char* n[] = {"vs", "hs", "ds", "ps", "gs", "cs"};
    return f >= 0 && f < 6 ? n[f] : "xs";
}

bool IsShaderCacheFile(std::string_view path) {
    std::string p = ToLower(path);
    return IEndsWith(p, ".bin") && p.find("shadercache") != std::string::npos;
}

namespace {

u32 U32(const Bytes& d, size_t p) {
    u32 v;
    memcpy(&v, d.data() + p, 4);
    return v;
}

// strict LZ4 block decode; returns the decoded size or 0 on malformed input
size_t Lz4Decode(const u8* src, size_t srcSize, u8* dst, size_t cap) {
    const u8 *ip = src, *ie = src + srcSize;
    u8 *op = dst, *oe = dst + cap;
    while (ip < ie) {
        u8 tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            u8 b;
            do { if (ip >= ie) return 0; b = *ip++; lit += b; } while (b == 255);
        }
        if (lit > (size_t)(ie - ip) || lit > (size_t)(oe - op)) return 0;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= ie) break;   // the last sequence carries literals only
        if (ie - ip < 2) return 0;
        size_t off = ip[0] | (ip[1] << 8);
        ip += 2;
        size_t ml = tok & 15;
        if (ml == 15) {
            u8 b;
            do { if (ip >= ie) return 0; b = *ip++; ml += b; } while (b == 255);
        }
        ml += 4;
        if (off == 0 || off > (size_t)(op - dst) || ml > (size_t)(oe - op)) return 0;
        const u8* m = op - off;
        while (ml--) *op++ = *m++;
    }
    return op - dst;
}

bool ValidDxbc(const u8* p, size_t n) {
    if (n < 32 || memcmp(p, "DXBC", 4) != 0) return false;
    u32 total;
    memcpy(&total, p + 24, 4);
    return total == n;
}

struct NameReader {
    const Bytes& d;
    const std::vector<std::string>* names;
    // FName (index + number) at p
    bool FNameAt(size_t p, std::string& out) const {
        if (p + 8 > d.size()) return false;
        i32 idx = (i32)U32(d, p), num = (i32)U32(d, p + 4);
        if (idx < 0 || idx >= (i32)names->size() || num < 0 || num > 1000) return false;
        out = (*names)[idx];
        if (num) out += "_" + std::to_string(num - 1);
        return true;
    }
    // FString (ANSI, null terminated) that ends at e; returns its start
    bool FStringEndingAt(size_t e, std::string& out, size_t& start) const {
        if (e < 6 || e > d.size() || d[e - 1] != 0) return false;
        for (size_t len = 2; len <= 256 && len + 4 <= e; len++) {
            if (U32(d, e - len - 4) != len) continue;
            const u8* s = d.data() + e - len;
            bool ok = true;
            for (size_t k = 0; k + 1 < len && ok; k++) ok = s[k] >= 0x20 && s[k] < 0x7f;
            if (!ok) continue;
            out.assign((const char*)s, len - 1);
            start = e - len - 4;
            return true;
        }
        return false;
    }
    // a name that ends at e: an FName for packages, an FString otherwise
    bool NameEndingAt(size_t e, std::string& out, size_t& start) const {
        if (names) {
            if (e < 8 || !FNameAt(e - 8, out)) return false;
            start = e - 8;
            return true;
        }
        return FStringEndingAt(e, out, start);
    }
};

// Walks the uniform buffer list forward from `p` (count, then FString struct name + FShaderUniformBufferParameter
// [u16 BaseIndex, u32 bIsBound] each); true when it ends exactly at `end`.
bool UniformListEndsAt(const Bytes& d, size_t p, size_t end, std::vector<std::string>& out) {
    if (p + 4 > end) return false;
    u32 n = U32(d, p);
    if (n > 64) return false;
    p += 4;
    out.clear();
    for (u32 i = 0; i < n; i++) {
        if (p + 4 > end) return false;
        i32 len = (i32)U32(d, p);
        if (len <= 1 || len > 256 || p + 4 + len + 6 > end || d[p + 4 + len - 1] != 0) return false;
        out.emplace_back((const char*)d.data() + p + 4, len - 1);
        p += 4 + len + 6;
    }
    return p == end;
}

}   // namespace

void ScanShaders(const Bytes& d, const std::vector<std::string>* names, std::vector<ShaderEntry>& out) {
    NameReader nr{d, names};
    std::vector<u8> buf;
    // shader map descriptions ("Compiling <material>: Platform=..., Usage=...") by position
    std::vector<std::pair<size_t, std::string>> usages;
    for (size_t p = 0; p + 10 < d.size();) {
        p = std::string_view((const char*)d.data(), d.size()).find("Compiling ", p);
        if (p == std::string_view::npos) break;
        size_t e = p;
        while (e < d.size() && d[e] >= 0x20 && d[e] < 0x7f) e++;
        if (e < d.size() && d[e] == 0 && p >= 4 && U32(d, p - 4) == e - p + 1) usages.emplace_back(p, std::string((const char*)d.data() + p, e - p));
        p = e;
    }
    // FShaderResource starts with SpecificType, always None in cooked data: anchor on it, then Target and the code size
    std::string pat;
    if (names) {
        auto it = std::find(names->begin(), names->end(), "None");
        if (it == names->end()) return;
        u32 idx = (u32)(it - names->begin());
        pat.assign((const char*)&idx, 4);
        pat.append(4, '\0');
    } else pat.assign("\x05\0\0\0None\0", 9);
    size_t lastEnd = 0;
    std::string_view sv((const char*)d.data(), d.size());
    for (size_t a = sv.find(pat); a != std::string_view::npos; a = sv.find(pat, a + 1)) {
        if (a < lastEnd) continue;   // inside a record already decoded
        for (size_t q = a + pat.size() + 8, once = 0; once < 1; once++) {
            if (q + 4 > d.size()) continue;
            u32 csize = U32(d, q), freq = U32(d, q - 8), plat = U32(d, q - 4);
            size_t codeStart = q + 4, codeEnd = codeStart + csize;
            if (freq > 5 || plat > 64 || csize < 32 || csize > (64u << 20) || codeEnd + 28 > d.size()) continue;
            // SoT: uncompressed size after NumTextureSamplers; fall back to the DXBC header (stock, uncompressed code)
            Bytes dxbc;
            if (codeEnd + 32 <= d.size()) {
                u32 usize = U32(d, codeEnd + 28);
                if (usize >= 32 && usize <= (64u << 20) && usize >= csize) {
                    buf.resize(usize);
                    size_t got = Lz4Decode(d.data() + codeStart, csize, buf.data(), usize);
                    // the decoded block wraps the bytecode (20 leading zero bytes and a short tail in SoT)
                    if (got == usize)
                        for (size_t k = 0; k + 32 <= got && k <= 4096 && dxbc.empty(); k++) {
                            if (memcmp(buf.data() + k, "DXBC", 4)) continue;
                            u32 total = U32(buf, k + 24);
                            if (total >= 32 && k + total <= got) dxbc.assign(buf.begin() + k, buf.begin() + k + total);
                        }
                }
            }
            if (dxbc.empty() && ValidDxbc(d.data() + codeStart, csize)) dxbc.assign(d.begin() + codeStart, d.begin() + codeEnd);
            if (dxbc.empty()) continue;

            ShaderEntry s;
            s.frequency = (int)freq;
            s.platform = plat;
            s.offset = q;
            s.numInstructions = U32(d, codeEnd + 20);
            s.numTextureSamplers = U32(d, codeEnd + 24);
            s.dxbc = std::move(dxbc);
            // FShaderResource starts with SpecificType (a name) before Target
            std::string specific;
            size_t resStart = 0;
            if (nr.NameEndingAt(q - 8, specific, resStart)) {
                // walk back to SerializeBase's tail: Type, SourceHash(20), Target(8), uniform buffer list, then the resource
                for (size_t g = resStart >= 12 ? resStart - 12 : 0; g + 4096 > resStart && g >= 28; g--) {
                    if (U32(d, g) != freq || U32(d, g + 4) != plat) continue;
                    std::vector<std::string> ubs;
                    if (!UniformListEndsAt(d, g + 8, resStart, ubs)) continue;
                    std::string type, vf;
                    size_t typeStart = 0, vfStart = 0;
                    if (!nr.NameEndingAt(g - 20, type, typeStart)) continue;
                    s.type = type;
                    s.uniformBuffers = std::move(ubs);
                    if (typeStart >= 20 && nr.NameEndingAt(typeStart - 20, vf, vfStart) && vf != "None") s.vertexFactory = vf;
                    break;
                }
                if (s.type.empty() && specific != "None") s.type = specific;
            }
            for (auto& u : usages)
                if (u.first < q) s.usage = u.second;
            lastEnd = codeEnd;
            out.push_back(std::move(s));
            break;
        }
    }
}

bool ReadPackageShaders(const Package& pkg, std::vector<ShaderEntry>& out, std::string* error) {
    out.clear();
    ScanShaders(pkg.Data(), &pkg.Names(), out);
    if (out.empty() && error) *error = "no compiled shaders in this package (only master materials and their instances with static permutations carry shader maps)";
    return !out.empty();
}
