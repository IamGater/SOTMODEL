// Game rule data: physical surfaces (physical materials and the materials using them), AI behavior trees.
#include "app/Api.h"
#include "core/Json.h"
#include "ue/GameConfig.h"
#include "ue/Properties.h"
#include "ue/BehaviorTree.h"
#include "app/WebUi.h"
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
    api.Register({"get_behavior_tree",
                  "An AI behavior tree (BehaviorTree package, Blueprints/AI/BehaviorTree): the node tree from the root (composites Selector / "
                  "Sequence / SimpleParallel, tasks with their settings), each node's decorators (conditions, with the editor's cooked "
                  "description) and services, RunBehavior subtrees, and the blackboard's keys with their types. format=text (default) is an "
                  "indented tree, json the structure.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"format":{"type":"string","enum":["text","json"],"default":"text"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                      if (!pkg) return ApiResult::Error(err);
                      BehaviorTreeInfo bt;
                      if (!ReadBehaviorTree(*pkg, bt, a.cache.get(), &err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("path", pkg->GamePath());
                      if (args.GetStr("format") == "json") {
                          w.KV("blackboard", bt.blackboard);
                          w.Key("keys");
                          w.BeginArray();
                          for (auto& k : bt.keys) {
                              w.BeginObject();
                              w.KV("name", k.name);
                              w.KV("type", k.type);
                              if (!k.detail.empty()) w.KV("detail", k.detail);
                              if (!k.from.empty()) w.KV("from", k.from);
                              w.EndObject();
                          }
                          w.EndArray();
                          auto aux = [&](const char* key, const std::vector<BtAux>& v) {
                              if (v.empty()) return;
                              w.Key(key);
                              w.BeginArray();
                              for (auto& x : v) {
                                  w.BeginObject();
                                  w.KV("class", x.cls);
                                  w.KV("name", x.name);
                                  if (!x.details.empty()) w.KV("details", x.details);
                                  w.EndObject();
                              }
                              w.EndArray();
                          };
                          w.Key("nodes");
                          w.BeginArray();
                          for (auto& n : bt.nodes) {
                              w.BeginObject();
                              w.KV("class", n.cls);
                              w.KV("name", n.name);
                              w.KV("composite", n.composite);
                              if (!n.details.empty()) w.KV("details", n.details);
                              if (!n.subtree.empty()) w.KV("subtree", n.subtree);
                              aux("decorators", n.decorators);
                              if (!n.decoratorLogic.empty()) w.KV("decoratorLogic", n.decoratorLogic);
                              aux("services", n.services);
                              if (!n.children.empty()) {
                                  w.Key("children");
                                  w.BeginArray();
                                  for (int c : n.children) w.Int(c);
                                  w.EndArray();
                              }
                              w.EndObject();
                          }
                          w.EndArray();
                      } else {
                          w.KV("tree", BehaviorTreeText(bt));
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
    api.Register({"export_web_ui",
                  "The game's HTML interface (Coherent UI pages: Athena/Content/WebRoot holds the front end menus, HUD, tooltips; "
                  "Athena/Content/uiresources the in-game HUD pages). Writes the whole web root (html, css, js, images, fonts) to "
                  "<outputDir>\\Web\\... keeping its folders so the pages open in a browser (without the game's data bindings), and "
                  "lists the .html pages. root: 'WebRoot' (default), 'uiresources', or any game path inside one.",
                  R"({"type":"object","properties":{"root":{"type":"string","default":"WebRoot"},"outputDir":{"type":"string"}}})",
                  "root",
                  [](Api& a, const JsonValue& args) {
                      std::string r = args.GetStr("root");
                      if (r.empty()) r = "WebRoot";
                      std::string root = WebRootOf(r);
                      if (root.empty()) root = WebRootOf("Athena/Content/" + r + "/");
                      if (root.empty()) return ApiResult::Error("not a web UI root: " + r);
                      std::string od = args.GetStr("outputDir");
                      std::wstring local;
                      int written = 0, total = 0;
                      std::string err;
                      if (!ExportWebRoot(a, root, od.empty() ? a.settings.outputDir : Widen(od), local, &written, &total, &err))
                          return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("root", root);
                      w.KV("local", Narrow(local));
                      w.KV("files", total);
                      w.KV("written", written);
                      if (!err.empty()) w.KV("firstError", err);
                      w.Key("pages");
                      w.BeginArray();
                      for (u32 i = 0; i < a.vfs.FileCount(); i++) {
                          std::string_view p = a.vfs.Path(i);
                          if (IStartsWith(p, root) && IsWebPageFile(p)) w.Str(p.substr(root.size()));
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult res;
                      res.json = w.Take();
                      return res;
                  }});
    api.Register({"serve_web_ui",
                  "Opens the game's HTML UI in a normal browser: starts a local web server (127.0.0.1, this process's lifetime; "
                  "--cli keeps running until killed) that serves the web roots straight from the archives with a script standing "
                  "in for the game (Coherent engine calls / events answered with sample or empty data: player, wallet, crew, "
                  "modes, settings, key bindings from the game's configs; feature flags, the game's fonts, Texture2D'...' "
                  "images as PNG, localised text) and returns the URL of a page. path: a page's game path, default the front "
                  "end (Athena/Content/WebRoot/Root/frontend.html, title screen); add ?state=Lobby / PirateSelection / "
                  "LoadUserSettings / ... (BootFlowStart<state>StateEvent) for another screen, ?event=Dispatcher:Name,... to "
                  "send game events, ?name= ?crew=1..4 ?ship=sloop|brigantine|galleon ?error= ?gamepad=1 "
                  "?features=Name,-Name ?lang= for other sample data, ?debug=1 to log the traffic in the console.",
                  R"({"type":"object","properties":{"path":{"type":"string","default":"Athena/Content/WebRoot/Root/frontend.html"}}})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string p = args.GetStr("path");
                      if (p.empty()) p = "Athena/Content/WebRoot/Root/frontend.html";
                      i64 e = a.vfs.Find(p);
                      if (e < 0 || WebRootOf(p).empty()) return ApiResult::Error("not a web UI file: " + p);
                      std::string err, url = WebUiUrl(a, a.vfs.Path((u32)e), &err);
                      if (url.empty()) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("url", url);
                      w.EndObject();
                      ApiResult res;
                      res.json = w.Take();
                      return res;
                  }});
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
