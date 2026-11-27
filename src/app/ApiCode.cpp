// Global code search over the decompiled Blueprints and the materials' translated HLSL (CodeIndex).
#include "app/Api.h"
#include "core/Json.h"

void RegisterCodeTools(Api& api) {
    api.Register({"search_code",
                  "Search the game's code: the decompiled C++ of every Blueprint (decompile_blueprint) and the translated HLSL of every "
                  "material and material function (get_material_hlsl). Returns matching lines (case-insensitive; every space-separated "
                  "term must be on the line, \"quoted terms\" may hold spaces) with the package path and line number. kind = all | "
                  "blueprint | material; folder = path substring. The index is generated once for the whole game (build=true: about "
                  "ten minutes the first time, then cached in CodeIndex.bin next to the settings).",
                  R"({"type":"object","properties":{"query":{"type":"string"},"kind":{"type":"string","enum":["all","blueprint","material"],"default":"all"},)"
                  R"("folder":{"type":"string"},"limit":{"type":"integer","default":100},"build":{"type":"boolean","default":true}},"required":["query"]})",
                  "query",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      auto idx = a.EnsureCodeIndex(args.GetBool("build", true), &err);
                      if (!idx) return ApiResult::Error(err);
                      std::string k = args.GetStr("kind", "all");
                      int kinds = k == "blueprint" ? 1 : k == "material" ? 2 : 3;
                      size_t limit = (size_t)std::clamp(args.GetNum("limit", 100), 1.0, 5000.0);
                      size_t docs = 0;
                      auto hits = idx->Search(args.GetStr("query"), kinds, args.GetStr("folder"), limit, a.vfs, &docs);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("documents", (i64)idx->Docs().size());
                      w.KV("documentsMatched", (i64)docs);
                      w.KV("truncated", hits.size() >= limit);
                      w.Key("hits");
                      w.BeginArray();
                      for (auto& h : hits) {
                          const CodeDoc& d = idx->Docs()[h.doc];
                          w.BeginObject();
                          w.KV("path", std::string(a.vfs.Path(d.entry)));
                          w.KV("kind", d.kind == CodeDoc::Blueprint ? "blueprint" : "material");
                          w.KV("line", (i64)h.line);
                          w.KV("text", h.text);
                          w.EndObject();
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
