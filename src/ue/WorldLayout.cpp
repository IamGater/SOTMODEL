#include "ue/WorldLayout.h"
#include "core/Vfs.h"
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace {

constexpr const char* kLayoutMap = "/Game/Maps/World_Layout/sot_world_layout_2";
constexpr const char* kIslandsAsset = "/Game/DataAssets/Island/Generated/sot_world_layout_2_generatedislands";

struct Volume { std::string name, origin; Vec3 loc, extent; float yaw = 0; bool hasExtent = false; };

// FBoxSphereBounds at the start of a UModel's native data (after the tagged properties and an optional object guid).
bool ModelExtent(const Package& pkg, size_t exportIdx, Vec3& ext) {
    size_t end = 0;
    pkg.ReadProperties(exportIdx, &end);
    const auto& ex = pkg.Exports()[exportIdx];
    size_t limit = (size_t)ex.serialOffset + ex.serialSize;
    for (size_t skip = 0; skip <= 24; skip++) {   // SoT: bool guid flag + 2 bytes before the bounds
        size_t at = end + skip;
        if (at + 28 > limit || at + 28 > pkg.Data().size()) continue;
        float f[7];
        memcpy(f, pkg.Data().data() + at, sizeof f);
        float len = std::sqrt(f[3] * f[3] + f[4] * f[4] + f[5] * f[5]);
        if (!(len > 1 && len < 1e7f && std::abs(len - f[6]) < 0.01f * len + 1)) continue;
        if (!(std::abs(f[0]) < 1e7f && std::abs(f[1]) < 1e7f && std::abs(f[2]) < 1e7f)) continue;
        ext = {f[3], f[4], f[5]};
        return true;
    }
    return false;
}

// "dvr_outpost_1_navmeshbounds_34" -> "dvr_outpost_1_navmeshbounds"
std::string StripNumber(const std::string& n) {
    size_t u = n.find_last_of('_');
    if (u != std::string::npos && u + 1 < n.size() && n.find_first_not_of("0123456789", u + 1) == std::string::npos) return n.substr(0, u);
    return n;
}

Vec3 VecField(const PropValue* v) {
    if (!v || !v->Fields()) return {};
    auto f = [&](const char* n) { auto x = v->Field(n); return x ? (float)x->AsFloat() : 0.f; };
    return {f("X"), f("Y"), f("Z")};
}

std::vector<Volume> ReadVolumes(const Package& pkg) {
    std::vector<Volume> res;
    for (size_t i = 0; i < pkg.Exports().size(); i++) {
        std::string cls = pkg.ExportClass(i);
        if (cls != "NavMeshBoundsVolume" && cls != "InstancedNavMeshBoundsVolume") continue;
        PropertyList p = pkg.ReadProperties(i);
        i32 comp = PropObj(p, "BrushComponent");
        if (comp <= 0) comp = PropObj(p, "RootComponent");
        if (comp <= 0 || (size_t)comp > pkg.Exports().size()) continue;
        PropertyList c = pkg.ReadProperties((size_t)comp - 1);
        Volume v;
        v.name = ToLower(pkg.Exports()[i].objectName);
        v.origin = ToLower(PropStr(p, "OriginatingLevelName"));
        v.loc = VecField(FindValue(c, "RelativeLocation"));
        if (auto r = FindValue(c, "RelativeRotation"); r && r->Fields())
            if (auto y = r->Field("Yaw")) v.yaw = (float)y->AsFloat();
        if (i32 b = PropObj(p, "Brush"); b > 0 && (size_t)b <= pkg.Exports().size()) {
            v.hasExtent = ModelExtent(pkg, (size_t)b - 1, v.extent);
            Vec3 sc = VecField(FindValue(c, "RelativeScale3D"));
            if (FindValue(c, "RelativeScale3D")) v.extent = {v.extent.x * sc.x, v.extent.y * sc.y, v.extent.z * sc.z};
        }
        res.push_back(std::move(v));
    }
    return res;
}

std::string DirOf(std::string_view p) { size_t s = p.find_last_of('/'); return std::string(s == std::string_view::npos ? std::string_view{} : p.substr(0, s)); }
std::string LeafOf(std::string_view p) { size_t s = p.find_last_of('/'); return std::string(s == std::string_view::npos ? p : p.substr(s + 1)); }

// "<island>/sharedassets/x.umap" -> "<island>"
std::string IslandDirOf(std::string_view levelPath) {
    std::string d = DirOf(levelPath);
    std::string leaf = ToLower(LeafOf(d));
    if (leaf == "lod") { d = DirOf(d); leaf = ToLower(LeafOf(d)); }
    if (leaf == "sharedassets" || leaf == "streaming_levels") return DirOf(d);
    return d;
}

// OriginatingLevelName -> the level file. A repeated level instance has a "_N" suffix, some hub levels lack the
// sharedassets folder segment.
i64 ResolveLevel(const Vfs& vfs, const std::string& name) {
    if (i64 e = vfs.FindPackage(name); e >= 0) return e;
    std::string dir = DirOf(name), leaf = LeafOf(name), base = leaf;
    size_t u = base.find_last_of('_');
    if (u != std::string::npos && u + 1 < base.size() && base.find_first_not_of("0123456789", u + 1) == std::string::npos) base.resize(u);
    for (const std::string& l : {leaf, base})
        for (const std::string& d : {dir, dir + "/sharedassets"})
            if (i64 e = vfs.FindPackage(d + "/" + l); e >= 0) return e;
    return -1;
}

// file name without folder and extension
std::string StemOfPath(std::string_view p) {
    std::string n = LeafOf(p);
    size_t d = n.find('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

float RoundTo(float v, float step) { return std::round(v / step) * step; }

std::mutex g_mutex;
std::unordered_map<const PackageCache*, std::unique_ptr<WorldLayout>> g_layouts;

bool Compute(PackageCache& cache, WorldLayout& out, std::string* error) {
    const Vfs& vfs = cache.FileSystem();
    std::string err;
    auto layout = cache.LoadPath(kLayoutMap, &err);
    if (!layout) { if (error) *error = "world layout: " + err; return false; }
    out.layoutMap = layout->GamePath();

    std::map<std::string, std::vector<Volume>> copies;   // originating level -> copied volumes
    for (auto& v : ReadVolumes(*layout))
        if (v.name.rfind("copied", 0) == 0 && !v.origin.empty()) { v.name = v.name.substr(6); copies[v.origin].push_back(v); }

    std::map<std::string, IslandPlacement> islands;   // lower-case dir -> island
    for (auto& [origin, ws] : copies) {
        i64 e = ResolveLevel(vfs, origin);
        if (e < 0) { LOGW("world layout: level not found: %s", origin.c_str()); continue; }
        std::string path(vfs.Path((u32)e));
        auto lvl = cache.Load((u32)e, &err);
        if (!lvl) continue;
        auto locals = ReadVolumes(*lvl);
        struct Vote { int n = 0; Vec3 t; float yaw = 0; };
        std::map<std::tuple<int, int, int, int>, Vote> votes;
        for (auto& w : ws)
            for (auto& l : locals) {
                bool exact = w.name == l.name || StripNumber(w.name) == l.name;
                if (!exact) {
                    if (!w.hasExtent || !l.hasExtent) continue;
                    if (std::abs(w.extent.x - l.extent.x) >= 1 || std::abs(w.extent.y - l.extent.y) >= 1 || std::abs(w.extent.z - l.extent.z) >= 1) continue;
                }
                float th = w.yaw - l.yaw;
                float c = std::cos(th * 3.14159265f / 180), s = std::sin(th * 3.14159265f / 180);
                Vec3 rl{c * l.loc.x - s * l.loc.y, s * l.loc.x + c * l.loc.y, l.loc.z};
                Vec3 t{RoundTo(w.loc.x - rl.x, 10), RoundTo(w.loc.y - rl.y, 10), RoundTo(w.loc.z - rl.z, 10)};
                auto key = std::make_tuple((int)std::lround(th * 10), (int)t.x, (int)t.y, (int)t.z);
                auto& v = votes[key];
                v.n += exact ? 100 : 1; v.t = t; v.yaw = RoundTo(th, 0.1f);
            }
        if (votes.empty()) continue;
        const Vote* best = nullptr;
        for (auto& [k, v] : votes) if (!best || v.n > best->n) best = &v;
        LevelPlacement lp{path, best->t, best->yaw, best->n};
        std::string dir = IslandDirOf(path);
        auto& isl = islands[ToLower(dir)];
        isl.dir = dir;
        isl.levels.push_back(lp);
    }
    // islands without volumes in the layout (Athena manual_tiles.json)
    struct Manual { const char* dir; Vec3 loc; float yaw; };
    static const Manual manual[] = {{"Athena/Content/Maps/reefs/rfs_feature_01_a", {223380, 119369, 0}, 0}};
    for (auto& m : manual) {
        if (islands.count(ToLower(m.dir))) continue;
        auto& isl = islands[ToLower(m.dir)];
        isl.dir = m.dir; isl.location = m.loc; isl.yaw = m.yaw; isl.manual = true;
    }
    for (auto& [k, isl] : islands) {
        if (isl.manual || isl.levels.empty()) continue;
        // the terrain sublevel decides, else the best voted level
        const LevelPlacement* pick = nullptr;
        for (auto& l : isl.levels) {
            bool terr = IEndsWith(StemOfPath(l.level), "_terrain");
            bool pterr = pick && IEndsWith(StemOfPath(pick->level), "_terrain");
            if (!pick || (terr && !pterr) || (terr == pterr && l.votes > pick->votes)) pick = &l;
        }
        isl.location = pick->location;
        isl.yaw = pick->yaw;
        isl.name = LeafOf(isl.dir);
    }
    // persistent map of each island folder
    for (u32 i = 0; i < (u32)vfs.FileCount(); i++) {
        std::string_view p = vfs.Path(i);
        if (!IEndsWith(p, ".umap")) continue;
        auto it = islands.find(ToLower(DirOf(p)));
        if (it == islands.end()) continue;
        auto& isl = it->second;
        std::string stem = StemOfPath(p);
        // prefer the map named like the folder; otherwise the shortest name
        bool named = IEquals(stem, LeafOf(isl.dir));
        if (isl.map.empty() || named || (!IEquals(StemOfPath(isl.map), LeafOf(isl.dir)) && p.size() < isl.map.size())) isl.map = std::string(p);
    }
    // island metadata
    if (auto gi = cache.LoadPath(kIslandsAsset, &err)) {
        i32 main = gi->MainExport();
        if (main > 0) {
            PropertyList p = gi->ReadProperties((size_t)main - 1);
            out.mapMin = VecField(FindValue(p, "WorldMapMin"));
            out.mapMax = VecField(FindValue(p, "WorldMapMax"));
            if (auto entries = FindValue(p, "IslandEntries"))
                for (auto& e : entries->items) {
                    if (!e.Fields()) continue;
                    std::string name = ToLower(PropStr(*e.Fields(), "IslandName"));
                    for (auto& [k, isl] : islands) {
                        if (!IEquals(LeafOf(isl.dir), name) && !IEquals(StemOfPath(isl.map), name)) continue;
                        isl.name = PropStr(*e.Fields(), "IslandName");
                        std::string t = PropStr(*e.Fields(), "IslandType");
                        isl.type = t.substr(t.find_last_of(':') == std::string::npos ? 0 : t.find_last_of(':') + 1);
                        isl.centre = VecField(e.Field("IslandBoundsCentre"));
                        isl.radius = (float)PropFloat(*e.Fields(), "IslandBoundsRadius");
                        if (i32 sea = PropObj(*e.Fields(), "Sea")) {
                            std::string s = gi->ObjectName(sea);
                            if (IEndsWith(s, "_C")) s.resize(s.size() - 2);
                            isl.sea = s;
                        }
                        break;
                    }
                }
        }
    }
    for (auto& [k, isl] : islands) {
        if (isl.name.empty()) isl.name = LeafOf(isl.dir);
        out.islands.push_back(std::move(isl));
    }
    return true;
}

}   // namespace

Mat4 LevelPlacement::Transform() const { return Mat4::FromTRS(location, RotatorToQuat(0, yaw, 0), {1, 1, 1}); }
Mat4 IslandPlacement::Transform() const { return Mat4::FromTRS(location, RotatorToQuat(0, yaw, 0), {1, 1, 1}); }

const IslandPlacement* WorldLayout::IslandOfLevel(std::string_view levelPath) const {
    std::string dir = IslandDirOf(levelPath);
    for (auto& i : islands)
        if (IEquals(i.dir, dir)) return &i;
    return nullptr;
}

bool WorldLayout::LevelTransform(std::string_view levelPath, Mat4& out) const {
    if (IStartsWith(levelPath, DirOf(layoutMap) + "/")) {   // the layout's own sublevels (not the other layouts next to it)
        if (!IStartsWith(StemOfPath(levelPath), StemOfPath(layoutMap))) return false;
        out = Mat4::Identity();
        return true;
    }
    const IslandPlacement* isl = IslandOfLevel(levelPath);
    if (!isl) return false;
    std::string want(levelPath);
    if (std::string lp = ToLower(want); lp.find("/lod/") != std::string::npos) {   // "<dir>/LOD/<level>lod1.umap" -> "<dir>/<level>.umap"
        std::string stem = StemOfPath(want);
        size_t k = ToLower(stem).rfind("lod");
        if (k != std::string::npos) want = DirOf(DirOf(want)) + "/" + stem.substr(0, k) + ".umap";
    }
    for (auto& l : isl->levels)
        if (IEquals(l.level, want)) { out = l.Transform(); return true; }
    out = isl->Transform();
    return true;
}

const WorldLayout* GetWorldLayout(PackageCache& cache, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_layouts[&cache];
    if (slot) return slot.get();
    auto l = std::make_unique<WorldLayout>();
    if (!Compute(cache, *l, error)) return nullptr;
    slot = std::move(l);
    return slot.get();
}

bool IsWorldLayoutMap(std::string_view path) {
    std::string s = StemOfPath(path);
    return IEquals(s, "sot_world_layout_2");
}
