#include "export/MeshExport.h"
#include "export/Png.h"
#include "ue/Texture.h"

bool TextureToPng(PackageCache& cache, const std::string& path, int maxSize, Bytes& png, std::string* error) {
    auto pkg = cache.LoadPath(path, error);
    if (!pkg) return false;
    i32 main = pkg->MainExport();
    if (!main || !IsTextureClass(pkg->ExportClass(main - 1))) { if (error) *error = "not a texture"; return false; }
    TextureInfo info;
    if (!ReadTextureInfo(*pkg, main - 1, info, error)) return false;
    Image img;
    if (!DecodeTexture(*pkg, info, maxSize > 0 ? -1 : 0, maxSize > 0 ? maxSize : (1 << 30), img, error)) return false;
    png = EncodePng(img.rgba.data(), img.width, img.height);
    return true;
}

int GltfMaterialResolver::Texture(const std::string& path, bool normal) {
    if (path.empty() || !m_opt.embedTextures) return -1;
    auto it = m_images.find(path);
    if (it != m_images.end()) return it->second;
    Bytes png;
    std::string err;
    int idx = -1;
    if (TextureToPng(m_cache, path, m_opt.maxTextureSize, png, &err)) idx = m_gltf.AddImagePng(png, path.substr(path.rfind('/') + 1));
    else LOGW("texture %s: %s", path.c_str(), err.c_str());
    (void)normal;
    m_images[path] = idx;
    return idx;
}

int GltfMaterialResolver::Resolve(const MeshMaterialSlot& slot) {
    std::string key = slot.materialPath.empty() ? "<none>:" + slot.name : slot.materialPath;
    auto it = m_materials.find(key);
    if (it != m_materials.end()) return it->second;
    GltfBuilder::MaterialDesc d;
    d.name = slot.name.empty() ? "None" : slot.name;
    if (!slot.materialPath.empty()) {
        MaterialInfo mi;
        if (ReadMaterial(m_cache, slot.materialPath, mi)) {
            d.baseColorImage = Texture(mi.diffuse, false);
            d.normalImage = Texture(mi.normal, true);
            d.doubleSided = mi.twoSided;
            d.masked = mi.blendMode == "BLEND_Masked";
            d.translucent = mi.blendMode == "BLEND_Translucent" || mi.blendMode == "BLEND_Additive";
        }
    }
    int idx = m_gltf.AddMaterial(d);
    m_materials[key] = idx;
    return idx;
}

std::vector<int> GltfMaterialResolver::ResolveAll(const MeshData& mesh) {
    std::vector<int> map;
    for (auto& s : mesh.materials) map.push_back(Resolve(s));
    // sections may reference slots beyond the material list
    int maxSlot = -1;
    for (auto& l : mesh.lods) for (auto& s : l.sections) maxSlot = std::max(maxSlot, s.materialIndex);
    while ((int)map.size() <= maxSlot) map.push_back(Resolve({"Material_" + std::to_string(map.size()), ""}));
    return map;
}

bool ExportMeshGlb(PackageCache& cache, const MeshData& mesh, const MeshExportOptions& opt, Bytes& out, std::string* error) {
    if (mesh.lods.empty()) { if (error) *error = "mesh has no LODs"; return false; }
    int lod = std::clamp(opt.lod, 0, (int)mesh.lods.size() - 1);
    GltfBuilder g;
    GltfMaterialResolver mats(cache, g, opt);
    std::vector<int> map = mats.ResolveAll(mesh);
    bool skinned = mesh.skeletal && !mesh.skeleton.empty();
    int meshIdx = g.AddMesh(mesh, lod, map, skinned);
    int node = g.AddNode(mesh.name, meshIdx);
    if (skinned) {
        int skin = g.AddSkin(mesh.skeleton, -1);
        g.SetNodeSkin(node, skin);
    }
    out = g.Finish();
    return true;
}

bool ExportMeshObj(const MeshData& mesh, int lodIndex, std::string& obj, std::string& mtl, const std::string& mtlName) {
    if (mesh.lods.empty()) return false;
    const MeshLod& lod = mesh.lods[std::clamp(lodIndex, 0, (int)mesh.lods.size() - 1)];
    obj = "# SOTModel export (metres, Y up)\nmtllib " + mtlName + "\no " + mesh.name + "\n";
    obj.reserve(lod.positions.size() * 80);
    for (auto& p : lod.positions) obj += Fmt("v %.6f %.6f %.6f\n", p.x * 0.01f, p.z * 0.01f, p.y * 0.01f);
    for (size_t i = 0; i < lod.positions.size(); i++) {
        Vec2 uv = lod.numUVs ? lod.uvs[i * lod.numUVs] : Vec2{};
        obj += Fmt("vt %.6f %.6f\n", uv.x, 1.0f - uv.y);
    }
    for (auto& n : lod.normals) obj += Fmt("vn %.4f %.4f %.4f\n", n.x, n.z, n.y);
    bool hasN = lod.normals.size() == lod.positions.size();
    for (auto& s : lod.sections) {
        std::string mat = s.materialIndex >= 0 && s.materialIndex < (int)mesh.materials.size() ? mesh.materials[s.materialIndex].name : "Material_" + std::to_string(s.materialIndex);
        obj += "usemtl " + mat + "\n";
        for (u32 t = 0; t < s.numTriangles; t++) {
            size_t b = s.firstIndex + (size_t)t * 3;
            if (b + 2 >= lod.indices.size()) break;
            u32 a = lod.indices[b] + s.baseVertex + 1, c = lod.indices[b + 1] + s.baseVertex + 1, d = lod.indices[b + 2] + s.baseVertex + 1;
            if (hasN) obj += Fmt("f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, c, c, c, d, d, d);
            else obj += Fmt("f %u/%u %u/%u %u/%u\n", a, a, c, c, d, d);
        }
    }
    mtl.clear();
    for (auto& m : mesh.materials) mtl += "newmtl " + m.name + "\nKd 0.8 0.8 0.8\n# " + m.materialPath + "\n\n";
    return true;
}

bool ExportAnimationGlb(PackageCache& cache, const AnimSequenceData& anim, const std::vector<Bone>& skeleton, const MeshData* mesh, const MeshExportOptions& opt,
                        Bytes& out, std::string* error) {
    if (skeleton.empty()) { if (error) *error = "skeleton has no bones"; return false; }
    GltfBuilder g;
    // joints: the mesh's bones when a mesh is included (so it deforms), otherwise the animation skeleton itself
    const std::vector<Bone>& joints = mesh && mesh->skeletal && !mesh->skeleton.empty() ? mesh->skeleton : skeleton;
    std::vector<int> boneNodes;
    int skin = g.AddSkin(joints, -1, &boneNodes);
    if (mesh && &joints == &mesh->skeleton && !mesh->lods.empty()) {
        GltfMaterialResolver mats(cache, g, opt);
        std::vector<int> map = mats.ResolveAll(*mesh);
        int meshIdx = g.AddMesh(*mesh, std::clamp(opt.lod, 0, (int)mesh->lods.size() - 1), map, true);
        int node = g.AddNode(mesh->name, meshIdx);
        g.SetNodeSkin(node, skin);
    }
    // skeleton bone index -> joint index (by name)
    std::vector<int> toJoint(skeleton.size(), -1);
    for (size_t s = 0; s < skeleton.size(); s++)
        for (size_t j = 0; j < joints.size(); j++)
            if (IEquals(skeleton[s].name, joints[j].name)) { toJoint[s] = (int)j; break; }
    std::vector<float> times(anim.numFrames);
    for (int f = 0; f < anim.numFrames; f++) times[f] = anim.numFrames > 1 ? f * anim.length / (anim.numFrames - 1) : 0.f;
    std::vector<GltfBuilder::Track> tracks;
    for (auto& t : anim.tracks) {
        if (t.bone < 0 || t.bone >= (i32)toJoint.size() || toJoint[t.bone] < 0) continue;
        GltfBuilder::Track tr;
        tr.node = boneNodes[toJoint[t.bone]];
        tr.times = times;
        tr.translations = t.translation;
        tr.rotations = t.rotation;
        tr.scales = t.scale;
        tracks.push_back(std::move(tr));
    }
    if (tracks.empty()) { if (error) *error = "no animated bone matches the skeleton"; return false; }
    g.AddAnimation(anim.name, tracks);
    out = g.Finish();
    return true;
}
