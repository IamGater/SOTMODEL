#include "app/Api.h"
#include "core/Json.h"
#include "ue/World.h"

static WorldOptions WorldOptionsFrom(const JsonValue& args) {
    WorldOptions o;
    o.sublevels = args.GetBool("sublevels", true);
    o.skipNonVisual = args.GetBool("skip_non_visual", true);
    o.skeletal = args.GetBool("skeletal", true);
    return o;
}

static const char* kWorldArgs =
    R"J("path":{"type":"string","description":"a .umap (persistent map or sublevel)"},)J"
    R"J("sublevels":{"type":"boolean","default":true,"description":"also load the world-composition sublevels under the map's folder"},)J"
    R"J("skip_non_visual":{"type":"boolean","default":true,"description":"skip audio / tall-tale / arena sublevels and helper meshes"},)J"
    R"J("skeletal":{"type":"boolean","default":true})J";

void RegisterWorldTools(Api& api) {
    api.Register({"get_world_info",
                  "Load a map (.umap) as a scene: every placed mesh with Blueprint component templates resolved, instanced meshes "
                  "expanded and the world-composition sublevels included. Returns the levels, totals and the most used meshes.",
                  std::string(R"J({"type":"object","properties":{)J") + kWorldArgs +
                      R"J(,"meshes":{"type":"integer","default":50,"description":"how many meshes to list (most instances first)"},)J"
                      R"J("unresolved":{"type":"integer","default":20,"description":"how many unresolved components to list"}},"required":["path"]})J",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      WorldLoader loader(*a.cache);
                      WorldData wd;
                      std::string err;
                      if (!loader.Load(args.GetStr("path"), WorldOptionsFrom(args), wd, &err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("map", wd.root);
                      w.KV("meshInstances", (i64)wd.InstanceCount());
                      w.KV("uniqueMeshes", (i64)wd.groups.size());
                      w.KV("splineMeshes", (i64)wd.splines.size());
                      w.KV("unresolvedComponents", (i64)wd.unresolved.size());
                      w.Key("boundsMin"); w.BeginArray(); w.Num(wd.boundsMin.x); w.Num(wd.boundsMin.y); w.Num(wd.boundsMin.z); w.EndArray();
                      w.Key("boundsMax"); w.BeginArray(); w.Num(wd.boundsMax.x); w.Num(wd.boundsMax.y); w.Num(wd.boundsMax.z); w.EndArray();
                      w.Key("levels");
                      w.BeginArray();
                      for (auto& l : wd.levels) {
                          w.BeginObject();
                          w.KV("path", l.path);
                          w.KV("actors", (i64)l.actors);
                          w.KV("meshComponents", (i64)l.meshComponents);
                          w.KV("instances", (i64)l.instances);
                          if (l.splines) w.KV("splines", (i64)l.splines);
                          if (l.unresolved) w.KV("unresolved", (i64)l.unresolved);
                          w.EndObject();
                      }
                      w.EndArray();
                      std::vector<const WorldMeshGroup*> gs;
                      for (auto& g : wd.groups) gs.push_back(&g);
                      std::sort(gs.begin(), gs.end(), [](auto x, auto y) { return x->instances.size() > y->instances.size(); });
                      size_t nm = (size_t)std::max<i64>(0, (i64)args.GetNum("meshes", 50));
                      w.Key("meshes");
                      w.BeginArray();
                      for (size_t i = 0; i < gs.size() && i < nm; i++) {
                          w.BeginObject();
                          w.KV("mesh", gs[i]->mesh);
                          w.KV("instances", (i64)gs[i]->instances.size());
                          if (gs[i]->skeletal) w.KV("skeletal", true);
                          if (gs[i]->drawDistance > 0) w.KV("drawDistanceCm", (double)gs[i]->drawDistance);
                          if (!gs[i]->materials.empty()) {
                              w.Key("materialOverrides");
                              w.BeginArray();
                              for (auto& m : gs[i]->materials) w.Str(m);
                              w.EndArray();
                          }
                          w.EndObject();
                      }
                      w.EndArray();
                      size_t nu = (size_t)std::max<i64>(0, (i64)args.GetNum("unresolved", 20));
                      if (nu && !wd.unresolved.empty()) {
                          w.Key("unresolvedSample");
                          w.BeginArray();
                          for (size_t i = 0; i < wd.unresolved.size() && i < nu; i++) w.Str(wd.unresolved[i]);
                          w.EndArray();
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
