#include "app/Api.h"
#include <map>
#include "core/Json.h"
#include "ue/World.h"
#include "ue/WorldLayout.h"
#include "app/RefIndex.h"
#include "render/Offscreen.h"
#include "export/Png.h"

// islands / proxies / region of a whole-sea load (path = sot_world_layout_2)
void SeaOptionsFrom(const JsonValue& args, WorldOptions& o) {
    o.islands = args.GetStr("islands");
    o.proxies = args.GetBool("proxies", false);
    if (const JsonValue* r = args.Get("region"); r && r->arr.size() >= 4) {
        o.region = true;
        o.regionMin = {(float)std::min(r->arr[0].num, r->arr[2].num), (float)std::min(r->arr[1].num, r->arr[3].num)};
        o.regionMax = {(float)std::max(r->arr[0].num, r->arr[2].num), (float)std::max(r->arr[1].num, r->arr[3].num)};
    }
}

static WorldOptions WorldOptionsFrom(const JsonValue& args) {
    WorldOptions o;
    o.sublevels = args.GetBool("sublevels", true);
    o.skipNonVisual = args.GetBool("skip_non_visual", true);
    o.skeletal = args.GetBool("skeletal", true);
    o.landscape = args.GetBool("landscape", true);
    o.meshFilter = args.GetStr("filter");
    SeaOptionsFrom(args, o);
    return o;
}

// lights / decals / fog / water of a loaded world: totals plus the first `limit` of each
static void WriteWorldEffects(JsonWriter& w, const WorldData& wd, int limit) {
    auto v3 = [&](const char* k, const Vec3& v) { w.Key(k); w.BeginArray(); w.Num(v.x); w.Num(v.y); w.Num(v.z); w.EndArray(); };
    static const char* types[] = {"point", "spot", "directional", "sky"};
    int count[4] = {};
    for (auto& l : wd.lights) count[l.type]++;
    w.Key("lights");
    w.BeginObject();
    for (int t = 0; t < 4; t++) w.KV(types[t], (i64)count[t]);
    w.Key("sample");
    w.BeginArray();
    for (size_t i = 0; i < wd.lights.size() && (int)i < limit; i++) {
        auto& l = wd.lights[i];
        w.BeginObject();
        w.KV("type", types[l.type]);
        v3("position", l.position);
        if (l.type != WorldLight::Point) v3("direction", l.direction);
        v3("color", l.color);
        w.KV("intensity", (double)l.intensity);
        if (l.type <= WorldLight::Spot) w.KV("radius", (double)l.radius);
        if (l.type == WorldLight::Spot) { w.KV("innerCone", (double)l.innerCone); w.KV("outerCone", (double)l.outerCone); }
        w.KV("level", wd.levels[l.level].path.substr(wd.levels[l.level].path.find_last_of('/') + 1));
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.Key("decals");
    w.BeginObject();
    w.KV("count", (i64)wd.decals.size());
    std::map<std::string, int> mats;
    for (auto& d : wd.decals) mats[d.material]++;
    w.Key("materials");
    w.BeginObject();
    int k = 0;
    for (auto& [m, c] : mats) { if (k++ >= limit) break; w.KV(m.empty() ? "(none)" : m, (i64)c); }
    w.EndObject();
    w.EndObject();
    w.Key("particleEmitters");
    w.BeginObject();
    w.KV("count", (i64)wd.emitters.size());
    std::map<std::string, int> systems;
    for (auto& e : wd.emitters) systems[e.system]++;
    w.Key("systems");
    w.BeginObject();
    k = 0;
    for (auto& [s, c] : systems) { if (k++ >= limit) break; w.KV(s, (i64)c); }
    w.EndObject();
    w.EndObject();
    if (!wd.fogs.empty()) {
        w.Key("heightFog");
        w.BeginArray();
        for (auto& f : wd.fogs) {
            w.BeginObject();
            v3("position", f.position);
            w.KV("density", (double)f.density);
            w.KV("heightFalloff", (double)f.heightFalloff);
            w.KV("maxOpacity", (double)f.maxOpacity);
            w.KV("startDistance", (double)f.startDistance);
            if (f.underwater) w.KV("underwater", true);
            v3("color", f.color);
            w.EndObject();
        }
        w.EndArray();
    }
    if (wd.hasWater) w.KV("waterLevel", (double)wd.waterLevel);
}

static const char* kWorldArgs =
    R"J("path":{"type":"string","description":"a .umap (persistent map or sublevel)"},)J"
    R"J("sublevels":{"type":"boolean","default":true,"description":"also load the world-composition sublevels under the map's folder"},)J"
    R"J("skip_non_visual":{"type":"boolean","default":true,"description":"skip audio / tall-tale / arena sublevels and helper meshes"},)J"
    R"J("skeletal":{"type":"boolean","default":true},)J"
    R"J("landscape":{"type":"boolean","default":true,"description":"build terrain meshes from Landscape heightmaps"},)J"
    R"J("filter":{"type":"string","description":"only meshes whose path contains one of these comma separated terms; '-term' excludes"},)J"
    R"J("islands":{"type":"string","description":"path = sot_world_layout_2 (the whole sea): only islands whose name / folder / sea matches these terms"},)J"
    R"J("proxies":{"type":"boolean","default":false,"description":"whole sea: use each island's merged LOD proxy meshes (fast overview)"},)J"
    R"J("region":{"type":"array","items":{"type":"number"},"description":"whole sea: [minX, minY, maxX, maxY] world cm - islands whose centre lies inside"})J";

void RegisterWorldTools(Api& api) {
    api.Register({"get_world_info",
                  "Load a map (.umap) as a scene: every placed mesh with Blueprint component templates resolved, instanced meshes "
                  "expanded and the world-composition sublevels included. Returns the levels, totals and the most used meshes.",
                  std::string(R"J({"type":"object","properties":{)J") + kWorldArgs +
                      R"J(,"meshes":{"type":"integer","default":50,"description":"how many meshes to list (most instances first)"},)J"
                      R"J("unresolved":{"type":"integer","default":20,"description":"how many unresolved components to list"},)J"
                      R"J("effects":{"type":"integer","default":20,"description":"how many lights / decal materials to list"}},"required":["path"]})J",
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
                      w.KV("landscapeComponents", (i64)wd.landscapes.size());
                      w.KV("unresolvedComponents", (i64)wd.unresolved.size());
                      w.Key("boundsMin"); w.BeginArray(); w.Num(wd.boundsMin.x); w.Num(wd.boundsMin.y); w.Num(wd.boundsMin.z); w.EndArray();
                      w.Key("boundsMax"); w.BeginArray(); w.Num(wd.boundsMax.x); w.Num(wd.boundsMax.y); w.Num(wd.boundsMax.z); w.EndArray();
                      WriteWorldEffects(w, wd, (int)args.GetNum("effects", 20));
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
                          if (l.landscapeComponents) w.KV("landscapeComponents", (i64)l.landscapeComponents);
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

    api.Register({"preview_world",
                  "Render a map (.umap) with all its meshes, Blueprint components and landscape to a PNG. The camera orbits the scene "
                  "(yaw/pitch/zoom) or looks from 'eye' to 'target' ([x,y,z] in cm, Unreal space). Use 'save_to' to also write the PNG.",
                  std::string(R"J({"type":"object","properties":{)J") + kWorldArgs +
                      R"J(,"width":{"type":"integer","default":1280},"height":{"type":"integer","default":720},)J"
                      R"J("yaw":{"type":"number","default":-135},"pitch":{"type":"number","default":-35},"zoom":{"type":"number","default":1},)J"
                      R"J("eye":{"type":"array","items":{"type":"number"}},"target":{"type":"array","items":{"type":"number"}},)J"
                      R"J("textures":{"type":"boolean","default":true},"draw_distance_scale":{"type":"number","default":1},)J"
                      R"J("lights":{"type":"boolean","default":true,"description":"the map's sun, sky light and nearest point / spot lights"},)J"
                      R"J("fog":{"type":"boolean","default":true,"description":"the map's exponential height fog"},)J"
                      R"J("water":{"type":"boolean","default":true,"description":"sea plane at the water level"},)J"
                      R"J("gizmos":{"type":"boolean","default":false,"description":"outlines of lights (range circles, spot cones) and decal boxes"},)J"
                      R"J("time_of_day":{"type":"number","description":"hour (0-24) of the sea's day cycle: sun / moon, sky light, sky dome and fog from the lighting curves; omitted = the map's own lights only"},)J"
                      R"J("lighting_region":{"type":"string","description":"lighting curve set under Curves/Lighting (WSP, Wilds, Volcanic, Standard, ...); default: from the map path"},"save_to":{"type":"string"}},"required":["path"]})J",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      WorldLoader loader(*a.cache);
                      WorldData wd;
                      std::string err;
                      WorldOptions o = WorldOptionsFrom(args);
                      o.landscapeStep = 2;
                      if (!loader.Load(args.GetStr("path"), o, wd, &err)) return ApiResult::Error(err);
                      WorldPreviewRequest req;
                      req.width = (int)args.GetNum("width", 1280);
                      req.height = (int)args.GetNum("height", 720);
                      req.yaw = (float)args.GetNum("yaw", -135);
                      req.pitch = (float)args.GetNum("pitch", -35);
                      req.zoom = (float)args.GetNum("zoom", 1);
                      req.draw.textures = args.GetBool("textures", true);
                      req.draw.grid = false;
                      req.world.drawDistanceScale = (float)args.GetNum("draw_distance_scale", 1);
                      req.world.lights = args.GetBool("lights", true);
                      req.world.fog = args.GetBool("fog", true);
                      req.world.water = args.GetBool("water", true);
                      req.world.gizmos = args.GetBool("gizmos", false);
                      TimeOfDayCurves tod;
                      TimeOfDayLight todLight;
                      std::string todRegion;
                      if (args.Get("time_of_day")) {
                          auto regions = TimeOfDayRegions(*a.cache);
                          todRegion = args.GetStr("lighting_region");
                          if (todRegion.empty()) todRegion = TimeOfDayRegionFor(wd.root, regions);
                          if (!LoadTimeOfDay(*a.cache, todRegion, tod, &err)) return ApiResult::Error("time_of_day: " + err);
                          todLight = EvaluateTimeOfDay(tod, (float)args.GetNum("time_of_day", 12));
                          req.world.timeOfDay = &todLight;
                      }
                      auto vec = [&](const char* k, Vec3& v) {
                          const JsonValue* j = args.Get(k);
                          if (!j || j->arr.size() < 3) return false;
                          v = {(float)j->arr[0].num, (float)j->arr[1].num, (float)j->arr[2].num};
                          return true;
                      };
                      req.useEye = vec("eye", req.eye) && vec("target", req.target);
                      Image img;
                      if (!RenderWorldPreview(*a.cache, wd, req, img, &err)) return ApiResult::Error(err);
                      ApiResult r;
                      ApiImage im;
                      im.mime = "image/png";
                      im.data = EncodePng(img.rgba.data(), img.width, img.height);
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("map", wd.root);
                      w.KV("levels", (i64)wd.levels.size());
                      w.KV("meshInstances", (i64)wd.InstanceCount());
                      w.KV("landscapeComponents", (i64)wd.landscapes.size());
                      w.KV("lights", (i64)wd.lights.size());
                      w.KV("decals", (i64)wd.decals.size());
                      if (wd.hasWater) w.KV("waterLevel", (double)wd.waterLevel);
                      if (req.world.timeOfDay) { w.KV("lightingRegion", todRegion); w.KV("sunrise", (double)tod.sunrise); w.KV("sunset", (double)tod.sunset); }
                      if (!save.empty()) w.KV("savedTo", save);
                      w.EndObject();
                      r.json = w.Take();
                      r.images.push_back(std::move(im));
                      return r;
                  }});

    api.Register({"get_world_layout",
                  "Where the islands, outposts and forts of the open world sit (sot_world_layout_2): every island folder with its "
                  "persistent map, sea, type, tile transform (location + yaw, recovered from the layout's copied NavMeshBounds "
                  "volumes), bounds centre and radius. 'levels' also lists each placed sublevel with its own transform.",
                  R"J({"type":"object","properties":{"filter":{"type":"string","description":"substring of the island name, folder or sea"},)J"
                  R"J("levels":{"type":"boolean","default":false}}})J",
                  "",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      const WorldLayout* L = GetWorldLayout(*a.cache, &err);
                      if (!L) return ApiResult::Error(err);
                      std::string filter = ToLower(args.GetStr("filter"));
                      bool levels = args.GetBool("levels", false);
                      auto v3 = [](JsonWriter& w, const char* k, const Vec3& v) { w.Key(k); w.BeginArray(); w.Num(v.x); w.Num(v.y); w.Num(v.z); w.EndArray(); };
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("layoutMap", L->layoutMap);
                      v3(w, "worldMapMin", L->mapMin);
                      v3(w, "worldMapMax", L->mapMax);
                      w.Key("islands");
                      w.BeginArray();
                      i64 n = 0;
                      for (auto& i : L->islands) {
                          if (!filter.empty() && ToLower(i.name + " " + i.dir + " " + i.sea).find(filter) == std::string::npos) continue;
                          n++;
                          w.BeginObject();
                          w.KV("name", i.name);
                          w.KV("dir", i.dir);
                          if (!i.map.empty()) w.KV("map", i.map);
                          if (!i.sea.empty()) w.KV("sea", i.sea);
                          if (!i.type.empty()) w.KV("type", i.type);
                          v3(w, "location", i.location);
                          w.KV("yaw", (double)i.yaw);
                          if (i.manual) w.KV("manual", true);
                          if (i.radius > 0) { v3(w, "centre", i.centre); w.KV("radius", (double)i.radius); }
                          w.KV("placedLevels", (i64)i.levels.size());
                          if (levels) {
                              w.Key("levels");
                              w.BeginArray();
                              for (auto& l : i.levels) {
                                  w.BeginObject();
                                  w.KV("level", l.level);
                                  v3(w, "location", l.location);
                                  w.KV("yaw", (double)l.yaw);
                                  w.KV("votes", (i64)l.votes);
                                  w.EndObject();
                              }
                              w.EndArray();
                          }
                          w.EndObject();
                      }
                      w.EndArray();
                      w.KV("count", n);
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"find_mesh_placements",
                  "Where a static / skeletal mesh is placed in the maps: the levels that use it (from the reference index, also through "
                  "Blueprints that use it), how many instances each level has and their world positions. Levels of the open world are "
                  "moved to their island's place in the sea (get_world_layout); other maps keep their own coordinates (inWorld=false).",
                  R"J({"type":"object","properties":{"mesh":{"type":"string"},"positions":{"type":"integer","default":10,"description":"positions listed per level"},)J"
                  R"J("blueprints":{"type":"boolean","default":true,"description":"also levels placing Blueprints that use the mesh"},"limit":{"type":"integer","default":200,"description":"max levels to load"}},"required":["mesh"]})J",
                  "mesh",
                  [](Api& a, const JsonValue& args) {
                      i64 e = a.ResolveFile(args.GetStr("mesh"));
                      if (e < 0) return ApiResult::Error("not found: " + args.GetStr("mesh"));
                      std::string err;
                      ReferenceIndex* idx = a.EnsureReferences(true, &err);
                      if (!idx) return ApiResult::Error(err);
                      std::vector<u32> levels, bps;
                      auto addLevels = [&](u32 of, bool followBp) {
                          if (auto* r = idx->Referencers(of))
                              for (u32 x : *r) {
                                  std::string_view p = a.vfs.Path(x);
                                  if (IEndsWith(p, ".umap")) { if (std::find(levels.begin(), levels.end(), x) == levels.end()) levels.push_back(x); }
                                  else if (followBp && std::find(bps.begin(), bps.end(), x) == bps.end()) bps.push_back(x);
                              }
                      };
                      addLevels((u32)e, true);
                      if (args.GetBool("blueprints", true)) {
                          std::vector<u32> b = bps;
                          for (u32 x : b) addLevels(x, false);
                      }
                      std::sort(levels.begin(), levels.end(), [&](u32 x, u32 y) { return a.vfs.Path(x) < a.vfs.Path(y); });
                      // "/game/models/x/y." - the mesh object path prefix the loader's filter matches
                      std::string pkgPath = ToLower(Vfs::NormalizePath(a.vfs.Path((u32)e)));
                      if (size_t d = pkgPath.rfind('.'); d != std::string::npos) pkgPath.resize(d);
                      if (pkgPath.rfind("athena/content/", 0) == 0) pkgPath = "/game/" + pkgPath.substr(15);
                      pkgPath += ".";
                      const WorldLayout* L = GetWorldLayout(*a.cache, &err);
                      WorldLoader loader(*a.cache);
                      WorldOptions o;
                      o.sublevels = false;
                      o.skipNonVisual = false;
                      o.landscape = false;
                      o.meshFilter = pkgPath;
                      int maxPos = (int)args.GetNum("positions", 10), limit = (int)args.GetNum("limit", 200);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("mesh", std::string(a.vfs.Path((u32)e)));
                      w.KV("referencingLevels", (i64)levels.size());
                      w.KV("referencingBlueprints", (i64)bps.size());
                      w.Key("levels");
                      w.BeginArray();
                      i64 total = 0;
                      int loaded = 0;
                      for (u32 lv : levels) {
                          if (loaded >= limit) break;
                          auto pkg = a.cache->Load(lv, &err);
                          if (!pkg) continue;
                          loaded++;
                          std::string path(a.vfs.Path(lv));
                          Mat4 t = Mat4::Identity();
                          bool inWorld = L && L->LevelTransform(path, t);
                          WorldData wd;
                          loader.AddLevel(*pkg, t, o, wd);
                          std::vector<Vec3> pos;
                          for (auto& g : wd.groups)
                              for (auto& in : g.instances) pos.push_back(in.transform.Translation());
                          for (auto& sp : wd.splines) pos.push_back(sp.transform.Translation());
                          if (pos.empty()) continue;
                          total += (i64)pos.size();
                          w.BeginObject();
                          w.KV("level", path);
                          if (inWorld) {
                              const IslandPlacement* isl = L->IslandOfLevel(path);
                              if (isl) w.KV("island", isl->name);
                          } else w.KV("inWorld", false);
                          w.KV("instances", (i64)pos.size());
                          w.Key("positions");
                          w.BeginArray();
                          for (size_t i = 0; i < pos.size() && (int)i < maxPos; i++) { w.BeginArray(); w.Num(pos[i].x); w.Num(pos[i].y); w.Num(pos[i].z); w.EndArray(); }
                          w.EndArray();
                          w.EndObject();
                      }
                      w.EndArray();
                      w.KV("instances", total);
                      if (loaded < (int)levels.size()) w.KV("truncated", true);
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
