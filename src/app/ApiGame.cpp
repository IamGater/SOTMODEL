// Game rule data: physical surfaces (physical materials and the materials using them).
#include "app/Api.h"
#include "core/Json.h"
#include "ue/GameConfig.h"
#include "ue/Properties.h"
#include <map>

namespace {

struct PhysMat {
    u32 entry = 0;
    std::string path, type;
    double friction = 0.7, restitution = 0.3, density = 1.0;
};

// physical material packages: pm_* and *physicalmaterial* files (named that way across the game), read one by one
std::vector<PhysMat> CollectPhysicalMaterials(Api& a) {
    std::vector<PhysMat> out;
    for (u32 i = 0; i < a.vfs.FileCount(); i++) {
        std::string_view p = a.vfs.Path(i);
        if (!IEndsWith(p, ".uasset")) continue;
        std::string name = ToLower(a.vfs.FileName(i));
        if (!IStartsWith(name, "pm_") && name.find("physicalmaterial") == std::string::npos) continue;
        std::string err;
        auto pkg = a.cache->Load(i, &err);
        if (!pkg) continue;
        i32 m = pkg->MainExport();
        if (!m || pkg->ExportClass((size_t)m - 1) != "PhysicalMaterial") continue;
        size_t end = 0;
        PropertyList props = pkg->ReadProperties((size_t)m - 1, &end, &err);
        PhysMat pm;
        pm.entry = i;
        pm.path = std::string(p);
        pm.type = PropStr(props, "SurfaceType");
        if (pm.type.empty()) pm.type = "SurfaceType_Default";
        pm.friction = PropFloat(props, "Friction", 0.7);
        pm.restitution = PropFloat(props, "Restitution", 0.3);
        pm.density = PropFloat(props, "Density", 1.0);
        out.push_back(std::move(pm));
    }
    return out;
}

int SurfaceNumber(const std::string& type) {   // SurfaceType12 -> 12, Default -> 0
    size_t d = type.find_first_of("0123456789");
    return d == std::string::npos ? 0 : atoi(type.c_str() + d);
}

}   // namespace

void RegisterGameTools(Api& api) {
    api.Register({"get_physical_materials",
                  "The game's physical surfaces: every surface type named in Athena/Config/DefaultEngine.ini (PhysicsSettings "
                  "PhysicalSurfaces, the names footstep / impact sounds switch on: Wwise 'surface' switch values) with the physical "
                  "material assets using it (friction, restitution, density). users=true adds the packages referencing each physical "
                  "material (materials, meshes, Blueprints) from the reference index (built once, a few minutes, then cached).",
                  R"({"type":"object","properties":{"users":{"type":"boolean","default":false},"surface":{"type":"string","description":"only this surface name or type"}}})",
                  "surface",
                  [](Api& a, const JsonValue& args) {
                      bool users = args.GetBool("users", false);
                      std::string only = ToLower(args.GetStr("surface"));
                      ReferenceIndex* refs = nullptr;
                      std::string rerr;
                      if (users) refs = a.EnsureReferences(true, &rerr);
                      auto mats = CollectPhysicalMaterials(a);
                      std::map<int, std::pair<std::string, std::string>> surfaces;   // number -> type, name
                      surfaces[0] = {"SurfaceType_Default", "Default"};
                      for (auto& [type, name] : GameConfig::PhysicalSurfaces()) surfaces[SurfaceNumber(type)] = {type, name};
                      for (auto& m : mats)
                          if (!surfaces.count(SurfaceNumber(m.type))) surfaces[SurfaceNumber(m.type)] = {m.type, ""};
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("config", "Athena/Config/DefaultEngine.ini");
                      if (users && !refs) w.KV("usersError", rerr);
                      w.Key("surfaces");
                      w.BeginArray();
                      for (auto& [num, tn] : surfaces) {
                          if (!only.empty() && ToLower(tn.first) != only && ToLower(tn.second) != only) continue;
                          w.BeginObject();
                          w.KV("type", tn.first);
                          w.KV("name", tn.second);
                          w.Key("physicalMaterials");
                          w.BeginArray();
                          for (auto& m : mats) {
                              if (SurfaceNumber(m.type) != num) continue;
                              w.BeginObject();
                              w.KV("path", m.path);
                              w.KV("friction", m.friction);
                              w.KV("restitution", m.restitution);
                              w.KV("density", m.density);
                              if (refs) {
                                  w.Key("usedBy");
                                  w.BeginArray();
                                  if (auto* r = refs->Referencers(m.entry))
                                      for (u32 e : *r) w.Str(a.vfs.Path(e));
                                  w.EndArray();
                              }
                              w.EndObject();
                          }
                          w.EndArray();
                          w.EndObject();
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
