#include "ue/World.h"
#include "core/Vfs.h"
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
    std::string n = StemOf(p);
    for (auto pre : {"volumetric_", "shape_", "cmn_shape_", "wil_fish_", "cyl"})
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

    std::string levelName = StemOf(pkg.GamePath());
    for (size_t i = 0; i < n; i++) {
        if (opt.cancel && (i & 63) == 0 && opt.cancel()) return;
        const std::string& c = cls[i];
        if (exps[i].outerIndex > 0 && IEquals(exps[exps[i].outerIndex - 1].objectName, "PersistentLevel")) lvl.actors++;
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
        if (opt.skipNonVisual && IsHelperMesh(mesh)) continue;
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

bool WorldLoader::Load(std::string_view mapPath, const WorldOptions& opt, WorldData& out, std::string* error) {
    out = WorldData{};
    m->groupIndex.clear();
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
    if (out.InstanceCount() == 0) out.boundsMin = out.boundsMax = {};
    return true;
}
