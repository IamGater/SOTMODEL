// Content search: packages by class, by what they import ("uses this texture / material / mesh"), by property values,
// and the reverse reference index (who imports this package).
#include <iterator>
#include "app/Api.h"
#include "core/Json.h"
#include "ue/DataTable.h"
#include "app/DepGraph.h"
#include "app/RefIndex.h"

namespace {

// property path match: "Speed", "Settings.Speed" or "Items[2].Name"; value: case-insensitive substring of the cell text
// (or exact with value_exact). Walks structs and arrays; records "Path=Value" of the hits.
void MatchProps(const PropertyList& props, const std::string& prefix, const std::string& want, const std::string& value, bool exact,
                std::vector<std::string>& hits, int depth = 0, const std::string& textKey = {}, const Package* pkg = nullptr) {
    if (depth > 8 || hits.size() > 20) return;
    for (auto& p : props) {
        std::string path = prefix + (prefix.empty() ? "" : ".") + p.name + (p.arrayIndex ? Fmt("[%d]", p.arrayIndex) : "");
        auto check = [&](const std::string& pth, const PropValue& v) {
            bool nameOk = want.empty() || IEquals(pth, want) || IEndsWith(pth, "." + want) || IEquals(pth.substr(pth.rfind('.') + 1), want);
            if (!nameOk) return;
            if (!textKey.empty() && !(v.type == PropType::Text && v.items.size() == 2 && IEquals(v.items[1].str, textKey))) return;
            // object references resolve through the package (TableCell alone shows bare indices as None)
            std::string text = v.type == PropType::Object && v.str.empty() && pkg && v.ObjIndex() ? pkg->FullPath(v.ObjIndex()) : TableCell(v);
            bool valueOk = value.empty() || (exact ? IEquals(text, value) : ToLower(text).find(ToLower(value)) != std::string::npos);
            if (valueOk) hits.push_back(pth + " = " + (text.size() > 160 ? text.substr(0, 160) + "..." : text));
        };
        check(path, p.value);
        if (p.value.type == PropType::Struct && p.value.Fields()) MatchProps(*p.value.Fields(), path, want, value, exact, hits, depth + 1, textKey, pkg);
        if (p.value.type == PropType::Array)
            for (size_t i = 0; i < p.value.items.size() && i < 256; i++) {
                const PropValue& it = p.value.items[i];
                std::string ip = path + Fmt("[%zu]", i);
                if (it.type == PropType::Struct && it.Fields()) MatchProps(*it.Fields(), ip, want, value, exact, hits, depth + 1, textKey, pkg);
                else check(ip, it);
            }
    }
}

}   // namespace

void RegisterSearchTools(Api& api) {
    api.Register({"find_references",
                  "Which packages import (use) the given package: e.g. the meshes using a material, the materials using a texture, the maps "
                  "placing a Blueprint. Uses a reference index built once over every package header (a few minutes the first time, then cached).",
                  R"({"type":"object","properties":{"path":{"type":"string"},"limit":{"type":"integer","default":500},"build":{"type":"boolean","default":true}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      i64 e = a.ResolveFile(args.GetStr("path"));
                      if (e < 0) return ApiResult::Error("not found: " + args.GetStr("path"));
                      std::string err;
                      ReferenceIndex* idx = a.EnsureReferences(args.GetBool("build", true), &err);
                      if (!idx) return ApiResult::Error(err);
                      const std::vector<u32>* refs = idx->Referencers((u32)e);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("path", std::string(a.vfs.Path((u32)e)));
                      w.KV("count", (i64)(refs ? refs->size() : 0));
                      w.Key("referencedBy");
                      w.BeginArray();
                      int limit = (int)args.GetNum("limit", 500);
                      if (refs)
                          for (size_t i = 0; i < refs->size() && (int)i < limit; i++) w.Str(std::string(a.vfs.Path((*refs)[i])));
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"get_dependency_graph",
                  "Dependency graph around a package: its referencers (packages importing it, from the reference index) and its references "
                  "(packages it imports, with the class of the imported object), 'depth' levels out. Nodes carry path, class, side "
                  "(-1 referencer, 0 root, 1 reference), depth and how many neighbours the per-node 'limit' left out; edges are "
                  "[importer, imported] node indices. direction = both | referencers | references.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"depth":{"type":"integer","default":1},"direction":{"type":"string","default":"both"},"limit":{"type":"integer","default":40},"max_nodes":{"type":"integer","default":400},"build":{"type":"boolean","default":true}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      i64 e = a.ResolveFile(args.GetStr("path"));
                      if (e < 0) return ApiResult::Error("not found: " + args.GetStr("path"));
                      std::string dir = ToLower(args.GetStr("direction", "both"));
                      bool wantIn = dir != "references", wantOut = dir != "referencers";
                      std::string err;
                      ReferenceIndex* idx = nullptr;
                      if (wantIn && !(idx = a.EnsureReferences(args.GetBool("build", true), &err))) return ApiResult::Error(err);
                      DepGraph g = BuildDepGraph(a.vfs, *a.cache, idx, (u32)e, (int)args.GetNum("depth", 1), wantIn, wantOut,
                                                 (int)args.GetNum("limit", 40), (int)args.GetNum("max_nodes", 400));
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("root", g.nodes[0].path);
                      w.Key("nodes");
                      w.BeginArray();
                      for (auto& n : g.nodes) {
                          w.BeginObject();
                          w.KV("path", n.path);
                          if (!n.cls.empty()) w.KV("class", n.cls);
                          w.KV("side", n.side);
                          w.KV("depth", n.depth);
                          if (n.hiddenReferencers) w.KV("moreReferencers", n.hiddenReferencers);
                          if (n.hiddenReferences) w.KV("moreReferences", n.hiddenReferences);
                          w.EndObject();
                      }
                      w.EndArray();
                      w.Key("edges");
                      w.BeginArray();
                      for (auto& [from, to] : g.edges) {
                          w.BeginArray();
                          w.Int(from);
                          w.Int(to);
                          w.EndArray();
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"search_content",
                  "Search inside packages. Filters (all given ones must match): 'class' = main asset class (StaticMesh, MaterialInstanceConstant, "
                  "BlueprintGeneratedClass, DataTable, ...); 'uses' = the package imports this asset (a texture, material, mesh, Blueprint: path "
                  "or /Game path); 'property' + optional 'value' = some export has this property (name or dotted path, searched in structs and "
                  "arrays) whose value contains 'value' (value_exact for equality); 'name' = substring of the path; 'text_key' = a localisation key "
                  "(search_localization lists them): packages with an FText of that key, from the reference index (whole game). With 'uses' the candidates come "
                  "from the reference index (fast, whole game); otherwise restrict 'folder' (packages are read, scan_limit caps how many).",
                  R"({"type":"object","properties":{"folder":{"type":"string"},"class":{"type":"string"},"uses":{"type":"string"},"property":{"type":"string"},"value":{"type":"string"},"value_exact":{"type":"boolean","default":false},"name":{"type":"string"},"text_key":{"type":"string"},"limit":{"type":"integer","default":100},"scan_limit":{"type":"integer","default":20000}}})",
                  "folder",
                  [](Api& a, const JsonValue& args) {
                      ContentQuery q;
                      q.cls = args.GetStr("class");
                      q.uses = args.GetStr("uses");
                      q.prop = args.GetStr("property");
                      q.value = args.GetStr("value");
                      q.name = args.GetStr("name");
                      q.folder = args.GetStr("folder");
                      q.textKey = args.GetStr("text_key");
                      q.exact = args.GetBool("value_exact", false);
                      q.limit = (int)args.GetNum("limit", 100);
                      q.scanLimit = (int)args.GetNum("scan_limit", 20000);
                      ContentSearchResult res;
                      std::string err;
                      if (!SearchContent(a, q, res, &err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.Key("results");
                      w.BeginArray();
                      for (auto& h : res.hits) {
                          w.BeginObject();
                          w.KV("path", std::string(a.vfs.Path(h.entry)));
                          if (!h.cls.empty()) w.KV("class", h.cls);
                          if (!h.matches.empty()) {
                              w.Key("matches");
                              w.BeginArray();
                              for (size_t i = 0; i < h.matches.size() && i < 10; i++) w.Str(h.matches[i]);
                              w.EndArray();
                          }
                          w.EndObject();
                      }
                      w.EndArray();
                      w.KV("found", (i64)res.hits.size());
                      w.KV("candidates", (i64)res.candidates);
                      w.KV("scanned", res.scanned);
                      if (res.truncated) w.KV("truncated", true);
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}

bool SearchContent(Api& a, const ContentQuery& q, ContentSearchResult& out, std::string* error, const std::atomic<bool>* cancel,
                   std::atomic<int>* progress, std::atomic<int>* total, bool buildIndex) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    std::string name = ToLower(q.name), folder = ToLower(Vfs::NormalizePath(q.folder));
    if (q.cls.empty() && q.uses.empty() && q.prop.empty() && q.value.empty() && name.empty() && q.textKey.empty()) return fail("give at least one filter");
    if (!folder.empty() && folder.back() != '/') folder += '/';
    // candidates
    std::vector<u32> cand;
    std::string err;
    if (!q.uses.empty()) {
        i64 t = a.ResolveFile(q.uses);
        if (t < 0) return fail("not found: " + q.uses);
        ReferenceIndex* idx = a.EnsureReferences(buildIndex, &err);
        if (!idx) return fail(err.empty() ? "the reference index is not ready" : err);
        if (auto* r = idx->Referencers((u32)t)) cand = *r;
    }
    if (!q.textKey.empty()) {   // packages whose bytes hold the key (then checked: which FText properties have it)
        ReferenceIndex* idx = a.EnsureReferences(buildIndex, &err, true);
        if (!idx) return fail(err.empty() ? "the reference index is not ready" : err);
        if (!idx->HasTextKeys()) return fail("the reference index was built without text keys: rebuild it (delete References.bin)");
        std::vector<u32> users;
        if (auto* r = idx->TextUsers(q.textKey)) users = *r;
        if (!q.uses.empty()) {
            std::sort(cand.begin(), cand.end());
            std::vector<u32> both;
            std::set_intersection(cand.begin(), cand.end(), users.begin(), users.end(), std::back_inserter(both));
            cand = std::move(both);
        } else
            cand = std::move(users);
    } else if (q.uses.empty()) {
        if (folder.empty() && (!q.prop.empty() || !q.value.empty() || !q.cls.empty()))
            return fail("restrict 'folder' (or use 'uses') - reading every package of the game takes long");
        for (u32 i = 0; i < a.vfs.FileCount(); i++) {
            std::string p = ToLower(a.vfs.Path(i));
            if (!IEndsWith(p, ".uasset") && !IEndsWith(p, ".umap")) continue;
            if (!folder.empty() && p.rfind(folder, 0) != 0) continue;
            cand.push_back(i);
        }
    }
    out = {};
    out.candidates = cand.size();
    if (total) *total = (int)cand.size();
    int done = 0;
    for (u32 e : cand) {
        if (progress) *progress = ++done;
        if (cancel && *cancel) { out.cancelled = true; break; }
        std::string low = ToLower(a.vfs.Path(e));
        if (!folder.empty() && low.rfind(folder, 0) != 0) continue;
        if (!name.empty() && low.find(name) == std::string::npos) continue;
        ContentHit hit;
        hit.entry = e;
        if (!q.cls.empty() || !q.prop.empty() || !q.value.empty() || !q.textKey.empty()) {   // path-only filters don't need to read the package
            if (++out.scanned > q.scanLimit) { out.truncated = true; break; }
            auto pkg = a.cache->Load(e, &err);
            if (!pkg) continue;
            i32 main = pkg->MainExport();
            hit.cls = main ? pkg->ExportClass(main - 1) : "";
            if (!q.cls.empty() && !IEquals(hit.cls, q.cls)) continue;
            auto& hits = hit.matches;
            if (!q.prop.empty() || !q.value.empty() || !q.textKey.empty()) {
                for (size_t x = 0; x < pkg->Exports().size() && hits.size() < 20; x++) {
                    std::string perr;
                    PropertyList props = pkg->ReadProperties(x, nullptr, &perr);
                    std::vector<std::string> h;
                    MatchProps(props, "", q.prop, q.value, q.exact, h, 0, q.textKey, pkg.get());
                    for (auto& s : h) hits.push_back(pkg->Exports()[x].objectName + ": " + s);
                    if (IsTableClass(pkg->ExportClass(x))) {   // DataTable rows count as properties too
                        TableData t;
                        if (ReadTable(*pkg, x, t))
                            for (auto& row : t.rows) {
                                std::vector<std::string> rh;
                                MatchProps(row.fields, "", q.prop, q.value, q.exact, rh, 0, q.textKey, pkg.get());
                                for (auto& s : rh) hits.push_back("Rows." + row.name + ": " + s);
                            }
                    }
                }
                // a text key outside properties (Blueprint bytecode, native data) still counts
                if (hits.empty() && !q.textKey.empty() && q.prop.empty() && q.value.empty()) hits.push_back("text key in the package data (not a property)");
                if (hits.empty()) continue;
            }
        }
        out.hits.push_back(std::move(hit));
        if ((int)out.hits.size() >= q.limit) { out.truncated = true; break; }
    }
    return true;
}
