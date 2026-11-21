// Localisation tools: the game's translated strings (.locres) by text, key or namespace, in any shipped language.
#include "app/Api.h"
#include "core/Json.h"

namespace {

// the current table, or a temporary one for another culture
std::shared_ptr<Localization> Table(Api& a, const std::string& culture, std::string& err) {
    if (culture.empty() || (a.localization && IEquals(a.localization->Culture(), culture))) return a.localization;
    auto t = std::make_shared<Localization>();
    if (!t->Load(a.vfs, culture, &err)) return nullptr;
    return t;
}

}   // namespace

void RegisterLocalizationTools(Api& api) {
    api.Register({"search_localization",
                  "Search the game's localised strings (.locres): text containing all query terms (case-insensitive; keys and namespaces too "
                  "with in_keys=true). 'culture' picks the language (en, de-DE, fr-FR, ru-RU, zh-CN, ...; default: the configured one). "
                  "Returns namespace, key and text, so FText values in package JSON can be matched.",
                  R"({"type":"object","properties":{"query":{"type":"string"},"culture":{"type":"string"},"in_keys":{"type":"boolean","default":false},"limit":{"type":"integer","default":100}},"required":["query"]})",
                  "query",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      auto t = Table(a, args.GetStr("culture"), err);
                      if (!t) return ApiResult::Error(err.empty() ? "no localisation loaded" : err);
                      auto hits = t->Search(args.GetStr("query"), (size_t)args.GetNum("limit", 100), args.GetBool("in_keys", false));
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("culture", t->Culture());
                      w.KV("strings", (i64)t->Size());
                      w.Key("results");
                      w.BeginArray();
                      for (auto* e : hits) {
                          w.BeginObject();
                          w.KV("namespace", e->ns);
                          w.KV("key", e->key);
                          w.KV("text", e->text);
                          w.EndObject();
                      }
                      w.EndArray();
                      w.Key("cultures");
                      w.BeginArray();
                      for (auto& c : Localization::Cultures(a.vfs)) w.Str(c);
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"get_localized_text",
                  "Translate one FText: its namespace + key in every shipped language (or just 'culture').",
                  R"({"type":"object","properties":{"namespace":{"type":"string","default":""},"key":{"type":"string"},"culture":{"type":"string"}},"required":["key"]})",
                  "key",
                  [](Api& a, const JsonValue& args) {
                      std::string ns = args.GetStr("namespace"), key = args.GetStr("key"), only = args.GetStr("culture");
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("namespace", ns);
                      w.KV("key", key);
                      w.Key("texts");
                      w.BeginObject();
                      for (auto& c : Localization::Cultures(a.vfs)) {
                          if (!only.empty() && !IEquals(c, only)) continue;
                          std::string err;
                          auto t = Table(a, c, err);
                          const std::string* s = t ? t->Find(ns, key) : nullptr;
                          if (s) w.KV(c, *s);
                      }
                      w.EndObject();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
