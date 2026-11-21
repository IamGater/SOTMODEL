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
bool UniformListEndsAt(const Bytes& d, size_t p, size_t end, std::vector<std::string>& out, std::vector<int>* slots = nullptr) {
    if (p + 4 > end) return false;
    u32 n = U32(d, p);
    if (n > 64) return false;
    p += 4;
    out.clear();
    if (slots) slots->clear();
    for (u32 i = 0; i < n; i++) {
        if (p + 4 > end) return false;
        i32 len = (i32)U32(d, p);
        if (len <= 1 || len > 256 || p + 4 + len + 6 > end || d[p + 4 + len - 1] != 0) return false;
        out.emplace_back((const char*)d.data() + p + 4, len - 1);
        if (slots) slots->push_back(d[p + 4 + len] | (d[p + 4 + len + 1] << 8));
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
            size_t resEnd = codeEnd + 28;   // after NumTextureSamplers
            if (codeEnd + 32 <= d.size()) {
                u32 usize = U32(d, codeEnd + 28);
                if (usize >= 32 && usize <= (64u << 20) && usize >= csize) {
                    buf.resize(usize);
                    size_t got = Lz4Decode(d.data() + codeStart, csize, buf.data(), usize);
                    // the decoded block wraps the bytecode (20 leading zero bytes and a short tail in SoT)
                    if (got == usize) resEnd = codeEnd + 32;
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
            // FMaterialShader::Serialize continues with MaterialUniformBuffer (u16 BaseIndex, bool bIsBound)
            // (SoT puts 12 more bytes before it: a u32, a zero u32 and another u32)
            for (size_t at : {resEnd + 12, resEnd})
                if (at + 6 <= d.size() && s.materialBufferSlot < 0) {
                    u32 bound = U32(d, at + 2);
                    int base = d[at] | (d[at + 1] << 8);
                    if (bound == 1 && base < 15) s.materialBufferSlot = base;
                }
            // FShaderResource starts with SpecificType (a name) before Target
            std::string specific;
            size_t resStart = 0;
            if (nr.NameEndingAt(q - 8, specific, resStart)) {
                // walk back to SerializeBase's tail: Type, SourceHash(20), Target(8), uniform buffer list, then the resource
                for (size_t g = resStart >= 12 ? resStart - 12 : 0; g + 4096 > resStart && g >= 28; g--) {
                    if (U32(d, g) != freq || U32(d, g + 4) != plat) continue;
                    std::vector<std::string> ubs;
                    std::vector<int> slots;
                    if (!UniformListEndsAt(d, g + 8, resStart, ubs, &slots)) continue;
                    s.uniformBufferSlots = std::move(slots);
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

// ------------------------------------------------------------------ uniform expression sets

namespace {

struct ExprReader {
    const Bytes& d;
    const std::vector<std::string>& names;
    size_t p = 0;
    bool ok = true;
    u32 U() { if (p + 4 > d.size()) { ok = false; return 0; } u32 v = U32(d, p); p += 4; return v; }
    u8 B() { if (p + 1 > d.size()) { ok = false; return 0; } return d[p++]; }
    float F() { u32 v = U(); float f; memcpy(&f, &v, 4); return f; }
    std::string Name() {
        u32 idx = U(), num = U();
        if (!ok || idx >= names.size() || num > 1000) { ok = false; return {}; }
        return num ? names[idx] + "_" + std::to_string(num - 1) : names[idx];
    }
    static std::string Num(float v) {
        std::string s = Fmt("%.6g", v);
        if (s.find_first_of(".e") == std::string::npos) s += ".0";
        return s;
    }
    // one serialized FMaterialUniformExpression (type FName + its fields) as text; texture types set `isTexture`
    std::string Expr(int depth, bool* isTexture = nullptr) {
        if (depth > 32) { ok = false; return {}; }
        std::string t = Name();
        if (!ok || t.rfind("FMaterialUniformExpression", 0) != 0) { ok = false; return {}; }
        t = t.substr(26);
        auto x = [&] { return Expr(depth + 1); };
        if (isTexture) *isTexture = t.find("Texture") != std::string::npos;
        if (t == "Constant") {
            float c[4] = {F(), F(), F(), F()};
            u8 vt = B();
            int n = vt == 1 ? 1 : vt == 2 ? 2 : vt == 4 ? 3 : 4;
            if (n == 1) return Num(c[0]);
            std::string s = Fmt("float%d(", n);
            for (int i = 0; i < n; i++) s += (i ? ", " : "") + Num(c[i]);
            return s + ")";
        }
        if (t == "Time") return "Time";
        if (t == "RealTime") return "RealTime";
        if (t == "VectorParameter") {
            std::string n = Name();
            float c[4] = {F(), F(), F(), F()};
            return Fmt("VectorParameter \"%s\" (default %s, %s, %s, %s)", n.c_str(), Num(c[0]).c_str(), Num(c[1]).c_str(), Num(c[2]).c_str(), Num(c[3]).c_str());
        }
        if (t == "ScalarParameter") {
            std::string n = Name();
            float v = F();
            return Fmt("ScalarParameter \"%s\" (default %s)", n.c_str(), Num(v).c_str());
        }
        if (t == "Texture" || t == "FlipBookTextureParameter") {
            i32 idx = (i32)U();
            U();   // sampler source
            return Fmt("%s #%d", t == "Texture" ? "Texture" : "FlipBookTexture", idx);
        }
        if (t == "TextureParameter") {
            std::string n = Name();
            i32 idx = (i32)U();
            U();
            return Fmt("TextureParameter \"%s\" (default #%d)", n.c_str(), idx);
        }
        if (t == "Sine") {
            std::string a = x();
            bool cosine = U() != 0;
            return std::string(cosine ? "cos(" : "sin(") + a + ")";
        }
        static const char* unary[][2] = {{"SquareRoot", "sqrt"}, {"Length", "length"}, {"Logarithm2", "log2"}, {"Periodic", "periodic"},
                                         {"Saturate", "saturate"}, {"Floor", "floor"}, {"Ceil", "ceil"}, {"Frac", "frac"}, {"Abs", "abs"}};
        for (auto& u : unary)
            if (t == u[0]) return std::string(u[1]) + "(" + x() + ")";
        if (t == "FoldedMath") {
            std::string a = x(), b = x();
            u8 op = B();
            static const char* ops[] = {" + ", " - ", " * ", " / "};
            if (op == 4) return "dot(" + a + ", " + b + ")";
            return "(" + a + (op < 4 ? ops[op] : " ? ") + b + ")";
        }
        if (t == "AppendVector") {
            std::string a = x(), b = x();
            U();
            return "append(" + a + ", " + b + ")";
        }
        if (t == "Min" || t == "Max" || t == "Fmod") {
            std::string a = x(), b = x();
            return ToLower(t) + "(" + a + ", " + b + ")";
        }
        if (t == "Clamp") {
            std::string a = x(), mn = x(), mx = x();
            return "clamp(" + a + ", " + mn + ", " + mx + ")";
        }
        if (t == "ComponentSwizzle") {
            std::string a = x();
            i8 idx[4] = {(i8)B(), (i8)B(), (i8)B(), (i8)B()};
            i8 n = (i8)B();
            std::string sw;
            for (int i = 0; i < std::clamp<int>(n, 0, 4); i++) sw += "rgba"[std::clamp<int>(idx[i], 0, 3)];
            return "(" + a + ")." + sw;
        }
        ok = false;
        return {};
    }
    // TArray<FMaterialUniformExpression*>; `textures`: 1 must be textures, 0 must not be, -1 any
    bool Array(std::vector<std::string>& out, int textures) {
        u32 n = U();
        if (!ok || n > 4096) return false;
        for (u32 i = 0; i < n; i++) {
            bool tex = false;
            std::string e = Expr(0, &tex);
            if (!ok) return false;
            if (textures >= 0 && tex != (textures == 1)) return false;
            out.push_back(std::move(e));
        }
        return true;
    }
};

}   // namespace

std::string MaterialUniformSet::Describe(int v, int c) const {
    if (v < 0) return {};
    if (v < (int)vectors.size()) return c < 0 ? vectors[v] : vectors[v] + "." + "rgba"[c & 3];
    int s = (v - (int)vectors.size()) * 4;
    if (c >= 0) return s + c < (int)scalars.size() ? scalars[s + c] : std::string();
    std::string r;
    for (int k = 0; k < 4 && s + k < (int)scalars.size(); k++) r += std::string(k ? "; " : "") + "xyzw"[k] + ": " + scalars[s + k];
    return r;
}

bool ReadMaterialUniformSets(const Package& pkg, std::vector<MaterialUniformSet>& out) {
    out.clear();
    const Bytes& d = pkg.Data();
    const auto& names = pkg.Names();
    std::vector<bool> exprName(names.size());
    bool any = false;
    for (size_t i = 0; i < names.size(); i++) any |= exprName[i] = names[i].rfind("FMaterialUniformExpression", 0) == 0;
    if (!any) return false;
    for (size_t p = 0; p + 12 <= d.size(); p += 1) {
        u32 n = U32(d, p), idx = U32(d, p + 4);
        if (n == 0 || n > 4096 || idx >= names.size() || !exprName[idx] || U32(d, p + 8) != 0) continue;
        // this is one of the four arrays: try it as vectors, scalars, 2D textures, cube textures (earlier ones empty)
        bool found = false;
        for (int k = 0; k < 4 && !found; k++) {
            if (p < (size_t)k * 4) break;
            size_t s = p - (size_t)k * 4;
            bool zeros = true;
            for (int j = 0; j < k; j++) zeros &= U32(d, s + j * 4) == 0;
            if (!zeros) continue;
            ExprReader r{d, names, s};
            MaterialUniformSet set;
            set.offset = s;
            if (!r.Array(set.vectors, 0) || !r.Array(set.scalars, 0) || !r.Array(set.textures2D, 1) || !r.Array(set.texturesCube, 1)) continue;
            u32 nc = r.U();   // ParameterCollections (FGuid each)
            if (!r.ok || nc > 64) continue;
            r.p += (size_t)nc * 16;
            if (!r.Array(set.perFrameScalars, 0) || !r.Array(set.perFrameVectors, 0)) continue;
            std::vector<std::string> prevS, prevV;
            if (!r.Array(prevS, 0) || !r.Array(prevV, 0)) continue;
            out.push_back(std::move(set));
            p = r.p - 1;
            found = true;
        }
    }
    return !out.empty();
}

void AssignUniformSets(std::vector<ShaderEntry>& shaders, const std::vector<MaterialUniformSet>& sets) {
    for (auto& s : shaders) {
        s.uniformSet = -1;
        for (size_t i = 0; i < sets.size(); i++)
            if (sets[i].offset < s.offset) s.uniformSet = (int)i;
    }
}

std::string AnnotateMaterialBuffer(std::string& hlsl, int slot, const MaterialUniformSet& set) {
    std::string tag = Fmt("cb%d_cb[", slot), out, legend;
    int nv = (int)set.vectors.size(), ns = ((int)set.scalars.size() + 3) / 4;
    legend += Fmt("// Material uniform buffer = cb%d (%d vector + %d scalar float4s)\n", slot, nv, ns);
    for (int i = 0; i < nv + ns; i++) legend += Fmt("//   cb%d[%d]%s = %s\n", slot, i, i < nv ? "" : " (scalars)", set.Describe(i, -1).c_str());
    for (size_t i = 0; i < set.textures2D.size(); i++) legend += Fmt("//   2D texture %zu = %s\n", i, set.textures2D[i].c_str());
    for (size_t i = 0; i < set.texturesCube.size(); i++) legend += Fmt("//   cube texture %zu = %s\n", i, set.texturesCube[i].c_str());
    size_t pos = 0;
    while (pos < hlsl.size()) {
        size_t e = hlsl.find('\n', pos);
        std::string line = hlsl.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? hlsl.size() : e + 1;
        std::vector<std::string> notes;
        bool decl = line.find("float4 " + tag) != std::string::npos;
        for (size_t k = decl ? std::string::npos : line.find(tag); k != std::string::npos; k = line.find(tag, k + 1)) {
            size_t a = k + tag.size(), b = line.find(']', a);
            if (b == std::string::npos) break;
            int idx = atoi(line.substr(a, b - a).c_str());
            std::string sw;
            if (b + 1 < line.size() && line[b + 1] == '.')
                for (size_t q = b + 2; q < line.size() && strchr("xyzw", line[q]); q++) sw += line[q];
            if (sw.empty()) sw = "xyzw";
            std::string seen;
            for (char c : sw) {
                if (seen.find(c) != std::string::npos) continue;
                seen += c;
                std::string desc = set.Describe(idx, (int)(strchr("xyzw", c) - "xyzw"));
                if (idx < nv) desc = set.Describe(idx, -1);
                std::string note = Fmt("cb%d[%d]%s = %s", slot, idx, idx < nv ? "" : ("." + std::string(1, c)).c_str(), desc.empty() ? "?" : desc.c_str());
                if (std::find(notes.begin(), notes.end(), note) == notes.end()) notes.push_back(note);
                if (idx < nv) break;
            }
        }
        out += line;
        if (!notes.empty()) {
            out += "   // ";
            for (size_t i = 0; i < notes.size(); i++) out += (i ? "; " : "") + notes[i];
        }
        if (e != std::string::npos) out += "\n";
    }
    hlsl = std::move(out);
    return legend;
}
