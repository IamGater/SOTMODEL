#include "app/Api.h"
#include "ue/Collision.h"
#include "ue/MeshExtras.h"
#include "core/Json.h"
#include "ue/Mesh.h"
#include "ue/Material.h"
#include "render/Offscreen.h"
#include "ue/AnimPose.h"
#include "ue/AnimAssets.h"
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
                  "bounds in cm, simple collision (BodySetup / PhysicsAsset shape counts), sockets (mesh + skeleton, bone, transform), morph targets "
                  "and for skeletal meshes the bone hierarchy.",
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
                      {
                          std::vector<MeshSocket> sockets;
                          ReadMeshSockets(*a.cache, *pkg, main - 1, m, sockets);
                          if (!sockets.empty()) {
                              w.Key("sockets");
                              w.BeginArray();
                              for (auto& so : sockets) {
                                  w.BeginObject();
                                  w.KV("name", so.name);
                                  if (!so.bone.empty()) w.KV("bone", so.bone);
                                  w.Key("location"); w.BeginArray(); w.Num(so.location.x); w.Num(so.location.y); w.Num(so.location.z); w.EndArray();
                                  w.Key("rotation"); w.BeginArray(); w.Num(so.rotation.x); w.Num(so.rotation.y); w.Num(so.rotation.z); w.EndArray();
                                  if (so.scale.x != 1 || so.scale.y != 1 || so.scale.z != 1) { w.Key("scale"); w.BeginArray(); w.Num(so.scale.x); w.Num(so.scale.y); w.Num(so.scale.z); w.EndArray(); }
                                  Vec3 t = so.transform.Translation();
                                  w.Key("meshSpace"); w.BeginArray(); w.Num(t.x); w.Num(t.y); w.Num(t.z); w.EndArray();
                                  if (so.source != "mesh") w.KV("from", so.source);
                                  w.EndObject();
                              }
                              w.EndArray();
                          }
                      }
                      if (m.skeletal) {
                          std::vector<MorphTarget> morphs;
                          ReadMorphTargets(*pkg, main - 1, morphs, nullptr, &m);
                          if (!morphs.empty()) {
                              w.Key("morphTargets");
                              w.BeginArray();
                              for (auto& mt : morphs) {
                                  w.BeginObject();
                                  w.KV("name", mt.name);
                                  w.KV("lod0Vertices", (i64)(mt.lods.empty() ? 0 : mt.lods[0].size()));
                                  w.KV("lods", (i64)mt.lods.size());
                                  if (mt.bodyPatch) w.KV("bodyPatch", true);   // MeshPatchAsset (wardrobe body shape)
                                  w.EndObject();
                              }
                              w.EndArray();
                          }
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
                  R"("collision":{"type":"boolean","default":false,"description":"overlay the simple collision (boxes, spheres, capsules, convex hulls) as green wireframe"},)"
                  R"("sockets":{"type":"boolean","default":false,"description":"draw socket axes: X red, Y green, Z blue"},)"
                  R"("morphs":{"type":"object","description":"skeletal meshes: morph target weights by name, e.g. {\"Female_N\": 1}"},"save_to":{"type":"string"}},"required":["path"]})",
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
                          req.overlays.push_back({0xff60ff40u, {}});
                          CollisionLines(col, req.overlays.back().segments);
                      }
                      std::vector<MeshSocket> sockets;
                      if (args.GetBool("sockets", false) && ReadMeshSockets(*a.cache, *pkg, main - 1, m, sockets)) {
                          float size = std::max(2.f, Length(m.boundsMax - m.boundsMin) * 0.06f);
                          PreviewRequest::Overlay x{0xff4040ffu, {}}, y{0xff40ff40u, {}}, z{0xffff6040u, {}};
                          SocketAxes(sockets, size, x.segments, y.segments, z.segments);
                          req.overlays.push_back(std::move(x));
                          req.overlays.push_back(std::move(y));
                          req.overlays.push_back(std::move(z));
                      }
                      std::vector<std::string> applied;
                      if (const JsonValue* mw = args.Get("morphs"); mw && !mw->obj.empty() && m.skeletal) {
                          std::vector<MorphTarget> morphs;
                          ReadMorphTargets(*pkg, main - 1, morphs, nullptr, &m);
                          std::vector<float> weights(morphs.size(), 0.f);
                          for (auto& [k, v] : mw->obj)
                              for (size_t i = 0; i < morphs.size(); i++)
                                  if (IEquals(morphs[i].name, k)) { weights[i] = (float)v.num; applied.push_back(morphs[i].name); }
                          int lod = std::clamp(req.lod, 0, (int)m.lods.size() - 1);
                          std::vector<Vec3> pos, nrm;
                          ApplyMorphs(m.lods[lod], lod, morphs, weights, pos, nrm);
                          m.lods[lod].positions = std::move(pos);
                          m.lods[lod].normals = std::move(nrm);
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
                      if (!applied.empty()) {
                          w.Key("morphsApplied");
                          w.BeginArray();
                          for (auto& n : applied) w.Str(n);
                          w.EndArray();
                      }
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

    api.Register({"get_anim_asset_info",
                  "Decode an AnimMontage (sections, slot tracks with segments, notifies with times), a BlendSpace / BlendSpace1D / AimOffset "
                  "(axes, samples with positions, grid size; pass x/y to get the sample weights at that point) or an animation blueprint "
                  "(baked state machines with states and transitions, sequence / blend space players, notify events).",
                  R"({"type":"object","properties":{"path":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"}},"required":["path"]})", "path",
                  [](Api& a, const JsonValue& args) {
                      std::shared_ptr<Package> pkg;
                      i32 ex;
                      std::string err;
                      if (!LoadObject(a, args.GetStr("path"), pkg, ex, err)) return ApiResult::Error(err);
                      std::string cls = pkg->ExportClass(ex - 1);
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("class", cls);
                      auto notifies = [&](const std::vector<AnimNotifyInfo>& ns) {
                          w.Key("notifies");
                          w.BeginArray();
                          for (auto& n : ns) {
                              w.BeginObject();
                              w.KV("name", n.name);
                              if (!n.cls.empty()) w.KV("class", n.cls);
                              w.KV("time", (double)n.time);
                              if (n.duration > 0) w.KV("duration", (double)n.duration);
                              w.KV("track", n.track);
                              if (!n.detail.empty()) w.KV("detail", n.detail);
                              if (!n.sound.empty()) w.KV("sound", n.sound);
                              if (n.itemAction != AnimNotifyInfo::NoItem) {
                                  static const char* actions[] = {"", "spawn", "destroy", "switch", "visibility"};
                                  w.KV("itemAction", actions[n.itemAction]);
                                  if (!n.itemClass.empty()) w.KV("itemClass", n.itemClass);
                                  w.KV("hand", n.itemHand ? "left" : "right");
                                  if (n.itemAction == AnimNotifyInfo::SwitchItem) w.KV("newHand", n.itemHand2 ? "left" : "right");
                                  if (n.itemAction == AnimNotifyInfo::ShowItem || n.itemAction == AnimNotifyInfo::SpawnItem) w.KV("visible", n.itemVisible);
                              }
                              if (!n.particle.empty()) {
                                  w.KV("particle", n.particle);
                                  if (!n.socket.empty()) w.KV("socket", n.socket);
                                  w.KV("attached", n.attached);
                              }
                              w.EndObject();
                          }
                          w.EndArray();
                      };
                      bool isBp = false;
                      for (size_t i = 0; i < pkg->Exports().size(); i++) isBp |= pkg->ExportClass(i) == "AnimBlueprintGeneratedClass";
                      if (cls == "AnimMontage") {
                          MontageInfo m;
                          ReadMontage(*pkg, ex - 1, m, &err);
                          w.KV("skeleton", m.skeleton);
                          w.KV("lengthSeconds", (double)m.length);
                          w.KV("rateScale", (double)m.rateScale);
                          w.KV("blendIn", (double)m.blendIn);
                          w.KV("blendOut", (double)m.blendOut);
                          w.Key("sections");
                          w.BeginArray();
                          for (auto& s : m.sections) {
                              w.BeginObject();
                              w.KV("name", s.name);
                              w.KV("start", (double)s.start);
                              if (!s.next.empty() && s.next != "None") w.KV("next", s.next);
                              w.EndObject();
                          }
                          w.EndArray();
                          w.Key("slots");
                          w.BeginArray();
                          for (auto& sl : m.slots) {
                              w.BeginObject();
                              w.KV("slot", sl.name);
                              w.Key("segments");
                              w.BeginArray();
                              for (auto& g : sl.segments) {
                                  w.BeginObject();
                                  w.KV("anim", g.anim);
                                  w.KV("start", (double)g.startPos);
                                  w.KV("length", (double)g.Length());
                                  w.KV("animStart", (double)g.animStart);
                                  w.KV("animEnd", (double)g.animEnd);
                                  w.KV("rate", (double)g.rate);
                                  if (g.loops != 1) w.KV("loops", g.loops);
                                  w.EndObject();
                              }
                              w.EndArray();
                              w.EndObject();
                          }
                          w.EndArray();
                          notifies(m.notifies);
                      } else if (IsBlendSpaceClass(cls)) {
                          BlendSpaceInfo b;
                          if (!ReadBlendSpace(*pkg, ex - 1, b, &err)) return ApiResult::Error(err);
                          w.KV("skeleton", b.skeleton);
                          w.KV("dimensions", b.dims);
                          w.Key("axes");
                          w.BeginArray();
                          for (int k = 0; k < b.dims; k++) {
                              w.BeginObject();
                              w.KV("name", b.axes[k].name);
                              w.KV("min", (double)b.axes[k].min);
                              w.KV("max", (double)b.axes[k].max);
                              w.KV("grid", b.axes[k].grid);
                              w.EndObject();
                          }
                          w.EndArray();
                          std::vector<float> wt;
                          bool at = args.Get("x") != nullptr;
                          if (at) b.Weights((float)args.GetNum("x", 0), (float)args.GetNum("y", 0), wt);
                          w.Key("samples");
                          w.BeginArray();
                          for (size_t i = 0; i < b.samples.size(); i++) {
                              w.BeginObject();
                              w.KV("anim", b.samples[i].anim);
                              w.KV("x", (double)b.samples[i].x);
                              if (b.dims == 2) w.KV("y", (double)b.samples[i].y);
                              if (at) w.KV("weight", (double)wt[i]);
                              w.EndObject();
                          }
                          w.EndArray();
                          w.KV("gridPoints", (i64)b.grid.size());
                      } else if (isBp) {
                          AnimBlueprintInfo bp;
                          if (!ReadAnimBlueprint(*pkg, bp, &err)) return ApiResult::Error(err);
                          w.KV("skeleton", bp.skeleton);
                          w.Key("stateMachines");
                          w.BeginArray();
                          for (auto& sm : bp.machines) {
                              w.BeginObject();
                              w.KV("name", sm.name);
                              w.KV("initialState", sm.initial >= 0 && sm.initial < (int)sm.states.size() ? sm.states[sm.initial].name : std::string());
                              w.Key("states");
                              w.BeginArray();
                              for (auto& st : sm.states) {
                                  w.BeginObject();
                                  w.KV("name", st.name);
                                  if (st.conduit) w.KV("conduit", true);
                                  w.KV("players", (i64)st.players.size());
                                  w.EndObject();
                              }
                              w.EndArray();
                              w.Key("transitions");
                              w.BeginArray();
                              auto nm = [&](int i) { return i >= 0 && i < (int)sm.states.size() ? sm.states[i].name : Fmt("#%d", i); };
                              for (auto& t : sm.transitions) {
                                  w.BeginObject();
                                  w.KV("from", nm(t.from));
                                  w.KV("to", nm(t.to));
                                  w.KV("crossfade", (double)t.crossfade);
                                  if (!t.blendMode.empty()) w.KV("blendMode", t.blendMode);
                                  if (!t.logic.empty()) w.KV("logic", t.logic);
                                  if (t.priority) w.KV("priority", t.priority);
                                  if (t.bidirectional) w.KV("bidirectional", true);
                                  w.EndObject();
                              }
                              w.EndArray();
                              w.EndObject();
                          }
                          w.EndArray();
                          w.Key("players");
                          w.BeginArray();
                          for (auto& pl : bp.players) {
                              w.BeginObject();
                              w.KV("node", pl.node);
                              w.KV("asset", pl.asset);
                              w.EndObject();
                          }
                          w.EndArray();
                          w.Key("notifyEvents");
                          w.BeginArray();
                          for (auto& n : bp.notifies) w.Str(n);
                          w.EndArray();
                      } else {
                          // anim sequences have notifies too
                          std::vector<AnimNotifyInfo> ns;
                          if (!ReadAnimNotifies(*pkg, pkg->ReadProperties(ex - 1), ns))
                              return ApiResult::Error("not a montage, blend space or animation blueprint: " + cls);
                          notifies(ns);
                      }
                      w.EndObject();
                      ApiResult r;
                      r.json = w.Take();
                      return r;
                  }});

    api.Register({"preview_blend_space",
                  "Render the blended pose of a blend space at input (x, y) on a skeletal mesh (found from the skeleton or passed as 'mesh'), "
                  "sampling every animation at 'time' fraction 0..1 of its length.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"time":{"type":"number","default":0},)"
                  R"("mesh":{"type":"string"},"size":{"type":"integer","default":512},"yaw":{"type":"number","default":-135},"pitch":{"type":"number","default":-15},)"
                  R"("save_to":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      std::shared_ptr<Package> pkg;
                      i32 ex;
                      std::string err;
                      if (!LoadObject(a, args.GetStr("path"), pkg, ex, err)) return ApiResult::Error(err);
                      BlendSpaceInfo b;
                      if (!IsBlendSpaceClass(pkg->ExportClass(ex - 1))) return ApiResult::Error("not a blend space: " + pkg->ExportClass(ex - 1));
                      if (!ReadBlendSpace(*pkg, ex - 1, b, &err)) return ApiResult::Error(err);
                      float x = (float)args.GetNum("x", (b.axes[0].min + b.axes[0].max) / 2), y = (float)args.GetNum("y", (b.axes[1].min + b.axes[1].max) / 2);
                      std::vector<float> wt;
                      b.Weights(x, y, wt);
                      std::string meshPath = args.GetStr("mesh");
                      if (meshPath.empty()) meshPath = FindMeshForSkeleton(*a.cache, b.skeleton);
                      if (meshPath.empty()) return ApiResult::Error("no skeletal mesh found for " + b.skeleton + " (pass 'mesh')");
                      std::shared_ptr<Package> mp, sp;
                      i32 me, se;
                      MeshData mesh;
                      std::vector<Bone> skel;
                      if (!LoadObject(a, meshPath, mp, me, err) || !ReadMesh(*mp, me - 1, mesh, &err)) return ApiResult::Error("mesh: " + err);
                      if (!LoadObject(a, b.skeleton, sp, se, err) || !ReadSkeleton(*sp, se - 1, skel, &err)) return ApiResult::Error("skeleton: " + err);
                      AnimPoser poser;
                      if (!poser.Init(mesh, skel, &err)) return ApiResult::Error(err);
                      float tf = std::clamp((float)args.GetNum("time", 0), 0.f, 1.f);
                      std::vector<std::vector<Bone>> poses(b.samples.size());
                      for (size_t i = 0; i < b.samples.size(); i++) {
                          if (wt[i] <= 0) continue;
                          std::shared_ptr<Package> ap;
                          i32 ae;
                          AnimSequenceData anim;
                          std::string e2;
                          if (!LoadObject(a, b.samples[i].anim, ap, ae, e2) || !ReadAnimSequence(*ap, ae - 1, anim, &e2)) { wt[i] = 0; continue; }
                          poser.MeshPose(anim, tf * std::max(0, anim.numFrames - 1), poses[i]);
                      }
                      std::vector<Bone> pose;
                      BlendPoses(poses, wt, pose);
                      if (pose.empty()) pose = poser.MeshBones();
                      MeshData m;
                      m.name = mesh.name;
                      m.skeletal = true;
                      m.materials = mesh.materials;
                      m.lods.push_back(mesh.lods[0]);
                      poser.Skin(mesh.lods[0], pose, m.lods[0].positions, m.lods[0].normals);
                      m.ComputeBounds();
                      PreviewRequest req;
                      req.width = req.height = std::clamp((int)args.GetNum("size", 512), 64, 2048);
                      req.yaw = (float)args.GetNum("yaw", -135);
                      req.pitch = (float)args.GetNum("pitch", -15);
                      std::vector<Image> imgs;
                      std::vector<MeshData> one;
                      one.push_back(std::move(m));
                      if (!RenderMeshPreviews(*a.cache, one, req, imgs, &err) || imgs.empty()) return ApiResult::Error(err);
                      ApiResult r;
                      ApiImage im;
                      im.mime = "image/png";
                      im.data = EncodePng(imgs[0].rgba.data(), imgs[0].width, imgs[0].height);
                      std::string save = args.GetStr("save_to");
                      if (!save.empty()) WriteWholeFile(Widen(save), im.data.data(), im.data.size());
                      JsonWriter w;
                      w.BeginObject();
                      w.KV("mesh", meshPath);
                      w.KV("x", (double)x);
                      w.KV("y", (double)y);
                      w.Key("weights");
                      w.BeginObject();
                      for (size_t i = 0; i < b.samples.size(); i++)
                          if (wt[i] > 0) w.KV(b.samples[i].anim.substr(b.samples[i].anim.rfind('/') + 1), (double)wt[i]);
                      w.EndObject();
                      if (!save.empty()) w.KV("savedTo", save);
                      w.EndObject();
                      r.json = w.Take();
                      r.images.push_back(std::move(im));
                      return r;
                  }});
}
