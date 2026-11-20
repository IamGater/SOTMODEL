#include "app/Api.h"
#include "core/Json.h"
#include "ue/Mesh.h"
#include "ue/Material.h"

static void WriteMaterialJson(JsonWriter& w, const MaterialInfo& m) {
    w.BeginObject();
    w.KV("path", m.path);
    w.Key("chain");
    w.BeginArray();
    for (auto& c : m.chain) w.Str(c);
    w.EndArray();
    w.KV("blendMode", m.blendMode);
    if (!m.shadingModel.empty()) w.KV("shadingModel", m.shadingModel);
    w.KV("twoSided", m.twoSided);
    w.Key("guessedSlots");
    w.BeginObject();
    if (!m.diffuse.empty()) w.KV("diffuse", m.diffuse);
    if (!m.normal.empty()) w.KV("normal", m.normal);
    if (!m.packed.empty()) w.KV("packedMasks", m.packed);
    if (!m.emissive.empty()) w.KV("emissive", m.emissive);
    w.EndObject();
    w.Key("textures");
    w.BeginArray();
    for (auto& t : m.textures) {
        w.BeginObject();
        w.KV("parameter", t.name);
        w.KV("texture", t.texturePath);
        w.KV("kind", ClassifyTexture(t.name == "<referenced>" ? "" : t.name, t.texturePath));
        w.KV("definedIn", t.definedIn);
        w.EndObject();
    }
    w.EndArray();
    w.Key("scalars");
    w.BeginObject();
    for (auto& s : m.scalars) w.KV(s.name, (double)s.value);
    w.EndObject();
    w.Key("vectors");
    w.BeginObject();
    for (auto& v : m.vectors) {
        w.Key(v.name);
        w.BeginArray();
        w.Num(v.r); w.Num(v.g); w.Num(v.b); w.Num(v.a);
        w.EndArray();
    }
    w.EndObject();
    if (!m.staticSwitches.empty()) {
        w.Key("staticSwitches");
        w.BeginArray();
        for (auto& s : m.staticSwitches) w.Str(s);
        w.EndArray();
    }
    w.EndObject();
}

void RegisterMeshTools(Api& api) {
    api.Register({"get_mesh_info",
                  "Decoded static/skeletal mesh summary: LODs (vertices, triangles, UV sets, sections), material slots with paths, "
                  "bounds in cm, and for skeletal meshes the bone hierarchy.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"bones":{"type":"boolean","default":true}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                      if (!pkg) return ApiResult::Error(err);
                      i32 main = pkg->MainExport();
                      MeshData m;
                      if (!main || !ReadMesh(*pkg, main - 1, m, &err)) return ApiResult::Error(err.empty() ? "not a mesh" : err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("name", m.name);
                      w.KV("type", m.skeletal ? "SkeletalMesh" : "StaticMesh");
                      w.Key("boundsCm");
                      w.BeginObject();
                      w.Key("min"); w.BeginArray(); w.Num(m.boundsMin.x); w.Num(m.boundsMin.y); w.Num(m.boundsMin.z); w.EndArray();
                      w.Key("max"); w.BeginArray(); w.Num(m.boundsMax.x); w.Num(m.boundsMax.y); w.Num(m.boundsMax.z); w.EndArray();
                      w.EndObject();
                      w.Key("lods");
                      w.BeginArray();
                      for (auto& l : m.lods) {
                          w.BeginObject();
                          w.KV("vertices", (i64)l.positions.size());
                          w.KV("triangles", (i64)(l.indices.size() / 3));
                          w.KV("uvSets", l.numUVs);
                          w.KV("vertexColors", !l.colors.empty());
                          if (l.influences) w.KV("maxInfluences", l.influences);
                          w.Key("sections");
                          w.BeginArray();
                          for (auto& s : l.sections) {
                              w.BeginObject();
                              w.KV("material", s.materialIndex);
                              w.KV("triangles", (i64)s.numTriangles);
                              w.EndObject();
                          }
                          w.EndArray();
                          w.EndObject();
                      }
                      w.EndArray();
                      w.Key("materials");
                      w.BeginArray();
                      for (auto& s : m.materials) {
                          w.BeginObject();
                          w.KV("slot", s.name);
                          w.KV("material", s.materialPath);
                          w.EndObject();
                      }
                      w.EndArray();
                      if (m.skeletal) {
                          if (!m.skeletonPath.empty()) w.KV("skeleton", m.skeletonPath);
                          w.KV("boneCount", (i64)m.skeleton.size());
                          if (args.GetBool("bones", true)) {
                              w.Key("bones");
                              w.BeginArray();
                              for (auto& b : m.skeleton) {
                                  w.BeginObject();
                                  w.KV("name", b.name);
                                  w.KV("parent", b.parent >= 0 ? m.skeleton[b.parent].name : std::string());
                                  w.EndObject();
                              }
                              w.EndArray();
                          }
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"get_material",
                  "Decode a material or material instance: the parent chain, every texture/scalar/vector parameter (child overrides "
                  "applied), blend mode, two-sidedness and guessed diffuse/normal/mask textures.",
                  R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      MaterialInfo m;
                      std::string err, path = args.GetStr("path");
                      if (a.ResolveFile(path) < 0) return ApiResult::Error("file not found: " + path);
                      if (!ReadMaterial(*a.cache, path, m, &err)) return ApiResult::Error(err);
                      JsonWriter w;
                      WriteMaterialJson(w, m);
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});
}
