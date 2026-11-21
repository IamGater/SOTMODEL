// Shader tools: the compiled D3D11 shaders of a material's shader maps (or the global shader cache) as a list, and one
// shader as HLSL, assembly, reflection or raw DXBC.
#include "app/Api.h"
#include <set>
#include "core/Json.h"
#include "ue/Shaders.h"
#include "render/ShaderTools.h"
#include "ue/MaterialGraph.h"
#include "ue/Material.h"
#include "ue/Mesh.h"
#include "render/Offscreen.h"
#include "render/GraphShader.h"
#include "export/Png.h"

namespace {

bool LoadShaders(Api& a, const std::string& path, std::vector<ShaderEntry>& out, std::string& name, std::string& err,
                 std::vector<MaterialUniformSet>* sets = nullptr) {
    out.clear();
    if (sets) sets->clear();
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
    if (!ReadPackageShaders(*pkg, out, &err)) return false;
    std::vector<MaterialUniformSet> local;
    ReadMaterialUniformSets(*pkg, sets ? *sets : local);
    AssignUniformSets(out, sets ? *sets : local);
    return true;
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
        for (size_t i = 0; i < s.uniformBuffers.size(); i++)
            w.Str(i < s.uniformBufferSlots.size() ? Fmt("%s (b%d)", s.uniformBuffers[i].c_str(), s.uniformBufferSlots[i]) : s.uniformBuffers[i]);
        w.EndArray();
    }
    if (s.materialBufferSlot >= 0) w.KV("materialBuffer", Fmt("b%d", s.materialBufferSlot));
    if (s.uniformSet >= 0) w.KV("uniformSet", s.uniformSet);
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
                  "'reflection' (constant buffers, bindings, signatures) or 'dxbc' (raw bytecode; needs save_to). For material shaders the "
                  "HLSL starts with a legend of the Material uniform buffer (cbN[i] -> vector / scalar parameters and folded uniform "
                  "expressions, texture order) and reads of it are commented with the parameter (annotate=false turns this off). 'save_to' writes "
                  "the result to a file instead of returning it.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"index":{"type":"integer","default":0},"format":{"type":"string","enum":["hlsl","hlsl_exact","asm","reflection","dxbc"],"default":"hlsl"},"annotate":{"type":"boolean","default":true},"save_to":{"type":"string"},"max_chars":{"type":"integer","default":200000}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::vector<ShaderEntry> list;
                      std::vector<MaterialUniformSet> sets;
                      std::string name, err;
                      if (!LoadShaders(a, args.GetStr("path"), list, name, err, &sets)) return ApiResult::Error(err);
                      i64 index = (i64)args.GetNum("index", 0);
                      if (index < 0 || index >= (i64)list.size()) return ApiResult::Error(Fmt("index out of range (0..%zu)", list.size() - 1));
                      auto& s = list[(size_t)index];
                      std::string fmt = args.GetStr("format", "hlsl"), text, save = args.GetStr("save_to");
                      bool ok = true;
                      if (fmt == "hlsl" || fmt == "hlsl_exact") {
                          ok = DecompileShader(s.dxbc, text, fmt == "hlsl_exact", &err);
                          // name the Material uniform buffer reads (cbN[i] -> parameter / uniform expression)
                          if (ok && args.GetBool("annotate", true) && s.materialBufferSlot >= 0 && s.uniformSet >= 0 && s.uniformSet < (int)sets.size())
                          {
                              std::string legend = AnnotateMaterialBuffer(text, s.materialBufferSlot, sets[s.uniformSet]);
                              text = legend + "\n" + text;
                          }
                      }
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

    api.Register({"get_material_hlsl",
                  "Translate a material's node graph to readable HLSL (UE 4.10 HLSLMaterialTranslator style): parameters with their "
                  "values, Custom nodes as functions, material functions inlined, static switches resolved, then the connected "
                  "attributes (BaseColor, Normal, Roughness, ..., WorldPositionOffset, CustomizedUVs). Works on a Material, a "
                  "MaterialInstance (master graph + the instance's values and static switches) or a MaterialFunction. 'save_to' writes a file.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"save_to":{"type":"string"},"max_chars":{"type":"integer","default":200000}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err, hlsl;
                      MaterialGraphStats st;
                      if (!a.EnsureMounted(&err)) return ApiResult::Error(err);
                      if (!MaterialToHlsl(*a.cache, args.GetStr("path"), hlsl, &st, &err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("nodes", (i64)st.nodes);
                      w.KV("functionCalls", (i64)st.functions);
                      w.Key("attributes");
                      w.BeginArray();
                      for (auto& x : st.attributes) w.Str(x);
                      w.EndArray();
                      w.Key("unsupported");
                      w.BeginArray();
                      for (auto& x : st.unsupported) w.Str(x);
                      w.EndArray();
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) {
                          std::wstring p = Widen(save);
                          size_t slash = p.find_last_of(L"\/");
                          if (slash != std::wstring::npos) CreateDirectories(p.substr(0, slash));
                          if (!WriteWholeFile(p, hlsl.data(), hlsl.size())) return ApiResult::Error("could not write " + save);
                          w.KV("written", save);
                      } else {
                          size_t maxc = (size_t)args.GetNum("max_chars", 200000);
                          if (hlsl.size() > maxc) { w.KV("truncated", true); hlsl.resize(maxc); }
                          w.KV("hlsl", hlsl);
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"get_material_graph",
                  "The expression nodes of a material or material function as a graph: name, type, caption (parameter / function / "
                  "constant), editor position and input links (input pin -> source node and output index).",
                  R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      std::vector<MaterialGraphNode> nodes;
                      if (!a.EnsureMounted(&err)) return ApiResult::Error(err);
                      if (!ReadMaterialGraph(*a.cache, args.GetStr("path"), nodes, &err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("count", (i64)nodes.size());
                      w.Key("nodes");
                      w.BeginArray();
                      for (auto& n : nodes) {
                          w.BeginObject();
                          w.KV("name", n.name);
                          w.KV("type", n.type);
                          if (!n.caption.empty()) w.KV("caption", n.caption);
                          w.KV("x", (i64)n.x);
                          w.KV("y", (i64)n.y);
                          if (!n.inputs.empty()) {
                              w.Key("inputs");
                              w.BeginArray();
                              for (auto& l : n.inputs) {
                                  w.BeginObject();
                                  w.KV("input", l.input);
                                  w.KV("from", l.from);
                                  if (l.output) w.KV("output", (i64)l.output);
                                  w.EndObject();
                              }
                              w.EndArray();
                          }
                          w.EndObject();
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"preview_material",
                  "Render a material on a sphere (or on 'mesh'). graph=true (default) compiles the material's translated node graph "
                  "(get_material_hlsl) into a pixel shader with stubbed engine inputs, so textures, parameters, panners and blend modes "
                  "act as in the graph; graph=false uses the viewer's simple diffuse / normal shading. 'time' fixes the material time; "
                  "'shader_source=true' returns the generated shader.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"graph":{"type":"boolean","default":true},"mesh":{"type":"string"},)"
                  R"("size":{"type":"integer","default":512},"yaw":{"type":"number","default":180},"pitch":{"type":"number","default":-10},)"
                  R"("time":{"type":"number","default":0},"shader_source":{"type":"boolean","default":false},"save_to":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err, path = args.GetStr("path");
                      if (!a.EnsureMounted(&err)) return ApiResult::Error(err);
                      MaterialInfo mi;
                      if (!ReadMaterial(*a.cache, path, mi, &err)) return ApiResult::Error(err);
                      MeshData m;
                      std::string meshPath = args.GetStr("mesh");
                      if (!meshPath.empty()) {
                          auto mp = a.cache->LoadPath(meshPath, &err);
                          if (!mp || !mp->MainExport() || !ReadMesh(*mp, mp->MainExport() - 1, m, &err)) return ApiResult::Error("mesh: " + err);
                          for (auto& s : m.materials) s.materialPath = path;
                      } else {
                          m.name = "sphere";
                          m.lods.push_back(MakePreviewSphere(50, 64, 32));
                          MeshMaterialSlot slot;
                          slot.materialPath = path;
                          m.materials.push_back(slot);
                          m.ComputeBounds();
                      }
                      PreviewRequest req;
                      req.width = req.height = std::clamp((int)args.GetNum("size", 512), 64, 2048);
                      req.yaw = (float)args.GetNum("yaw", 180);
                      req.pitch = (float)args.GetNum("pitch", -10);
                      req.draw.grid = false;
                      req.draw.graphMaterials = args.GetBool("graph", true);
                      req.draw.time = (float)args.GetNum("time", 0);
                      std::vector<std::string> notes;
                      req.graphNotes = &notes;
                      Image img;
                      if (!RenderMeshPreview(*a.cache, m, req, img, &err)) return ApiResult::Error(err);
                      ApiResult r;
                      ApiImage im;
                      im.mime = "image/png";
                      im.data = EncodePng(img.rgba.data(), img.width, img.height);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("material", path);
                      w.KV("blendMode", mi.blendMode);
                      if (!mi.shadingModel.empty()) w.KV("shadingModel", mi.shadingModel);
                      w.Key("graphShader");
                      w.BeginArray();
                      for (auto& n : notes) w.Str(n);
                      w.EndArray();
                      if (args.GetBool("shader_source", false)) {
                          std::string hlsl;
                          GraphShaderSource src;
                          if (MaterialToHlsl(*a.cache, path, hlsl, nullptr, &err) && BuildGraphShaderSource(hlsl, mi.blendMode, mi.shadingModel, src, &err)) w.KV("shaderSource", src.hlsl);
                      }
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) {
                          WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                          w.KV("savedTo", save);
                      }
                      w.EndObject();
                      r.json = w.Take();
                      r.images.push_back(std::move(im));
                      return r;
                  }});

    api.Register({"get_material_uniforms",
                  "The uniform expression sets of a material's shader maps (one per quality level / feature level): what the "
                  "Material uniform buffer holds. Vector expressions take one float4 each (cbN[0..]), scalar expressions follow packed "
                  "four per float4; 2D / cube texture expressions are in binding order. Each shader in list_shaders names its set "
                  "('uniformSet') and the buffer register ('materialBuffer').",
                  R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      std::vector<ShaderEntry> list;
                      std::vector<MaterialUniformSet> sets;
                      std::string name, err;
                      if (!LoadShaders(a, args.GetStr("path"), list, name, err, &sets)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("source", name);
                      w.Key("sets");
                      w.BeginArray();
                      for (size_t i = 0; i < sets.size(); i++) {
                          auto& st = sets[i];
                          int shaders = 0;
                          std::set<int> slots;
                          for (auto& s : list)
                              if (s.uniformSet == (int)i) {
                                  shaders++;
                                  if (s.materialBufferSlot >= 0) slots.insert(s.materialBufferSlot);
                              }
                          w.BeginObject();
                          w.KV("index", (i64)i);
                          w.KV("shaders", shaders);
                          std::string sl;
                          for (int x : slots) sl += (sl.empty() ? "b" : ", b") + std::to_string(x);
                          if (!sl.empty()) w.KV("materialBuffer", sl);
                          w.Key("registers");
                          w.BeginArray();
                          int nv = (int)st.vectors.size(), ns = ((int)st.scalars.size() + 3) / 4;
                          for (int k = 0; k < nv + ns; k++) w.Str(Fmt("[%d] %s", k, st.Describe(k, -1).c_str()));
                          w.EndArray();
                          auto arr = [&](const char* key, const std::vector<std::string>& v) {
                              if (v.empty()) return;
                              w.Key(key);
                              w.BeginArray();
                              for (auto& x : v) w.Str(x);
                              w.EndArray();
                          };
                          arr("textures2D", st.textures2D);
                          arr("texturesCube", st.texturesCube);
                          arr("perFrameScalars", st.perFrameScalars);
                          arr("perFrameVectors", st.perFrameVectors);
                          w.EndObject();
                      }
                      w.EndArray();
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
