#include "ue/World.h"
#include "ue/WorldLayout.h"
#include "core/Vfs.h"
#include "ue/Texture.h"
#include <unordered_map>
#include <unordered_set>
#include <map>

bool IsWorldClass(std::string_view cls) { return IEquals(cls, "World"); }

size_t WorldData::InstanceCount() const {
    size_t n = splines.size();
    for (auto& g : groups) n += g.instances.size();
    return n;
}

namespace {

// One source of component properties: a level export or a Blueprint template (object references resolve in `pkg`).
struct PropLayer {
    const Package* pkg = nullptr;
    i32 exportIdx = -1;   // 0-based export in pkg
    const PropertyList* props = nullptr;
};

// Properties of a component: the level's values over its Blueprint templates (layers[0] = highest priority).
struct CompView {
    std::vector<PropLayer> layers;
    const PropValue* Get(std::string_view name, const PropLayer** from = nullptr) const {
        for (auto& l : layers) {
            const PropValue* v = FindValue(*l.props, name);
            if (!v) continue;
            if (v->type == PropType::Object && v->i == 0) continue;   // a null in the level (unresolved asset) must not hide the template's value
            if (from) *from = &l;
            return v;
        }
        return nullptr;
    }
    float Float(std::string_view name, float def = 0) const { auto v = Get(name); return v ? (float)v->AsFloat(def) : def; }
    bool Bool(std::string_view name, bool def) const { auto v = Get(name); return v ? v->AsBool(def) : def; }
};

struct BpTemplates {
    std::vector<std::shared_ptr<Package>> pkgs;   // keeps the template packages alive
    std::deque<PropertyList> storage;
    std::shared_ptr<BpTemplates> parent;
    std::unordered_map<std::string, std::vector<PropLayer>> vars;   // lower-case component name -> layers, highest priority first
};

Vec3 VecOf(const PropValue* v, Vec3 def = {}) {
    if (!v || !v->Fields()) return def;
    auto f = [&](const char* n, float d) { auto x = v->Field(n); return x ? (float)x->AsFloat(d) : d; };
    return {f("X", def.x), f("Y", def.y), f("Z", def.z)};
}
Vec2 Vec2Of(const PropValue* v, Vec2 def = {}) {
    if (!v || !v->Fields()) return def;
    auto f = [&](const char* n, float d) { auto x = v->Field(n); return x ? (float)x->AsFloat(d) : d; };
    return {f("X", def.x), f("Y", def.y)};
}

Mat4 LocalTransform(const CompView& c) {
    Vec3 loc = VecOf(c.Get("RelativeLocation"));
    Vec4 q{0, 0, 0, 1};
    if (auto r = c.Get("RelativeRotation"); r && r->Fields()) {
        auto f = [&](const char* n) { auto x = r->Field(n); return x ? (float)x->AsFloat() : 0.f; };
        q = RotatorToQuat(f("Pitch"), f("Yaw"), f("Roll"));
    }
    Vec3 s = VecOf(c.Get("RelativeScale3D"), {1, 1, 1});
    return Mat4::FromTRS(loc, q, s);
}

// object reference -> "/Game/Pkg.Obj" (empty for null)
std::string ObjectPathOf(const Package& pkg, const PropValue& v) {
    i32 idx = v.ObjIndex();
    if (idx == 0) return {};
    if (idx > 0) return pkg.PackageName() + "." + pkg.ObjectName(idx);
    return pkg.FullPath(idx);
}

bool IsMeshComponentClass(std::string_view cls) {
    return IEndsWith(cls, "StaticMeshComponent") || IEquals(cls, "SkeletalMeshComponent") || IEquals(cls, "SplineMeshComponent") ||
           IEquals(cls, "GlintComponent") || IEquals(cls, "DestructibleComponent") || IEquals(cls, "PoseableMeshComponent");
}
bool IsInstancedClass(std::string_view cls) { return IEndsWith(cls, "InstancedStaticMeshComponent"); }

std::string StemOf(std::string_view path) {
    size_t s = path.find_last_of('/');
    std::string_view n = s == std::string_view::npos ? path : path.substr(s + 1);
    size_t d = n.find('.');
    if (d != std::string_view::npos) n = n.substr(0, d);
    return ToLower(n);
}

bool EndsWithLevelSuffix(const std::string& stem) {
    static const char* skip[] = {"_talltales", "_design_talltales", "_design_arena", "_audio", "_calmwater", "_calmwaters", "_riddles"};
    for (auto s : skip)
        if (IEndsWith(stem, s)) return true;
    return false;
}

// helper / editor meshes that are not part of the visible scene (Athena extract_world SKIP_MESH_PAT)
bool IsHelperMesh(const std::string& meshPath) {
    std::string p = ToLower(meshPath);
    if (p.rfind("/engine/", 0) == 0) return true;
    if (p.find("/atmospherics/") != std::string::npos || p.find("/clouds/") != std::string::npos) return true;
    if (p.rfind("/game/maps/", 0) == 0 && p.find("/lod/") != std::string::npos) return true;   // HLOD stand-ins of whole levels / landscapes
    std::string n = StemOf(p);
    for (auto pre : {"volumetric_", "shape_", "cmn_shape_", "wil_fish_", "cyl", "sm_proxy_"})   // sm_proxy_: merged HLOD stand-in of a whole level
        if (n.rfind(pre, 0) == 0) return true;
    if (n == "cube" || n == "quad_plane" || n == "sphere" || n == "plane") return true;
    return n.find("procanim") != std::string::npos || n.find("splineeditormesh") != std::string::npos;
}

// FStaticMeshInstanceData bulk array (SoT: 64-byte FMatrix + 16 bytes of lightmap / shadowmap UV bias) in the native tail of an
// instanced component. Returns false when no instance array is found.
bool ReadInstances(const Package& pkg, i32 exportIdx, size_t nativeStart, std::vector<Mat4>& out) {
    const auto& ex = pkg.Exports()[exportIdx];
    size_t end = (size_t)ex.serialOffset + ex.serialSize;
    const u8* d = pkg.Data().data();
    for (size_t p = nativeStart; p + 8 <= end && p < nativeStart + 4096; p += 4) {
        i32 elem, count;
        memcpy(&elem, d + p, 4);
        memcpy(&count, d + p + 4, 4);
        if (elem != 80 || count < 0 || p + 8 + (size_t)count * 80 > end) continue;
        if (count == 0) return true;
        const u8* base = d + p + 8;
        float w;
        memcpy(&w, base + 60, 4);
        if (w != 1.f) continue;
        out.resize(count);
        for (i32 i = 0; i < count; i++) memcpy(out[i].m, base + (size_t)i * 80, 64);
        return true;
    }
    return false;
}

// paint layer name -> preview colour (Athena terrain fallbacks: sand, mud, grass, rock, seabed, coral)
u32 LayerColor(const std::string& layer) {
    std::string l = ToLower(layer);
    struct { const char* key; u32 rgb; } groups[] = {
        {"rock_coral", 0x7A756C}, {"coral", 0x7A756C}, {"seabed", 0xCDBE9A}, {"sand", 0xEBE2CD}, {"grass", 0x5C8A3C},
        {"dirt", 0x8A7B68}, {"mud", 0x8A7B68}, {"scree", 0x8A7B68}, {"rock", 0x8C8478}, {"cliff", 0x8C8478}, {"snow", 0xF0F0F4},
    };
    for (auto& g : groups)
        if (l.find(g.key) != std::string::npos) return g.rgb;
    return 0x9A9285;
}

bool IsProxyMesh(const std::string& meshPath) {
    std::string p = ToLower(meshPath);
    return p.rfind("/game/maps/", 0) == 0 && p.find("/lod/") != std::string::npos;
}

bool PassesFilter(const std::string& meshPath, const std::string& filter) {
    if (filter.empty()) return true;
    std::string p = ToLower(meshPath);
    bool anyInclude = false, included = false;
    size_t a = 0;
    while (a <= filter.size()) {
        size_t b = filter.find(',', a);
        if (b == std::string::npos) b = filter.size();
        std::string t = ToLower(filter.substr(a, b - a));
        a = b + 1;
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.empty()) continue;
        if (t[0] == '-') {
            if (t.size() > 1 && p.find(t.substr(1)) != std::string::npos) return false;
        } else {
            anyInclude = true;
            if (p.find(t) != std::string::npos) included = true;
        }
    }
    return !anyInclude || included;
}

}   // namespace

struct WorldLoader::Impl {
    PackageCache& cache;
    std::unordered_map<std::string, std::shared_ptr<BpTemplates>> bp;   // lower-case class package -> templates
    std::unordered_map<std::string, std::string> meshByName;           // lower-case mesh asset name -> object path
    bool meshIndexBuilt = false;
    std::map<std::pair<std::string, std::string>, size_t> groupIndex;   // (mesh, materials) -> group

    explicit Impl(PackageCache& c) : cache(c) {}

    std::shared_ptr<BpTemplates> Templates(const std::string& classPackage, int depth = 0) {
        std::string key = ToLower(classPackage);
        if (auto it = bp.find(key); it != bp.end()) return it->second;
        auto t = std::make_shared<BpTemplates>();
        bp[key] = t;   // placeholder against cycles
        auto pkg = depth < 16 ? cache.LoadPath(classPackage) : nullptr;
        if (!pkg) return t;
        t->pkgs.push_back(pkg);
        const auto& exps = pkg->Exports();
        // parent Blueprint class first (lowest priority)
        for (size_t i = 0; i < exps.size(); i++) {
            if (pkg->ExportClass(i) != "BlueprintGeneratedClass") continue;
            i32 sup = exps[i].superIndex;
            if (sup < 0) {
                std::string sp = pkg->ImportPackage(sup);
                if (IStartsWith(sp, "/Game/")) {
                    t->parent = Templates(sp, depth + 1);
                    t->vars = t->parent->vars;
                }
            }
            break;
        }
        auto push = [&](const std::string& var, i32 e) {
            t->storage.push_back(pkg->ReadProperties(e));
            auto& v = t->vars[ToLower(var)];
            v.insert(v.begin(), PropLayer{pkg.get(), e, &t->storage.back()});
        };
        auto isCdo = [&](i32 outer) { return outer > 0 && (exps[outer - 1].objectFlags & RF_ClassDefaultObject); };
        // components of the class default object (native components configured in the Blueprint)
        for (size_t i = 0; i < exps.size(); i++)
            if (isCdo(exps[i].outerIndex) && IEndsWith(pkg->ExportClass(i), "Component")) push(exps[i].objectName, (i32)i);
        // SimpleConstructionScript nodes: VariableName -> ComponentTemplate
        std::unordered_set<std::string> scsVars;
        for (size_t i = 0; i < exps.size(); i++) {
            if (pkg->ExportClass(i) != "SCS_Node") continue;
            PropertyList p = pkg->ReadProperties(i);
            i32 tmpl = PropObj(p, "ComponentTemplate");
            std::string var = PropStr(p, "VariableName");
            if (tmpl > 0 && !var.empty()) {
                push(var, tmpl - 1);
                scsVars.insert(ToLower(var));
            }
        }
        // '<variable>_GEN_VARIABLE' overrides of inherited components (InheritableComponentHandler) go over the parent's template
        for (size_t i = 0; i < exps.size(); i++) {
            const std::string& n = exps[i].objectName;
            if (!IEndsWith(n, "_GEN_VARIABLE") || !IEndsWith(pkg->ExportClass(i), "Component") || isCdo(exps[i].outerIndex)) continue;
            std::string var = n.substr(0, n.size() - 13);
            if (scsVars.count(ToLower(var))) continue;
            push(var, (i32)i);
        }
        return t;
    }

    // Blueprint-spawned static mesh components that carry no mesh reference are named after their mesh
    // ('wsp_rock_xtra_small__e2_0' -> 'wsp_rock_xtra_small__e', 'bsp_rock_medium_01_b' -> itself).
    std::string MeshFromComponentName(const std::string& name) {
        if (!meshIndexBuilt) {
            meshIndexBuilt = true;
            const Vfs& vfs = cache.FileSystem();
            const std::string_view prefix = "Athena/Content/Models/";
            for (u32 i = 0; i < (u32)vfs.FileCount(); i++) {
                std::string_view p = vfs.Path(i);
                if (p.size() <= prefix.size() || !IStartsWith(p, prefix) || !IEndsWith(p, ".uasset")) continue;
                std::string stem = StemOf(p);
                std::string pkgPath = "/Game/" + std::string(p.substr(15, p.size() - 15 - 7));
                meshByName.emplace(stem, pkgPath + "." + std::string(p.substr(p.find_last_of('/') + 1, p.size() - p.find_last_of('/') - 1 - 7)));
            }
        }
        std::string n = ToLower(name);
        auto stripDigits = [](std::string s) { while (!s.empty() && isdigit((u8)s.back())) s.pop_back(); return s; };
        auto stripIndex = [](std::string s) {
            size_t u = s.find_last_of('_');
            if (u != std::string::npos && u + 1 < s.size() && std::all_of(s.begin() + u + 1, s.end(), [](char c) { return isdigit((u8)c); })) s.resize(u);
            return s;
        };
        std::string a = stripIndex(n);
        std::string c[] = {n, a, stripDigits(a), stripDigits(n)};
        for (auto& k : c)
            if (auto it = meshByName.find(k); it != meshByName.end()) return it->second;
        std::string s = n;   // strip every trailing "_<digits>"
        for (std::string prev; prev != s;) { prev = s; s = stripIndex(s); }
        if (auto it = meshByName.find(s); it != meshByName.end()) return it->second;
        return {};
    }

    // Stacking order (bottom -> top, lower case) of the paint layers of a landscape material: its master material's
    // LandscapeLayerSwitch chain (each switch's LayerNotUsed leads to the layer below; the unique head is the top layer).
    std::unordered_map<std::string, std::vector<std::string>> layerOrders;
    const std::vector<std::string>& LayerOrder(const std::string& materialPath) {
        auto [it, fresh] = layerOrders.try_emplace(ToLower(materialPath));
        if (!fresh) return it->second;
        std::string path = materialPath;
        for (int depth = 0; depth < 8 && !path.empty(); depth++) {
            auto pkg = cache.LoadPath(path);
            if (!pkg) break;
            i32 main = pkg->MainExport();
            if (!main) break;
            std::string cls = pkg->ExportClass(main - 1);
            if (cls != "Material") {   // a material instance: follow Parent
                PropertyList mp = pkg->ReadProperties(main - 1);
                const PropValue* parent = FindValue(mp, "Parent");
                path = parent ? ObjectPathOf(*pkg, *parent) : std::string();
                continue;
            }
            std::unordered_map<i32, std::pair<std::string, i32>> sw;   // switch export -> (layer, switch below)
            for (size_t e = 0; e < pkg->Exports().size(); e++) {
                if (pkg->ExportClass(e) != "MaterialExpressionLandscapeLayerSwitch") continue;
                PropertyList sp = pkg->ReadProperties(e);
                const PropValue* nu = FindValue(sp, "LayerNotUsed");
                const PropValue* ex = nu ? nu->Field("Expression") : nullptr;
                sw[(i32)e + 1] = {ToLower(PropStr(sp, "ParameterName")), ex ? ex->ObjIndex() : 0};
            }
            std::unordered_set<i32> below;
            for (auto& [k, v] : sw) below.insert(v.second);
            std::vector<i32> heads;
            for (auto& [k, v] : sw)
                if (!below.count(k)) heads.push_back(k);
            if (heads.size() == 1) {
                std::unordered_set<i32> seen;
                for (i32 cur = heads[0]; sw.count(cur) && seen.insert(cur).second; cur = sw[cur].second) it->second.push_back(sw[cur].first);
                std::reverse(it->second.begin(), it->second.end());
            }
            break;
        }
        return it->second;
    }

    WorldMeshGroup& Group(WorldData& out, const std::string& mesh, const std::vector<std::string>& materials, bool skeletal) {
        std::string mk;
        for (auto& m : materials) mk += m + "|";
        auto key = std::make_pair(mesh, mk);
        auto it = groupIndex.find(key);
        if (it != groupIndex.end() && it->second < out.groups.size() && out.groups[it->second].mesh == mesh) return out.groups[it->second];
        groupIndex[key] = out.groups.size();
        auto& g = out.groups.emplace_back();
        g.mesh = mesh;
        g.materials = materials;
        g.skeletal = skeletal;
        return g;
    }
};

WorldLoader::WorldLoader(PackageCache& cache) : m(std::make_unique<Impl>(cache)) {}
WorldLoader::~WorldLoader() = default;

void WorldLoader::AddLevel(const Package& pkg, const Mat4& levelTransform, const WorldOptions& opt, WorldData& out) {
    const auto& exps = pkg.Exports();
    const size_t n = exps.size();
    u32 levelIndex = (u32)out.levels.size();
    WorldLevel& lvl = out.levels.emplace_back();
    lvl.path = pkg.GamePath();
    lvl.transform = levelTransform;

    std::vector<std::string> cls(n);
    for (size_t i = 0; i < n; i++) cls[i] = pkg.ExportClass(i);
    // properties of components and actors (read once)
    std::vector<PropertyList> props(n);
    std::vector<size_t> nativeStart(n, 0);
    std::vector<char> loaded(n, 0);
    auto Props = [&](size_t i) -> const PropertyList& {
        if (!loaded[i]) {
            loaded[i] = 1;
            props[i] = pkg.ReadProperties(i, &nativeStart[i]);
        }
        return props[i];
    };
    // Blueprint templates of the actor that owns a component
    auto TemplatesOf = [&](i32 actor) -> std::shared_ptr<BpTemplates> {
        if (actor <= 0) return nullptr;
        i32 c = exps[actor - 1].classIndex;
        if (c >= 0) return nullptr;   // native class or the level's own script class
        std::string p = pkg.ImportPackage(c);
        if (!IStartsWith(p, "/Game/")) return nullptr;
        return m->Templates(p);
    };
    std::vector<CompView> views(n);
    std::vector<char> viewBuilt(n, 0);
    auto View = [&](size_t i) -> const CompView& {
        if (!viewBuilt[i]) {
            viewBuilt[i] = 1;
            CompView& v = views[i];
            v.layers.push_back({&pkg, (i32)i, &Props(i)});
            if (auto t = TemplatesOf(exps[i].outerIndex)) {
                auto it = t->vars.find(ToLower(exps[i].objectName));
                if (it != t->vars.end()) v.layers.insert(v.layers.end(), it->second.begin(), it->second.end());
            }
        }
        return views[i];
    };
    // component -> level space (AttachParent chain)
    std::vector<Mat4> world(n);
    std::vector<char> worldState(n, 0);   // 0 todo, 1 in progress, 2 done
    std::function<Mat4(size_t)> World = [&](size_t i) -> Mat4 {
        if (worldState[i] == 2) return world[i];
        if (worldState[i] == 1) return Mat4::Identity();   // cycle guard
        worldState[i] = 1;
        const CompView& v = View(i);
        Mat4 mtx = LocalTransform(v);
        const PropLayer* from = nullptr;
        if (auto ap = v.Get("AttachParent", &from)) {
            i32 parent = 0;
            if (from->pkg == &pkg) parent = ap->ObjIndex();
            else {   // a template's reference: the sibling component of the same actor with that name
                std::string name = from->pkg->ObjectName(ap->ObjIndex());
                for (size_t k = 0; k < n && !parent; k++)
                    if (exps[k].outerIndex == exps[i].outerIndex && IEquals(exps[k].objectName, name)) parent = (i32)k + 1;
            }
            if (parent > 0 && (size_t)parent - 1 != i && !v.Bool("bAbsoluteLocation", false)) mtx = mtx * World(parent - 1);
        }
        world[i] = mtx;
        worldState[i] = 2;
        return mtx;
    };

    auto MaterialsOf = [&](const CompView& v) {
        std::vector<std::string> mats;
        const PropLayer* from = nullptr;
        if (auto om = v.Get("OverrideMaterials", &from); om && om->type == PropType::Array) {
            for (auto& it : om->items) mats.push_back(ObjectPathOf(*from->pkg, it));
            while (!mats.empty() && mats.back().empty()) mats.pop_back();
        }
        return mats;
    };
    auto DrawDistance = [&](const CompView& v) {
        float d = v.Float("CachedMaxDrawDistance");
        if (d <= 0) d = v.Float("LDMaxDrawDistance");
        float e = v.Float("InstanceEndCullDistance");
        if (e > 0 && (d <= 0 || e < d)) d = e;
        return d > 0 ? d : 0.f;
    };
    auto Note = [&](const Mat4& w) {
        Vec3 t = w.Translation();
        out.boundsMin = {std::min(out.boundsMin.x, t.x), std::min(out.boundsMin.y, t.y), std::min(out.boundsMin.z, t.z)};
        out.boundsMax = {std::max(out.boundsMax.x, t.x), std::max(out.boundsMax.y, t.y), std::max(out.boundsMax.z, t.z)};
    };
    auto AddInstance = [&](WorldMeshGroup& g, const Mat4& w, float dist) {
        g.instances.push_back({w, levelIndex});
        g.drawDistance = (g.instances.size() == 1 || (g.drawDistance > 0 && dist > 0)) ? std::max(g.drawDistance, dist) : 0.f;
        lvl.instances++;
        Note(w);
    };

    // ---- Landscape: one mesh per component from its heightmap (R<<8|G, 32768 = 0, 1/128 per unit) and paint-layer weightmaps
    std::unordered_map<i32, std::shared_ptr<Image>> texCache;
    auto Texture = [&](i32 idx) -> std::shared_ptr<Image> {
        if (idx <= 0) return nullptr;
        if (auto it = texCache.find(idx); it != texCache.end()) return it->second;
        auto img = std::make_shared<Image>();
        TextureInfo ti;
        std::string err;
        if (!ReadTextureInfo(pkg, idx - 1, ti, &err) || !DecodeTexture(pkg, ti, 0, 1 << 16, *img, &err)) {
            LOGW("landscape texture %s: %s", pkg.ObjectName(idx).c_str(), err.c_str());
            img = nullptr;
        }
        texCache[idx] = img;
        return img;
    };
    auto AddLandscape = [&](size_t i) {
        const PropertyList& P = Props(i);
        int nsub = (int)PropInt(P, "NumSubsections", 1);
        int subq = (int)PropInt(P, "SubsectionSizeQuads", 0);
        int cq = (int)PropInt(P, "ComponentSizeQuads", nsub * subq);
        if (subq <= 0 || cq <= 0) return;
        auto hm = Texture(PropObj(P, "HeightmapTexture"));
        if (!hm) return;
        auto bias = [&](std::string_view prop, const Image& img, int& ox, int& oy) {
            ox = oy = 0;
            if (auto sb = FindValue(P, prop)) {
                if (auto z = sb->Field("Z")) ox = (int)lroundf((float)z->AsFloat() * img.width);
                if (auto w = sb->Field("W")) oy = (int)lroundf((float)w->AsFloat() * img.height);
            }
        };
        int hx, hy;
        bias("HeightmapScaleBias", *hm, hx, hy);
        auto texel = [&](int vtx) { int s = std::min(vtx / subq, nsub - 1); return s * (subq + 1) + (vtx - s * subq); };
        int step = std::max(1, opt.landscapeStep);
        while (step > 1 && cq % step) step--;
        int side = cq / step + 1;
        // paint layers
        struct Layer { std::shared_ptr<Image> img; int ch, ox, oy; u32 rgb; bool visibility; int rank; };
        std::vector<Layer> layers;
        const std::vector<std::string>* order = nullptr;
        if (exps[i].outerIndex > 0) {
            const PropValue* lm = FindValue(Props(exps[i].outerIndex - 1), "LandscapeMaterial");
            std::string mpath = lm ? ObjectPathOf(pkg, *lm) : std::string();
            if (!mpath.empty()) order = &m->LayerOrder(mpath);
        }
        WorldLandscape& L = out.landscapes.emplace_back();
        if (const PropValue* wt = FindValue(P, "WeightmapTextures"); wt && wt->type == PropType::Array) {
            std::vector<std::shared_ptr<Image>> wimg;
            for (auto& t : wt->items) wimg.push_back(Texture(t.ObjIndex()));
            if (const PropValue* al = FindValue(P, "WeightmapLayerAllocations"); al && al->type == PropType::Array)
                for (auto& a : al->items) {
                    if (!a.Fields()) continue;
                    auto li = a.Field("LayerInfo");
                    auto ti = a.Field("WeightmapTextureIndex");
                    auto tc = a.Field("WeightmapTextureChannel");
                    size_t t = ti ? (size_t)ti->AsInt() : 0;
                    if (!li || t >= wimg.size() || !wimg[t]) continue;
                    std::string name = pkg.ObjectName(li->ObjIndex());
                    if (IEndsWith(name, "_LayerInfo")) name.resize(name.size() - 10);
                    if (ToLower(name).find("grass_paint") != std::string::npos) continue;   // foliage density masks, not a surface
                    // "DataLayer" is the landscape visibility layer: painted areas are holes
                    int rank = -1;
                    if (order) {
                        auto f = std::find(order->begin(), order->end(), ToLower(name));
                        if (f != order->end()) rank = (int)(f - order->begin());
                    }
                    Layer l{wimg[t], tc ? (int)tc->AsInt() & 3 : 0, 0, 0, LayerColor(name), IEquals(name, "DataLayer") || IEquals(name, "DataLayer__"), rank};
                    bias("WeightmapScaleBias", *wimg[t], l.ox, l.oy);
                    if (l.visibility) layers.insert(layers.begin(), l);
                    else layers.push_back(l);
                    L.layers.push_back(name);
                }
        }
        // stacked layers bottom -> top after the visibility layer, then the ones the material does not stack
        std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) {
            auto key = [](const Layer& l) { return l.visibility ? -2 : l.rank < 0 ? 1 << 20 : l.rank; };
            return key(a) < key(b);
        });
        std::vector<float> wv(layers.size());
        Mat4 w = World(i) * levelTransform;
        L.name = pkg.ObjectName(exps[i].outerIndex) + "." + exps[i].objectName;
        L.level = levelIndex;
        MeshLod& m = L.mesh;
        m.positions.resize((size_t)side * side);
        m.colors.resize(m.positions.size());
        m.numUVs = 1;
        m.uvs.resize(m.positions.size());
        std::vector<u8> hole;
        if (!layers.empty() && layers[0].visibility) hole.assign(m.positions.size(), 0);
        Vec3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++) {
                int vx = x * step, vy = y * step;
                int tx = std::clamp(hx + texel(vx), 0, hm->width - 1), ty = std::clamp(hy + texel(vy), 0, hm->height - 1);
                const u8* px = &hm->rgba[((size_t)ty * hm->width + tx) * 4];
                float z = (float)((int)((px[0] << 8) | px[1]) - 32768) / 128.f;
                Vec3 p = w.TransformPoint({(float)vx, (float)vy, z});
                size_t k = (size_t)y * side + x;
                m.positions[k] = p;
                m.uvs[k] = {(float)vx / cq, (float)vy / cq};
                bmin = {std::min(bmin.x, p.x), std::min(bmin.y, p.y), std::min(bmin.z, p.z)};
                bmax = {std::max(bmax.x, p.x), std::max(bmax.y, p.y), std::max(bmax.z, p.z)};
                for (size_t li = 0; li < layers.size(); li++) {
                    const Layer& l = layers[li];
                    int lx = std::clamp(l.ox + texel(vx), 0, l.img->width - 1), ly = std::clamp(l.oy + texel(vy), 0, l.img->height - 1);
                    u8 raw = l.img->rgba[((size_t)ly * l.img->width + lx) * 4 + l.ch];
                    if (l.visibility) hole[k] = raw;
                    wv[li] = raw / 255.f;
                }
                float r = 0, g = 0, b = 0, sum = 0;
                auto add = [&](u32 rgb, float wgt) {
                    r += wgt * ((rgb >> 16) & 255); g += wgt * ((rgb >> 8) & 255); b += wgt * (rgb & 255);
                    sum += wgt;
                };
                // the material lerps each stacked layer over the ones below by its own weight
                int firstStacked = -1, lastStacked = -1;
                for (size_t li = 0; li < layers.size(); li++)
                    if (!layers[li].visibility && layers[li].rank >= 0) { if (firstStacked < 0) firstStacked = (int)li; lastStacked = (int)li; }
                if (firstStacked >= 0) {
                    float remaining = 1;
                    for (int li = lastStacked; li >= firstStacked; li--) {
                        add(layers[li].rgb, wv[li] * remaining);
                        remaining *= 1 - wv[li];
                    }
                    add(layers[firstStacked].rgb, remaining);
                }
                for (size_t li = 0; li < layers.size(); li++)   // layers outside the stack: weighted as painted
                    if (!layers[li].visibility && layers[li].rank < 0) add(layers[li].rgb, wv[li]);
                if (sum < 1) {   // the base layer takes the remainder
                    u32 base = 0x9A9285;
                    for (auto& l : layers)
                        if (!l.visibility) { base = l.rgb; break; }
                    add(base, 1 - sum);
                }
                m.colors[k] = (u32)(r / sum) | ((u32)(g / sum) << 8) | ((u32)(b / sum) << 16) | 0xff000000u;
            }
        // normals from the height grid (world space)
        m.normals.resize(m.positions.size());
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++) {
                const Vec3& px0 = m.positions[(size_t)y * side + std::max(x - 1, 0)];
                const Vec3& px1 = m.positions[(size_t)y * side + std::min(x + 1, side - 1)];
                const Vec3& py0 = m.positions[(size_t)std::max(y - 1, 0) * side + x];
                const Vec3& py1 = m.positions[(size_t)std::min(y + 1, side - 1) * side + x];
                m.normals[(size_t)y * side + x] = Normalize(Cross(px1 - px0, py1 - py0));
            }
        m.indices.reserve((size_t)(side - 1) * (side - 1) * 6);
        for (int y = 0; y + 1 < side; y++)
            for (int x = 0; x + 1 < side; x++) {
                u32 i00 = y * side + x, i10 = i00 + 1, i01 = i00 + side, i11 = i01 + 1;
                if (!hole.empty() && (int)hole[i00] + hole[i10] + hole[i01] + hole[i11] >= 2 * 255) continue;
                m.indices.insert(m.indices.end(), {i00, i11, i10, i00, i01, i11});
            }
        if (m.indices.empty()) {   // entirely a hole
            out.landscapes.pop_back();
            return;
        }
        MeshSection sec;
        sec.numTriangles = (u32)m.indices.size() / 3;
        sec.maxVertex = (u32)m.positions.size() - 1;
        m.sections.push_back(sec);
        L.boundsMin = bmin;
        L.boundsMax = bmax;
        out.boundsMin = {std::min(out.boundsMin.x, bmin.x), std::min(out.boundsMin.y, bmin.y), std::min(out.boundsMin.z, bmin.z)};
        out.boundsMax = {std::max(out.boundsMax.x, bmax.x), std::max(out.boundsMax.y, bmax.y), std::max(out.boundsMax.z, bmax.z)};
        lvl.landscapeComponents++;
    };

    // ---- lights, decals, height fog, the water plane. Returns true when the component was one of them.
    auto AddEffect = [&](size_t i) -> bool {
        const std::string& c = cls[i];
        bool point = IEquals(c, "PointLightComponent"), spot = IEquals(c, "SpotLightComponent"), dir = IEquals(c, "DirectionalLightComponent"),
             sky = IEquals(c, "SkyLightComponent");
        if (point || spot || dir || sky) {
            const CompView& v = View(i);
            if (!v.Bool("bVisible", true)) return true;
            Mat4 w = World(i) * levelTransform;
            WorldLight& L = out.lights.emplace_back();
            L.type = spot ? WorldLight::Spot : dir ? WorldLight::Directional : sky ? WorldLight::Sky : WorldLight::Point;
            L.position = w.Translation();
            L.direction = Normalize(w.TransformDir({1, 0, 0}));
            if (auto col = v.Get("LightColor"); col && col->Fields()) {
                auto ch = [&](const char* n) { auto x = col->Field(n); return x ? (float)x->AsFloat(255) / 255.f : 1.f; };
                auto lin = [](float x) { return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f); };
                L.color = {lin(ch("R")), lin(ch("G")), lin(ch("B"))};
            }
            L.intensity = v.Float("Intensity", dir ? 3.14159f : sky ? 1.f : 5000.f);
            L.radius = v.Float("AttenuationRadius", 1000);
            L.innerCone = v.Float("InnerConeAngle", 0);
            L.outerCone = v.Float("OuterConeAngle", 44);
            L.inverseSquared = v.Bool("bUseInverseSquaredFalloff", true);
            L.castShadows = v.Bool("CastShadows", true);
            L.level = levelIndex;
            return true;
        }
        if (IEquals(c, "DecalComponent")) {
            const CompView& v = View(i);
            if (!v.Bool("bVisible", true) || v.Bool("bHiddenInGame", false)) return true;
            WorldDecal& d = out.decals.emplace_back();
            const PropLayer* from = nullptr;
            if (auto m = v.Get("DecalMaterial", &from)) d.material = ObjectPathOf(*from->pkg, *m);
            d.transform = World(i) * levelTransform;
            d.size = VecOf(v.Get("DecalSize"), {1, 1, 1});
            d.level = levelIndex;
            return true;
        }
        if (IEquals(c, "ExponentialHeightFogComponent")) {
            const CompView& v = View(i);
            WorldFog& f = out.fogs.emplace_back();
            f.position = (World(i) * levelTransform).Translation();
            f.density = v.Float("FogDensity", 0.02f);
            f.heightFalloff = v.Float("FogHeightFalloff", 0.2f);
            f.maxOpacity = v.Float("FogMaxOpacity", 1);
            f.startDistance = v.Float("StartDistance", 0);
            f.underwater = ToLower(exps[i].objectName).find("underwater") != std::string::npos;
            if (auto col = v.Get("FogInscatteringColor"); col && col->Fields()) {
                auto ch = [&](const char* n, float d) { auto x = col->Field(n); return x ? (float)x->AsFloat(d) : d; };
                f.color = {ch("R", 0.447f), ch("G", 0.639f), ch("B", 1.f)};
            }
            f.level = levelIndex;
            return true;
        }
        if (IEquals(c, "FFTWaterComponent")) {
            float z = (World(i) * levelTransform).Translation().z;
            if (!out.hasWater) out.waterLevel = z;
            out.hasWater = true;
            return true;
        }
        return false;
    };

    std::string levelName = StemOf(pkg.GamePath());
    for (size_t i = 0; i < n; i++) {
        if (opt.cancel && (i & 63) == 0 && opt.cancel()) return;
        const std::string& c = cls[i];
        if (exps[i].outerIndex > 0 && IEquals(exps[exps[i].outerIndex - 1].objectName, "PersistentLevel")) lvl.actors++;
        if (IEquals(c, "LandscapeComponent")) {
            if (opt.landscape) AddLandscape(i);
            continue;
        }
        if (opt.lights && !(exps[i].objectFlags & (RF_ClassDefaultObject | RF_ArchetypeObject)) && AddEffect(i)) continue;
        if (!IsMeshComponentClass(c)) continue;
        if (exps[i].objectFlags & (RF_ClassDefaultObject | RF_ArchetypeObject)) continue;
        bool skeletal = IEquals(c, "SkeletalMeshComponent") || IEquals(c, "PoseableMeshComponent") || IEquals(c, "DestructibleComponent");
        if (skeletal && !opt.skeletal) continue;
        const CompView& v = View(i);
        if (!v.Bool("bVisible", true) || v.Bool("bHiddenInGame", false)) continue;
        lvl.meshComponents++;

        const PropLayer* from = nullptr;
        const PropValue* sm = v.Get(skeletal ? "SkeletalMesh" : "StaticMesh", &from);
        if (!sm && skeletal) sm = v.Get("DestructibleMesh", &from);
        std::string mesh = sm ? ObjectPathOf(*from->pkg, *sm) : std::string();
        if (mesh.empty()) {
            // a piece already baked into its actor's MergedStaticMeshComponent has no mesh reference of its own
            if (auto ap = v.Get("AttachParent", &from); ap && from->pkg == &pkg && ap->ObjIndex() > 0 && IEquals(cls[ap->ObjIndex() - 1], "MergedStaticMeshComponent"))
                continue;
            if (IEquals(c, "StaticMeshComponent") && TemplatesOf(exps[i].outerIndex)) mesh = m->MeshFromComponentName(exps[i].objectName);
            if (mesh.empty()) {
                if (!IsInstancedClass(c)) {
                    lvl.unresolved++;
                    if (out.unresolved.size() < 5000) out.unresolved.push_back(levelName + ": " + pkg.ObjectName(exps[i].outerIndex) + "." + exps[i].objectName);
                }
                continue;
            }
        }
        if (opt.skipNonVisual && IsHelperMesh(mesh) && !(opt.proxies && IsProxyMesh(mesh))) continue;
        if (!PassesFilter(mesh, opt.meshFilter)) continue;
        Mat4 w = World(i) * levelTransform;
        std::vector<std::string> mats = MaterialsOf(v);
        float dist = DrawDistance(v);

        if (IEquals(c, "SplineMeshComponent")) {
            auto& s = out.splines.emplace_back();
            s.mesh = mesh;
            s.materials = mats;
            s.transform = w;
            s.level = levelIndex;
            if (auto sp = v.Get("SplineParams")) {
                s.params.startPos = VecOf(sp->Field("StartPos"));
                s.params.startTangent = VecOf(sp->Field("StartTangent"));
                s.params.startScale = Vec2Of(sp->Field("StartScale"), {1, 1});
                s.params.startOffset = Vec2Of(sp->Field("StartOffset"));
                s.params.startRoll = sp->Field("StartRoll") ? (float)sp->Field("StartRoll")->AsFloat() : 0.f;
                s.params.endPos = VecOf(sp->Field("EndPos"));
                s.params.endTangent = VecOf(sp->Field("EndTangent"));
                s.params.endScale = Vec2Of(sp->Field("EndScale"), {1, 1});
                s.params.endOffset = Vec2Of(sp->Field("EndOffset"));
                s.params.endRoll = sp->Field("EndRoll") ? (float)sp->Field("EndRoll")->AsFloat() : 0.f;
            }
            s.params.upDir = VecOf(v.Get("SplineUpDir"), {0, 0, 1});
            if (auto fa = v.Get("ForwardAxis")) {
                std::string a = fa->str;
                s.params.forwardAxis = a.empty() ? (int)fa->AsInt() : (a.back() == 'Y' ? 1 : a.back() == 'Z' ? 2 : 0);
            }
            lvl.splines++;
            Note(w);
            continue;
        }
        if (IsInstancedClass(c)) {
            std::vector<Mat4> inst;
            Props(i);
            bool found = ReadInstances(pkg, (i32)i, nativeStart[i], inst);
            if ((!found || inst.empty()) && v.layers.size() > 1) {   // instances authored in the Blueprint template
                for (size_t l = 1; l < v.layers.size() && inst.empty(); l++) {
                    size_t ns = 0;
                    v.layers[l].pkg->ReadProperties(v.layers[l].exportIdx, &ns);
                    ReadInstances(*v.layers[l].pkg, v.layers[l].exportIdx, ns, inst);
                }
            }
            if (inst.empty()) continue;
            auto& g = m->Group(out, mesh, mats, false);
            for (auto& im : inst) AddInstance(g, im * w, dist);
            continue;
        }
        AddInstance(m->Group(out, mesh, mats, skeletal), w, dist);
    }
}

std::vector<std::string> WorldLoader::MapLevels(std::string_view mapPath, bool skipNonVisual) const {
    const Vfs& vfs = m->cache.FileSystem();
    std::vector<std::string> res;
    i64 e = vfs.Find(mapPath);
    if (e < 0) e = vfs.FindPackage(mapPath);
    if (e < 0) return res;
    std::string root(vfs.Path((u32)e));
    res.push_back(root);
    std::string dir = root.substr(0, root.find_last_of('/') + 1);
    std::string folder = ToLower(dir.substr(0, dir.size() - 1));
    folder = folder.substr(folder.find_last_of('/') + 1);
    if (folder == "sharedassets" || folder == "streaming_levels" || folder == "lod") return res;   // a sublevel opened on its own
    for (u32 i = 0; i < (u32)vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (p.size() <= dir.size() || !IStartsWith(p, dir) || !IEndsWith(p, ".umap") || i == (u32)e) continue;
        std::string_view rel = p.substr(dir.size());
        if (rel.find('/') == std::string_view::npos) continue;   // other persistent maps next to this one
        std::string lrel = ToLower(rel);
        if (lrel.find("lod/") != std::string::npos || lrel.find("layerinfo") != std::string::npos) continue;
        if (skipNonVisual && EndsWithLevelSuffix(StemOf(p))) continue;
        res.emplace_back(p);
    }
    std::sort(res.begin() + 1, res.end());
    return res;
}

// The islands' LOD proxy levels ("<island>/sharedassets/LOD/<level>lod1.umap"): the lowest LOD of every base level.
static std::vector<std::string> ProxyLevels(const Vfs& vfs, const std::string& islandDir) {
    std::map<std::string, std::pair<int, std::string>> best;   // base level -> (lod, path)
    std::string dir = islandDir + "/";
    for (u32 i = 0; i < (u32)vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (!IStartsWith(p, dir) || !IEndsWith(p, ".umap")) continue;
        std::string lp = ToLower(p);
        if (lp.find("/lod/") == std::string::npos) continue;
        std::string stem = StemOf(p);
        size_t k = stem.rfind("lod");
        if (k == std::string::npos) continue;
        int lod = atoi(stem.c_str() + k + 3);
        if (EndsWithLevelSuffix(stem.substr(0, k))) continue;
        auto& b = best[stem.substr(0, k)];
        if (b.second.empty() || lod < b.first) b = {lod, std::string(p)};
    }
    std::vector<std::string> res;
    for (auto& [k, v] : best) res.push_back(v.second);
    return res;
}

bool WorldLoader::Load(std::string_view mapPath, const WorldOptions& opt, WorldData& out, std::string* error) {
    out = WorldData{};
    m->groupIndex.clear();
    if (opt.sublevels && IsWorldLayoutMap(mapPath)) {
        const WorldLayout* L = GetWorldLayout(m->cache, error);
        if (!L) return false;
        struct Item { std::string path, island; Mat4 t; };
        std::vector<Item> items;
        // the layout's own sublevels (sea rocks, world regions, ...); with an island filter only those it names
        for (auto& l : MapLevels(L->layoutMap, opt.skipNonVisual))
            if (IStartsWith(StemOf(l), "sot_world_layout_2") && (opt.islands.empty() ? !opt.region : PassesFilter(l, opt.islands)))
                items.push_back({l, {}, Mat4::Identity()});
        for (auto& isl : L->islands) {
            if (!PassesFilter(isl.name + " " + isl.dir + " " + isl.sea, opt.islands)) continue;
            if (opt.region) {
                Vec3 c = isl.radius > 0 ? isl.centre : isl.location;
                if (c.x < opt.regionMin.x || c.y < opt.regionMin.y || c.x > opt.regionMax.x || c.y > opt.regionMax.y) continue;
            }
            std::vector<std::string> lv;
            if (opt.proxies) lv = ProxyLevels(m->cache.FileSystem(), isl.dir);
            if (lv.empty() && !isl.map.empty()) lv = MapLevels(isl.map, opt.skipNonVisual);
            for (auto& l : lv) {
                Item it{l, isl.name, isl.Transform()};
                L->LevelTransform(l, it.t);
                items.push_back(std::move(it));
            }
        }
        if (opt.lights)   // the open world's one FFT water actor and its sun / sky light / height fog
            for (const char* p : {"/Game/Maps/wide_sea_of_plunder/wsp_common/wsp_water", "/Game/Maps/wide_sea_of_plunder/wsp_common/wsp_lighting"})
                if (i64 e = m->cache.FileSystem().FindPackage(p); e >= 0) items.push_back({std::string(m->cache.FileSystem().Path((u32)e)), {}, Mat4::Identity()});
        out.root = L->layoutMap;
        for (size_t i = 0; i < items.size(); i++) {
            if (opt.cancel && opt.cancel()) { if (error) *error = "cancelled"; return false; }
            if (opt.progress) opt.progress((int)i, (int)items.size(), items[i].path);
            std::string err;
            auto pkg = m->cache.LoadPath(items[i].path, &err);
            if (!pkg) { LOGW("world: %s: %s", items[i].path.c_str(), err.c_str()); continue; }
            AddLevel(*pkg, items[i].t, opt, out);
            out.levels.back().island = items[i].island;
        }
        if (opt.progress) opt.progress((int)items.size(), (int)items.size(), {});
        if (out.InstanceCount() == 0 && out.landscapes.empty()) out.boundsMin = out.boundsMax = {};
        return true;
    }
    std::vector<std::string> levels = opt.sublevels ? MapLevels(mapPath, opt.skipNonVisual) : std::vector<std::string>{};
    if (levels.empty()) {
        i64 e = m->cache.FileSystem().Find(mapPath);
        if (e < 0) e = m->cache.FileSystem().FindPackage(mapPath);
        if (e < 0) { if (error) *error = "map not found: " + std::string(mapPath); return false; }
        levels.emplace_back(m->cache.FileSystem().Path((u32)e));
    }
    out.root = levels[0];
    for (size_t i = 0; i < levels.size(); i++) {
        if (opt.cancel && opt.cancel()) { if (error) *error = "cancelled"; return false; }
        if (opt.progress) opt.progress((int)i, (int)levels.size(), levels[i]);
        std::string err;
        auto pkg = m->cache.LoadPath(levels[i], &err);
        if (!pkg) {
            LOGW("world: %s: %s", levels[i].c_str(), err.c_str());
            continue;
        }
        if (i == 0 && !pkg->FindExport("PersistentLevel")) { if (error) *error = "not a map: " + levels[i]; return false; }
        AddLevel(*pkg, Mat4::Identity(), opt, out);
    }
    if (opt.progress) opt.progress((int)levels.size(), (int)levels.size(), {});
    if (out.InstanceCount() == 0 && out.landscapes.empty()) out.boundsMin = out.boundsMax = {};
    return true;
}
