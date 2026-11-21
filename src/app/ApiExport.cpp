// Scene exports: whole maps and several animations of one skeleton as single glTF files.
#include "app/Api.h"
#include "core/Json.h"
#include "export/MeshExport.h"
#include "ue/AnimPose.h"
#include "ue/Mesh.h"

namespace {

std::wstring OutFile(Api& a, const std::string& od, const std::string& gamePath, const std::wstring& ext) {
    std::string rel = gamePath.substr(0, gamePath.rfind('.'));
    if (!a.settings.exportKeepDirectories) rel = rel.substr(rel.rfind('/') + 1);
    std::wstring out = (od.empty() ? a.settings.outputDir : Widen(od)) + L"\\" + Widen(rel) + ext;
    for (auto& c : out) if (c == L'/') c = L'\\';
    CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
    return out;
}

ApiResult Written(const std::vector<std::string>& files, const std::string& note = {}) {
    JsonWriter w;
    w.BeginObject();
    w.KV("ok", true);
    if (!note.empty()) w.KV("note", note);
    w.Key("files");
    w.BeginArray();
    for (auto& f : files) w.Str(f);
    w.EndArray();
    w.EndObject();
    ApiResult r;
    r.json = w.Take();
    return r;
}

}   // namespace

bool ExportAnimationSet(PackageCache& cache, const std::vector<std::shared_ptr<Package>>& animPkgs, const std::string& meshPath, Bytes& glb, std::string& note,
                        std::string* error) {
    std::vector<AnimSequenceData> anims;
    std::string skeletonPath;
    for (auto& p : animPkgs) {
        i32 main = p->MainExport();
        if (!main || !IsAnimSequenceClass(p->ExportClass(main - 1))) continue;
        AnimSequenceData a;
        std::string err;
        if (!ReadAnimSequence(*p, main - 1, a, &err)) { note += p->ShortName() + ": " + err + "; "; continue; }
        if (skeletonPath.empty()) skeletonPath = a.skeletonPath;
        if (!IEquals(a.skeletonPath, skeletonPath)) { note += p->ShortName() + ": other skeleton; "; continue; }
        anims.push_back(std::move(a));
    }
    if (anims.empty()) { if (error) *error = "no animation sequences"; return false; }
    std::string err;
    auto sp = cache.LoadPath(skeletonPath, &err);
    std::vector<Bone> skeleton;
    i32 se = sp ? sp->MainExport() : 0;
    if (!se || !ReadSkeleton(*sp, se - 1, skeleton, &err)) { if (error) *error = "skeleton: " + err; return false; }
    std::string mp = meshPath.empty() ? FindMeshForSkeleton(cache, skeletonPath) : meshPath;
    MeshData mesh;
    auto mpkg = mp.empty() ? nullptr : cache.LoadPath(mp);
    bool withMesh = mpkg && mpkg->MainExport() && ReadMesh(*mpkg, mpkg->MainExport() - 1, mesh) && mesh.skeletal;
    MeshExportOptions mo;
    mo.maxTextureSize = 1024;
    std::vector<const AnimSequenceData*> list;
    for (auto& a : anims) list.push_back(&a);
    if (!ExportAnimationsGlb(cache, list, skeleton, withMesh ? &mesh : nullptr, mo, glb, error)) return false;
    note += Fmt("%zu animation(s) on %s", anims.size(), withMesh ? mesh.name.c_str() : "the bare skeleton");
    return true;
}

void RegisterExportTools(Api& api) {
    api.Register({"export_world",
                  "Export a whole map (.umap) as one glTF .glb for Blender: a node per level, every placed mesh (with material overrides and "
                  "textures, downscaled to max_texture_size) and the landscape terrain. Large maps take a while.",
                  R"({"type":"object","properties":{"path":{"type":"string"},"output_dir":{"type":"string"},"textures":{"type":"boolean","default":true},"max_texture_size":{"type":"integer","default":1024},"lod":{"type":"integer","default":0},"landscape":{"type":"boolean","default":true},"sublevels":{"type":"boolean","default":true},"filter":{"type":"string"}},"required":["path"]})",
                  "path",
                  [](Api& a, const JsonValue& args) {
                      WorldLoader loader(*a.cache);
                      WorldOptions opt;
                      opt.sublevels = args.GetBool("sublevels", true);
                      opt.landscape = args.GetBool("landscape", true);
                      opt.meshFilter = args.GetStr("filter");
                      WorldData world;
                      std::string err;
                      i64 e = a.ResolveFile(args.GetStr("path"));
                      if (e < 0) return ApiResult::Error("map not found: " + args.GetStr("path"));
                      std::string gp(a.vfs.Path((u32)e));
                      if (!loader.Load(gp, opt, world, &err)) return ApiResult::Error(err);
                      WorldExportOptions wo;
                      wo.textures = args.GetBool("textures", true);
                      wo.maxTextureSize = (int)args.GetNum("max_texture_size", 1024);
                      wo.lod = (int)args.GetNum("lod", 0);
                      wo.landscape = opt.landscape;
                      Bytes glb;
                      if (!ExportWorldGlb(*a.cache, world, wo, glb, &err)) return ApiResult::Error(err);
                      std::wstring out = OutFile(a, args.GetStr("output_dir"), gp, L".glb");
                      if (!WriteWholeFile(out, glb.data(), glb.size())) return ApiResult::Error("could not write " + Narrow(out));
                      return Written({Narrow(out)}, Fmt("%zu levels, %zu meshes, %zu instances, %zu landscape patches", world.levels.size(), world.groups.size(),
                                                        world.InstanceCount(), world.landscapes.size()));
                  }});

    api.Register({"export_animations",
                  "Export several animations of one skeleton into one glTF .glb: the skinned mesh once plus one animation (Blender action) per "
                  "sequence. 'animations' lists paths; or 'folder' takes every AnimSequence in it (and below) that uses the same skeleton as the first one. "
                  "'mesh' picks the skeletal mesh (found automatically otherwise).",
                  R"({"type":"object","properties":{"animations":{"type":"array","items":{"type":"string"}},"folder":{"type":"string"},"mesh":{"type":"string"},"output_dir":{"type":"string"},"name":{"type":"string"},"limit":{"type":"integer","default":200}}})",
                  "folder",
                  [](Api& a, const JsonValue& args) {
                      std::vector<std::shared_ptr<Package>> pkgs;
                      std::string err;
                      int limit = (int)args.GetNum("limit", 200);
                      if (const JsonValue* list = args.Get("animations")) {
                          for (auto& v : list->arr) {
                              auto p = a.LoadPackage(v.str, &err);
                              if (p) pkgs.push_back(p);
                          }
                      }
                      std::string folder = args.GetStr("folder");
                      if (!folder.empty()) {
                          std::string prefix = ToLower(Vfs::NormalizePath(folder));
                          if (!prefix.empty() && prefix.back() != '/') prefix += '/';
                          for (u32 i = 0; i < a.vfs.FileCount() && (int)pkgs.size() < limit; i++) {
                              std::string p = ToLower(a.vfs.Path(i));
                              if (p.rfind(prefix, 0) != 0 || !IEndsWith(p, ".uasset")) continue;
                              auto pkg = a.cache->Load(i, &err);
                              if (pkg && pkg->MainExport() && IsAnimSequenceClass(pkg->ExportClass(pkg->MainExport() - 1))) pkgs.push_back(pkg);
                          }
                      }
                      if (pkgs.empty()) return ApiResult::Error("no animations given (use 'animations' or 'folder')");
                      Bytes glb;
                      std::string note;
                      if (!ExportAnimationSet(*a.cache, pkgs, args.GetStr("mesh"), glb, note, &err)) return ApiResult::Error(err);
                      std::string name = args.GetStr("name");
                      std::string base = name.empty() ? pkgs[0]->GamePath() : pkgs[0]->GamePath().substr(0, pkgs[0]->GamePath().rfind('/') + 1) + name + ".uasset";
                      if (name.empty()) base = base.substr(0, base.rfind('/') + 1) + "Animations_" + pkgs[0]->ShortName() + ".uasset";
                      std::wstring out = OutFile(a, args.GetStr("output_dir"), base, L".glb");
                      if (!WriteWholeFile(out, glb.data(), glb.size())) return ApiResult::Error("could not write " + Narrow(out));
                      return Written({Narrow(out)}, note);
                  }});
}
