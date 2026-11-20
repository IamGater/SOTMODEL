#include "app/Api.h"
#include "core/Json.h"
#include "ue/Mesh.h"
#include "ue/Material.h"
#include "render/Offscreen.h"
#include "export/Png.h"

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

    api.Register({"preview_mesh",
                  "Render a mesh (with its textures) to a PNG image so it can be looked at. yaw/pitch orbit the camera in degrees "
                  "(yaw -135 pitch -20 is a 3/4 front view; yaw 0 looks along +X). Use 'save_to' to also write the PNG.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"width":{"type":"integer","default":768},"height":{"type":"integer","default":768},)"
                  R"("yaw":{"type":"number","default":-135},"pitch":{"type":"number","default":-20},"lod":{"type":"integer","default":0},)"
                  R"("textures":{"type":"boolean","default":true},"wireframe":{"type":"boolean","default":false},"grid":{"type":"boolean","default":true},"save_to":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::string err;
                      auto pkg = a.LoadPackage(args.GetStr("path"), &err);
                      if (!pkg) return ApiResult::Error(err);
                      i32 main = pkg->MainExport();
                      MeshData m;
                      if (!main || !ReadMesh(*pkg, main - 1, m, &err)) return ApiResult::Error(err.empty() ? "not a mesh" : err);
                      PreviewRequest req;
                      req.width = (int)args.GetNum("width", 768);
                      req.height = (int)args.GetNum("height", 768);
                      req.yaw = (float)args.GetNum("yaw", -135);
                      req.pitch = (float)args.GetNum("pitch", -20);
                      req.lod = (int)args.GetNum("lod", 0);
                      req.draw.textures = args.GetBool("textures", true);
                      req.draw.wireframe = args.GetBool("wireframe", false);
                      req.draw.grid = args.GetBool("grid", true);
                      Image img;
                      if (!RenderMeshPreview(*a.cache, m, req, img, &err)) return ApiResult::Error(err);
                      ApiResult r;
                      ApiImage im;
                      im.mime = "image/png";
                      im.data = EncodePng(img.rgba.data(), img.width, img.height);
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("path", pkg->GamePath());
                      w.KV("lodCount", (i64)m.lods.size());
                      w.KV("triangles", (i64)(m.lods[std::clamp(req.lod, 0, (int)m.lods.size() - 1)].indices.size() / 3));
                      w.KV("sizeCm", Fmt("%.1f x %.1f x %.1f", m.boundsMax.x - m.boundsMin.x, m.boundsMax.y - m.boundsMin.y, m.boundsMax.z - m.boundsMin.z));
                      if (!save.empty()) w.KV("savedTo", save);
                      w.EndObject();
                      r.json = w.Take();
                      r.images.push_back(std::move(im));
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
