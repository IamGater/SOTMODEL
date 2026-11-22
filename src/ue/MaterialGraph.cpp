// Material graph -> HLSL (port of the Athena WorldPipeline ue_material_hlsl.py translator, emitting UE 4.10 names).
#include "ue/MaterialGraph.h"
#include "ue/AssetJson.h"
#include "ue/Material.h"
#include "core/JsonValue.h"
#include <map>
#include <set>
#include <cmath>
#include <array>
#include <regex>
#include <optional>
#include <windows.h>
#include <process.h>

namespace {

const char* kSwz = "xyzw";

std::string Num(double v) {
    if (std::isnan(v) || std::isinf(v)) return "0.0";
    std::string s = Fmt("%.7g", v);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string Ident(const std::string& s) {
    std::string r;
    for (char c : s) r += isalnum((u8)c) ? c : '_';
    while (r.find("__") != std::string::npos) r.erase(r.find("__"), 1);
    if (r.empty() || isdigit((u8)r[0])) r = "_" + r;
    return r;
}

// Global parameter names: HLSL keywords / intrinsics / prelude globals a parameter may be named after get a _
std::string ParamName(const std::string& s) {
    std::string r = Ident(s);
    static const char* const reserved[] = {"abs", "min", "max", "clamp", "lerp", "saturate", "pow", "exp", "log", "sin", "cos", "tan",
                                           "dot", "cross", "length", "normalize", "frac", "floor", "ceil", "round", "sign", "step",
                                           "smoothstep", "distance", "reflect", "sqrt", "rsqrt", "fmod", "mul", "float", "int", "bool",
                                           "half", "uint", "true", "false", "in", "out", "inout", "return", "if", "else", "for", "while",
                                           "static", "const", "struct", "Texture", "Texture2D", "TextureCube", "SamplerState", "View",
                                           "Primitive", "Parameters", "Material", "Rope", "register", "sampler", "texture", "matrix", "vector"};
    for (auto w : reserved) if (r == w) return r + "_";
    return r;
}

std::string Ty(int n) { return n <= 1 ? "float" : Fmt("float%d", n); }

// "MaterialExpressionAdd'MaterialExpressionAdd_3'" -> MaterialExpressionAdd_3
std::string RefName(const JsonValue* r) {
    if (!r) return {};
    std::string on = r->GetStr("ObjectName");
    size_t a = on.find('\''), b = on.rfind('\'');
    std::string s = a != std::string::npos && b > a ? on.substr(a + 1, b - a - 1) : on;
    if (size_t c = s.rfind(':'); c != std::string::npos) s = s.substr(c + 1);
    if (size_t d = s.rfind('.'); d != std::string::npos) s = s.substr(d + 1);
    return s;
}

std::string ObjPath(const JsonValue* r) { return r ? r->GetStr("ObjectPath") : std::string(); }

struct Graph {
    std::string name, path;
    JsonValue doc;
    std::map<std::string, const JsonValue*> by;   // export name -> export object
    const JsonValue* root = nullptr;               // Material / MaterialFunction export
    bool isFunction = false;
};

struct Pin {
    const Graph* g = nullptr;
    int ctx = 0;
    const JsonValue* pin = nullptr;
};

struct Val {
    enum Kind { Float, Static, Tex, Attrs } kind = Float;
    std::string code;
    int n = 1;
    bool sv = false;                      // static value
    std::string texType;                  // Tex: Texture2D / TextureCube
    std::map<std::string, Pin> attrs;     // Attrs
    static Val F(std::string c, int n) { Val v; v.code = std::move(c); v.n = n; return v; }
};

struct Ctx {
    std::string id;
    std::map<std::string, Pin> args;   // function inputs by name and by id
};

const std::vector<std::string>& Attrs() {
    static const std::vector<std::string> a = {"BaseColor", "Metallic", "Specular", "Roughness", "EmissiveColor", "Opacity", "OpacityMask", "Normal",
                                               "WorldPositionOffset", "WorldDisplacement", "TessellationMultiplier", "SubsurfaceColor", "ClearCoat",
                                               "ClearCoatRoughness", "AmbientOcclusion", "Refraction", "CustomizedUVs0", "CustomizedUVs1", "CustomizedUVs2",
                                               "CustomizedUVs3", "CustomizedUVs4", "CustomizedUVs5", "CustomizedUVs6", "CustomizedUVs7", "PixelDepthOffset"};
    return a;
}
int AttrSize(const std::string& a) {
    if (a == "BaseColor" || a == "EmissiveColor" || a == "Normal" || a == "WorldPositionOffset" || a == "WorldDisplacement" || a == "SubsurfaceColor") return 3;
    if (a == "Refraction" || a.rfind("CustomizedUVs", 0) == 0) return 2;
    return 1;
}
// UE 4.10 FMaterialAttributeDefinitionMap defaults
std::string AttrDefault(const std::string& a) {
    static const std::map<std::string, std::string> d = {
        {"BaseColor", "float3(0.0, 0.0, 0.0)"}, {"Metallic", "0.0"}, {"Specular", "0.5"}, {"Roughness", "0.5"}, {"EmissiveColor", "float3(0.0, 0.0, 0.0)"},
        {"Opacity", "1.0"}, {"OpacityMask", "1.0"}, {"Normal", "float3(0.0, 0.0, 1.0)"}, {"WorldPositionOffset", "float3(0.0, 0.0, 0.0)"},
        {"WorldDisplacement", "float3(0.0, 0.0, 0.0)"}, {"TessellationMultiplier", "1.0"}, {"SubsurfaceColor", "float3(1.0, 1.0, 1.0)"},
        {"ClearCoat", "1.0"}, {"ClearCoatRoughness", "0.1"}, {"AmbientOcclusion", "1.0"}, {"Refraction", "float2(1.0, 0.0)"}, {"PixelDepthOffset", "0.0"}};
    auto it = d.find(a);
    if (it != d.end()) return it->second;
    return Fmt("Parameters.TexCoords[%c].xy", a.back());   // an unconnected customized UV passes the texture coordinate through
}
bool IsVertexAttr(const std::string& a) {
    return a == "WorldPositionOffset" || a == "WorldDisplacement" || a == "TessellationMultiplier" || a.rfind("CustomizedUVs", 0) == 0;
}
// Sea of Thieves' BreakMaterialAttributes output order (see ue_material_hlsl.py): UE's first 16, two SoT outputs, CustomizedUVs 0-7
std::string BreakOutput(int o) {
    if (o < 16) return Attrs()[o];
    if (o >= 18 && o < 26) return Fmt("CustomizedUVs%d", o - 18);
    return {};
}

struct Param {
    std::string kind, name, ident, decl, comment;
};

class Translator {
public:
    Translator(PackageCache& cache) : m_cache(cache) {}

    PackageCache& m_cache;
    std::map<std::string, std::unique_ptr<Graph>> graphs;
    std::map<std::string, bool> statics;
    std::map<std::string, float> scalarOverride;
    std::map<std::string, std::array<float, 4>> vectorOverride;
    std::map<std::string, std::string> textureOverride;
    std::vector<Param> params;
    std::map<std::string, size_t> paramIndex;   // kind|name
    std::map<std::string, std::string> customs; // function name -> definition
    std::vector<Ctx> ctxs;
    std::map<std::string, int> callSites;
    std::set<std::string> unsupported;
    int functionCalls = 0, nodeCount = 0;

    // per stage
    bool vertex = false;
    std::vector<std::string> lines;
    std::map<std::string, Val> memo;
    int counter = 0;

    Graph* LoadGraph(const std::string& path, std::string* error) {
        std::string key = ToLower(path);
        auto it = graphs.find(key);
        if (it != graphs.end()) return it->second.get();
        auto pkg = m_cache.LoadPath(path, error);
        if (!pkg) return nullptr;
        auto g = std::make_unique<Graph>();
        g->path = pkg->PackageName();
        g->name = pkg->ShortName();
        std::string json = PackageToJson(*pkg);
        if (!JsonValue::Parse(json, g->doc, error)) return nullptr;
        for (auto& o : g->doc.arr) {
            std::string type = o.GetStr("Type"), name = o.GetStr("Name");
            if (!name.empty()) g->by[name] = &o;
            if (!g->root && (type == "Material" || type == "MaterialFunction")) {
                g->root = &o;
                g->isFunction = type == "MaterialFunction";
                g->name = name;
            }
        }
        if (!g->root) {
            if (error) *error = path + " has no Material / MaterialFunction export";
            return nullptr;
        }
        Graph* raw = g.get();
        graphs[key] = std::move(g);
        return raw;
    }

    // ---- helpers
    static const JsonValue* P(const JsonValue* props, const char* k) { return props ? props->Get(k) : nullptr; }
    static double NumOf(const JsonValue* props, const char* k, double def) {
        const JsonValue* v = P(props, k);
        return v && v->type == JsonValue::Type::Number ? v->num : v && v->type == JsonValue::Type::Bool ? (v->b ? 1 : 0) : def;
    }
    static bool BoolOf(const JsonValue* props, const char* k, bool def) {
        const JsonValue* v = P(props, k);
        return v ? (v->type == JsonValue::Type::Bool ? v->b : v->num != 0) : def;
    }
    static std::string StrOf(const JsonValue* props, const char* k, const std::string& def = {}) {
        const JsonValue* v = P(props, k);
        return v && v->type == JsonValue::Type::String ? v->str : def;
    }

    std::string Cast(const Val& v, int n) {
        if (v.kind != Val::Float) return "0.0 /* not a value */";
        if (v.n == n) return v.code;
        if (v.n == 1) {
            if (n == 1) return v.code;
            std::string r = Ty(n) + "(";
            for (int i = 0; i < n; i++) r += (i ? ", " : "") + v.code;
            return r + ")";
        }
        if (n < v.n) return "(" + v.code + ")." + std::string(kSwz, n);
        std::string r = Ty(n) + "(" + v.code;
        for (int i = v.n; i < n; i++) r += ", 0.0";
        return r + ")";
    }

    Val Tmp(const std::string& code, int n, const std::string& comment = {}) {
        std::string name = Fmt("Local%d", counter++);
        lines.push_back("    " + Ty(n) + " " + name + " = " + code + ";" + (comment.empty() ? "" : "   // " + comment));
        return Val::F(name, n);
    }

    std::string ParamIdent(const std::string& kind, const std::string& name, const std::string& declType, const std::string& init, const std::string& comment) {
        std::string key = kind + "|" + name;
        auto it = paramIndex.find(key);
        if (it != paramIndex.end()) return params[it->second].ident;
        std::string prefix = kind == "texture" ? "Tex_" : kind == "landscape" ? "LayerWeight_" : "";
        std::string id = prefix + ParamName(name);
        while (std::any_of(params.begin(), params.end(), [&](const Param& p) { return p.ident == id; })) id += "_";
        Param p{kind, name, id, declType + " " + id + (init.empty() ? "" : " = " + init) + ";", comment};
        paramIndex[key] = params.size();
        params.push_back(p);
        return id;
    }

    // ---- evaluation
    Val Input(const Pin& pin, const Val* def = nullptr) {
        const JsonValue* in = pin.pin;
        const JsonValue* expr = in ? in->Get("Expression") : nullptr;
        if (!expr || expr->IsNull() || RefName(expr).empty() || RefName(expr) == "None") {
            if (def) return *def;
            Val none;
            none.n = 0;
            return none;
        }
        Val v = Node(pin.g, pin.ctx, RefName(expr), (int)in->GetNum("OutputIndex", 0));
        if (BoolOf(in, "Mask", false) && v.kind == Val::Float) {
            std::string m;
            const char* keys[] = {"MaskR", "MaskG", "MaskB", "MaskA"};
            for (int i = 0; i < 4; i++)
                if (BoolOf(in, keys[i], false)) m += kSwz[i];
            // the editor keeps stale masks on pins fed by scalars (RGB on a scalar output): a scalar stays a scalar
            if (!m.empty() && v.n > 1 && m != std::string(kSwz, v.n)) {
                bool ok = true;
                for (char c : m) ok = ok && (int)(strchr(kSwz, c) - kSwz) < v.n;
                if (ok) v = Val::F("(" + v.code + ")." + m, (int)m.size());
            }
        }
        return v;
    }
    static bool Has(const Val& v) { return v.n > 0 || v.kind != Val::Float; }

    // an input of the node's properties, or a constant fallback
    Val A(const Graph* g, int c, const JsonValue* p, const char* k, std::optional<double> konst = std::nullopt) {
        Val v = Input({g, c, P(p, k)});
        if (!Has(v) && konst) return Val::F(Num(*konst), 1);
        return v;
    }

    Val Unsupported(const std::string& what) {
        unsupported.insert(what);
        return Val::F("0.0 /* " + what + " */", 1);
    }

    Val Node(const Graph* g, int c, const std::string& name, int out) {
        std::string key = g->path + "|" + std::to_string(c) + "|" + name + "|" + std::to_string(out);
        auto mit = memo.find(key);
        if (mit != memo.end()) return mit->second;
        auto it = g->by.find(name);
        if (it == g->by.end()) return Unsupported("missing node " + name);
        const JsonValue* o = it->second;
        std::string t = o->GetStr("Type");
        if (t.rfind("MaterialExpression", 0) == 0) t = t.substr(18);
        const JsonValue* props = o->Get("Properties");
        memo[key] = Val::F("0.0 /* cycle */", 1);
        nodeCount++;
        Val v = Eval(g, c, t, props, out);
        // compound expressions become locals so later uses share them (like UE's code chunks)
        if (v.kind == Val::Float && v.n > 0) {
            bool simple = true;
            for (char ch : v.code) simple = simple && (isalnum((u8)ch) || ch == '_' || ch == '.' || ch == '-' || ch == '[' || ch == ']');
            // constant vectors stay inline too: float3(0.0, 0.0, 1.0)
            static const std::regex constVec(R"(^float[234]\((?:-?[0-9.eE+-]+(?:, )?)+\)$)");
            if (!simple && std::regex_match(v.code, constVec)) simple = true;
            if (!simple) v = Tmp(v.code, v.n, t + (g->isFunction ? " (" + g->name + ")" : ""));
        }
        memo[key] = v;
        return v;
    }

    // HLSL promotes a scalar operand by itself: only vectors of different widths need a cast
    std::string Arg(const Val& v, int n) { return v.n == 1 && v.kind == Val::Float ? v.code : Cast(v, n); }
    Val BinOp(const char* op, const Val& x, const Val& y) {
        int n = std::max(x.n, y.n);
        if (x.n != y.n && x.n != 1 && y.n != 1) n = std::min(x.n, y.n);   // UE rejects this; keep it readable
        return Val::F("(" + Arg(x, n) + " " + op + " " + Arg(y, n) + ")", n);
    }
    Val Fn2(const char* f, const Val& x, const Val& y) {
        int n = std::max(x.n, y.n);
        return Val::F(std::string(f) + "(" + Arg(x, n) + ", " + Arg(y, n) + ")", n);
    }
    Val Unary(const char* f, const Val& x) { return Val::F(std::string(f) + "(" + x.code + ")", x.n); }
    Val Channel(const Val& s, int out) {   // texture / vector outputs: RGB, R, G, B, A
        if (out == 0) return Val::F(s.code + ".rgb", 3);
        if (out <= 4) return Val::F(s.code + "." + std::string(1, "rgba"[out - 1]), 1);
        return Val::F(s.code, 4);
    }

    bool StaticParam(const JsonValue* p) {
        std::string n = StrOf(p, "ParameterName");
        auto it = statics.find(n);
        return it != statics.end() ? it->second : BoolOf(p, "DefaultValue", false);
    }

    Val TexCoord(int index, double u = 1, double v = 1) {
        std::string code = Fmt("Parameters.TexCoords[%d].xy", index);
        if (u != 1 || v != 1) code = "(" + code + " * float2(" + Num(u) + ", " + Num(v) + "))";
        return Val::F(code, 2);
    }

    Val TexObject(const JsonValue* p, const std::string& type = "Texture2D") {
        std::string name = StrOf(p, "ParameterName");
        std::string path = ObjPath(P(p, "Texture"));
        if (!name.empty() && textureOverride.count(name)) path = textureOverride[name];
        std::string sampler = StrOf(p, "SamplerType", "SAMPLERTYPE_Color");
        std::string label = name.empty() ? (path.empty() ? std::string("Texture") : path.substr(path.rfind('/') + 1)) : name;
        if (size_t d = label.find('.'); d != std::string::npos) label = label.substr(0, d);
        std::string id = ParamIdent("texture", name.empty() ? "__" + path : name, type, "",
                                    (name.empty() ? "texture " : "TextureParameter \"" + name + "\" ") + path + " (" + sampler.substr(sampler.find('_') + 1) + ")");
        // reuse readable identifiers for constant textures
        if (name.empty()) {
            Param& pr = params[paramIndex["texture|__" + path]];
            if (pr.ident.rfind("Tex___", 0) == 0) {
                std::string nid = "Tex_" + Ident(label);
                while (std::any_of(params.begin(), params.end(), [&](const Param& q) { return q.ident == nid; })) nid += "_";
                pr.decl = type + " " + nid + ";";
                pr.ident = nid;
                id = nid;
            }
        }
        Val v;
        v.kind = Val::Tex;
        v.code = id;
        v.texType = sampler;
        return v;
    }

    Val TexSample(const Graph* g, int c, const JsonValue* p, int out, const Val& obj, std::optional<Val> uvIn = std::nullopt, bool full = false) {
        Val uv = uvIn ? *uvIn : A(g, c, p, "Coordinates");
        if (!Has(uv)) uv = TexCoord((int)NumOf(p, "ConstCoordinate", 0));
        std::string mv = StrOf(p, "MipValueMode", "TMVM_None");
        Val mip = A(g, c, p, "MipValue");
        if (!Has(mip) && NumOf(p, "ConstMipValue", -1) != -1) mip = Val::F(Num(NumOf(p, "ConstMipValue", 0)), 1);
        std::string mipc = Has(mip) ? Cast(mip, 1) : "0.0";
        if (vertex && mv == "TMVM_None") mv = "TMVM_MipLevel";   // no derivatives outside the pixel shader
        std::string src = StrOf(p, "SamplerSource", "SSM_FromTextureAsset");
        std::string smp = src == "SSM_Wrap_WorldGroupSettings" ? "Material.Wrap_WorldGroupSettings" : src == "SSM_Clamp_WorldGroupSettings" ? "Material.Clamp_WorldGroupSettings"
                                                                                                                                              : obj.code + "Sampler";
        bool cube = obj.texType == "Cube";
        std::string uvc = Cast(uv, cube ? 3 : 2), code;
        std::string fn = cube ? "TextureCubeSample" : "Texture2DSample";
        if (mv == "TMVM_MipLevel") code = fn + "Level(" + obj.code + ", " + smp + ", " + uvc + ", " + mipc + ")";
        else if (mv == "TMVM_MipBias") code = fn + "Bias(" + obj.code + ", " + smp + ", " + uvc + ", " + mipc + ")";
        else code = fn + "(" + obj.code + ", " + smp + ", " + uvc + ")";
        std::string st = StrOf(p, "SamplerType", obj.texType.empty() ? "SAMPLERTYPE_Color" : obj.texType);
        if (st.find("Normal") != std::string::npos) code = "UnpackNormalMap(" + code + ")";
        else if (st.find("Grayscale") != std::string::npos || st.find("Alpha") != std::string::npos) code = "ProcessMaterialGreyscaleTextureLookup(" + code + ")";
        else if (st.find("LinearColor") == std::string::npos && st.find("Masks") == std::string::npos && st.find("Color") != std::string::npos) code = "ProcessMaterialColorTextureLookup(" + code + ")";
        else if (st.find("LinearColor") != std::string::npos) code = "ProcessMaterialLinearColorTextureLookup(" + code + ")";
        Val s = Tmp(code, 4, "texture sample");
        return full ? s : Channel(s, out);
    }

    Val FunctionCall(const Graph* g, int c, const std::string& nodeName, const JsonValue* p, int out) {
        std::string err;
        std::string fpath = ObjPath(P(p, "MaterialFunction"));
        if (fpath.empty()) return Unsupported("function call without a function");
        Graph* fg = LoadGraph(fpath.substr(0, fpath.find('.')), &err);
        if (!fg) return Unsupported("function " + fpath + ": " + err);
        std::string site = g->path + "|" + std::to_string(c) + "|" + nodeName;
        int ctx;
        auto sit = callSites.find(site);
        if (sit != callSites.end()) ctx = sit->second;
        else {
            Ctx cx;
            cx.id = fg->name + Fmt("#%zu", ctxs.size());
            if (const JsonValue* ins = P(p, "FunctionInputs"))
                for (auto& fi : ins->arr) {
                    const JsonValue* in = fi.Get("Input");
                    Pin pin{g, c, in};
                    if (in) cx.args[in->GetStr("InputName")] = pin;
                    std::string id = fi.GetStr("ExpressionInputId");
                    if (!id.empty()) cx.args["#" + id] = pin;
                }
            ctx = (int)ctxs.size();
            ctxs.push_back(cx);
            callSites[site] = ctx;
            functionCalls++;
            lines.push_back("    // ---- " + fg->name + " (" + fg->path + ")");
        }
        // which FunctionOutput: by output id, then by name, then the only one
        std::string oid, want;
        if (const JsonValue* outs = P(p, "FunctionOutputs"); outs && out < (int)outs->arr.size()) {
            oid = outs->arr[out].GetStr("ExpressionOutputId");
            if (const JsonValue* o = outs->arr[out].Get("Output")) want = o->GetStr("OutputName");
        }
        std::vector<const JsonValue*> fo;
        for (auto& [n, o] : fg->by)
            if (o->GetStr("Type") == "MaterialExpressionFunctionOutput") fo.push_back(o);
        const JsonValue* tgt = nullptr;
        for (auto* o : fo) if (!oid.empty() && StrOf(o->Get("Properties"), "Id") == oid) tgt = o;
        for (auto* o : fo) if (!tgt && !want.empty() && StrOf(o->Get("Properties"), "OutputName") == want) tgt = o;
        if (!tgt && fo.size() == 1) tgt = fo[0];
        if (!tgt) return Unsupported("output " + want + " of " + fg->name);
        return Node(fg, ctx, tgt->GetStr("Name"), 0);
    }

    Val Eval(const Graph* g, int c, const std::string& t, const JsonValue* p, int o);
};

Val Translator::Eval(const Graph* g, int c, const std::string& t, const JsonValue* p, int o) {
    auto ab = [&](double da, double db) { return std::make_pair(A(g, c, p, "A", NumOf(p, "ConstA", da)), A(g, c, p, "B", NumOf(p, "ConstB", db))); };
    // constants & parameters
    if (t == "Constant") return Val::F(Num(NumOf(p, "R", 0)), 1);
    if (t == "Constant2Vector") return Val::F("float2(" + Num(NumOf(p, "R", 0)) + ", " + Num(NumOf(p, "G", 0)) + ")", 2);
    if (t == "Constant3Vector" || t == "Constant4Vector") {
        const JsonValue* k = P(p, "Constant");
        int n = t[8] - '0';
        std::string s = Ty(n) + "(";
        for (int i = 0; i < n; i++) s += (i ? ", " : "") + Num(NumOf(k, i == 0 ? "R" : i == 1 ? "G" : i == 2 ? "B" : "A", 0));
        return Val::F(s + ")", n);
    }
    if (t == "ScalarParameter") {
        std::string name = StrOf(p, "ParameterName", "None");
        double v = scalarOverride.count(name) ? scalarOverride[name] : NumOf(p, "DefaultValue", 0);
        return Val::F(ParamIdent("scalar", name, "float", Num(v), "ScalarParameter \"" + name + "\"" + (scalarOverride.count(name) ? " (instance value)" : "")), 1);
    }
    if (t == "VectorParameter") {
        std::string name = StrOf(p, "ParameterName", "None");
        const JsonValue* d = P(p, "DefaultValue");
        std::array<float, 4> v = {(float)NumOf(d, "R", 0), (float)NumOf(d, "G", 0), (float)NumOf(d, "B", 0), (float)NumOf(d, "A", 0)};
        if (vectorOverride.count(name)) v = vectorOverride[name];
        std::string id = ParamIdent("vector", name, "float4", "float4(" + Num(v[0]) + ", " + Num(v[1]) + ", " + Num(v[2]) + ", " + Num(v[3]) + ")",
                                    "VectorParameter \"" + name + "\"" + (vectorOverride.count(name) ? " (instance value)" : ""));
        return o == 0 ? Val::F(id + ".rgb", 3) : Val::F(id + "." + std::string(1, kSwz[std::min(o - 1, 3)]), 1);
    }
    if (t == "StaticBool" || t == "StaticBoolParameter") {
        Val v;
        v.kind = Val::Static;
        v.sv = t == "StaticBool" ? BoolOf(p, "Value", false) : StaticParam(p);
        v.code = v.sv ? "true" : "false";
        return v;
    }
    if (t == "StaticSwitchParameter") {
        bool on = StaticParam(p);
        lines.push_back("    // static switch \"" + StrOf(p, "ParameterName") + "\" = " + (on ? "true" : "false"));
        Val r = Input({g, c, P(p, on ? "A" : "B")});
        return Has(r) ? r : Val::F("0.0", 1);
    }
    if (t == "StaticSwitch") {
        Val sv = Input({g, c, P(p, "Value")});
        bool on = sv.kind == Val::Static ? sv.sv : BoolOf(p, "DefaultValue", false);
        Val r = Input({g, c, P(p, on ? "A" : "B")});
        return Has(r) ? r : Val::F("0.0", 1);
    }
    if (t == "StaticComponentMaskParameter") {
        Val x = A(g, c, p, "Input");
        std::string m;
        const char* keys[] = {"DefaultR", "DefaultG", "DefaultB", "DefaultA"};
        for (int i = 0; i < 4; i++) if (BoolOf(p, keys[i], false)) m += kSwz[i];
        if (m.empty()) m = "x";
        lines.push_back("    // static component mask \"" + StrOf(p, "ParameterName") + "\" = " + m);
        return x.n == 1 ? Val::F(Cast(x, (int)m.size()), (int)m.size()) : Val::F("(" + x.code + ")." + m, (int)m.size());
    }
    if (t == "QualitySwitch") {
        const JsonValue* ins = P(p, "Inputs");
        if (ins && ins->arr.size() > 2) {
            Val hi = Input({g, c, &ins->arr[2]});
            if (Has(hi)) return hi;   // the PC client renders High quality
        }
        return A(g, c, p, "Default");
    }
    if (t == "FeatureLevelSwitch") {
        const JsonValue* ins = P(p, "Inputs");
        if (ins && ins->arr.size() > 3) {
            Val sm5 = Input({g, c, &ins->arr[3]});
            if (Has(sm5)) return sm5;
        }
        return A(g, c, p, "Default");
    }
    // math
    if (t == "Add") { auto [x, y] = ab(0, 1); return BinOp("+", x, y); }
    if (t == "Subtract") { auto [x, y] = ab(1, 1); return BinOp("-", x, y); }
    if (t == "Multiply") { auto [x, y] = ab(0, 1); return BinOp("*", x, y); }
    if (t == "Divide") { auto [x, y] = ab(1, 2); return BinOp("/", x, y); }
    if (t == "Min") { auto [x, y] = ab(0, 1); return Fn2("min", x, y); }
    if (t == "Max") { auto [x, y] = ab(0, 1); return Fn2("max", x, y); }
    if (t == "Fmod") { auto [x, y] = ab(0, 1); return Fn2("fmod", x, y); }
    if (t == "Power") {
        Val b = A(g, c, p, "Base"), e = A(g, c, p, "Exponent", NumOf(p, "ConstExponent", 2));
        return Val::F("ClampedPow(" + b.code + ", " + Cast(e, b.n) + ")", b.n);
    }
    if (t == "LinearInterpolate") {
        Val x = A(g, c, p, "A", NumOf(p, "ConstA", 0)), y = A(g, c, p, "B", NumOf(p, "ConstB", 1)), a = A(g, c, p, "Alpha", NumOf(p, "ConstAlpha", 0.5));
        int n = std::max({x.n, y.n, a.n});
        return Val::F("lerp(" + Cast(x, n) + ", " + Cast(y, n) + ", " + Arg(a, n) + ")", n);
    }
    if (t == "Clamp") {
        Val x = A(g, c, p, "Input"), lo = A(g, c, p, "Min", NumOf(p, "MinDefault", 0)), hi = A(g, c, p, "Max", NumOf(p, "MaxDefault", 1));
        std::string mode = StrOf(p, "ClampMode", "CMODE_Clamp");
        if (mode == "CMODE_ClampMin") return Val::F("max(" + x.code + ", " + Arg(lo, x.n) + ")", x.n);
        if (mode == "CMODE_ClampMax") return Val::F("min(" + x.code + ", " + Arg(hi, x.n) + ")", x.n);
        return Val::F("clamp(" + x.code + ", " + Arg(lo, x.n) + ", " + Arg(hi, x.n) + ")", x.n);
    }
    if (t == "OneMinus") { Val x = A(g, c, p, "Input"); return Val::F("(1.0 - " + x.code + ")", x.n); }
    static const std::map<std::string, const char*> unary = {{"Abs", "abs"}, {"Floor", "floor"}, {"Ceil", "ceil"}, {"Frac", "frac"}, {"SquareRoot", "sqrt"},
                                                              {"Saturate", "saturate"}, {"Round", "round"}, {"Truncate", "trunc"}, {"Sign", "sign"},
                                                              {"Arcsine", "asin"}, {"Arccosine", "acos"}, {"Arctangent", "atan"}, {"Tangent", "tan"},
                                                              {"Logarithm2", "log2"}, {"Exponential", "exp"}};
    if (auto it = unary.find(t); it != unary.end()) return Unary(it->second, A(g, c, p, "Input"));
    if (t == "DDX" || t == "DDY") return Unary(t == "DDX" ? "ddx" : "ddy", A(g, c, p, "Value"));
    if (t == "Sine" || t == "Cosine") {
        Val x = A(g, c, p, "Input");
        double per = NumOf(p, "Period", 1);
        std::string f = t == "Sine" ? "sin" : "cos";
        return Val::F(per <= 0 ? f + "(" + x.code + ")" : f + "(" + x.code + " * " + Num(6.283185307179586 / per) + ")", x.n);
    }
    if (t == "Arctangent2") { auto [y, x] = std::make_pair(A(g, c, p, "Y"), A(g, c, p, "X")); return Fn2("atan2", y, x); }
    if (t == "Normalize") return Unary("normalize", A(g, c, p, "VectorInput"));
    if (t == "DotProduct") { Val x = A(g, c, p, "A"), y = A(g, c, p, "B"); int n = std::max(x.n, y.n); return Val::F("dot(" + Cast(x, n) + ", " + Cast(y, n) + ")", 1); }
    if (t == "CrossProduct") return Val::F("cross(" + Cast(A(g, c, p, "A"), 3) + ", " + Cast(A(g, c, p, "B"), 3) + ")", 3);
    if (t == "Distance") { Val x = A(g, c, p, "A"), y = A(g, c, p, "B"); int n = std::max(x.n, y.n); return Val::F("distance(" + Cast(x, n) + ", " + Cast(y, n) + ")", 1); }
    if (t == "ComponentMask") {
        Val x = A(g, c, p, "Input");
        std::string m;
        const char* keys[] = {"R", "G", "B", "A"};
        for (int i = 0; i < 4; i++) if (BoolOf(p, keys[i], false)) m += kSwz[i];
        if (m.empty()) m = "x";
        if (x.n == 1) return Val::F(Cast(x, (int)m.size()), (int)m.size());
        return Val::F("(" + x.code + ")." + m, (int)m.size());
    }
    if (t == "AppendVector") {
        Val x = A(g, c, p, "A"), y = A(g, c, p, "B");
        int n = std::min(4, x.n + y.n);
        if (x.n >= 4) return Val::F(x.code, x.n);   // nothing fits (UE reports an error; keep A)
        // more than 4 components (UE reports an error): keep what fits so the shader still compiles
        std::string b = x.n + y.n > 4 ? "(" + y.code + ")." + std::string("xyzw").substr(0, 4 - x.n) : y.code;
        return Val::F(Ty(n) + "(" + x.code + ", " + b + ")", n);
    }
    if (t == "ConstantBiasScale") {
        Val x = A(g, c, p, "Input");
        return Val::F("((" + x.code + " + " + Num(NumOf(p, "Bias", 1)) + ") * " + Num(NumOf(p, "Scale", 0.5)) + ")", x.n);
    }
    if (t == "If") {
        Val a = A(g, c, p, "A"), b = A(g, c, p, "B", NumOf(p, "ConstB", 0));
        Val gt = A(g, c, p, "AGreaterThanB"), lt = A(g, c, p, "ALessThanB"), eq = A(g, c, p, "AEqualsB");
        if (a.n > 1) a = Val::F(Cast(a, 1), 1);   // UE compares scalars (wider inputs are truncated)
        if (b.n > 1) b = Val::F(Cast(b, 1), 1);
        int n = std::max({gt.n, lt.n, Has(eq) ? eq.n : 1});
        if (!Has(eq)) return Val::F("((" + a.code + " >= " + b.code + ") ? " + Cast(gt, n) + " : " + Cast(lt, n) + ")", n);
        return Val::F("((abs(" + a.code + " - " + b.code + ") > " + Num(NumOf(p, "EqualsThreshold", 0.00001)) + ") ? ((" + a.code + " >= " + b.code + ") ? " +
                      Cast(gt, n) + " : " + Cast(lt, n) + ") : " + Cast(eq, n) + ")", n);
    }
    if (t == "Desaturation") {
        std::string x = Cast(A(g, c, p, "Input"), 3);
        Val f = A(g, c, p, "Fraction");
        const JsonValue* lw = P(p, "LuminanceFactors");
        std::string lum = "dot(" + x + ", float3(" + Num(NumOf(lw, "R", 0.3)) + ", " + Num(NumOf(lw, "G", 0.59)) + ", " + Num(NumOf(lw, "B", 0.11)) + "))";
        if (!Has(f)) return Val::F(lum, 1);
        return Val::F("lerp(" + x + ", " + lum + ", " + (f.n == 1 ? f.code : Cast(f, 3)) + ")", 3);
    }
    if (t == "SphereMask") {
        Val a = A(g, c, p, "A"), b = A(g, c, p, "B");
        int n = std::max(a.n, b.n);
        Val r = A(g, c, p, "Radius", NumOf(p, "AttenuationRadius", 256)), h = A(g, c, p, "Hardness", NumOf(p, "HardnessPercent", 100));
        return Val::F("saturate((1.0 - distance(" + Cast(a, n) + ", " + Cast(b, n) + ") / " + r.code + ") * (1.0 / max(1.0 - " + h.code + " * 0.01, 0.00001)))", 1);
    }
    if (t == "GIReplace") {
        Val d = A(g, c, p, "Default"), dyn = A(g, c, p, "DynamicIndirect");
        if (!Has(dyn)) return d;
        int n = std::max(d.n, dyn.n);
        return Val::F("(GetGIReplaceState() ? " + Cast(dyn, n) + " : " + Cast(d, n) + ")", n);
    }
    if (t == "IfShadowPass") {
        Val d = A(g, c, p, "NotShadow");
        if (!Has(d)) d = A(g, c, p, "Default", 0.0);
        Val sh = A(g, c, p, "Shadow", 0.0);
        int n = std::max(d.n, sh.n);
        return Val::F("(IsShadowDepthShader() ? " + Cast(sh, n) + " : " + Cast(d, n) + ")", n);
    }
    if (t == "DistanceCullFade") return Val::F("GetDistanceCullFade()", 1);
    if (t == "ViewSize") return Val::F("View.ViewSizeAndInvSize.xy", 2);
    if (t == "SceneTexelSize") return Val::F("View.ViewSizeAndInvSize.zw", 2);
    // geometric inputs
    if (t == "Time") {
        std::string tm = BoolOf(p, "bIgnorePause", false) ? "View.RealTime" : "View.GameTime";
        if (!BoolOf(p, "bOverride_Period", false)) return Val::F(tm, 1);
        double per = NumOf(p, "Period", 0);
        return per == 0 ? Val::F("0.0", 1) : Val::F("fmod(" + tm + ", " + Num(per) + ")", 1);
    }
    if (t == "WorldPosition") {
        std::string mode = StrOf(p, "WorldPositionShaderOffset", "WPT_Default");
        return Val::F(mode.find("ExcludeAll") != std::string::npos ? "GetWorldPosition_NoMaterialOffsets(Parameters)" : "GetWorldPosition(Parameters)", 3);
    }
    if (t == "CameraPositionWS") return Val::F("ResolvedView.WorldCameraOrigin", 3);
    if (t == "CameraVectorWS") return Val::F("Parameters.CameraVector", 3);
    if (t == "ObjectPositionWS") return Val::F("GetObjectWorldPosition(Parameters)", 3);
    if (t == "ActorPositionWS") return Val::F("GetActorWorldPosition()", 3);
    if (t == "ObjectRadius") return Val::F("GetPrimitiveData().ObjectWorldPositionAndRadius.w", 1);
    if (t == "ObjectBounds") return Val::F("Primitive.ObjectBounds", 3);
    if (t == "ObjectOrientation") return Val::F("GetObjectOrientation()", 3);
    if (t == "BoneMatrix") return Val::F(BoolOf(p, "bPosition", false) ? "GetObjectWorldPosition(Parameters)" : "Primitive.LocalToWorld[2].xyz", 3);
    if (t == "VertexNormalWS") return Val::F("Parameters.TangentToWorld[2]", 3);
    if (t == "PixelNormalWS") return Val::F("Parameters.WorldNormal", 3);
    if (t == "ReflectionVectorWS") {
        Val n = A(g, c, p, "CustomWorldNormal");
        if (!Has(n)) return Val::F("Parameters.ReflectionVector", 3);
        std::string nn = BoolOf(p, "bNormalizeCustomWorldNormal", false) ? "normalize(" + Cast(n, 3) + ")" : Cast(n, 3);
        return Val::F("ReflectionAboutCustomWorldNormal(Parameters, " + nn + ")", 3);
    }
    if (t == "TwoSidedSign") return Val::F("Parameters.TwoSidedSign", 1);
    if (t == "PixelDepth") return Val::F("GetPixelDepth(Parameters)", 1);
    if (t == "PerInstanceRandom") return Val::F("GetPerInstanceRandom(Parameters)", 1);
    if (t == "PerInstanceFadeAmount") return Val::F("GetPerInstanceFadeAmount(Parameters)", 1);
    if (t == "VertexColor") {
        const char* sw[] = {".rgb", ".r", ".g", ".b", ".a"};
        return Val::F(std::string("Parameters.VertexColor") + sw[std::min(o, 4)], o == 0 ? 3 : 1);
    }
    if (t == "TextureCoordinate") return TexCoord((int)NumOf(p, "CoordinateIndex", 0), NumOf(p, "UTiling", 1), NumOf(p, "VTiling", 1));
    if (t == "ScreenPosition") return Val::F(o == 0 ? "GetViewportUV(Parameters)" : "GetPixelPosition(Parameters)", 2);
    if (t == "Panner") {
        Val uv = A(g, c, p, "Coordinate");
        if (!Has(uv)) uv = TexCoord((int)NumOf(p, "ConstCoordinate", 0));
        Val tm = A(g, c, p, "Time");
        if (!Has(tm)) tm = Val::F("View.GameTime", 1);
        std::string off = "float2(" + tm.code + " * " + Num(NumOf(p, "SpeedX", 0)) + ", " + tm.code + " * " + Num(NumOf(p, "SpeedY", 0)) + ")";
        if (BoolOf(p, "bFractionalPart", false)) off = "frac(" + off + ")";
        if (uv.n > 2) off = Ty(uv.n) + "(" + off + std::string(uv.n == 3 ? ", 0.0" : ", 0.0, 0.0") + ")";
        return Val::F("(" + off + " + " + Cast(uv, std::max(uv.n, 2)) + ")", std::max(uv.n, 2));
    }
    if (t == "Rotator") {
        Val uv = A(g, c, p, "Coordinate");
        if (!Has(uv)) uv = TexCoord((int)NumOf(p, "ConstCoordinate", 0));
        Val tm = A(g, c, p, "Time");
        if (!Has(tm)) tm = Val::F("View.GameTime", 1);
        double cx = NumOf(p, "CenterX", 0.5), cy = NumOf(p, "CenterY", 0.5), sp = NumOf(p, "Speed", 0.25);
        Val ang = Tmp(tm.code + " * " + Num(sp), 1, "Rotator angle");
        Val cs = Tmp("float2(cos(" + ang.code + "), sin(" + ang.code + "))", 2, "Rotator cos / sin");
        std::string d = "(" + Cast(uv, 2) + " - float2(" + Num(cx) + ", " + Num(cy) + "))";
        std::string r = "(float2(dot(float2(" + cs.code + ".x, -" + cs.code + ".y), " + d + "), dot(float2(" + cs.code + ".y, " + cs.code + ".x), " + d + ")) + float2(" +
                        Num(cx) + ", " + Num(cy) + "))";
        if (uv.n == 3) return Val::F("float3(" + r + ", (" + uv.code + ").z)", 3);
        return Val::F(r, 2);
    }
    if (t == "Transform" || t == "TransformPosition") {
        Val x = A(g, c, p, "Input");
        auto last = [](std::string s) { return s.substr(s.rfind('_') + 1); };
        std::string src = last(StrOf(p, "TransformSourceType", t == "Transform" ? "TRANSFORMSOURCE_Tangent" : "TRANSFORMPOSSOURCE_Local"));
        std::string dst = last(StrOf(p, "TransformType", t == "Transform" ? "TRANSFORM_World" : "TRANSFORMPOSSOURCE_World"));
        std::string xin = x.n == 1 && src == "Tangent" ? "float3(0.0, 0.0, " + x.code + ")" : Cast(x, 3);
        return Val::F(std::string(t == "Transform" ? "TransformVector_" : "TransformPosition_") + src + "To" + dst + "(Parameters, " + xin + ")", 3);
    }
    if (t == "RotateAboutAxis") {
        std::string axis = Cast(A(g, c, p, "NormalizedRotationAxis"), 3), piv = Cast(A(g, c, p, "PivotPoint"), 3), pos = Cast(A(g, c, p, "Position"), 3);
        Val ang = A(g, c, p, "RotationAngle");
        return Val::F("RotateAboutAxis(float4(" + axis + ", " + Cast(ang, 1) + " * " + Num(6.283185307179586 / NumOf(p, "Period", 1)) + "), " + piv + ", " + pos + ")", 3);
    }
    if (t == "Fresnel") {
        Val e = A(g, c, p, "ExponentIn", NumOf(p, "Exponent", 5)), b = A(g, c, p, "BaseReflectFractionIn", NumOf(p, "BaseReflectFraction", 0.04));
        Val n = A(g, c, p, "Normal");
        std::string nn = Has(n) ? Cast(n, 3) : "Parameters.WorldNormal";
        Val base = Tmp("pow(max(1.0 - max(dot(" + nn + ", Parameters.CameraVector), 0.0), 0.0001), " + e.code + ")", 1, "Fresnel");
        return Val::F("(" + base.code + " * (1.0 - " + b.code + ") + " + b.code + ")", 1);
    }
    if (t == "BumpOffset") {
        Val uv = A(g, c, p, "Coordinate");
        if (!Has(uv)) uv = TexCoord((int)NumOf(p, "ConstCoordinate", 0));
        Val h = A(g, c, p, "Height"), ratio = A(g, c, p, "HeightRatioInput", NumOf(p, "HeightRatio", 0.05));
        return Val::F("(" + Cast(uv, 2) + " + TransformWorldVectorToTangent(Parameters.TangentToWorld, Parameters.CameraVector).xy * ((" + Cast(h, 1) + " - " +
                      Num(NumOf(p, "ReferencePlane", 0.5)) + ") * " + Cast(ratio, 1) + "))", 2);
    }
    if (t == "DepthFade") {
        Val op = A(g, c, p, "InOpacity", NumOf(p, "OpacityDefault", 1)), d = A(g, c, p, "FadeDistance", NumOf(p, "FadeDistanceDefault", 100));
        return Val::F("(" + op.code + " * saturate((CalcSceneDepth(ScreenAlignedPosition(Parameters.ScreenPosition)) - GetPixelDepth(Parameters)) / max(" + d.code + ", 0.00001)))", 1);
    }
    if (t == "SceneDepth" || t == "SceneColor") {
        Val in = A(g, c, p, "Input");
        std::string uv = Has(in) ? Cast(in, 2) : "ScreenAlignedPosition(Parameters.ScreenPosition)";
        if (StrOf(p, "InputMode").find("OffsetFraction") != std::string::npos) uv = "CalcScreenUVFromOffsetFraction(Parameters.ScreenPosition, " + (Has(in) ? Cast(in, 2) : "float2(0.0, 0.0)") + ")";
        return t == "SceneDepth" ? Val::F("CalcSceneDepth(" + uv + ")", 1) : Val::F("DecodeSceneColorForMaterialNode(" + uv + ")", 3);
    }
    if (t == "SceneTexture") {
        Val uv = A(g, c, p, "Coordinates");
        return Val::F("SceneTextureLookup(" + (Has(uv) ? Cast(uv, 2) : std::string("GetDefaultSceneTextureUV(Parameters)")) + ", " + StrOf(p, "SceneTextureId", "PPI_SceneColor") + ")", 4);
    }
    if (t == "EyeAdaptation") return Val::F("EyeAdaptationLookup()", 1);
    if (t == "SpeedTree")   // UE 4.10: world position offset from the SpeedTree wind / LOD system
        return Val::F(Fmt("GetSpeedTreeVertexOffset(Parameters, %s, %s, %s, %s, %s)", StrOf(p, "GeometryType", "STG_Branch").c_str(),
                          StrOf(p, "WindType", "STW_None").c_str(), StrOf(p, "LODType", "STLOD_Pop").c_str(),
                          Num(NumOf(p, "BillboardThreshold", 0.9)).c_str(), BoolOf(p, "bAccurateWindVelocities", false) ? "true" : "false"), 3);
    // Athena (SoT) engine nodes: values the renderer provides; their HLSL lives in the game's shaders
    if (t == "LumaAvg") return Val::F("AthenaLumaAvg()   /* SoT: average scene luminance */", 1);
    if (t == "CausticTextureCoordinate") return Val::F("AthenaCausticTextureCoordinate(Parameters)   /* SoT */", 2);
    if (t == "CausticTexture") {
        Val uv = A(g, c, p, "Coordinates");
        return Val::F("AthenaCausticTexture(" + (Has(uv) ? Cast(uv, 2) : std::string("AthenaCausticTextureCoordinate(Parameters)")) + ")   /* SoT: underwater caustics */", 3);
    }
    if (t == "Noise") {
        Val pos = A(g, c, p, "Position"), fw = A(g, c, p, "FilterWidth", 0.0);
        static const std::map<std::string, int> fns = {{"NOISEFUNCTION_Simplex", 0}, {"NOISEFUNCTION_Perlin", 1}, {"NOISEFUNCTION_Gradient", 2}, {"NOISEFUNCTION_FastGradient", 3}};
        auto fit = fns.find(StrOf(p, "NoiseFunction", "NOISEFUNCTION_Simplex"));
        return Val::F(Fmt("MaterialExpressionNoise(%s, %s, %d, %d, %s, %d, %s, %s, %s, %s)", (Has(pos) ? Cast(pos, 3) : std::string("GetWorldPosition(Parameters)")).c_str(),
                          Num(NumOf(p, "Scale", 1)).c_str(), (int)NumOf(p, "Quality", 0), fit != fns.end() ? fit->second : 0, BoolOf(p, "bTurbulence", true) ? "true" : "false",
                          std::clamp((int)NumOf(p, "Levels", 6), 1, 10), Num(NumOf(p, "OutputMin", -1)).c_str(), Num(NumOf(p, "OutputMax", 1)).c_str(),
                          Num(NumOf(p, "LevelScale", 2)).c_str(), fw.code.c_str()), 1);
    }
    // textures
    if (t == "TextureObjectParameter" || t == "TextureObject") return TexObject(p);
    if (t == "TextureSampleParameter2D" || t == "TextureSample" || t == "PredicatedTextureSampleParameter2D" || t == "PredicatedTextureSample") {
        Val obj = A(g, c, p, "TextureObject");
        if (obj.kind != Val::Tex) obj = TexObject(p);
        if (t.rfind("Predicated", 0) == 0) {
            // Sea of Thieves: with Predicate > 0 the texture is read, otherwise DefaultColour / 255 stands in
            Val pred = A(g, c, p, "Predicate");
            const JsonValue* dc = P(p, "DefaultColour");
            std::string def = Fmt("float4(%s, %s, %s, %s)", Num(NumOf(dc, "R", 0) / 255).c_str(), Num(NumOf(dc, "G", 0) / 255).c_str(), Num(NumOf(dc, "B", 0) / 255).c_str(),
                                  Num(NumOf(dc, "A", 0) / 255).c_str());
            if (!Has(pred)) return Channel(Tmp(def, 4, "predicated sample, predicate unconnected: the default colour"), o);
            Val s = TexSample(g, c, p, o, obj, std::nullopt, true);
            return Channel(Tmp("(" + Cast(pred, 1) + " > 0.0) ? " + s.code + " : " + def, 4, "predicated sample"), o);
        }
        return TexSample(g, c, p, o, obj);
    }
    if (t == "TextureSampleParameterCube") {
        Val obj = TexObject(p, "TextureCube");
        obj.texType = "Cube";
        return TexSample(g, c, p, o, obj);
    }
    if (t == "TextureSampleParameterSubUV" || t == "ParticleSubUV") {
        Val obj = TexObject(p);
        Val a = TexSample(g, c, p, o, obj, Val::F("Parameters.Particle.SubUVCoords[0].xy", 2), true);
        if (BoolOf(p, "bBlend", true)) {
            Val b = TexSample(g, c, p, o, obj, Val::F("Parameters.Particle.SubUVCoords[1].xy", 2), true);
            a = Tmp("lerp(" + a.code + ", " + b.code + ", Parameters.Particle.SubUVLerp)", 4, "sub UV blend");
        }
        return Channel(a, o);
    }
    if (t == "ParticleColor") return o == 0 ? Val::F("Parameters.Particle.Color.rgb", 3) : Val::F(std::string("Parameters.Particle.Color.") + "rgba"[std::min(o - 1, 3)], 1);
    if (t == "DynamicParameter") return Val::F(std::string("Parameters.Particle.DynamicParameter.") + "rgba"[std::min(o, 3)], 1);
    if (t == "ParticleRelativeTime") return Val::F("Parameters.Particle.RelativeTime", 1);
    if (t == "ParticlePositionWS") return Val::F("Parameters.Particle.PositionAndSize.xyz", 3);
    if (t == "ParticleSize") return Val::F("Parameters.Particle.Size", 2);
    if (t == "ParticleRadius") return Val::F("max(Parameters.Particle.PositionAndSize.w, 0.001)", 1);
    if (t == "CollectionParameter") {
        std::string colPath = ObjPath(P(p, "Collection"));
        std::string col = colPath.substr(colPath.rfind('.') + 1);
        std::string pn = StrOf(p, "ParameterName");
        // the collection lists it under ScalarParameters (float) or VectorParameters (float4)
        bool scalar = false;
        if (auto cp = m_cache.LoadPath(colPath))
            if (i32 main = cp->MainExport()) {
                PropertyList cprops = cp->ReadProperties((size_t)main - 1);
                if (const PropValue* sp = FindValue(cprops, "ScalarParameters"))
                    for (auto& it : sp->items)
                        if (const PropValue* n = it.Field("ParameterName"); n && IEquals(n->str, pn)) scalar = true;
            }
        std::string id = ParamIdent("collection", col + "." + pn, scalar ? "float" : "float4", "", "MaterialParameterCollection " + col + ", parameter \"" + pn + "\"");
        return Val::F(id, scalar ? 1 : 4);
    }
    // Sea of Thieves nodes
    if (t == "LocalDisturbanceTexture") {
        Val uv = A(g, c, p, "Coordinates");
        return Val::F("Texture2DSampleLevel(View.LocalDisturbanceTexture, View.BilinearWrapSampler, " + Cast(uv, 2) + ", 0)", 4);
    }
    if (t == "UnderwaterTranslucencyTexture" || t == "LowResClouds") {
        Val uv = A(g, c, p, "Coordinates");
        return Val::F(std::string(t == "LowResClouds" ? "SampleLowResClouds(" : "SampleUnderwaterTranslucency(") + (Has(uv) ? Cast(uv, 2) : std::string("ScreenAlignedPosition(Parameters.ScreenPosition)")) + ")", 4);
    }
    if (t == "RopeParameter") return Val::F("Rope." + StrOf(p, "RopeParameterType", "RPT_Start").substr(4), 3);
    // landscape
    if (t == "LandscapeLayerSample" || t == "LandscapeVisibilityMask") {
        std::string name = t == "LandscapeVisibilityMask" ? std::string("__LANDSCAPE_VISIBILITY__") : StrOf(p, "ParameterName", "None");
        return Val::F(ParamIdent("landscape", name, "float", "", "paint layer weight of \"" + name + "\" on this landscape component"), 1);
    }
    if (t == "LandscapeLayerWeight") {
        std::string name = StrOf(p, "ParameterName", "None");
        std::string w = ParamIdent("landscape", name, "float", "", "paint layer weight of \"" + name + "\" on this landscape component");
        Val base = A(g, c, p, "Base", NumOf(p, "ConstBase", 0)), layer = A(g, c, p, "Layer");
        int n = std::max(base.n, layer.n);
        return Val::F("(" + Cast(base, n) + " + " + Cast(layer, n) + " * " + w + ")", n);
    }
    if (t == "LandscapeLayerSwitch") {
        // a per-component static permutation: the material's code for components that paint this layer
        lines.push_back("    // landscape layer switch \"" + StrOf(p, "ParameterName") + "\": components painting this layer (LayerNotUsed otherwise)");
        Val v = A(g, c, p, "LayerUsed");
        return Has(v) ? v : A(g, c, p, "LayerNotUsed");
    }
    if (t == "LandscapeLayerCoords") {
        static const std::map<std::string, std::string> map = {{"TCMT_Auto", "LandscapeLayerUV"}, {"TCMT_XY", "LandscapeLayerUV_XY"}, {"TCMT_XZ", "LandscapeLayerUV_XZ"}, {"TCMT_YZ", "LandscapeLayerUV_YZ"}};
        auto it = map.find(StrOf(p, "MappingType", "TCMT_Auto"));
        double sc = NumOf(p, "MappingScale", 0), rot = NumOf(p, "MappingRotation", 0) * 3.14159265358979 / 180;
        std::string uv = "Parameters." + (it != map.end() ? it->second : std::string("LandscapeLayerUV"));
        std::string r = Fmt("float2(dot(%s, float2(%s, %s)), dot(%s, float2(%s, %s)))", uv.c_str(), Num(cos(rot)).c_str(), Num(sin(rot)).c_str(), uv.c_str(), Num(-sin(rot)).c_str(),
                            Num(cos(rot)).c_str());
        if (sc != 0) r = "(" + r + " / " + Num(sc) + ")";
        return Val::F("(" + r + " + float2(" + Num(NumOf(p, "MappingPanU", 0)) + ", " + Num(NumOf(p, "MappingPanV", 0)) + "))", 2);
    }
    if (t == "LandscapeLayerBlend") {
        const JsonValue* layers = P(p, "Layers");
        if (!layers || layers->arr.empty()) return Val::F("0.0", 1);
        struct L { std::string name, type; Val layer, height; };
        std::vector<L> ls;
        int n = 1;
        for (auto& l : layers->arr) {
            L x;
            x.name = l.GetStr("LayerName");
            x.type = l.GetStr("BlendType", "LB_WeightBlend");
            x.layer = Input({g, c, l.Get("LayerInput")});
            if (!Has(x.layer)) {
                const JsonValue* k = l.Get("ConstLayerInput");
                x.layer = Val::F("float3(" + Num(NumOf(k, "X", 0)) + ", " + Num(NumOf(k, "Y", 0)) + ", " + Num(NumOf(k, "Z", 0)) + ")", 3);
            }
            x.height = Input({g, c, l.Get("HeightInput")});
            if (!Has(x.height)) x.height = Val::F(Num(l.GetNum("ConstHeightInput", 0)), 1);
            n = std::max(n, x.layer.n);
            ls.push_back(std::move(x));
        }
        lines.push_back("    // landscape layer blend");
        std::vector<std::string> weights;
        std::string sum;
        for (auto& l : ls) {
            std::string w = ParamIdent("landscape", l.name, "float", "", "paint layer weight of \"" + l.name + "\" on this landscape component");
            if (l.type == "LB_HeightBlend") w = Tmp("clamp(lerp(-1.0, 1.0, " + w + ") + " + Cast(l.height, 1) + ", 0.0001, 1.0)", 1, "height blend weight of " + l.name).code;
            weights.push_back(w);
            if (l.type != "LB_AlphaBlend") sum += (sum.empty() ? "" : " + ") + w;
        }
        Val total = Tmp(sum.empty() ? "1.0" : "max(" + sum + ", 0.0001)", 1, "weight sum");
        std::string acc;
        for (size_t i = 0; i < ls.size(); i++)
            if (ls[i].type != "LB_AlphaBlend") acc += (acc.empty() ? "" : " + ") + Cast(ls[i].layer, n) + " * " + weights[i];
        Val r = Tmp(acc.empty() ? Cast(Val::F("0.0", 1), n) : "(" + acc + ") / " + total.code, n, "weight / height blended layers");
        for (size_t i = 0; i < ls.size(); i++)
            if (ls[i].type == "LB_AlphaBlend") r = Tmp("lerp(" + r.code + ", " + Cast(ls[i].layer, n) + ", " + weights[i] + ")", n, "alpha blend of " + ls[i].name);
        return r;
    }
    if (t == "LandscapeGrassOutput") return Val::F("0.0", 1);
    // custom HLSL
    if (t == "Custom") {
        std::vector<std::pair<std::string, Val>> ins;
        if (const JsonValue* arr = P(p, "Inputs"))
            for (auto& i : arr->arr) {
                Val v = Input({g, c, i.Get("Input")});
                if (!Has(v)) v = Val::F("0.0", 1);
                std::string nm = i.GetStr("InputName");
                ins.push_back({Ident(nm.empty() ? "In" : nm), v});
            }
        std::string ot = StrOf(p, "OutputType", "CMOT_Float3");
        int n = ot == "CMOT_Float1" ? 1 : ot == "CMOT_Float2" ? 2 : ot == "CMOT_Float4" ? 4 : 3;
        std::string code = StrOf(p, "Code");
        code.erase(std::remove(code.begin(), code.end(), '\r'), code.end());
        std::string sig;
        for (auto& [nm, v] : ins) sig += Fmt("|%s:%d:%d", nm.c_str(), v.n, (int)v.kind);
        std::string fname;
        for (auto& [k, def] : customs)
            if (def.find("/*sig*/" + sig + "\n") != std::string::npos && def.find(code) != std::string::npos) fname = k;
        if (fname.empty()) {
            fname = Fmt("CustomExpression%zu", customs.size());
            std::string params = "FMaterialPixelParameters Parameters";
            for (auto& [nm, v] : ins)
                params += v.kind == Val::Tex ? ", Texture2D " + nm + ", SamplerState " + nm + "Sampler" : ", " + Ty(v.n) + " " + nm;
            std::string desc = StrOf(p, "Description");
            std::string body = code.find("return") == std::string::npos ? "return " + code + ";" : code;
            std::string indented;
            size_t s = 0;
            while (s <= body.size()) {
                size_t e = body.find('\n', s);
                if (e == std::string::npos) e = body.size();
                indented += "    " + body.substr(s, e - s) + "\n";
                s = e + 1;
            }
            customs[fname] = "/*sig*/" + sig + "\n" + (desc.empty() ? "" : "// " + desc + "\n") + Ty(n) + " " + fname + "(" + params + ")\n{\n" + indented + "}\n";
        }
        std::string args = "Parameters";
        for (auto& [nm, v] : ins) args += v.kind == Val::Tex ? ", " + v.code + ", " + v.code + "Sampler" : ", " + v.code;
        return Val::F(fname + "(" + args + ")", n);
    }
    // functions
    if (t == "MaterialFunctionCall") {
        std::string nodeName;
        for (auto& [nm, ob] : g->by)
            if (ob->Get("Properties") == p) nodeName = nm;
        return FunctionCall(g, c, nodeName, p, o);
    }
    if (t == "FunctionOutput") {
        Val v = A(g, c, p, "A");
        return Has(v) ? v : Val::F("0.0", 1);
    }
    if (t == "FunctionInput") {
        std::string name = StrOf(p, "InputName");
        std::string it = StrOf(p, "InputType", "FunctionInput_Vector3");
        int width = it == "FunctionInput_Scalar" ? 1 : it == "FunctionInput_Vector2" ? 2 : it == "FunctionInput_Vector3" ? 3 : it == "FunctionInput_Vector4" ? 4 : 0;
        const Ctx& cx = ctxs[c];
        const Pin* bound = nullptr;
        if (auto a = cx.args.find(name); a != cx.args.end()) bound = &a->second;
        else if (auto b = cx.args.find("#" + StrOf(p, "Id")); b != cx.args.end()) bound = &b->second;
        if (bound) {
            Val v = Input(*bound);
            if (Has(v)) return width && v.kind == Val::Float && v.n != width ? Val::F(Cast(v, width), width) : v;
        }
        if (c == 0 && g->isFunction) {   // translating a function on its own: its inputs are parameters
            if (width) return Val::F(ParamIdent("input", name, Ty(width), "", "function input \"" + name + "\""), width);
            if (it == "FunctionInput_MaterialAttributes") {
                Val v;
                v.kind = Val::Attrs;
                v.code = ParamIdent("input", name, "FMaterialAttributes", "", "function input \"" + name + "\"");
                return v;
            }
        }
        Val pv = A(g, c, p, "Preview");
        if (Has(pv)) return pv;
        if (it == "FunctionInput_StaticBool") { Val v; v.kind = Val::Static; v.code = "false"; return v; }
        if (it == "FunctionInput_Texture2D" || it == "FunctionInput_TextureCube") {   // unbound: a texture parameter of the function
            std::string type = it == "FunctionInput_TextureCube" ? "TextureCube" : "Texture2D";
            Val v;
            v.kind = Val::Tex;
            v.code = ParamIdent("input", name, type, "", "function input \"" + name + "\" (no texture bound)");
            v.texType = "SAMPLERTYPE_Color";
            return v;
        }
        if (it == "FunctionInput_MaterialAttributes") { Val v; v.kind = Val::Attrs; return v; }
        const JsonValue* pvv = P(p, "PreviewValue");
        std::string s = width == 1 ? Num(NumOf(pvv, "X", 0)) : Ty(width) + "(";
        if (width > 1) {
            const char* k[] = {"X", "Y", "Z", "W"};
            for (int i = 0; i < width; i++) s += (i ? ", " : "") + Num(NumOf(pvv, k[i], 0));
            s += ")";
        }
        return Val::F(s, std::max(width, 1));
    }
    // material attributes
    if (t == "MakeMaterialAttributes") {
        Val v;
        v.kind = Val::Attrs;
        for (auto& a : Attrs()) {
            const JsonValue* pin = nullptr;
            if (a.rfind("CustomizedUVs", 0) == 0) {
                int i = a.back() - '0';
                if (const JsonValue* arr = P(p, "CustomizedUVs"); arr && arr->type == JsonValue::Type::Array && i < (int)arr->arr.size()) pin = &arr->arr[i];
                else pin = P(p, Fmt("CustomizedUVs[%d]", i).c_str());
            } else pin = P(p, a.c_str());
            if (pin && pin->Get("Expression") && !pin->Get("Expression")->IsNull()) v.attrs[a] = {g, c, pin};
        }
        return v;
    }
    if (t == "BreakMaterialAttributes") {
        Val src = Input({g, c, P(p, "MaterialAttributes")});
        std::string a = BreakOutput(o);
        if (a.empty()) return Unsupported(Fmt("BreakMaterialAttributes output %d", o));
        if (src.kind == Val::Attrs && src.attrs.empty() && !src.code.empty()) return Val::F(src.code + "." + a, AttrSize(a));
        if (src.kind != Val::Attrs || !src.attrs.count(a)) return Val::F(AttrDefault(a), AttrSize(a));
        Val v = Input(src.attrs[a]);
        return Has(v) ? v : Val::F(AttrDefault(a), AttrSize(a));
    }
    return Unsupported(t);
}

}   // namespace

static bool MaterialToHlslImpl(PackageCache& cache, const std::string& path, std::string& hlsl, MaterialGraphStats* stats, std::string* error);

// Graphs nest deeply (functions inside functions) and every node is a recursive call: translate on a thread with a large stack.
bool MaterialToHlsl(PackageCache& cache, const std::string& path, std::string& hlsl, MaterialGraphStats* stats, std::string* error) {
    struct Job { PackageCache* cache; const std::string* path; std::string* hlsl; MaterialGraphStats* stats; std::string* error; bool ok = false; std::string crash; } job{&cache, &path, &hlsl, stats, error};
    auto run = [](void* arg) -> unsigned {
        Job& j = *(Job*)arg;
        try {
            j.ok = MaterialToHlslImpl(*j.cache, *j.path, *j.hlsl, j.stats, j.error);
        } catch (const std::exception& e) { j.crash = e.what(); }
        return 0;
    };
    HANDLE th = (HANDLE)_beginthreadex(nullptr, 256u << 20, run, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!th) return MaterialToHlslImpl(cache, path, hlsl, stats, error);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (!job.crash.empty() && error) *error = job.crash;
    return job.ok;
}

static bool MaterialToHlslImpl(PackageCache& cache, const std::string& path, std::string& hlsl, MaterialGraphStats* stats, std::string* error) {
    Translator tr(cache);
    std::string err;
    auto pkg = cache.LoadPath(path, &err);
    if (!pkg) {
        if (error) *error = err;
        return false;
    }
    // an instance: the master's graph with the instance's values
    MaterialInfo mi;
    std::string graphPath = pkg->PackageName();
    bool isFunction = false;
    for (size_t i = 0; i < pkg->Exports().size(); i++)
        if (pkg->ExportClass(i) == "MaterialFunction") isFunction = true;
    if (!isFunction && ReadMaterial(cache, path, mi, nullptr) && !mi.baseMaterial.empty()) {
        graphPath = mi.baseMaterial.substr(0, mi.baseMaterial.find('.'));
        for (auto& s : mi.scalars) tr.scalarOverride.emplace(s.name, s.value);
        for (auto& v : mi.vectors) tr.vectorOverride.emplace(v.name, std::array<float, 4>{v.r, v.g, v.b, v.a});
        for (auto& tp : mi.textures)
            if (tp.name != "<referenced>") tr.textureOverride.emplace(tp.name, tp.texturePath);
        for (auto& s : mi.staticSwitches) {
            size_t eq = s.rfind('=');
            if (eq != std::string::npos) tr.statics.emplace(s.substr(0, eq), s.substr(eq + 1) == "true");
        }
    }
    Graph* g = tr.LoadGraph(graphPath, &err);
    if (!g) {
        if (error) *error = err;
        return false;
    }
    tr.ctxs.push_back({"root", {}});
    const JsonValue* root = g->root->Get("Properties");

    std::string header = "// " + std::string(g->isFunction ? "Material function " : "Material ") + g->name + " (" + g->path + ") translated to HLSL\n";
    header += "// UE 4.10 HLSLMaterialTranslator conventions: Parameters = FMaterialPixelParameters / FMaterialVertexParameters, View, Primitive.\n";
    if (mi.chain.size() > 1) {
        header += "// instance chain:";
        for (auto& c : mi.chain) header += " " + c;
        header += "\n";
    }
    if (!tr.statics.empty()) {
        header += "// static switches:";
        for (auto& [k, v] : tr.statics) header += " " + k + "=" + (v ? "true" : "false");
        header += "\n";
    }
    if (!g->isFunction) {
        std::string info;
        for (const char* k : {"BlendMode", "ShadingModel", "MaterialDomain"})
            if (auto s = Translator::StrOf(root, k); !s.empty()) info += std::string(" ") + k + "=" + s;
        if (Translator::BoolOf(root, "TwoSided", false)) info += " TwoSided";
        if (Translator::BoolOf(root, "bUseMaterialAttributes", false)) info += " UseMaterialAttributes";
        if (!info.empty()) header += "//" + info + "\n";
    }

    std::vector<std::string> connected;
    std::string body;
    auto stage = [&](bool vertex, const std::vector<std::string>& attrs, const char* signature, const char* target) {
        tr.vertex = vertex;
        tr.lines.clear();
        tr.memo.clear();
        tr.counter = 0;
        std::vector<std::string> assigns;
        Val attrsVal;
        bool useAttrs = !g->isFunction && Translator::BoolOf(root, "bUseMaterialAttributes", false);
        if (useAttrs) attrsVal = tr.Input({g, 0, root->Get("MaterialAttributes")});
        for (auto& a : attrs) {
            Val v;
            v.n = 0;
            if (useAttrs) {
                if (attrsVal.kind == Val::Attrs && attrsVal.attrs.count(a)) v = tr.Input(attrsVal.attrs[a]);
            } else {
                const JsonValue* pin = nullptr;
                if (a.rfind("CustomizedUVs", 0) == 0) {
                    int i = a.back() - '0';
                    if (const JsonValue* arr = root->Get("CustomizedUVs"); arr && arr->type == JsonValue::Type::Array && i < (int)arr->arr.size()) pin = &arr->arr[i];
                    else pin = root->Get(Fmt("CustomizedUVs[%d]", i));
                } else pin = root->Get(a);
                v = tr.Input({g, 0, pin});
            }
            if (!Translator::Has(v) || v.kind != Val::Float || v.code.empty()) continue;
            if (tr.Cast(v, AttrSize(a)) == AttrDefault(a)) continue;   // the attribute's default: nothing to write
            connected.push_back(a);
            std::string field = a.rfind("CustomizedUVs", 0) == 0 ? Fmt("CustomizedUVs[%c]", a.back()) : a;
            assigns.push_back(Fmt("    %s.%s = %s;", target, field.c_str(), tr.Cast(v, AttrSize(a)).c_str()));
        }
        if (assigns.empty()) return;
        body += std::string("\n") + signature + "\n{\n";
        for (auto& l : tr.lines) body += l + "\n";
        if (!tr.lines.empty()) body += "\n";
        for (auto& l : assigns) body += l + "\n";
        body += "}\n";
    };
    if (g->isFunction) {
        // each output of the function
        tr.lines.clear();
        std::vector<std::string> outs;
        std::vector<std::pair<std::string, const JsonValue*>> fo;
        for (auto& [n, o] : g->by)
            if (o->GetStr("Type") == "MaterialExpressionFunctionOutput") fo.push_back({n, o});
        std::sort(fo.begin(), fo.end(), [](auto& a, auto& b) { return Translator::NumOf(a.second->Get("Properties"), "SortPriority", 0) < Translator::NumOf(b.second->Get("Properties"), "SortPriority", 0); });
        for (auto& [n, o] : fo) {
            Val v = tr.Node(g, 0, n, 0);
            std::string on = Translator::StrOf(o->Get("Properties"), "OutputName", n);
            if (v.kind == Val::Attrs) {
                for (auto& [a, pin] : v.attrs) {
                    Val x = tr.Input(pin);
                    if (Translator::Has(x) && x.kind == Val::Float) outs.push_back(Fmt("    %s.%s = %s;", Ident(on).c_str(), a.c_str(), x.code.c_str()));
                }
            } else if (v.kind == Val::Float) outs.push_back(Fmt("    %s = %s;   // output \"%s\"", Ident(on).c_str(), v.code.c_str(), on.c_str()));
            connected.push_back(on);
        }
        body += "\nvoid " + Ident(g->name) + "(FMaterialPixelParameters Parameters)\n{\n";
        for (auto& l : tr.lines) body += l + "\n";
        body += "\n";
        for (auto& l : outs) body += l + "\n";
        body += "}\n";
    } else {
        std::vector<std::string> pixel = {"Normal"}, vtx;
        for (auto& a : Attrs())
            if (a != "Normal") (IsVertexAttr(a) ? vtx : pixel).push_back(a);
        stage(false, pixel, "void CalcPixelMaterialInputs(in out FMaterialPixelParameters Parameters, in out FPixelMaterialInputs PixelMaterialInputs)", "PixelMaterialInputs");
        stage(true, vtx, "void CalcVertexMaterialInputs(in out FMaterialVertexParameters Parameters, in out FVertexMaterialInputs VertexMaterialInputs)", "VertexMaterialInputs");
    }

    if (!tr.unsupported.empty()) {
        header += "// not translated (evaluate to 0):";
        for (auto& u : tr.unsupported) header += " " + u + ";";
        header += "\n";
    }
    std::string decls;
    const char* sections[][2] = {{"scalar", "scalar parameters"}, {"vector", "vector parameters"}, {"texture", "textures (each with a SamplerState <name>Sampler)"},
                                 {"collection", "material parameter collections"}, {"landscape", "landscape paint layers"}, {"input", "function inputs"}};
    for (auto& s : sections) {
        std::vector<const Param*> ps;
        for (auto& p : tr.params) if (p.kind == s[0]) ps.push_back(&p);
        if (ps.empty()) continue;
        decls += std::string("\n// ---- ") + s[1] + "\n";
        for (auto* p : ps) decls += Fmt("%-56s // %s\n", p->decl.c_str(), p->comment.c_str());
    }
    std::string customs;
    for (auto& [k, def] : tr.customs) customs += "\n" + def.substr(def.find('\n') + 1);
    if (!customs.empty()) customs = "\n// ---- custom expressions" + customs;
    hlsl = header + decls + customs + body;
    if (stats) {
        stats->nodes = tr.nodeCount;
        stats->functions = tr.functionCalls;
        stats->unsupported.assign(tr.unsupported.begin(), tr.unsupported.end());
        stats->attributes = connected;
    }
    return true;
}

bool ReadMaterialGraph(PackageCache& cache, const std::string& path, std::vector<MaterialGraphNode>& nodes, std::string* error) {
    nodes.clear();
    auto pkg = cache.LoadPath(path, error);
    if (!pkg) return false;
    JsonValue doc;
    if (!JsonValue::Parse(PackageToJson(*pkg), doc, error)) return false;
    for (auto& o : doc.arr) {
        std::string type = o.GetStr("Type");
        bool root = type == "Material";
        if (type.rfind("MaterialExpression", 0) != 0 && !root) continue;
        if (type == "MaterialExpressionComment") continue;
        MaterialGraphNode n;
        n.name = o.GetStr("Name");
        n.type = root ? type : type.substr(18);
        const JsonValue* p = o.Get("Properties");
        if (!p) { nodes.push_back(n); continue; }
        n.x = (int)p->GetNum(root ? "EditorX" : "MaterialExpressionEditorX", 0);
        n.y = (int)p->GetNum(root ? "EditorY" : "MaterialExpressionEditorY", 0);
        if (auto s = p->GetStr("ParameterName"); !s.empty()) n.caption = s;
        else if (const JsonValue* f = p->Get("MaterialFunction")) {
            n.caption = ObjPath(f).substr(ObjPath(f).rfind('.') + 1);
            n.function = ObjPath(f).substr(0, ObjPath(f).find('.'));
        }
        if (const JsonValue* outs = p->Get("FunctionOutputs"))
            for (auto& fo : outs->arr)
                if (const JsonValue* o2 = fo.Get("Output")) n.outputs.push_back(o2->GetStr("OutputName"));
        if (n.outputs.empty() && (n.type.find("TextureSample") != std::string::npos || n.type == "VertexColor" || n.type == "ParticleColor"))
            n.outputs = {"RGB", "R", "G", "B", "A"};
        if (n.outputs.empty() && n.type == "VectorParameter") n.outputs = {"RGB", "R", "G", "B", "A"};
        else if (auto s2 = p->GetStr("Text"); !s2.empty()) n.caption = s2;
        else if (n.type == "Constant") n.caption = Num(p->GetNum("R", 0));
        auto link = [&](const std::string& input, const JsonValue& pin) {
            const JsonValue* e = pin.IsObject() ? pin.Get("Expression") : nullptr;
            if (!e || e->IsNull() || RefName(e).empty() || RefName(e) == "None") return;
            n.inputs.push_back({input, RefName(e), (int)pin.GetNum("OutputIndex", 0)});
        };
        for (auto& [k, v] : p->obj) {
            if (k == "Material" || k == "Function" || k == "Expressions") continue;
            if (v.IsObject() && v.Get("Expression")) link(k, v);
            else if (v.type == JsonValue::Type::Array)
                for (size_t i = 0; i < v.arr.size(); i++) {
                    const JsonValue& el = v.arr[i];
                    if (!el.IsObject()) continue;
                    if (el.Get("Expression")) link(Fmt("%s[%zu]", k.c_str(), i), el);
                    for (auto& [k2, v2] : el.obj)   // FunctionInputs[i].Input, Inputs[i].Input, Layers[i].LayerInput
                        if (v2.IsObject() && v2.Get("Expression")) link(Fmt("%s[%zu].%s", k.c_str(), i, el.GetStr("InputName", el.GetStr("LayerName", k2)).c_str()), v2);
                }
        }
        nodes.push_back(std::move(n));
    }
    // cooked packages keep no editor positions (only the material node's): lay the graph out in columns by the longest
    // distance to an output, rows ordered by where each node's consumers are
    int placed = 0;
    for (auto& n : nodes) placed += n.x || n.y ? 1 : 0;
    if (placed <= 1 && nodes.size() > 1) {
        std::map<std::string, int> index;
        for (int i = 0; i < (int)nodes.size(); i++) index[nodes[i].name] = i;
        std::vector<std::vector<std::pair<int, int>>> consumers(nodes.size());   // (consumer, input slot)
        for (int i = 0; i < (int)nodes.size(); i++)
            for (int k = 0; k < (int)nodes[i].inputs.size(); k++)
                if (auto it = index.find(nodes[i].inputs[k].from); it != index.end()) consumers[it->second].push_back({i, k});
        std::vector<int> depth(nodes.size(), 0);
        for (size_t pass = 0; pass < nodes.size(); pass++) {
            bool changed = false;
            for (int i = 0; i < (int)nodes.size(); i++)
                for (auto& [c, k] : consumers[i])
                    if (depth[i] < depth[c] + 1) { depth[i] = depth[c] + 1; changed = true; }
            if (!changed) break;
        }
        int maxDepth = *std::max_element(depth.begin(), depth.end());
        std::vector<float> y(nodes.size(), 0);
        auto height = [&](int i) { return 22 + std::max<size_t>({nodes[i].inputs.size(), nodes[i].outputs.size(), (size_t)1}) * 16 + 30.f; };
        for (int L = 0; L <= maxDepth; L++) {
            std::vector<std::pair<float, int>> col;
            for (int i = 0; i < (int)nodes.size(); i++) {
                if (depth[i] != L) continue;
                float key = 0;
                for (auto& [c, k] : consumers[i]) key += y[c] + k * 16.f;
                col.push_back({consumers[i].empty() ? -1.f : key / consumers[i].size(), i});
            }
            std::sort(col.begin(), col.end());
            float cy = 0;
            for (auto& [key, i] : col) {
                cy = std::max(cy, key - height(i) * 0.25f);   // near its consumers, without overlapping the previous node
                y[i] = cy;
                cy += height(i);
            }
        }
        for (int i = 0; i < (int)nodes.size(); i++) {
            nodes[i].x = -depth[i] * 260;
            nodes[i].y = (int)y[i];
        }
    }
    return true;
}
