// Content search: packages by class, by what they import ("uses this texture / material / mesh"), by property values,
// and the reverse reference index (who imports this package).
#include "app/Api.h"
#include "core/Json.h"
#include "ue/DataTable.h"

namespace {

// property path match: "Speed", "Settings.Speed" or "Items[2].Name"; value: case-insensitive substring of the cell text
// (or exact with value_exact). Walks structs and arrays; records "Path=Value" of the hits.
void MatchProps(const PropertyList& props, const std::string& prefix, const std::string& want, const std::string& value, bool exact,
                std::vector<std::string>& hits, int depth = 0) {
    if (depth > 8 || hits.size() > 20) return;
    for (auto& p : props) {
        std::string path = prefix + (prefix.empty() ? "" : ".") + p.name + (p.arrayIndex ? Fmt("[%d]", p.arrayIndex) : "");
        auto check = [&](const std::string& pth, const PropValue& v) {
            bool nameOk = want.empty() || IEquals(pth, want) || IEndsWith(pth, "." + want) || IEquals(pth.substr(pth.rfind('.') + 1), want);
            if (!nameOk) return;
            std::string text = TableCell(v);
            bool valueOk = value.empty() || (exact ? IEquals(text, value) : ToLower(text).find(ToLower(value)) != std::string::npos);
            if (valueOk) hits.push_back(pth + " = " + (text.size() > 160 ? text.substr(0, 160) + "..." : text));
        };
        check(path, p.value);
        if (p.value.type == PropType::Struct && p.value.Fields()) MatchProps(*p.value.Fields(), path, want, value, exact, hits, depth + 1);
        if (p.value.type == PropType::Array)
            for (size_t i = 0; i < p.value.items.size() && i < 256; i++) {
                const PropValue& it = p.value.items[i];
                std::string ip = path + Fmt("[%zu]", i);
                if (it.type == PropType::Struct && it.Fields()) MatchProps(*it.Fields(), ip, want, value, exact, hits, depth + 1);
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

    api.Register({"search_content",
                  "Search inside packages. Filters (all given ones must match): 'class' = main asset class (StaticMesh, MaterialInstanceConstant, "
                  "BlueprintGeneratedClass, DataTable, ...); 'uses' = the package imports this asset (a texture, material, mesh, Blueprint: path "
                  "or /Game path); 'property' + optional 'value' = some export has this property (name or dotted path, searched in structs and "
                  "arrays) whose value contains 'value' (value_exact for equality); 'name' = substring of the path. With 'uses' the candidates come "
                  "from the reference index (fast, whole game); otherwise restrict 'folder' (packages are read, scan_limit caps how many).",
                  R"({"type":"object","properties":{"folder":{"type":"string"},"class":{"type":"string"},"uses":{"type":"string"},"property":{"type":"string"},"value":{"type":"string"},"value_exact":{"type":"boolean","default":false},"name":{"type":"string"},"limit":{"type":"integer","default":100},"scan_limit":{"type":"integer","default":20000}}})",
                  "folder",
                  [](Api& a, const JsonValue& args) {
                      std::string cls = args.GetStr("class"), uses = args.GetStr("uses"), prop = args.GetStr("property"), value = args.GetStr("value");
                      std::string name = ToLower(args.GetStr("name")), folder = ToLower(Vfs::NormalizePath(args.GetStr("folder")));
                      bool exact = args.GetBool("value_exact", false);
                      int limit = (int)args.GetNum("limit", 100), scanLimit = (int)args.GetNum("scan_limit", 20000);
                      if (cls.empty() && uses.empty() && prop.empty() && value.empty() && name.empty()) return ApiResult::Error("give at least one filter");
                      if (!folder.empty() && folder.back() != '/') folder += '/';
                      // candidates
                      std::vector<u32> cand;
                      std::string err, usesPkg;
                      if (!uses.empty()) {
                          i64 t = a.ResolveFile(uses);
                          if (t < 0) return ApiResult::Error("not found: " + uses);
                          ReferenceIndex* idx = a.EnsureReferences(true, &err);
                          if (!idx) return ApiResult::Error(err);
                          if (auto* r = idx->Referencers((u32)t)) cand = *r;
                      } else {
                          if (folder.empty() && (!prop.empty() || !value.empty() || !cls.empty()))
                              return ApiResult::Error("restrict 'folder' (or use 'uses') - reading every package of the game takes long");
                          for (u32 i = 0; i < a.vfs.FileCount(); i++) {
                              std::string p = ToLower(a.vfs.Path(i));
                              if (!IEndsWith(p, ".uasset") && !IEndsWith(p, ".umap")) continue;
                              if (!folder.empty() && p.rfind(folder, 0) != 0) continue;
                              cand.push_back(i);
                          }
                      }
                      JsonWriter w;
                      w.BeginObject();
                      w.Key("results");
                      w.BeginArray();
                      int found = 0, scanned = 0;
                      bool truncated = false;
                      for (u32 e : cand) {
                          std::string path(a.vfs.Path(e)), low = ToLower(path);
                          if (!folder.empty() && low.rfind(folder, 0) != 0) continue;
                          if (!name.empty() && low.find(name) == std::string::npos) continue;
                          if (cls.empty() && prop.empty() && value.empty()) {   // path-only filters: no need to read the package
                              w.BeginObject();
                              w.KV("path", path);
                              w.EndObject();
                              if (++found >= limit) { truncated = true; break; }
                              continue;
                          }
                          if (++scanned > scanLimit) { truncated = true; break; }
                          auto pkg = a.cache->Load(e, &err);
                          if (!pkg) continue;
                          i32 main = pkg->MainExport();
                          std::string mainCls = main ? pkg->ExportClass(main - 1) : "";
                          if (!cls.empty() && !IEquals(mainCls, cls)) continue;
                          std::vector<std::string> hits;
                          if (!prop.empty() || !value.empty()) {
                              for (size_t x = 0; x < pkg->Exports().size() && hits.size() < 20; x++) {
                                  std::string perr;
                                  PropertyList props = pkg->ReadProperties(x, nullptr, &perr);
                                  std::vector<std::string> h;
                                  MatchProps(props, "", prop, value, exact, h);
                                  for (auto& s : h) hits.push_back(pkg->Exports()[x].objectName + ": " + s);
                                  if (IsTableClass(pkg->ExportClass(x))) {   // DataTable rows count as properties too
                                      TableData t;
                                      if (ReadTable(*pkg, x, t))
                                          for (auto& row : t.rows) {
                                              std::vector<std::string> rh;
                                              MatchProps(row.fields, "", prop, value, exact, rh);
                                              for (auto& s : rh) hits.push_back("Rows." + row.name + ": " + s);
                                          }
                                  }
                              }
                              if (hits.empty()) continue;
                          }
                          w.BeginObject();
                          w.KV("path", path);
                          w.KV("class", mainCls);
                          if (!hits.empty()) {
                              w.Key("matches");
                              w.BeginArray();
                              for (size_t i = 0; i < hits.size() && i < 10; i++) w.Str(hits[i]);
                              w.EndArray();
                          }
                          w.EndObject();
                          if (++found >= limit) { truncated = true; break; }
                      }
                      w.EndArray();
                      w.KV("found", found);
                      w.KV("candidates", (i64)cand.size());
                      w.KV("scanned", scanned);
                      if (truncated) w.KV("truncated", true);
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
