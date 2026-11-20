#include "ue/Material.h"
#include "core/Vfs.h"

bool IsMaterialClass(std::string_view c) {
    return c == "Material" || c == "MaterialInstanceConstant" || c == "MaterialInstanceDynamic" || c == "MaterialFunction";
}

std::string ClassifyTexture(std::string_view param, std::string_view tex) {
    std::string p = ToLower(param), t = ToLower(tex);
    size_t dot = t.rfind('.');
    if (dot != std::string::npos && t.find('/', dot) == std::string::npos) t.resize(dot);
    auto ends = [&](const char* s) { return IEndsWith(t, s); };
    auto has = [&](const char* s) { return p.find(s) != std::string::npos; };
    // parameter names first, then the SoT texture suffix convention (_di diffuse, _no normal, _mr/_orm packed masks, _em emissive)
    if (has("normal") || has("_no") || p == "no") return "normal";
    if (has("diffuse") || has("albedo") || has("basecolo") || has("base colo") || p == "di" || has("_di")) return "diffuse";
    if (has("metal") || has("rough") || has("occlusion") || has("specular")) return "packed";
    if (has("emissive") || has("emis")) return "emissive";
    if (ends("_no") || ends("_n") || ends("_nrm") || ends("_normal")) return "normal";
    if (ends("_di") || ends("_d") || ends("_alb") || ends("_albedo") || ends("_diffuse") || ends("_col") || ends("_bc")) return "diffuse";
    if (ends("_mr") || ends("_orm") || ends("_rma") || ends("_spec") || ends("_sp") || ends("_mra") || ends("_ma")) return "packed";
    if (ends("_em") || ends("_emissive") || ends("_e")) return "emissive";
    if (ends("_mask") || ends("_ms") || ends("_m") || ends("_op") || ends("_alpha")) return "mask";
    return "";
}

static std::string ParamName(const PropValue& v) {
    if (auto n = v.Field("ParameterName")) return n->str;
    if (auto info = v.Field("ParameterInfo")) if (auto n = info->Field("Name")) return n->str;
    return {};
}

bool ReadMaterial(PackageCache& cache, const std::string& path, MaterialInfo& m, std::string* error) {
    m.path = path;
    std::string cur = path;
    bool blendSet = false, twoSidedSet = false;
    for (int depth = 0; depth < 16 && !cur.empty(); depth++) {
        std::string err;
        auto pkg = cache.LoadPath(cur, &err);
        if (!pkg) {
            if (depth == 0) { if (error) *error = err; return false; }
            m.chain.push_back(cur + " (missing)");
            break;
        }
        // the object inside the package: by name after '.', else the main export
        std::string objName;
        size_t dot = cur.rfind('.');
        if (dot != std::string::npos && cur.find('/', dot) == std::string::npos) objName = cur.substr(dot + 1);
        i32 ex = objName.empty() ? 0 : pkg->FindExport(objName);
        if (!ex) ex = pkg->MainExport();
        if (!ex) break;
        std::string cls = pkg->ExportClass(ex - 1);
        std::string here = pkg->FullPath(ex);
        m.chain.push_back(here + " (" + cls + ")");
        PropertyList props = pkg->ReadProperties(ex - 1);

        if (auto t = FindValue(props, "TextureParameterValues"))
            for (auto& v : t->items) {
                std::string name = ParamName(v);
                auto pv = v.Field("ParameterValue");
                if (name.empty() || !pv || !pv->ObjIndex()) continue;
                if (std::any_of(m.textures.begin(), m.textures.end(), [&](auto& x) { return x.name == name; })) continue;   // child wins
                m.textures.push_back({name, pkg->FullPath(pv->ObjIndex()), here});
            }
        if (auto s = FindValue(props, "ScalarParameterValues"))
            for (auto& v : s->items) {
                std::string name = ParamName(v);
                auto pv = v.Field("ParameterValue");
                if (name.empty() || !pv) continue;
                if (std::any_of(m.scalars.begin(), m.scalars.end(), [&](auto& x) { return x.name == name; })) continue;
                m.scalars.push_back({name, (float)pv->AsFloat(), here});
            }
        if (auto s = FindValue(props, "VectorParameterValues"))
            for (auto& v : s->items) {
                std::string name = ParamName(v);
                auto pv = v.Field("ParameterValue");
                if (name.empty() || !pv) continue;
                if (std::any_of(m.vectors.begin(), m.vectors.end(), [&](auto& x) { return x.name == name; })) continue;
                auto c = [&](const char* k) { auto f = pv->Field(k); return f ? (float)f->AsFloat() : 0.f; };
                m.vectors.push_back({name, c("R"), c("G"), c("B"), c("A"), here});
            }
        if (auto sp = FindValue(props, "StaticParameters"))
            if (auto sw = sp->Field("StaticSwitchParameters"))
                for (auto& v : sw->items) {
                    auto val = v.Field("Value");
                    m.staticSwitches.push_back(ParamName(v) + "=" + (val && val->AsBool() ? "true" : "false"));
                }
        if (auto bo = FindValue(props, "BasePropertyOverrides")) {
            if (!blendSet && bo->Field("bOverride_BlendMode") && bo->Field("bOverride_BlendMode")->AsBool() && bo->Field("BlendMode")) { m.blendMode = bo->Field("BlendMode")->str; blendSet = true; }
            if (!twoSidedSet && bo->Field("bOverride_TwoSided") && bo->Field("bOverride_TwoSided")->AsBool() && bo->Field("TwoSided")) { m.twoSided = bo->Field("TwoSided")->AsBool(); twoSidedSet = true; }
        }
        if (cls == "Material") {
            m.baseMaterial = here;
            if (!blendSet && FindValue(props, "BlendMode")) m.blendMode = PropStr(props, "BlendMode");
            if (!twoSidedSet) m.twoSided = PropBool(props, "TwoSided", false);
            m.shadingModel = PropStr(props, "ShadingModel");
            // cooked base materials keep no expression graph: list the textures they reference
            for (size_t i = 0; i < pkg->Imports().size(); i++) {
                const auto& im = pkg->Imports()[i];
                if (im.className.find("Texture") == std::string::npos) continue;
                std::string tp = pkg->FullPath(-(i32)i - 1);
                if (std::any_of(m.textures.begin(), m.textures.end(), [&](auto& x) { return x.texturePath == tp; })) continue;
                m.textures.push_back({"<referenced>", tp, here});
            }
            break;
        }
        i32 parent = PropObj(props, "Parent");
        cur = parent ? pkg->FullPath(parent) : std::string();
    }
    for (auto& t : m.textures) {
        std::string kind = ClassifyTexture(t.name == "<referenced>" ? "" : t.name, t.texturePath);
        std::string* slot = kind == "diffuse" ? &m.diffuse : kind == "normal" ? &m.normal : kind == "packed" ? &m.packed : kind == "emissive" ? &m.emissive : nullptr;
        if (slot && slot->empty()) *slot = t.texturePath;
    }
    return true;
}
