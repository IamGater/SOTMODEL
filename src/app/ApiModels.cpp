#include "app/Api.h"
#include "ue/Collision.h"
#include "core/Json.h"
#include "ue/Mesh.h"
#include "ue/Material.h"
#include "render/Offscreen.h"
#include "ue/AnimPose.h"
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

// Animation + its skeleton (+ a mesh to show it on).
struct AnimContext {
    AnimSequenceData anim;
    std::vector<Bone> skeleton;
    MeshData mesh;
    std::string meshPath;
    AnimPoser poser;
};

static bool LoadObject(Api& a, const std::string& path, std::shared_ptr<Package>& pkg, i32& ex, std::string& err) {
    pkg = a.LoadPackage(path, &err);
    if (!pkg) return false;
    ex = 0;
    size_t dot = path.rfind('.');
    if (dot != std::string::npos && path.find('/', dot) == std::string::npos) ex = pkg->FindExport(path.substr(dot + 1));
    if (!ex) ex = pkg->MainExport();
    if (!ex) { err = "package has no main export"; return false; }
    return true;
}

static bool LoadAnimContext(Api& a, const JsonValue& args, AnimContext& c, bool needMesh, std::string& err) {
    std::shared_ptr<Package> pkg;
    i32 ex;
    if (!LoadObject(a, args.GetStr("path"), pkg, ex, err)) return false;
    if (!IsAnimSequenceClass(pkg->ExportClass(ex - 1))) { err = "not an AnimSequence: " + pkg->ExportClass(ex - 1); return false; }
    if (!ReadAnimSequence(*pkg, ex - 1, c.anim, &err)) return false;
    if (c.anim.skeletonPath.empty()) { err = "animation has no skeleton"; return false; }
    std::shared_ptr<Package> sp;
    i32 se;
    if (!LoadObject(a, c.anim.skeletonPath, sp, se, err) || !ReadSkeleton(*sp, se - 1, c.skeleton, &err)) { err = "skeleton: " + err; return false; }
    if (!needMesh) return true;
    c.meshPath = args.GetStr("mesh");
    if (c.meshPath.empty()) c.meshPath = FindMeshForSkeleton(*a.cache, c.anim.skeletonPath);
    if (c.meshPath.empty()) { err = "no skeletal mesh found for " + c.anim.skeletonPath + " (pass 'mesh')"; return false; }
    std::shared_ptr<Package> mp;
    i32 me;
    if (!LoadObject(a, c.meshPath, mp, me, err) || !ReadMesh(*mp, me - 1, c.mesh, &err)) { err = "mesh: " + err; return false; }
    if (!c.mesh.skeletal) { err = "mesh is not skeletal"; return false; }
    return c.poser.Init(c.mesh, c.skeleton, &err);
}

static float MaxTurnDeg(const std::vector<Vec4>& r) {
    float turn = 0;
    for (size_t f = 1; f < r.size(); f++) {
        const Vec4 &p = r[0], &q = r[f];
        float d = fabsf(p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w);
        turn = std::max(turn, 2 * acosf(std::min(1.f, d)) * 180 / kPi);
    }
    return turn;
}

void RegisterMeshTools(Api& api) {
    api.Register({"get_animation_info",
                  "Decode an AnimSequence: frames, length, fps, compression, skeleton and the animated bones (with how much each "
                  "moves), plus a skeletal mesh that can display it.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"bones":{"type":"boolean","default":true}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      AnimContext c;
                      std::string err;
                      if (!LoadAnimContext(a, args, c, false, err)) return ApiResult::Error(err);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("name", c.anim.name);
                      w.KV("frames", c.anim.numFrames);
                      w.KV("lengthSeconds", (double)c.anim.length);
                      w.KV("fps", (double)c.anim.FrameRate());
                      w.KV("encoding", c.anim.encoding);
                      w.KV("rotationFormat", c.anim.rotationFormat);
                      w.KV("translationFormat", c.anim.translationFormat);
                      w.KV("additiveType", c.anim.additiveType);
                      w.KV("skeleton", c.anim.skeletonPath);
                      w.KV("skeletonBones", (i64)c.skeleton.size());
                      w.KV("tracks", (i64)c.anim.tracks.size());
                      std::string mesh = FindMeshForSkeleton(*a.cache, c.anim.skeletonPath);
                      if (!mesh.empty()) w.KV("previewMesh", mesh);
                      if (args.GetBool("bones", true)) {
                          w.Key("animatedBones");
                          w.BeginArray();
                          for (auto& t : c.anim.tracks) {
                              w.BeginObject();
                              w.KV("bone", t.bone >= 0 && t.bone < (i32)c.skeleton.size() ? c.skeleton[t.bone].name : Fmt("#%d", t.bone));
                              float move = 0;
                              for (size_t f = 1; f < t.translation.size(); f++) move = std::max(move, Length(t.translation[f] - t.translation[0]));
                              w.KV("maxMoveCm", (double)move);
                              w.KV("maxTurnDeg", (double)MaxTurnDeg(t.rotation));
                              if (!t.scale.empty()) w.KV("scaleKeys", true);
                              w.EndObject();
                          }
                          w.EndArray();
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"preview_animation",
                  "Render an animation on a skeletal mesh as a contact sheet PNG: 'frames' evenly spaced poses (or one pose at 'time' "
                  "seconds). 'mesh' picks the mesh; by default one using the animation's skeleton is found automatically.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"mesh":{"type":"string"},"frames":{"type":"integer","default":6},)"
                  R"("time":{"type":"number"},"size":{"type":"integer","default":320},"yaw":{"type":"number","default":-135},"pitch":{"type":"number","default":-15},)"
                  R"("lod":{"type":"integer","default":0},"save_to":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      AnimContext c;
                      std::string err;
                      if (!LoadAnimContext(a, args, c, true, err)) return ApiResult::Error(err);
                      int lod = std::clamp((int)args.GetNum("lod", 0), 0, (int)c.mesh.lods.size() - 1);
                      std::vector<float> frames;
                      if (args.Get("time")) frames.push_back((float)args.GetNum("time", 0) * c.anim.FrameRate());
                      else {
                          int n = std::clamp((int)args.GetNum("frames", 6), 1, 16);
                          for (int i = 0; i < n; i++) frames.push_back(n > 1 ? (float)i * (c.anim.numFrames - 1) / (n - 1) : 0.f);
                      }
                      std::vector<MeshData> posed;
                      Vec3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
                      for (float f : frames) {
                          MeshData m;
                          m.name = c.mesh.name;
                          m.skeletal = true;
                          m.materials = c.mesh.materials;
                          m.lods.push_back(c.mesh.lods[lod]);
                          std::vector<Bone> pose;
                          c.poser.MeshPose(c.anim, f, pose);
                          c.poser.Skin(c.mesh.lods[lod], pose, m.lods[0].positions, m.lods[0].normals);
                          m.ComputeBounds();
                          bmin = {std::min(bmin.x, m.boundsMin.x), std::min(bmin.y, m.boundsMin.y), std::min(bmin.z, m.boundsMin.z)};
                          bmax = {std::max(bmax.x, m.boundsMax.x), std::max(bmax.y, m.boundsMax.y), std::max(bmax.z, m.boundsMax.z)};
                          posed.push_back(std::move(m));
                      }
                      PreviewRequest req;
                      req.width = req.height = std::clamp((int)args.GetNum("size", 320), 64, 1024);
                      req.yaw = (float)args.GetNum("yaw", -135);
                      req.pitch = (float)args.GetNum("pitch", -15);
                      req.fixedBounds = true;
                      req.boundsMin = bmin;
                      req.boundsMax = bmax;
                      std::vector<Image> imgs;
                      if (!RenderMeshPreviews(*a.cache, posed, req, imgs, &err)) return ApiResult::Error(err);
                      int cols = std::min<int>((int)imgs.size(), 4), rows = ((int)imgs.size() + cols - 1) / cols;
                      int cw = req.width, ch = req.height;
                      Image sheet;
                      sheet.width = cols * cw;
                      sheet.height = rows * ch;
                      sheet.rgba.assign((size_t)sheet.width * sheet.height * 4, 0);
                      for (size_t i = 0; i < imgs.size(); i++) {
                          int ox = (int)(i % cols) * cw, oy = (int)(i / cols) * ch;
                          for (int y = 0; y < ch; y++) memcpy(&sheet.rgba[((size_t)(oy + y) * sheet.width + ox) * 4], &imgs[i].rgba[(size_t)y * cw * 4], (size_t)cw * 4);
                      }
                      ApiResult r;
                      ApiImage im;
                      im.mime = "image/png";
                      im.data = EncodePng(sheet.rgba.data(), sheet.width, sheet.height);
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("animation", c.anim.name);
                      w.KV("mesh", c.meshPath);
                      w.KV("matchedBones", c.poser.MatchedBones());
                      w.KV("meshBones", (i64)c.mesh.skeleton.size());
                      w.Key("framesShown");
                      w.BeginArray();
                      for (float f : frames) w.Num(f);
                      w.EndArray();
                      w.KV("layout", Fmt("%d columns x %d rows, left to right", cols, rows));
                      if (!save.empty()) w.KV("savedTo", save);
                      w.EndObject();
                      r.json = w.Take();
                      r.images.push_back(std::move(im));
                      return r;
                  }});

    api.Register({"get_mesh_info",
                  "Decoded static/skeletal mesh summary: LODs (vertices, triangles, UV sets, sections), material slots with paths, "
                  "bounds in cm, simple collision (BodySetup / PhysicsAsset shape counts), and for skeletal meshes the bone hierarchy.",
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
                      {
                          CollisionData col;
                          std::string cerr;
                          w.Key("collision");
                          w.BeginObject();
                          if (ReadMeshCollision(*a.cache, *pkg, main - 1, m, col, &cerr)) {
                              w.KV("source", col.source);
                              w.KV("boxes", (i64)col.Count(CollisionShape::Box));
                              w.KV("spheres", (i64)col.Count(CollisionShape::Sphere));
                              w.KV("capsules", (i64)col.Count(CollisionShape::Capsule));
                              w.KV("convex", (i64)col.Count(CollisionShape::Convex));
                              if (col.bodies) w.KV("bodies", (i64)col.bodies);
                          } else w.KV("none", cerr);
                          if (!col.traceFlag.empty()) w.KV("traceFlag", col.traceFlag);
                          w.EndObject();
                      }
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
                  R"("textures":{"type":"boolean","default":true},"wireframe":{"type":"boolean","default":false},"grid":{"type":"boolean","default":true},)"
                  R"("collision":{"type":"boolean","default":false,"description":"overlay the simple collision (boxes, spheres, capsules, convex hulls) as green wireframe"},"save_to":{"type":"string"}},"required":["path"]})",
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
                      CollisionData col;
                      if (args.GetBool("collision", false) && ReadMeshCollision(*a.cache, *pkg, main - 1, m, col)) {
                          CollisionLines(col, req.lines);
                          req.lineColor = 0xff60ff40u;
                      }
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
