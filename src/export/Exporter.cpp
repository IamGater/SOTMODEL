#include "export/Exporter.h"
#include "ue/AssetJson.h"
#include "ue/Texture.h"
#include "export/Png.h"
#include "export/MeshExport.h"
#include "ue/Mesh.h"
#include "ue/AnimPose.h"
#include "ue/World.h"
#include "ue/World.h"

std::wstring ExportPathFor(const ExportOptions& opt, const Package& pkg, const std::wstring& ext) {
    std::string rel = pkg.GamePath();
    size_t dot = rel.rfind('.');
    if (dot != std::string::npos) rel.resize(dot);
    if (!opt.keepDirectories) rel = rel.substr(rel.rfind('/') + 1);
    std::wstring p = opt.outputDir + L"\\" + Widen(rel) + ext;
    for (auto& c : p) if (c == L'/') c = L'\\';
    return p;
}

static bool WriteOut(const std::wstring& path, const void* data, size_t size, std::vector<std::string>& written) {
    CreateDirectories(path.substr(0, path.find_last_of(L'\\')));
    if (!WriteWholeFile(path, data, size)) return false;
    written.push_back(Narrow(path));
    return true;
}

static Bytes MakeDds(const TextureInfo& t, const Bytes& data, int w, int h) {
    // DX10-header DDS so every BC format round-trips
    u32 dxgi = 0;
    const std::string& f = t.pixelFormat;
    bool srgb = t.srgb;
    if (f == "PF_DXT1") dxgi = srgb ? 72 : 71;
    else if (f == "PF_DXT3") dxgi = srgb ? 75 : 74;
    else if (f == "PF_DXT5") dxgi = srgb ? 78 : 77;
    else if (f == "PF_BC4") dxgi = 80;
    else if (f == "PF_BC5" || f == "PF_ATI2") dxgi = 83;
    else if (f == "PF_BC6H") dxgi = 95;
    else if (f == "PF_BC7") dxgi = srgb ? 99 : 98;
    else if (f == "PF_B8G8R8A8") dxgi = srgb ? 91 : 87;
    else if (f == "PF_R8G8B8A8") dxgi = srgb ? 29 : 28;
    else if (f == "PF_G8") dxgi = 61;
    else if (f == "PF_FloatRGBA") dxgi = 10;
    else if (f == "PF_A32B32G32R32F") dxgi = 2;
    if (!dxgi) return {};
    Bytes o(4 + 124 + 20, 0);
    auto put = [&](size_t at, u32 v) { memcpy(o.data() + at, &v, 4); };
    memcpy(o.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
    put(12, h);
    put(16, w);
    put(20, (u32)data.size());
    put(28, 1);
    put(76, 32);
    put(80, 4);
    memcpy(o.data() + 84, "DX10", 4);
    put(108, 0x1000);
    put(128, dxgi);
    put(132, 3);   // texture2d
    put(140, 1);   // array size
    o.insert(o.end(), data.begin(), data.end());
    return o;
}

static bool ExportTexture(const Package& pkg, size_t exportIdx, const ExportOptions& opt, std::vector<std::string>& written, std::string* error) {
    TextureInfo info;
    if (!ReadTextureInfo(pkg, exportIdx, info, error)) return false;
    if (opt.textureFormat == 1) {
        int mip = 0;
        while (mip + 1 < (int)info.mips.size() && info.mips[mip].bulk.Unused()) mip++;
        Bytes data;
        if (!LoadBulkData(pkg, info.mips[mip].bulk, data, error)) return false;
        Bytes dds = MakeDds(info, data, info.mips[mip].sizeX, info.mips[mip].sizeY);
        if (!dds.empty()) return WriteOut(ExportPathFor(opt, pkg, L".dds"), dds.data(), dds.size(), written);
    }
    Image img;
    if (!DecodeTexture(pkg, info, 0, 1 << 30, img, error)) return false;
    Bytes png = EncodePng(img.rgba.data(), img.width, img.height);
    return WriteOut(ExportPathFor(opt, pkg, L".png"), png.data(), png.size(), written);
}

bool ExportPackage(PackageCache& cache, const Package& pkg, const ExportOptions& opt, std::vector<std::string>& written, std::string* error) {
    bool ok = true;
    i32 main = pkg.MainExport();
    std::string cls = main ? pkg.ExportClass(main - 1) : "";
    if (IsTextureClass(cls)) {
        std::string err;
        if (!ExportTexture(pkg, main - 1, opt, written, &err)) { ok = false; if (error) *error = err; }
    } else if (IsStaticMeshClass(cls) || IsSkeletalMeshClass(cls)) {
        std::string err;
        MeshData mesh;
        if (!ReadMesh(pkg, main - 1, mesh, &err)) { ok = false; if (error) *error = err; }
        else if (opt.meshFormat == 1) {
            std::string obj, mtl;
            std::wstring path = ExportPathFor(opt, pkg, L".obj");
            std::wstring mtlPath = path.substr(0, path.size() - 4) + L".mtl";
            ExportMeshObj(mesh, 0, obj, mtl, Narrow(mtlPath.substr(mtlPath.find_last_of(L'\\') + 1)));
            ok &= WriteOut(path, obj.data(), obj.size(), written);
            ok &= WriteOut(mtlPath, mtl.data(), mtl.size(), written);
        } else {
            MeshExportOptions mo;
            mo.maxTextureSize = opt.maxTextureSize;
            mo.embedTextures = opt.embedTextures;
            Bytes glb;
            if (!ExportMeshGlb(cache, mesh, mo, glb, &err)) { ok = false; if (error) *error = err; }
            else ok &= WriteOut(ExportPathFor(opt, pkg, L".glb"), glb.data(), glb.size(), written);
        }
    } else if (IsAnimSequenceClass(cls)) {
        std::string err;
        AnimSequenceData anim;
        std::vector<Bone> skeleton;
        if (!ReadAnimSequence(pkg, main - 1, anim, &err)) { ok = false; if (error) *error = err; }
        else {
            auto sp = cache.LoadPath(anim.skeletonPath, &err);
            i32 se = sp ? sp->MainExport() : 0;
            if (!se || !ReadSkeleton(*sp, se - 1, skeleton, &err)) { ok = false; if (error) *error = "skeleton: " + err; }
            else {
                MeshData mesh;
                bool withMesh = false;
                if (opt.animationMesh) {
                    std::string mp = FindMeshForSkeleton(cache, anim.skeletonPath);
                    auto mpkg = mp.empty() ? nullptr : cache.LoadPath(mp);
                    withMesh = mpkg && mpkg->MainExport() && ReadMesh(*mpkg, mpkg->MainExport() - 1, mesh);
                }
                MeshExportOptions mo;
                mo.maxTextureSize = opt.maxTextureSize ? opt.maxTextureSize : 1024;
                mo.embedTextures = opt.embedTextures;
                Bytes glb;
                if (!ExportAnimationGlb(cache, anim, skeleton, withMesh ? &mesh : nullptr, mo, glb, &err)) { ok = false; if (error) *error = err; }
                else ok &= WriteOut(ExportPathFor(opt, pkg, L".glb"), glb.data(), glb.size(), written);
            }
        }
    } else if (IsWorldClass(cls) && opt.worlds) {   // the whole map (with its sublevels) as one scene
        std::string err;
        WorldLoader loader(cache);
        WorldOptions wopt;
        WorldData world;
        WorldExportOptions wo;
        wo.maxTextureSize = opt.maxTextureSize ? opt.maxTextureSize : 1024;
        wo.textures = opt.embedTextures;
        Bytes glb;
        if (!loader.Load(pkg.GamePath(), wopt, world, &err) || !ExportWorldGlb(cache, world, wo, glb, &err)) { ok = false; if (error) *error = "map: " + err; }
        else ok &= WriteOut(ExportPathFor(opt, pkg, L".glb"), glb.data(), glb.size(), written);
    }
    if (opt.json) {
        std::string j = PackageToJson(pkg);
        ok &= WriteOut(ExportPathFor(opt, pkg, L".json"), j.data(), j.size(), written);
    }
    if (opt.raw) {
        std::wstring ext = IEndsWith(pkg.GamePath(), ".umap") ? L".umap" : L".uasset";
        ok &= WriteOut(ExportPathFor(opt, pkg, ext), pkg.Data().data(), pkg.Data().size(), written);
    }
    if (!ok && error && error->empty()) *error = "could not write output files";
    return ok;
}
