// Shader tools: the compiled D3D11 shaders of a material's shader maps (or the global shader cache) as a list, and one
// shader as HLSL, assembly, reflection or raw DXBC.
#include "app/Api.h"
#include "core/Json.h"
#include "ue/Shaders.h"
#include "render/ShaderTools.h"

namespace {

bool LoadShaders(Api& a, const std::string& path, std::vector<ShaderEntry>& out, std::string& name, std::string& err) {
    out.clear();
    if (IsShaderCacheFile(path) || IEndsWith(path, ".bin")) {
        i64 e = a.ResolveFile(path);
        if (e < 0) { err = "file not found: " + path; return false; }
        Bytes data;
        if (!a.vfs.Read((u32)e, data, &err)) return false;
        name = std::string(a.vfs.Path((u32)e));
        ScanShaders(data, nullptr, out);
        if (out.empty()) { err = "no shaders found in " + name; return false; }
        return true;
    }
    auto pkg = a.LoadPackage(path, &err);
    if (!pkg) return false;
    name = pkg->PackageName();
    return ReadPackageShaders(*pkg, out, &err);
}

void WriteEntry(JsonWriter& w, const ShaderEntry& s, size_t index) {
    w.BeginObject();
    w.KV("index", (i64)index);
    w.KV("stage", ShaderFrequencyName(s.frequency));
    w.KV("type", s.type);
    if (!s.vertexFactory.empty()) w.KV("vertexFactory", s.vertexFactory);
    w.KV("instructions", (i64)s.numInstructions);
    w.KV("textureSamplers", (i64)s.numTextureSamplers);
    w.KV("bytes", (i64)s.dxbc.size());
    if (!s.uniformBuffers.empty()) {
        w.Key("uniformBuffers");
        w.BeginArray();
        for (auto& u : s.uniformBuffers) w.Str(u);
        w.EndArray();
    }
    if (!s.usage.empty()) w.KV("usage", s.usage);
    w.EndObject();
}

}   // namespace

void RegisterShaderTools(Api& api) {
    api.Register({"list_shaders",
                  "List the compiled D3D11 (SM5) shaders stored in a material package's shader maps, or in "
                  "Engine/Content/GlobalShaderCache-PCD3D_SM5.bin: index, stage, shader type, vertex factory, instruction count, "
                  "uniform buffers, shader map usage. Optional filters: 'stage' (vertex/pixel/compute/...), 'type' and "
                  "'vertex_factory' wildcards. Use the index with decompile_shader.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"stage":{"type":"string"},"type":{"type":"string"},"vertex_factory":{"type":"string"},"offset":{"type":"integer","default":0},"limit":{"type":"integer","default":500}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::vector<ShaderEntry> list;
                      std::string name, err;
                      if (!LoadShaders(a, args.GetStr("path"), list, name, err)) return ApiResult::Error(err);
                      std::string stage = args.GetStr("stage"), type = args.GetStr("type"), vf = args.GetStr("vertex_factory");
                      size_t offset = (size_t)args.GetNum("offset", 0), limit = (size_t)args.GetNum("limit", 500), matched = 0;
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("source", name);
                      w.KV("count", (i64)list.size());
                      w.Key("shaders");
                      w.BeginArray();
                      for (size_t i = 0; i < list.size(); i++) {
                          auto& s = list[i];
                          if (!stage.empty() && !IEquals(stage, ShaderFrequencyName(s.frequency)) && !IEquals(stage, ShaderStagePrefix(s.frequency))) continue;
                          if (!type.empty() && !WildcardMatch(type, s.type)) continue;
                          if (!vf.empty() && !WildcardMatch(vf, s.vertexFactory)) continue;
                          if (matched++ < offset || matched > offset + limit) continue;
                          WriteEntry(w, s, i);
                      }
                      w.EndArray();
                      w.KV("matched", (i64)matched);
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"decompile_shader",
                  "Decompile one compiled shader (index from list_shaders) of a material package or the global shader cache. "
                  "format: 'hlsl' (default; readable HLSL with constant buffer members, textures and samplers named from the "
                  "reflection data), 'hlsl_exact' (temps as raw uint4 lanes, recompiles bit-exactly), 'asm' (D3D disassembly), "
                  "'reflection' (constant buffers, bindings, signatures) or 'dxbc' (raw bytecode; needs save_to). 'save_to' writes "
                  "the result to a file instead of returning it.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"index":{"type":"integer","default":0},"format":{"type":"string","enum":["hlsl","hlsl_exact","asm","reflection","dxbc"],"default":"hlsl"},"save_to":{"type":"string"},"max_chars":{"type":"integer","default":200000}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::vector<ShaderEntry> list;
                      std::string name, err;
                      if (!LoadShaders(a, args.GetStr("path"), list, name, err)) return ApiResult::Error(err);
                      i64 index = (i64)args.GetNum("index", 0);
                      if (index < 0 || index >= (i64)list.size()) return ApiResult::Error(Fmt("index out of range (0..%zu)", list.size() - 1));
                      auto& s = list[(size_t)index];
                      std::string fmt = args.GetStr("format", "hlsl"), text, save = args.GetStr("save_to");
                      bool ok = true;
                      if (fmt == "hlsl" || fmt == "hlsl_exact") ok = DecompileShader(s.dxbc, text, fmt == "hlsl_exact", &err);
                      else if (fmt == "asm") ok = DisassembleShader(s.dxbc, text, &err);
                      else if (fmt == "reflection") {
                          ShaderReflectionInfo info;
                          ok = ReflectShader(s.dxbc, info, &err);
                          if (ok) text = ReflectionToText(info);
                      } else if (fmt == "dxbc") {
                          if (save.empty()) return ApiResult::Error("format 'dxbc' needs save_to");
                      } else return ApiResult::Error("unknown format " + fmt);
                      if (!ok) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("source", name);
                      w.Key("shader");
                      WriteEntry(w, s, (size_t)index);
                      w.KV("format", fmt);
                      if (!save.empty()) {
                          std::wstring p = Widen(save);
                          size_t slash = p.find_last_of(L"\\/");
                          if (slash != std::wstring::npos) CreateDirectories(p.substr(0, slash));
                          const void* d = fmt == "dxbc" ? (const void*)s.dxbc.data() : (const void*)text.data();
                          size_t n = fmt == "dxbc" ? s.dxbc.size() : text.size();
                          if (!WriteWholeFile(p, d, n)) return ApiResult::Error("could not write " + save);
                          w.KV("written", save);
                          w.KV("bytes", (i64)n);
                      } else {
                          size_t maxc = (size_t)args.GetNum("max_chars", 200000);
                          if (text.size() > maxc) {
                              w.KV("truncated", true);
                              text.resize(maxc);
                          }
                          w.KV("text", text);
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
