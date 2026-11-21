#include "render/ShaderTools.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <regex>
#include <map>
#include <set>
#include <cmath>

using Microsoft::WRL::ComPtr;

// {8d536ca1-0cca-4956-a837-786963755584}: defined here so dxguid.lib is not needed
static const GUID kIID_ID3D11ShaderReflection = {0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};

static bool Disasm(const Bytes& dxbc, UINT flags, std::string& out, std::string* error) {
    ComPtr<ID3DBlob> blob;
    HRESULT hr = D3DDisassemble(dxbc.data(), dxbc.size(), flags, nullptr, &blob);
    if (FAILED(hr) || !blob) {
        if (error) *error = Fmt("D3DDisassemble failed 0x%08x", (unsigned)hr);
        return false;
    }
    out.assign((const char*)blob->GetBufferPointer(), blob->GetBufferSize());
    while (!out.empty() && out.back() == 0) out.pop_back();
    return true;
}

bool DisassembleShader(const Bytes& dxbc, std::string& out, std::string* error) { return Disasm(dxbc, 0, out, error); }

static std::string TypeName(ID3D11ShaderReflectionType* t, bool& isArray) {
    D3D11_SHADER_TYPE_DESC td{};
    t->GetDesc(&td);
    isArray = td.Elements > 0;
    std::string base = td.Name ? td.Name : "";
    if (base.empty()) {
        const char* s = td.Type == D3D_SVT_FLOAT ? "float" : td.Type == D3D_SVT_INT ? "int" : td.Type == D3D_SVT_UINT ? "uint" : td.Type == D3D_SVT_BOOL ? "bool" : "struct";
        base = s;
        if (td.Class == D3D_SVC_VECTOR) base += std::to_string(td.Columns);
        else if (td.Class == D3D_SVC_MATRIX_ROWS || td.Class == D3D_SVC_MATRIX_COLUMNS) base += Fmt("%ux%u", td.Rows, td.Columns);
    }
    if (td.Elements) base += Fmt("[%u]", td.Elements);
    return base;
}

static std::string CompType(D3D_REGISTER_COMPONENT_TYPE t) {
    return t == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : t == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
}

bool ReflectShader(const Bytes& dxbc, ShaderReflectionInfo& out, std::string* error) {
    out = {};
    ComPtr<ID3D11ShaderReflection> refl;
    HRESULT hr = D3DReflect(dxbc.data(), dxbc.size(), kIID_ID3D11ShaderReflection, (void**)refl.GetAddressOf());
    if (FAILED(hr) || !refl) {
        if (error) *error = Fmt("D3DReflect failed 0x%08x (reflection data stripped?)", (unsigned)hr);
        return false;
    }
    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    UINT ver = sd.Version;
    UINT type = D3D11_SHVER_GET_TYPE(ver);
    static const char* stage[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
    out.target = Fmt("%s_%u_%u", type < 6 ? stage[type] : "xs", D3D11_SHVER_GET_MAJOR(ver), D3D11_SHVER_GET_MINOR(ver));
    out.instructions = sd.InstructionCount;
    out.tempRegisters = sd.TempRegisterCount;
    for (UINT i = 0; i < sd.BoundResources; i++) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
        ShaderBinding b;
        b.name = bd.Name ? bd.Name : "";
        b.slot = bd.BindPoint;
        b.count = bd.BindCount;
        switch (bd.Type) {
        case D3D_SIT_CBUFFER: b.kind = "cbuffer"; break;
        case D3D_SIT_TBUFFER: b.kind = "tbuffer"; break;
        case D3D_SIT_TEXTURE: b.kind = "texture"; break;
        case D3D_SIT_SAMPLER: b.kind = "sampler"; break;
        case D3D_SIT_STRUCTURED: b.kind = "structured"; break;
        case D3D_SIT_BYTEADDRESS: b.kind = "byteaddress"; break;
        default: b.kind = "uav"; break;
        }
        switch (bd.Dimension) {
        case D3D_SRV_DIMENSION_BUFFER: b.dimension = "buffer"; break;
        case D3D_SRV_DIMENSION_TEXTURE1D: b.dimension = "1d"; break;
        case D3D_SRV_DIMENSION_TEXTURE2D: b.dimension = "2d"; break;
        case D3D_SRV_DIMENSION_TEXTURE2DARRAY: b.dimension = "2darray"; break;
        case D3D_SRV_DIMENSION_TEXTURE2DMS: b.dimension = "2dms"; break;
        case D3D_SRV_DIMENSION_TEXTURE3D: b.dimension = "3d"; break;
        case D3D_SRV_DIMENSION_TEXTURECUBE: b.dimension = "cube"; break;
        case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: b.dimension = "cubearray"; break;
        default: break;
        }
        out.bindings.push_back(b);
        if (bd.Type == D3D_SIT_CBUFFER) {
            ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName(bd.Name);
            D3D11_SHADER_BUFFER_DESC cd{};
            if (!cb || FAILED(cb->GetDesc(&cd))) continue;
            ShaderConstantBuffer c;
            c.name = b.name;
            c.slot = bd.BindPoint;
            c.size = cd.Size;
            for (UINT v = 0; v < cd.Variables; v++) {
                ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                D3D11_SHADER_VARIABLE_DESC vd{};
                if (!var || FAILED(var->GetDesc(&vd))) continue;
                bool isArray = false;
                c.variables.push_back({vd.Name ? vd.Name : "", TypeName(var->GetType(), isArray), vd.StartOffset, vd.Size});
            }
            out.cbuffers.push_back(std::move(c));
        }
    }
    auto sig = [&](bool input, UINT n, std::vector<ShaderSignatureElement>& dst) {
        for (UINT i = 0; i < n; i++) {
            D3D11_SIGNATURE_PARAMETER_DESC pd{};
            if (FAILED(input ? refl->GetInputParameterDesc(i, &pd) : refl->GetOutputParameterDesc(i, &pd))) continue;
            ShaderSignatureElement e;
            e.semantic = pd.SemanticName ? pd.SemanticName : "";
            e.index = pd.SemanticIndex;
            e.reg = pd.Register;
            e.type = CompType(pd.ComponentType);
            e.mask = input ? pd.Mask : pd.Mask;
            dst.push_back(e);
        }
    };
    sig(true, sd.InputParameters, out.inputs);
    sig(false, sd.OutputParameters, out.outputs);
    return true;
}

std::string ReflectionToText(const ShaderReflectionInfo& r) {
    std::string s = r.instructions ? Fmt("// %s, %u instructions, %u temp registers\n\n", r.target.c_str(), r.instructions, r.tempRegisters)
                                   : Fmt("// %s\n\n", r.target.c_str());
    if (r.cbuffers.empty() && r.bindings.empty())
        s += "// resource definitions (RDEF) are stripped: constant buffers, textures and samplers are only known by register\n"
             "// (see the dcl_ lines of the assembly)\n";
    for (auto& c : r.cbuffers) {
        s += Fmt("cbuffer %s : register(b%u)   // %u bytes\n{\n", c.name.c_str(), c.slot, c.size);
        for (auto& v : c.variables) s += Fmt("    %-24s %-48s // offset %5u, %4u bytes\n", v.type.c_str(), (v.name + ";").c_str(), v.offset, v.size);
        s += "};\n\n";
    }
    for (auto& b : r.bindings)
        if (b.kind != "cbuffer")
            s += Fmt("%-10s %-8s %-48s register(%c%u)%s\n", b.kind.c_str(), b.dimension.c_str(), b.name.c_str(),
                     b.kind == "sampler" ? 's' : b.kind == "uav" ? 'u' : 't', b.slot, b.count > 1 ? Fmt("  x%u", b.count).c_str() : "");
    auto sig = [&](const char* title, const std::vector<ShaderSignatureElement>& v) {
        s += Fmt("\n// %s\n", title);
        for (auto& e : v) {
            std::string mask;
            for (int k = 0; k < 4; k++)
                if (e.mask & (1 << k)) mask += "xyzw"[k];
            s += Fmt("//   %c%u.%-4s %-6s %s%u\n", title[0] == 'i' ? 'v' : 'o', e.reg, mask.c_str(), e.type.c_str(), e.semantic.c_str(), e.index);
        }
    };
    sig("inputs", r.inputs);
    sig("outputs", r.outputs);
    return s;
}

// ================================================================== DXBC -> HLSL
// Port of the Athena dx2hlsl translator (literal, statement by statement) with a readable mode.
namespace {

const char* kComp = "xyzw";

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> SplitOps(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(' || ch == '[') depth++;
        if (ch == ')' || ch == ']') depth--;
        if (ch == ',' && depth == 0) {
            out.push_back(Trim(cur));
            cur.clear();
        } else cur += ch;
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

bool IsInt(const std::string& v) { return !v.empty() && std::all_of(v.begin() + (v[0] == '-'), v.end(), [](char c) { return isdigit((u8)c); }) && v != "-"; }

std::string FloatLit(const std::string& v) {
    if (v.rfind("0x", 0) == 0 || v.rfind("-0x", 0) == 0) {
        u32 u = (u32)(std::stoll(v, nullptr, 16) & 0xffffffff);
        float f;
        memcpy(&f, &u, 4);
        if (std::isnan(f) || std::isinf(f) || (u && fabsf(f) < 1e-30f) || fabsf(f) > 1e30f) return Fmt("asfloat(%uu)", u);
        std::string r = Fmt("%.9g", f);
        if (r.find_first_of(".e") == std::string::npos) r += ".0";
        return r;
    }
    if (IsInt(v)) {
        if (std::stoll(v) == 0) return "0.0";
        return Fmt("asfloat(%uu)", (u32)(std::stoll(v) & 0xffffffff));
    }
    std::string r = v;
    if (r.find_first_of(".e") == std::string::npos) r += ".0";
    // trim "0.500000" -> "0.5"
    if (r.find('e') == std::string::npos && r.find('.') != std::string::npos) {
        while (r.size() > 2 && r.back() == '0' && r[r.size() - 2] != '.') r.pop_back();
    }
    return r;
}

std::string SIntLit(const std::string& v) {
    if (v.rfind("0x", 0) == 0) return Fmt("%d", (i32)(u32)std::stoull(v, nullptr, 16));
    if (IsInt(v)) return Fmt("%d", (i32)(std::stoll(v) & 0xffffffff));
    float f = std::stof(v);
    i32 u;
    memcpy(&u, &f, 4);
    return Fmt("%d", u);
}

std::string IntLit(const std::string& v) {
    if (v.rfind("0x", 0) == 0) return Fmt("%uu", (u32)std::stoull(v, nullptr, 16));
    if (IsInt(v)) return Fmt("%uu", (u32)(std::stoll(v) & 0xffffffff));
    float f = std::stof(v);
    u32 u;
    memcpy(&u, &f, 4);
    return Fmt("%uu", u);
}

struct Translator {
    bool exact;
    std::map<std::string, std::string> names;   // cb0 / t0 / s0 / v0 / o0 / oDepth -> identifier
    std::set<std::string> uintInputs;           // v registers holding integer lanes
    std::map<std::string, std::string> texDim;   // t3 -> texture2d, texturecube, ...
    std::set<std::string> texUint;               // integer textures
    std::map<std::string, int> tid;
    // cbuffer member lookup for comments: cbN -> variables
    std::map<int, const ShaderConstantBuffer*> cbs;
    int maxr = -1;
    int ind = 1;
    std::vector<std::string> lines;
    std::set<std::string> lineNotes;   // members referenced on the current line
    struct Res {
        char kind = 0;          // s structured, r raw (RW/ByteAddressBuffer), t typed UAV, g groupshared structured, h groupshared raw
        int stride = 4;         // bytes (structured)
        std::string dim;        // typed: texture2d, buffer, ...
        bool uintData = false;  // typed: integer element type
    };
    std::map<std::string, Res> res;   // u0 / t3 / g0 -> layout
    std::vector<std::string> gsOut;   // geometry shader: output registers copied into each emitted vertex

    std::string Name(const std::string& k) {
        auto it = names.find(k);
        return it != names.end() ? it->second : k;
    }

    void Note(int cb, int reg, const std::string& comps) {
        auto it = cbs.find(cb);
        if (it == cbs.end()) return;
        for (char c : comps) {
            u32 byte = (u32)reg * 16 + (u32)(strchr(kComp, c) - kComp) * 4;
            for (auto& v : it->second->variables)
                if (byte >= v.offset && byte < v.offset + v.size) {
                    std::string n = v.name;
                    if (v.type.find('[') != std::string::npos && v.size > 16) n += Fmt("[%u]", (byte - v.offset) / 16);
                    lineNotes.insert(n);
                    break;
                }
        }
    }

    struct Base { std::string expr, swz; bool lanesUint = false; };

    Base BaseOf(const std::string& tok) {
        static const std::regex reg(R"(^(r|v|o)(\d+)(?:\.([xyzw]+))?$)");
        static const std::regex cbc(R"(^cb(\d+)\[(\d+)\](?:\.([xyzw]+))?$)");
        static const std::regex cbd(R"(^cb(\d+)\[(r\d+)\.([xyzw]) \+ (\d+)\](?:\.([xyzw]+))?$)");
        static const std::regex xr(R"(^x(\d+)\[(\d+|r\d+\.[xyzw](?: \+ \d+)?)\](?:\.([xyzw]+))?$)");
        static const std::regex icb(R"(^icb\[(r\d+)\.([xyzw]) \+ (\d+)\](?:\.([xyzw]+))?$)");
        static const std::regex th(R"(^(vThreadGroupID|vThreadIDInGroupFlattened|vThreadIDInGroup|vThreadID|vPrim|vCoverage)(?:\.([xyzw]+))?$)");
        std::smatch m;
        if (std::regex_match(tok, m, reg)) {
            std::string k = m[1].str() + m[2].str();
            if (m[1] == "r") {
                maxr = std::max(maxr, std::stoi(m[2]));
                return {k, m[3], exact};
            }
            return {Name(k), m[3], m[1] == "v" && uintInputs.count(k) > 0};
        }
        static const std::regex gsv(R"(^v\[(\d+)\]\[(\d+)\](?:\.([xyzw]+))?$)");
        if (std::regex_match(tok, m, gsv)) return {"input[" + m[1].str() + "].v" + m[2].str(), m[3], uintInputs.count("v" + m[2].str()) > 0};
        if (std::regex_match(tok, m, cbc)) {
            Note(std::stoi(m[1]), std::stoi(m[2]), m[3].matched ? m[3].str() : "xyzw");
            return {Name("cb" + m[1].str()) + "[" + m[2].str() + "]", m[3], false};
        }
        if (std::regex_match(tok, m, cbd)) {
            Base r = BaseOf(m[2]);
            std::string idx = exact ? r.expr + "." + m[3].str() : "asuint(" + r.expr + "." + m[3].str() + ")";
            return {Name("cb" + m[1].str()) + "[" + idx + " + " + m[4].str() + "u]", m[5], false};
        }
        if (std::regex_match(tok, m, xr)) return {"x" + m[1].str() + "[" + XIndex(m[2]) + "]", m[3], exact};
        if (std::regex_match(tok, m, icb)) {
            Base r = BaseOf(m[1]);
            return {"icb[asint(" + r.expr + "." + m[2].str() + ") + " + m[3].str() + "]", m[4], false};
        }
        if (std::regex_match(tok, m, th)) {
            int w = tid.count(m[1]) ? tid[m[1]] : (m[1] == "vThreadIDInGroupFlattened" ? 1 : 2);
            std::string pad;
            for (int i = w; i < 4; i++) pad += ", 0";
            return {"uint4(" + m[1].str() + pad + ")", m[2], true};
        }
        throw std::runtime_error("operand " + tok);
    }

    std::string XIndex(const std::string& s) {
        if (IsInt(s)) return s;
        static const std::regex re(R"(^(r\d+)\.([xyzw])(?: \+ (\d+))?$)");
        std::smatch m;
        if (!std::regex_match(s, m, re)) throw std::runtime_error("index " + s);
        maxr = std::max(maxr, std::stoi(m[1].str().substr(1)));
        std::string lane = m[1].str() + "." + m[2].str();
        if (!exact) lane = "asuint(" + lane + ")";
        return lane + (m[3].matched ? " + " + m[3].str() + "u" : "");
    }

    // source operand for destination lanes `comps`, as kind f (float) / i (int) / u (uint)
    std::string Src(const std::string& tok0, const std::vector<int>& comps, char kind = 'f') {
        bool neg = false, ab = false;
        std::string t = tok0;
        if (!t.empty() && t[0] == '-') { neg = true; t = t.substr(1); }
        if (t.size() > 1 && t.front() == '|' && t.back() == '|') { ab = true; t = t.substr(1, t.size() - 2); }
        std::string e;
        if (t.rfind("l(", 0) == 0) {
            std::vector<std::string> vals;
            for (auto& v : SplitOps(t.substr(2, t.size() - 3))) vals.push_back(v);
            if (vals.size() == 1) { std::string v0 = vals[0]; vals.assign(4, v0); }
            std::vector<std::string> items;
            for (int c : comps) {
                const std::string& v = vals[std::min<size_t>(c, vals.size() - 1)];
                items.push_back(kind == 'f' ? FloatLit(v) : kind == 'i' ? SIntLit(v) : IntLit(v));
            }
            std::string ty = kind == 'f' ? "float" : kind == 'i' ? "int" : "uint";
            if (items.size() == 1) e = items[0];
            else {
                e = ty + std::to_string(items.size()) + "(";
                for (size_t i = 0; i < items.size(); i++) e += (i ? ", " : "") + items[i];
                e += ")";
            }
        } else {
            Base b = BaseOf(t);
            std::string sw = b.swz.empty() ? "xyzw" : b.swz;
            if (sw.size() == 1) sw = std::string(4, sw[0]);
            while (sw.size() < 4) sw += sw.back();
            std::string pick;
            for (int c : comps) pick += sw[c];
            e = b.expr + "." + pick;
            if (kind == 'i') e = "asint(" + e + ")";
            else if (kind == 'u') { if (!b.lanesUint) e = "asuint(" + e + ")"; }
            else if (b.lanesUint) e = "asfloat(" + e + ")";
        }
        if (ab) e = "abs(" + e + ")";
        if (neg) e = "(-" + e + ")";
        return e;
    }

    struct Dest { std::string name, mask; bool none = false, temp = false; };
    Dest DestOf(const std::string& tok) {
        static const std::regex ro(R"(^(r|o)(\d+)\.([xyzw]+)$)");
        static const std::regex xr(R"(^x(\d+)\[(\d+|r\d+\.[xyzw](?: \+ \d+)?)\]\.([xyzw]+)$)");
        std::smatch m;
        if (std::regex_match(tok, m, ro)) {
            if (m[1] == "r") {
                maxr = std::max(maxr, std::stoi(m[2]));
                return {"r" + m[2].str(), m[3], false, true};
            }
            return {Name("o" + m[2].str()), m[3], false, false};
        }
        if (tok == "oDepth" || tok == "oDepthGE" || tok == "oDepthLE") return {Name("oDepth"), "", false, false};
        if (tok == "null") return {"", "", true};
        if (std::regex_match(tok, m, xr)) return {"x" + m[1].str() + "[" + XIndex(m[2]) + "]", m[3], false, true};
        throw std::runtime_error("dest " + tok);
    }

    // expression of `kind` -> the destination's lane type
    std::string Store(const Dest& d, const std::string& e, char kind) {
        bool uintLanes = d.temp && exact;
        if (uintLanes) return kind == 'u' ? e : "asuint(" + e + ")";
        return kind == 'f' ? e : "asfloat(" + e + ")";
    }

    void Emit(const std::string& s) { lines.push_back(std::string(ind * 4, ' ') + s); }

    void Assign(const std::string& dtok, const std::function<std::string(const std::vector<int>&)>& fn, char kind, bool sat = false, bool scalar = false) {
        Dest d = DestOf(dtok);
        if (d.none) return;
        if (d.mask.empty()) {
            std::string e = fn({0});
            Emit(d.name + " = " + (sat ? "saturate(" + e + ")" : e) + ";");
            return;
        }
        std::vector<int> comps;
        for (char c : d.mask) comps.push_back((int)(strchr(kComp, c) - kComp));
        std::string e;
        if (scalar) {
            e = fn({});
            if (sat) e = "saturate(" + e + ")";
            if (comps.size() > 1) e = Fmt("(%s%zu)(%s)", kind == 'f' ? "float" : kind == 'u' ? "uint" : "int", comps.size(), e.c_str());
        } else {
            e = fn(comps);
            if (sat) e = "saturate(" + e + ")";
        }
        Emit(d.name + "." + d.mask + " = " + Store(d, e, kind) + ";");
    }

    static std::string VecType(const char* base, size_t n) { return n > 1 ? Fmt("%s%zu", base, n) : base; }

    void Line(const std::string& raw);
    bool MemOp(const std::string& op, const std::vector<std::string>& ops);
    std::string Lane(const std::string& reg, const std::string& idx, const std::string& off, int lane);
    std::string Coord(const Res& r, const std::string& tok);
    void TexOp(const std::string& op, const std::vector<std::string>& ops, bool sat);
};

// one 32-bit lane of a structured / raw resource
std::string Translator::Lane(const std::string& reg, const std::string& idx, const std::string& off, int lane) {
    auto it = res.find(reg);
    if (it == res.end()) throw std::runtime_error("undeclared resource " + reg);
    const Res& r = it->second;
    std::string n = Name(reg);
    switch (r.kind) {
    case 's': return n + "[" + idx + "].v[(" + off + " >> 2) + " + std::to_string(lane) + "]";
    case 'g': return n + "[" + idx + " * " + std::to_string(r.stride / 4) + "u + (" + off + " >> 2) + " + std::to_string(lane) + "]";
    case 'h': return n + "[(" + idx + " >> 2) + " + std::to_string(lane) + "]";
    case 'r': return n + ".Load(" + idx + " + " + std::to_string(lane * 4) + ")";
    }
    throw std::runtime_error("resource kind " + reg);
}

std::string Translator::Coord(const Res& r, const std::string& tok) {
    int n = r.dim == "buffer" || r.dim == "texture1d" ? 1 : r.dim == "texture2d" || r.dim == "texture1darray" ? 2 : 3;
    std::vector<int> c;
    for (int i = 0; i < n; i++) c.push_back(i);
    return Src(tok, c, 'u');
}

// UAV / structured / raw / groupshared memory, atomics, resinfo; false when `op` is not one of them
bool Translator::MemOp(const std::string& op0, const std::vector<std::string>& ops) {
    std::string op = op0.substr(0, op0.find('('));
    static const std::regex resTok(R"(^([utg]\d+)(?:\.([xyzw]+))?$)");
    auto resOf = [&](const std::string& tok, std::string& swz) {
        std::smatch m;
        if (!std::regex_match(tok, m, resTok)) throw std::runtime_error("resource operand " + tok);
        swz = m[2].matched ? m[2].str() : "xyzw";
        return m[1].str();
    };
    auto lanes = [](const std::string& mask) {
        std::vector<int> v;
        for (char c : mask) v.push_back((int)(strchr(kComp, c) - kComp));
        return v;
    };
    auto vec = [](const char* base, const std::vector<std::string>& items) {
        if (items.size() == 1) return items[0];
        std::string e = Fmt("%s%zu(", base, items.size());
        for (size_t i = 0; i < items.size(); i++) e += (i ? ", " : "") + items[i];
        return e + ")";
    };
    if (op == "ld_structured" || op == "ld_structured_indexable" || op == "ld_raw" || op == "ld_raw_indexable") {
        bool raw = op.rfind("ld_raw", 0) == 0;
        std::string swz, reg = resOf(ops[raw ? 2 : 3], swz);
        Dest d = DestOf(ops[0]);
        std::string idx = Src(ops[1], {0}, 'u'), off = raw ? "0u" : Src(ops[2], {0}, 'u');
        std::vector<std::string> items;
        for (int c : lanes(d.mask)) items.push_back(Lane(reg, idx, off, (int)(strchr(kComp, swz[std::min<size_t>(c, swz.size() - 1)]) - kComp)));
        Emit(d.name + "." + d.mask + " = " + Store(d, vec("uint", items), 'u') + ";");
        return true;
    }
    if (op == "store_structured" || op == "store_raw") {
        bool raw = op == "store_raw";
        std::string mask, reg = resOf(ops[0], mask);
        std::string idx = Src(ops[1], {0}, 'u'), off = raw ? "0u" : Src(ops[2], {0}, 'u');
        const std::string& val = ops[raw ? 2 : 3];
        std::vector<int> ls = lanes(mask);
        for (size_t i = 0; i < ls.size(); i++) {
            std::string v = Src(val, {ls[i]}, 'u');
            if (res[reg].kind == 'r') Emit(Name(reg) + ".Store(" + idx + " + " + std::to_string(ls[i] * 4) + ", " + v + ");");
            else Emit(Lane(reg, idx, off, ls[i]) + " = " + v + ";");
        }
        return true;
    }
    if (op == "store_uav_typed") {
        std::string mask, reg = resOf(ops[0], mask);
        const Res& r = res[reg];
        Emit(Name(reg) + "[" + Coord(r, ops[1]) + "] = " + Src(ops[2], {0, 1, 2, 3}, r.uintData ? 'u' : 'f') + ";");
        return true;
    }
    if (op == "ld_uav_typed" || op == "ld_uav_typed_indexable") {
        std::string swz, reg = resOf(ops[2], swz);
        const Res& r = res[reg];
        Dest d = DestOf(ops[0]);
        std::string pick;
        for (int c : lanes(d.mask)) pick += swz[std::min<size_t>(c, swz.size() - 1)];
        Emit(d.name + "." + d.mask + " = " + Store(d, "(" + Name(reg) + "[" + Coord(r, ops[1]) + "])." + pick, r.uintData ? 'u' : 'f') + ";");
        return true;
    }
    if (op == "imm_atomic_alloc" || op == "imm_atomic_consume") {
        std::string swz, reg = resOf(ops[1], swz);
        Assign(ops[0], [&](const std::vector<int>&) { return Name(reg) + (op == "imm_atomic_alloc" ? ".IncrementCounter()" : ".DecrementCounter()"); }, 'u', false, true);
        return true;
    }
    static const std::map<std::string, std::string> atomics = {
        {"iadd", "InterlockedAdd"}, {"and", "InterlockedAnd"}, {"or", "InterlockedOr"}, {"xor", "InterlockedXor"}, {"imax", "InterlockedMax"},
        {"imin", "InterlockedMin"}, {"umax", "InterlockedMax"}, {"umin", "InterlockedMin"}, {"exch", "InterlockedExchange"},
        {"cmp_exch", "InterlockedCompareExchange"}, {"cmp_store", "InterlockedCompareStore"}};
    bool imm = op.rfind("imm_atomic_", 0) == 0;
    if (imm || op.rfind("atomic_", 0) == 0) {
        std::string kind = op.substr(imm ? 11 : 7);
        auto it = atomics.find(kind);
        if (it == atomics.end()) return false;
        size_t a = imm ? 1 : 0;
        std::string swz, reg = resOf(ops[a], swz);
        const Res& r = res[reg];
        char k = kind == "imax" || kind == "imin" ? 'i' : 'u';
        std::vector<std::string> args;
        for (size_t i = a + 2; i < ops.size(); i++) args.push_back(Src(ops[i], {0}, k));
        std::string target;
        bool method = r.kind == 'r';
        if (r.kind == 't') target = Name(reg) + "[" + Coord(r, ops[a + 1]) + "]";
        else if (r.kind == 's' || r.kind == 'g') target = Lane(reg, Src(ops[a + 1], {0}, 'u'), Src(ops[a + 1], {1}, 'u'), 0);
        else if (r.kind == 'h') target = Lane(reg, Src(ops[a + 1], {0}, 'u'), "0u", 0);
        else target = Src(ops[a + 1], {0}, 'u');
        std::string call = method ? Name(reg) + "." + it->second + "(" + target : it->second + "(" + target;
        for (auto& x : args) call += ", " + x;
        if (imm) {
            Dest d = DestOf(ops[0]);
            if (d.none) { Emit(call + ");"); return true; }
            Emit("{ " + std::string(k == 'i' ? "int" : "uint") + " orig; " + call + ", orig); " + d.name + "." + d.mask + " = " + Store(d, "orig", k) + "; }");
        } else Emit(call + ");");
        return true;
    }
    if (op == "resinfo" || op == "resinfo_indexable") {
        // resinfo_indexable(texture2d)(float,float,float,float)_uint r0.xy, l(0), t0.xyzw
        bool asUint = IEndsWith(op0, "_uint"), rcp = IEndsWith(op0, "_rcpfloat");
        std::string swz, reg = resOf(ops[2], swz);
        Dest d = DestOf(ops[0]);
        std::string dim = op0.find("texture3d") != std::string::npos ? "3d" : op0.find("texturecube") != std::string::npos ? "cube"
                        : op0.find("texture2darray") != std::string::npos ? "2darray" : op0.find("texture1d") != std::string::npos ? "1d" : "2d";
        std::string mip = Src(ops[1], {0}, 'u'), n = Name(reg), dims;
        if (dim == "3d" || dim == "2darray") dims = "w, h, d, l";
        else if (dim == "1d") dims = "w, l";
        else dims = "w, h, l";
        std::string all = dim == "3d" || dim == "2darray" ? "float4(w, h, d, l)" : dim == "1d" ? "float4(w, 0, 0, l)" : "float4(w, h, 0, l)";
        std::string pick;
        for (int c : lanes(d.mask)) pick += swz[std::min<size_t>(c, swz.size() - 1)];
        std::string v = (rcp ? "(1.0 / " + all + ")" : all) + "." + pick;
        Emit("{ uint w, h, d, l; " + n + ".GetDimensions(" + mip + ", " + dims + "); " + d.name + "." + d.mask + " = " +
             (asUint ? Store(d, "(" + VecType("uint", d.mask.size()) + ")(" + v + ")", 'u') : Store(d, v, 'f')) + "; }");
        return true;
    }
    if (op == "bufinfo" || op == "bufinfo_indexable") {
        std::string swz, reg = resOf(ops[1], swz);
        auto it = res.find(reg);
        std::string n = Name(reg);
        std::string call = it != res.end() && (it->second.kind == 's') ? n + ".GetDimensions(c, s)" : n + ".GetDimensions(c)";
        Assign(ops[0], [&](const std::vector<int>&) { return "c"; }, 'u', false, true);
        lines.back() = std::string(ind * 4, ' ') + "{ uint c, s; " + call + "; " + Trim(lines.back()) + " }";
        return true;
    }
    if (op.rfind("sync", 0) == 0) {
        bool t = op.find("_t") != std::string::npos, global = op.find("uglobal") != std::string::npos;
        Emit(std::string(global ? "AllMemoryBarrier" : "GroupMemoryBarrier") + (t ? "WithGroupSync" : "") + "();");
        return true;
    }
    return false;
}

void Translator::TexOp(const std::string& op, const std::vector<std::string>& ops, bool sat) {
    Dest d = DestOf(ops[0]);
    std::vector<int> comps;
    for (char c : d.mask) comps.push_back((int)(strchr(kComp, c) - kComp));
    bool ldms = op.rfind("ldms", 0) == 0, spos = op.rfind("samplepos", 0) == 0, lod = op.rfind("lod", 0) == 0;
    const std::string& texTok = ops[spos ? 1 : 2];
    static const std::regex tr(R"(^t(\d+)\.([xyzw]{4})$)");
    std::smatch tm;
    if (!std::regex_match(texTok, tm, tr)) throw std::runtime_error("texture " + texTok);
    std::string treg = "t" + tm[1].str(), tex = Name(treg), tsw = tm[2];
    std::string dim = texDim.count(treg) ? texDim[treg] : "texture2d";
    int n = dim == "texture1d" || dim == "buffer" ? 1 : dim == "texture2d" || dim == "texture2dms" || dim == "texture1darray" ? 2 : dim == "texturecubearray" ? 4 : 3;
    std::vector<int> uv, grad;
    for (int i = 0; i < n; i++) uv.push_back(i);
    for (int i = 0; i < (dim == "texture3d" || dim == "texturecube" || dim == "texturecubearray" ? 3 : dim == "texture1d" || dim == "texture1darray" ? 1 : 2); i++) grad.push_back(i);
    // immediate texel offset: sample_aoffimmi(1,0,0)_indexable(...)
    std::string offset;
    static const std::regex aoff(R"(aoffimmi\((-?\d+),(-?\d+),(-?\d+)\))");
    std::smatch om;
    if (std::regex_search(op, om, aoff)) {
        int k = (int)grad.size();
        offset = k == 1 ? ", " + om[1].str() : k == 2 ? ", int2(" + om[1].str() + ", " + om[2].str() + ")" : ", int3(" + om[1].str() + ", " + om[2].str() + ", " + om[3].str() + ")";
        if (om[1] == "0" && om[2] == "0" && om[3] == "0") offset.clear();
    }
    auto samp = [&](const std::string& s) { return Name(s.substr(0, s.find('.'))); };
    auto has = [&](const char* p) { return op.rfind(p, 0) == 0; };
    std::string full;
    char kind = texUint.count(treg) ? 'u' : 'f';
    bool scalarResult = false;
    if (spos) {
        full = tex + ".GetSamplePosition(" + Src(ops[2], {0}, 'i') + ")";
        kind = 'f';
        std::string pick;
        for (int c : comps) pick += tsw[c];
        std::string e = "float4(" + full + ", 0, 0)." + std::string(pick);
        Emit(d.name + "." + d.mask + " = " + Store(d, e, 'f') + ";");
        return;
    }
    if (lod) {
        // lod dst, coord, t, s: x = clamped, y = unclamped
        std::string c = Src(ops[1], uv), sm = samp(ops[3]);
        std::string e = "float4(" + tex + ".CalculateLevelOfDetail(" + sm + ", " + c + "), " + tex + ".CalculateLevelOfDetailUnclamped(" + sm + ", " + c + "), 0, 0)";
        std::string pick;
        for (int cc : comps) pick += tsw[cc];
        Emit(d.name + "." + d.mask + " = " + Store(d, e + "." + pick, 'f') + ";");
        return;
    }
    if (has("sample_l")) full = tex + ".SampleLevel(" + samp(ops[3]) + ", " + Src(ops[1], uv) + ", " + Src(ops[4], {0}) + offset + ")";
    else if (has("sample_b")) full = tex + ".SampleBias(" + samp(ops[3]) + ", " + Src(ops[1], uv) + ", " + Src(ops[4], {0}) + offset + ")";
    else if (has("sample_d")) full = tex + ".SampleGrad(" + samp(ops[3]) + ", " + Src(ops[1], uv) + ", " + Src(ops[4], grad) + ", " + Src(ops[5], grad) + offset + ")";
    else if (has("sample_c_lz")) { full = tex + ".SampleCmpLevelZero(" + samp(ops[3]) + ", " + Src(ops[1], uv) + ", " + Src(ops[4], {0}) + offset + ")"; scalarResult = true; }
    else if (has("sample_c")) { full = tex + ".SampleCmp(" + samp(ops[3]) + ", " + Src(ops[1], uv) + ", " + Src(ops[4], {0}) + offset + ")"; scalarResult = true; }
    else if (has("sample")) full = tex + ".Sample(" + samp(ops[3]) + ", " + Src(ops[1], uv) + offset + ")";
    else if (ldms) {
        std::vector<int> c(uv.begin(), uv.end());
        full = tex + ".Load(" + Src(ops[1], c, 'i') + ", " + Src(ops[3], {0}, 'i') + offset + ")";
    } else if (has("ld")) {
        if (dim == "buffer") full = tex + ".Load(" + Src(ops[1], {0}, 'i') + ")";
        else {
            std::string c = n == 1 ? Src(ops[1], {0}, 'i') : Src(ops[1], uv, 'i');
            full = tex + ".Load(" + VecType("int", uv.size() + 1) + "(" + c + ", " + Src(ops[1], {3}, 'i') + ")" + offset + ")";
        }
    } else if (has("gather4_c")) {
        full = tex + ".GatherCmp(" + samp(ops[3]) + ", " + Src(ops[1], uv) + ", " + Src(ops[4], {0}) + offset + ")";
    } else if (has("gather4")) {
        std::string s = ops[3];
        char ch = s.size() > 2 && s[s.size() - 2] == '.' ? s.back() : 'x';
        const char* fn = ch == 'y' ? "GatherGreen" : ch == 'z' ? "GatherBlue" : ch == 'w' ? "GatherAlpha" : "GatherRed";
        full = tex + "." + fn + "(" + samp(s) + ", " + Src(ops[1], uv) + offset + ")";
        kind = 'f';
    } else throw std::runtime_error("texture op " + op);
    std::string pick;
    for (int c : comps) pick += scalarResult ? 'x' : tsw[c];
    std::string e = scalarResult ? (comps.size() > 1 ? VecType("float", comps.size()) + "(" + full + ")" : full) : "(" + full + ")." + pick;
    if (sat) e = "saturate(" + e + ")";
    Emit(d.name + "." + d.mask + " = " + Store(d, e, kind) + ";");
}

void Translator::Line(const std::string& raw) {
    std::string ln = Trim(raw);
    // strip the instruction number D3DDisassemble can prefix ("  12: mul ...")
    if (ln.empty() || ln.rfind("//", 0) == 0 || ln.rfind("dcl_", 0) == 0 || ln == "{" || ln == "}" || ln[0] == '{') return;
    static const std::regex ver(R"(^[vpcghd]s_\d_\d$)");
    if (std::regex_match(ln, ver)) return;
    size_t sp = 0;
    int paren = 0;
    while (sp < ln.size() && (paren || (ln[sp] != ' ' && ln[sp] != '\t'))) {
        if (ln[sp] == '(') paren++;
        if (ln[sp] == ')') paren--;
        sp++;
    }
    std::string op = ln.substr(0, sp), rest = sp < ln.size() ? Trim(ln.substr(sp)) : "";
    std::vector<std::string> ops = SplitOps(rest);
    bool sat = false;
    if (IEndsWith(op, "_sat")) { sat = true; op.resize(op.size() - 4); }
    if (op.size() > 3 && (IEndsWith(op, "_opc") || IEndsWith(op, "_uint"))) {}   // keep
    auto F = [&](size_t i, const std::vector<int>& c, char k = 'f') { return Src(ops.at(i), c, k); };
    auto nary = [&](const std::function<std::string(const std::vector<std::string>&)>& fmt) {
        Assign(ops[0], [&](const std::vector<int>& c) {
            std::vector<std::string> a;
            for (size_t i = 1; i < ops.size(); i++) a.push_back(F(i, c));
            return fmt(a);
        }, 'f', sat);
    };
    static const std::map<std::string, std::string> unaryFn = {
        {"sqrt", "sqrt"}, {"rsq", "rsqrt"}, {"log", "log2"}, {"exp", "exp2"}, {"frc", "frac"}, {"round_pi", "ceil"}, {"round_ni", "floor"},
        {"round_z", "trunc"}, {"round_ne", "round"}, {"deriv_rtx", "ddx"}, {"deriv_rty", "ddy"}, {"deriv_rtx_coarse", "ddx_coarse"},
        {"deriv_rty_coarse", "ddy_coarse"}, {"deriv_rtx_fine", "ddx_fine"}, {"deriv_rty_fine", "ddy_fine"}, {"rcp", "rcp"}};
    if (op == "mov") {
        bool fl = sat || !exact || (ops[1][0] == '-' || ops[1][0] == '|');
        Assign(ops[0], [&](const std::vector<int>& c) { return F(1, c, fl ? 'f' : 'u'); }, fl ? 'f' : 'u', sat);
        return;
    }
    if (op == "add") return nary([](auto& a) { return "(" + a[0] + " + " + a[1] + ")"; });
    if (op == "mul") return nary([](auto& a) { return "(" + a[0] + " * " + a[1] + ")"; });
    if (op == "div") return nary([](auto& a) { return "(" + a[0] + " / " + a[1] + ")"; });
    if (op == "mad") return nary([](auto& a) { return "mad(" + a[0] + ", " + a[1] + ", " + a[2] + ")"; });
    if (op == "min") return nary([](auto& a) { return "min(" + a[0] + ", " + a[1] + ")"; });
    if (op == "max") return nary([](auto& a) { return "max(" + a[0] + ", " + a[1] + ")"; });
    if (auto it = unaryFn.find(op); it != unaryFn.end()) {
        std::string fn = it->second;
        return nary([&](auto& a) { return fn + "(" + a[0] + ")"; });
    }
    static const std::map<std::string, std::pair<std::string, char>> cmp = {
        {"lt", {"<", 'f'}}, {"ge", {">=", 'f'}}, {"ne", {"!=", 'f'}}, {"eq", {"==", 'f'}}, {"ilt", {"<", 'i'}}, {"ige", {">=", 'i'}},
        {"ieq", {"==", 'i'}}, {"ine", {"!=", 'i'}}, {"ult", {"<", 'u'}}, {"uge", {">=", 'u'}}};
    if (auto it = cmp.find(op); it != cmp.end()) {
        auto [o, k] = it->second;
        Assign(ops[0], [&](const std::vector<int>& c) {
            return "(" + VecType("uint", c.size()) + ")((" + F(1, c, k) + ") " + o + " (" + F(2, c, k) + ")) * 0xffffffffu";
        }, 'u');
        return;
    }
    if (op == "dp2" || op == "dp3" || op == "dp4") {
        int n = op[2] - '0';
        std::vector<int> cc;
        for (int i = 0; i < n; i++) cc.push_back(i);
        Assign(ops[0], [&](const std::vector<int>&) { return "dot(" + F(1, cc) + ", " + F(2, cc) + ")"; }, 'f', sat, true);
        return;
    }
    if (op == "movc") {
        char k = (sat || !exact) ? 'f' : 'u';
        auto pick = [&](size_t i, const std::vector<int>& c) {
            if (k == 'u' && (ops[i][0] == '-' || ops[i][0] == '|')) return "asuint(" + F(i, c, 'f') + ")";
            return F(i, c, k);
        };
        Assign(ops[0], [&](const std::vector<int>& c) { return "((" + F(1, c, 'u') + ") != 0u ? (" + pick(2, c) + ") : (" + pick(3, c) + "))"; }, k, sat);
        return;
    }
    if ((op == "imul" || op == "umul") && ops.size() == 4) {
        if (ops[0] != "null") throw std::runtime_error("high half of a 32x32 multiply");
        char k = op == "imul" ? 'i' : 'u';
        Assign(ops[1], [&](const std::vector<int>& c) { return "(" + Src(ops[2], c, k) + " * " + Src(ops[3], c, k) + ")"; }, k);
        return;
    }
    static const std::map<std::string, std::pair<char, std::string>> intops = {
        {"iadd", {'i', "+"}}, {"imul", {'i', "*"}}, {"and", {'u', "&"}}, {"or", {'u', "|"}}, {"xor", {'u', "^"}},
        {"umin", {'u', "min"}}, {"umax", {'u', "max"}}, {"imin", {'i', "min"}}, {"imax", {'i', "max"}}};
    if (auto it = intops.find(op); it != intops.end()) {
        auto [k, o] = it->second;
        Assign(ops[0], [&](const std::vector<int>& c) {
            if (o.size() > 1) return o + "(" + F(1, c, k) + ", " + F(2, c, k) + ")";
            return "(" + F(1, c, k) + " " + o + " " + F(2, c, k) + ")";
        }, k);
        return;
    }
    if (op == "imad") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + F(1, c, 'i') + " * " + F(2, c, 'i') + " + " + F(3, c, 'i') + ")"; }, 'i'); return; }
    if (op == "umad") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + F(1, c, 'u') + " * " + F(2, c, 'u') + " + " + F(3, c, 'u') + ")"; }, 'u'); return; }
    if (op == "ishl") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + F(1, c, 'u') + " << (" + F(2, c, 'u') + " & 31u))"; }, 'u'); return; }
    if (op == "ushr") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + F(1, c, 'u') + " >> (" + F(2, c, 'u') + " & 31u))"; }, 'u'); return; }
    if (op == "ishr") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + F(1, c, 'i') + " >> (" + F(2, c, 'i') + " & 31))"; }, 'i'); return; }
    if (op == "ineg") { Assign(ops[0], [&](const std::vector<int>& c) { return "(-" + F(1, c, 'i') + ")"; }, 'i'); return; }
    if (op == "bfrev") { Assign(ops[0], [&](const std::vector<int>& c) { return "reversebits(" + F(1, c, 'u') + ")"; }, 'u'); return; }
    if (op == "countbits") { Assign(ops[0], [&](const std::vector<int>& c) { return "countbits(" + F(1, c, 'u') + ")"; }, 'u'); return; }
    if (op == "firstbit_lo") { Assign(ops[0], [&](const std::vector<int>& c) { return "firstbitlow(" + F(1, c, 'u') + ")"; }, 'u'); return; }
    if (op == "firstbit_hi") { Assign(ops[0], [&](const std::vector<int>& c) { return "(31u - firstbithigh(" + F(1, c, 'u') + "))"; }, 'u'); return; }
    if (op == "firstbit_shi") { Assign(ops[0], [&](const std::vector<int>& c) { return "(31u - firstbithigh(" + F(1, c, 'i') + "))"; }, 'u'); return; }
    if (op == "not") { Assign(ops[0], [&](const std::vector<int>& c) { return "(~" + F(1, c, 'u') + ")"; }, 'u'); return; }
    if (op == "ftou") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + VecType("uint", c.size()) + ")(" + F(1, c) + ")"; }, 'u'); return; }
    if (op == "ftoi") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + VecType("int", c.size()) + ")(" + F(1, c) + ")"; }, 'i'); return; }
    if (op == "utof") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + VecType("float", c.size()) + ")(" + F(1, c, 'u') + ")"; }, 'f'); return; }
    if (op == "itof") { Assign(ops[0], [&](const std::vector<int>& c) { return "(" + VecType("float", c.size()) + ")(" + F(1, c, 'i') + ")"; }, 'f'); return; }
    if (op == "f32tof16") { Assign(ops[0], [&](const std::vector<int>& c) { return "f32tof16(" + F(1, c) + ")"; }, 'u'); return; }
    if (op == "f16tof32") { Assign(ops[0], [&](const std::vector<int>& c) { return "f16tof32(" + F(1, c, 'u') + ")"; }, 'f'); return; }
    if (op == "ubfe" || op == "ibfe" || op == "bfi") {
        // literal lane of an operand (width / offset are almost always immediates)
        auto lit = [&](const std::string& tok, int i, u32& v) {
            if (tok.rfind("l(", 0) != 0) return false;
            auto vals = SplitOps(tok.substr(2, tok.size() - 3));
            const std::string& s = vals[std::min<size_t>(i, vals.size() - 1)];
            if (!IsInt(s) && s.rfind("0x", 0) != 0) return false;
            v = (u32)(s.rfind("0x", 0) == 0 ? std::stoull(s, nullptr, 16) : (u64)std::stoll(s)) & 31u;
            return true;
        };
        auto lane = [&](int i) {
            u32 wl, ol;
            if (lit(ops[1], i, wl) && lit(ops[2], i, ol)) {
                u32 mask = wl ? (u32)((((u64)1 << wl) - 1) << ol) : 0;
                if (op == "bfi")
                    return Fmt("(((%s << %uu) & 0x%08xu) | (%s & 0x%08xu))", Src(ops[3], {i}, 'u').c_str(), ol, mask, Src(ops[4], {i}, 'u').c_str(), ~mask);
                if (!wl) return std::string(op == "ubfe" ? "0u" : "0u");
                if (op == "ubfe") return wl + ol < 32 ? Fmt("((%s >> %uu) & 0x%xu)", Src(ops[3], {i}, 'u').c_str(), ol, (u32)(((u64)1 << wl) - 1)) : Fmt("(%s >> %uu)", Src(ops[3], {i}, 'u').c_str(), ol);
                return wl + ol < 32 ? Fmt("asuint((%s << %uu) >> %uu)", Src(ops[3], {i}, 'i').c_str(), 32 - wl - ol, 32 - wl) : Fmt("asuint(%s >> %uu)", Src(ops[3], {i}, 'i').c_str(), ol);
            }
            std::string w = "(" + Src(ops[1], {i}, 'u') + " & 31u)", o = "(" + Src(ops[2], {i}, 'u') + " & 31u)";
            if (op == "bfi") {
                std::string m = "((((1u << " + w + ") - 1u) << " + o + ") & 0xffffffffu)";
                return "(((" + Src(ops[3], {i}, 'u') + " << " + o + ") & " + m + ") | (" + Src(ops[4], {i}, 'u') + " & ~" + m + "))";
            }
            if (op == "ubfe") {
                std::string v = Src(ops[3], {i}, 'u');
                return "(" + w + " == 0u ? 0u : (" + w + " + " + o + " < 32u ? ((" + v + " << (32u - " + w + " - " + o + ")) >> (32u - " + w + ")) : (" + v + " >> " + o + ")))";
            }
            std::string v = Src(ops[3], {i}, 'i');
            return "asuint(" + w + " == 0u ? 0 : (" + w + " + " + o + " < 32u ? ((" + v + " << (32u - " + w + " - " + o + ")) >> (32u - " + w + ")) : (" + v + " >> " + o + ")))";
        };
        Assign(ops[0], [&](const std::vector<int>& c) {
            if (c.size() == 1) return lane(c[0]);
            std::string e = VecType("uint", c.size()) + "(";
            for (size_t i = 0; i < c.size(); i++) e += (i ? ", " : "") + lane(c[i]);
            return e + ")";
        }, 'u');
        return;
    }
    if (op == "sincos" || op == "udiv") {
        auto regOf = [](const std::string& t) {
            static const std::regex re(R"(^-?\|?(r\d+))");
            std::smatch m;
            return std::regex_search(t, m, re) ? m[1].str() : std::string();
        };
        std::set<std::string> used;
        for (size_t i = 2; i < ops.size(); i++) if (auto r = regOf(ops[i]); !r.empty()) used.insert(r);
        auto emitN = [&](int which) {
            if (ops[which] == "null") return;
            if (op == "sincos") { Assign(ops[which], [&](const std::vector<int>& c) { return std::string(which ? "cos(" : "sin(") + F(2, c) + ")"; }, 'f', sat); return; }
            Assign(ops[which], [&](const std::vector<int>& c) { return "(" + F(2, c, 'u') + (which ? " % " : " / ") + F(3, c, 'u') + ")"; }, 'u');
        };
        bool c0 = used.count(regOf(ops[0])) > 0;
        if (c0) { emitN(1); emitN(0); }
        else { emitN(0); emitN(1); }
        return;
    }
    auto cond = [&](size_t i, bool nz) { return "((" + Src(ops[i], {0}, 'u') + (nz ? ") != 0u)" : ") == 0u)"); };
    if (op == "discard_nz") { Emit("if " + cond(0, true) + " discard;"); return; }
    if (op == "discard_z") { Emit("if " + cond(0, false) + " discard;"); return; }
    if (op == "if_nz") { Emit("if " + cond(0, true) + " {"); ind++; return; }
    if (op == "if_z") { Emit("if " + cond(0, false) + " {"); ind++; return; }
    if (op == "else") { ind--; Emit("} else {"); ind++; return; }
    if (op == "endif" || op == "endloop" || op == "endswitch") { ind = std::max(1, ind - 1); Emit("}"); return; }
    if (op == "ret") { Emit("return;"); return; }
    if (op == "loop") { Emit("[loop] while (true) {"); ind++; return; }
    if (op == "breakc_nz") { Emit("if " + cond(0, true) + " break;"); return; }
    if (op == "breakc_z") { Emit("if " + cond(0, false) + " break;"); return; }
    if (op == "break") { Emit("break;"); return; }
    if (op == "continue") { Emit("continue;"); return; }
    if (op == "continuec_nz") { Emit("if " + cond(0, true) + " continue;"); return; }
    if (op == "continuec_z") { Emit("if " + cond(0, false) + " continue;"); return; }
    if (op == "retc_nz") { Emit("if " + cond(0, true) + " return;"); return; }
    if (op == "retc_z") { Emit("if " + cond(0, false) + " return;"); return; }
    if (op == "switch") { Emit("switch (" + Src(ops[0], {0}, 'i') + ") {"); ind++; return; }
    if (op == "case") { Emit("case " + Src(ops[0], {0}, 'i') + ":"); return; }
    if (op == "default") { Emit("default:"); return; }
    if (op == "nop") return;
    if (op == "emit_stream" || op == "emit") {
        std::string e = "{ GSOut vtx;";
        for (auto& o : gsOut) e += " vtx." + o + " = " + o + ";";
        Emit(e + " stream.Append(vtx); }");
        return;
    }
    if (op == "cut_stream" || op == "cut") { Emit("stream.RestartStrip();"); return; }
    if (MemOp(op, ops)) return;
    if (op.rfind("sample", 0) == 0 || op.rfind("ld_indexable", 0) == 0 || op.rfind("ld_aoffimmi", 0) == 0 || op == "ld" || op.rfind("ldms", 0) == 0 ||
        op.rfind("gather4", 0) == 0 || op.rfind("lod", 0) == 0) { TexOp(op, ops, sat); return; }
    if (op == "swapc") {
        // swapc d0, d1, cond, a, b: d0 = cond ? b : a, d1 = cond ? a : b (computed before either is written)
        Dest d0 = DestOf(ops[0]), d1 = DestOf(ops[1]);
        char k = exact ? 'u' : 'f';
        auto sel = [&](const Dest& d, bool first) {
            std::vector<int> c;
            for (char ch : d.mask) c.push_back((int)(strchr(kComp, ch) - kComp));
            return "((" + Src(ops[2], c, 'u') + ") != 0u ? (" + Src(ops[first ? 4 : 3], c, k) + ") : (" + Src(ops[first ? 3 : 4], c, k) + "))";
        };
        std::string a = sel(d0, true), b = sel(d1, false);
        std::string ty0 = VecType(exact ? "uint" : "float", d0.mask.size()), ty1 = VecType(exact ? "uint" : "float", d1.mask.size());
        Emit("{ " + ty0 + " s0 = " + a + "; " + ty1 + " s1 = " + b + "; " + (d0.none ? "" : d0.name + "." + d0.mask + " = s0; ") +
             (d1.none ? "" : d1.name + "." + d1.mask + " = s1; ") + "}");
        return;
    }
    throw std::runtime_error("op " + op);
}

std::string Ident(std::string s) {
    for (auto& c : s)
        if (!isalnum((u8)c) && c != '_') c = '_';
    while (!s.empty() && s[0] == '_') s.erase(s.begin());
    if (s.empty() || isdigit((u8)s[0])) s = "b_" + s;
    return s;
}

}   // namespace

bool DecompileShader(const Bytes& dxbc, std::string& hlsl, bool bitExact, std::string* error) {
    std::string asmText;
    if (!Disasm(dxbc, bitExact ? 0x80 : 0, asmText, error)) return false;   // 0x80: hex literals (exact bit patterns)
    ShaderReflectionInfo refl;
    bool hasRefl = ReflectShader(dxbc, refl, nullptr);

    Translator t;
    t.exact = bitExact;
    std::string decl;
    std::string target;
    {
        static const std::regex ver(R"((^|\n)\s*([vpcghd]s_\d_\d)\s*(\r?\n|$))");
        std::smatch m;
        if (std::regex_search(asmText, m, ver)) target = m[2];
    }
    decl += "// Decompiled from DXBC (" + target + ")" + (bitExact ? ", bit-exact form: temps hold raw 32-bit lanes" : "") + "\n";
    decl += "// Constant buffer reads are annotated with the members they touch.\n\n";

    // constant buffers: names from reflection, sizes from dcl_constantbuffer
    static const std::regex dclcb(R"(dcl_constantbuffer (?:CB|cb)(\d+)\[(\d+)\])");
    std::map<int, int> cbSizes;
    for (std::sregex_iterator it(asmText.begin(), asmText.end(), dclcb), end; it != end; ++it) cbSizes[std::stoi((*it)[1])] = std::stoi((*it)[2]);
    for (auto& [slot, regs] : cbSizes) {
        const ShaderConstantBuffer* cb = nullptr;
        for (auto& c : refl.cbuffers)
            if ((int)c.slot == slot) cb = &c;
        std::string n = cb ? Ident(cb->name) : Fmt("cb%d", slot);
        std::string arr = n + "_cb";
        t.names[Fmt("cb%d", slot)] = arr;
        if (cb) t.cbs[slot] = cb;
        decl += Fmt("cbuffer %s : register(b%d)\n{\n    float4 %s[%d];\n", n.c_str(), slot, arr.c_str(), regs);
        if (cb)
            for (auto& v : cb->variables)
                decl += Fmt("    // [%3u].%c  %-20s %s\n", v.offset / 16, kComp[(v.offset % 16) / 4], v.type.c_str(), v.name.c_str());
        decl += "};\n";
    }
    // textures / samplers / uavs
    static const std::regex dclres(R"(dcl_resource_(\w+?)(?:\(\d+\))? \(([^)]*)\) t(\d+))");
    for (std::sregex_iterator it(asmText.begin(), asmText.end(), dclres), end; it != end; ++it) {
        std::string dim = (*it)[1], fmt = (*it)[2];
        int slot = std::stoi((*it)[3]);
        std::string name = Fmt("t%d", slot);
        for (auto& b : refl.bindings)
            if (b.kind != "sampler" && b.kind != "cbuffer" && b.kind != "uav" && (int)b.slot == slot) name = Ident(b.name);
        t.names[Fmt("t%d", slot)] = name;
        t.texDim[Fmt("t%d", slot)] = dim;
        if (fmt.find("uint") != std::string::npos || fmt.find("sint") != std::string::npos) t.texUint.insert(Fmt("t%d", slot));
        std::string ty = dim == "texture2d" ? "Texture2D" : dim == "texture3d" ? "Texture3D" : dim == "texturecube" ? "TextureCube" : dim == "texture2darray" ? "Texture2DArray"
                       : dim == "texture1d" ? "Texture1D" : dim == "buffer" ? "Buffer" : dim == "texturecubearray" ? "TextureCubeArray" : dim == "texture2dms" ? "Texture2DMS" : "Texture2D";
        std::string comp = fmt.find("uint") != std::string::npos ? "uint4" : fmt.find("sint") != std::string::npos ? "int4" : "float4";
        decl += Fmt("%s<%s> %s : register(t%d);\n", ty.c_str(), comp.c_str(), name.c_str(), slot);
    }
    {
        static const std::regex dclbuf(R"(dcl_(resource|uav)_(structured|raw)(?:_glc)?(?:_opc)? ([tu])(\d+)(?:, (\d+))?)");
        static const std::regex dcltyped(R"(dcl_uav_typed_(\w+?)(?:_glc)? \(([^)]*)\) u(\d+))");
        static const std::regex dcltgsm(R"(dcl_tgsm_(structured|raw) g(\d+), (\d+)(?:, (\d+))?)");
        std::set<std::string> counters;
        static const std::regex counter(R"(imm_atomic_(?:alloc|consume) \w+(?:\.\w+)?, (u\d+))");
        for (std::sregex_iterator it(asmText.begin(), asmText.end(), counter), end; it != end; ++it) counters.insert((*it)[1]);
        for (std::sregex_iterator it(asmText.begin(), asmText.end(), dclbuf), end; it != end; ++it) {
            bool uav = (*it)[1] == "uav", structured = (*it)[2] == "structured";
            std::string reg = (*it)[3].str() + (*it)[4].str(), name = reg;
            for (auto& b : refl.bindings)
                if ((int)b.slot == std::stoi((*it)[4]) && ((b.kind == "uav") == uav) && b.kind != "sampler" && b.kind != "cbuffer") name = Ident(b.name);
            t.names[reg] = name;
            Translator::Res r;
            r.kind = structured ? 's' : 'r';
            if (structured) {
                r.stride = (*it)[5].matched ? std::stoi((*it)[5]) : 4;
                decl += Fmt("struct %s_t { uint v[%d]; };\n", name.c_str(), std::max(1, r.stride / 4));
                decl += Fmt("%s<%s_t> %s : register(%s);\n", uav ? (counters.count(reg) ? "RWStructuredBuffer" : "RWStructuredBuffer") : "StructuredBuffer",
                            name.c_str(), name.c_str(), reg.c_str());
            } else decl += Fmt("%s %s : register(%s);\n", uav ? "RWByteAddressBuffer" : "ByteAddressBuffer", name.c_str(), reg.c_str());
            t.res[reg] = r;
        }
        std::set<std::string> atomicTargets;
        static const std::regex atom(R"(atomic_\w+ (?:[^,\n]+, )?(u\d+),)");
        for (std::sregex_iterator it(asmText.begin(), asmText.end(), atom), end; it != end; ++it) atomicTargets.insert((*it)[1]);
        for (std::sregex_iterator it(asmText.begin(), asmText.end(), dcltyped), end; it != end; ++it) {
            std::string dim = (*it)[1], fmt = (*it)[2], reg = "u" + (*it)[3].str(), name = reg;
            for (auto& b : refl.bindings)
                if (b.kind == "uav" && (int)b.slot == std::stoi((*it)[3])) name = Ident(b.name);
            t.names[reg] = name;
            Translator::Res r;
            r.kind = 't';
            r.dim = dim;
            r.uintData = fmt.find("uint") != std::string::npos || fmt.find("sint") != std::string::npos;
            t.res[reg] = r;
            std::string ty = dim == "texture2d" ? "RWTexture2D" : dim == "texture3d" ? "RWTexture3D" : dim == "texture2darray" ? "RWTexture2DArray"
                           : dim == "texture1d" ? "RWTexture1D" : "RWBuffer";
            bool scalar = atomicTargets.count(reg) > 0;
            decl += Fmt("%s<%s> %s : register(%s);\n", ty.c_str(), r.uintData ? (scalar ? "uint" : "uint4") : "float4", name.c_str(), reg.c_str());
        }
        for (std::sregex_iterator it(asmText.begin(), asmText.end(), dcltgsm), end; it != end; ++it) {
            std::string reg = "g" + (*it)[2].str();
            t.names[reg] = reg;
            Translator::Res r;
            if ((*it)[1] == "structured") {
                r.kind = 'g';
                r.stride = std::stoi((*it)[3]);
                decl += Fmt("groupshared uint %s[%d];\n", reg.c_str(), std::stoi((*it)[3]) / 4 * std::stoi((*it)[4]));
            } else {
                r.kind = 'h';
                decl += Fmt("groupshared uint %s[%d];\n", reg.c_str(), std::stoi((*it)[3]) / 4);
            }
            t.res[reg] = r;
        }
    }
    static const std::regex dclsamp(R"(dcl_sampler s(\d+), (\w+))");
    for (std::sregex_iterator it(asmText.begin(), asmText.end(), dclsamp), end; it != end; ++it) {
        int slot = std::stoi((*it)[1]);
        std::string name = Fmt("s%d", slot);
        for (auto& b : refl.bindings)
            if (b.kind == "sampler" && (int)b.slot == slot) name = Ident(b.name);
        t.names[Fmt("s%d", slot)] = name;
        decl += Fmt("%s %s : register(s%d);\n", (*it)[2] == "mode_comparison" ? "SamplerComparisonState" : "SamplerState", name.c_str(), slot);
    }
    for (auto& [k, w] : std::map<std::string, int>{}) (void)k, (void)w;
    static const std::regex dcltid(R"(dcl_input (vThreadGroupID|vThreadIDInGroup|vThreadID)\.([xyz]+))");
    for (std::sregex_iterator it(asmText.begin(), asmText.end(), dcltid), end; it != end; ++it) t.tid[(*it)[1]] = (int)(*it)[2].length();
    // immediate constant buffer
    static const std::regex icb(R"(dcl_immediateConstantBuffer \{([\s\S]*?)\}\s*\n\s*dcl_)");
    std::smatch im;
    if (std::regex_search(asmText, im, icb)) {
        static const std::regex row(R"(\{([^{}]*)\})");
        std::string body = im[1];
        std::vector<std::string> rows;
        for (std::sregex_iterator it(body.begin(), body.end(), row), end; it != end; ++it) {
            std::string r = "float4(";
            auto vals = SplitOps((*it)[1]);
            for (size_t i = 0; i < vals.size(); i++) r += (i ? ", " : "") + FloatLit(vals[i]);
            rows.push_back(r + ")");
        }
        decl += Fmt("static const float4 icb[%zu] = {", rows.size());
        for (size_t i = 0; i < rows.size(); i++) decl += (i ? ", " : "") + rows[i];
        decl += "};\n";
    }

    // entry point signature from the declarations; semantics from reflection, else from the system value names
    std::string params;
    bool isVs = target.rfind("vs", 0) == 0, isPs = target.rfind("ps", 0) == 0;
    struct Reg { std::string sv; };
    std::map<int, Reg> ins, outs;
    int gsVerts = 0;
    bool isGs = target.rfind("gs", 0) == 0;
    {
        static const std::regex dcl(R"(^\s*dcl_(input|output)\w*\s+(?:[a-z_]+\s+)*([vo])(\d+)(?:\.[xyzw]+)?(?:,\s*(\w+))?)");
        size_t p = 0;
        while (p < asmText.size()) {
            size_t e = asmText.find('\n', p);
            if (e == std::string::npos) e = asmText.size();
            std::string l = asmText.substr(p, e - p);
            p = e + 1;
            std::smatch m;
            static const std::regex gsin(R"(^\s*dcl_input\w*\s+v\[(\d+)\]\[(\d+)\](?:\.[xyzw]+)?(?:,\s*(\w+))?)");
            if (std::regex_search(l, m, gsin)) {
                gsVerts = std::stoi(m[1]);
                Reg& r = ins[std::stoi(m[2])];
                if (m[3].matched) r.sv = m[3];
                continue;
            }
            if (!std::regex_search(l, m, dcl)) continue;
            auto& dst = m[2] == "v" ? ins : outs;
            Reg& r = dst[std::stoi(m[3])];
            if (m[4].matched) r.sv = m[4];
        }
    }
    static const std::map<std::string, std::pair<const char*, const char*>> svNames = {   // asm name -> semantic, type
        {"position", {"SV_Position", "float4"}}, {"vertex_id", {"SV_VertexID", "uint"}}, {"instance_id", {"SV_InstanceID", "uint"}},
        {"is_front_face", {"SV_IsFrontFace", "bool"}}, {"sampleIndex", {"SV_SampleIndex", "uint"}}, {"primitive_id", {"SV_PrimitiveID", "uint"}},
        {"clip_distance", {"SV_ClipDistance", "float4"}}, {"cull_distance", {"SV_CullDistance", "float4"}},
        {"rendertarget_array_index", {"SV_RenderTargetArrayIndex", "uint"}}, {"viewport_array_index", {"SV_ViewportArrayIndex", "uint"}}};
    auto sem = [&](bool in, int reg, const Reg& r) -> std::pair<std::string, std::string> {   // semantic, type
        for (auto& e : in ? refl.inputs : refl.outputs)
            if ((int)e.reg == reg) {
                std::string s = e.semantic;
                if (!IStartsWith(s, "SV_") || e.index) s += std::to_string(e.index);
                static const char* scalars[] = {"SV_RenderTargetArrayIndex", "SV_ViewportArrayIndex", "SV_VertexID", "SV_InstanceID",
                                                "SV_PrimitiveID", "SV_SampleIndex", "SV_Coverage", "SV_GSInstanceID"};
                for (const char* sc : scalars)
                    if (IEquals(e.semantic, sc)) return {s, e.type == "int" ? "int" : "uint"};
                if (IEquals(e.semantic, "SV_IsFrontFace")) return {s, "bool"};
                return {s, e.type == "float" ? "float4" : e.type == "int" ? "int4" : "uint4"};
            }
        if (auto it = svNames.find(r.sv); it != svNames.end()) return {it->second.first, it->second.second};
        if (in) return {Fmt(isVs ? "ATTRIBUTE%d" : "TEXCOORD%d", reg), "float4"};
        return {Fmt(isPs ? "SV_Target%d" : "TEXCOORD%d", reg), "float4"};
    };
    std::string gsIn, gsOutStruct, outInit;
    for (auto& [reg, r] : ins) {
        auto [s, ty] = sem(true, reg, r);
        std::string name = "v" + std::to_string(reg);
        t.names[name] = name;
        if (ty[0] != 'f') t.uintInputs.insert(name);
        if (isGs) gsIn += Fmt("    %s %s : %s;\n", ty.c_str(), name.c_str(), s.c_str());
        else params += Fmt("    %s %s : %s,\n", ty.c_str(), name.c_str(), s.c_str());
    }
    for (auto& [reg, r] : outs) {
        auto [s, ty] = sem(false, reg, r);
        std::string name = "o" + std::to_string(reg);
        t.names[name] = name;
        if (isGs) {
            gsOutStruct += Fmt("    %s %s : %s;\n", ty.c_str(), name.c_str(), s.c_str());
            outInit += Fmt("    %s %s = 0;\n", ty.c_str(), name.c_str());
            t.gsOut.push_back(name);
        } else {
            params += Fmt("    out %s %s : %s,\n", ty.c_str(), name.c_str(), s.c_str());
            outInit += "    " + name + " = 0;\n";
        }
    }
    if (isGs) {
        static const std::regex prim(R"(dcl_inputprimitive (\w+))"), topo(R"(dcl_outputtopology (\w+))");
        std::smatch pm, tm;
        std::string pr = std::regex_search(asmText, pm, prim) ? pm[1].str() : "triangle";
        std::string tp = std::regex_search(asmText, tm, topo) ? tm[1].str() : "trianglestrip";
        const char* primKw = pr == "point" ? "point" : pr == "line" ? "line" : pr == "lineadj" ? "lineadj" : pr == "triangleadj" ? "triangleadj" : "triangle";
        const char* streamTy = tp == "pointlist" ? "PointStream" : tp == "linestrip" ? "LineStream" : "TriangleStream";
        decl += "\nstruct GSIn\n{\n" + gsIn + "};\nstruct GSOut\n{\n" + gsOutStruct + "};\n";
        params = Fmt("    %s GSIn input[%d],\n    inout %s<GSOut> stream,\n", primKw, gsVerts ? gsVerts : 3, streamTy) + params;
    }
    if (asmText.find("oDepth") != std::string::npos) {
        t.names["oDepth"] = "oDepth";
        params += "    out float oDepth : SV_Depth,\n";
        outInit += "    oDepth = 0;\n";
    }
    for (const char* sv : {"vThreadID", "vThreadGroupID", "vThreadIDInGroupFlattened", "vThreadIDInGroup"}) {
        static const std::regex word(R"(\b(vThreadID|vThreadGroupID|vThreadIDInGroupFlattened|vThreadIDInGroup)\b)");
        (void)word;
        if (asmText.find(std::string("dcl_input ") + sv) == std::string::npos) continue;
        int w = t.tid.count(sv) ? t.tid[sv] : 1;
        std::string svs = std::string(sv) == "vThreadID" ? "SV_DispatchThreadID" : std::string(sv) == "vThreadGroupID" ? "SV_GroupID"
                        : std::string(sv) == "vThreadIDInGroup" ? "SV_GroupThreadID" : "SV_GroupIndex";
        params += Fmt("    uint%s %s : %s,\n", w > 1 ? std::to_string(w).c_str() : "", sv, svs.c_str());
    }
    if (!params.empty()) params.resize(params.size() - 2);

    // body
    int failed = 0;
    std::vector<std::string> asmLines;
    {
        size_t p = 0;
        while (p <= asmText.size()) {
            size_t e = asmText.find('\n', p);
            if (e == std::string::npos) e = asmText.size();
            asmLines.push_back(asmText.substr(p, e - p));
            p = e + 1;
        }
    }
    for (auto& l : asmLines) {
        size_t before = t.lines.size();
        t.lineNotes.clear();
        try {
            t.Line(l);
        } catch (const std::exception& e) {
            t.lines.resize(before);
            t.Emit("// untranslated: " + Trim(l) + "   (" + e.what() + ")");
            failed++;
        }
        if (!t.lineNotes.empty() && t.lines.size() > before) {
            std::string note = "   // ";
            size_t k = 0;
            for (auto& n : t.lineNotes) note += (k++ ? ", " : "") + n;
            t.lines.back() += note;
        }
    }
    std::string temps;
    if (t.maxr >= 0) {
        temps = bitExact ? "    uint4 " : "    float4 ";
        for (int i = 0; i <= t.maxr; i++) temps += Fmt("%sr%d = 0", i ? ", " : "", i);
        temps += ";\n";
    }
    static const std::regex dclx(R"(dcl_indexableTemp x(\d+)\[(\d+)\], \d+)");
    for (std::sregex_iterator it(asmText.begin(), asmText.end(), dclx), end; it != end; ++it)
        temps += Fmt("    %s x%s[%s];\n", bitExact ? "uint4" : "float4", (*it)[1].str().c_str(), (*it)[2].str().c_str());

    temps += outInit;
    std::string attrs;
    static const std::regex maxout(R"(dcl_maxout (\d+))");
    std::smatch mo;
    if (std::regex_search(asmText, mo, maxout)) attrs += "[maxvertexcount(" + mo[1].str() + ")]\n";
    static const std::regex threads(R"(dcl_thread_group (\d+), (\d+), (\d+))");
    std::smatch tg;
    if (std::regex_search(asmText, tg, threads)) attrs += Fmt("[numthreads(%s, %s, %s)]\n", tg[1].str().c_str(), tg[2].str().c_str(), tg[3].str().c_str());
    hlsl = decl + "\n" + attrs + "void main(\n" + params + ")\n{\n" + temps;
    for (auto& l : t.lines) hlsl += l + "\n";
    hlsl += "}\n";
    if (failed) hlsl = Fmt("// %d instruction(s) could not be translated (kept as comments)\n", failed) + hlsl;
    if (!hasRefl) hlsl = "// no reflection data in this shader: registers keep their numbers\n" + hlsl;
    return true;
}
