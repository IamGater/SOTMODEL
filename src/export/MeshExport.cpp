#include "export/MeshExport.h"
#include "ue/Collision.h"
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
    if (opt.collision && !opt.collision->shapes.empty()) {
        MeshData cm;
        cm.name = "UCX_" + mesh.name;
        cm.lods.push_back(CollisionMesh(*opt.collision));
        GltfBuilder::MaterialDesc mat;
        mat.name = "Collision";
        mat.baseColor[0] = 0.2f; mat.baseColor[1] = 0.9f; mat.baseColor[2] = 0.3f; mat.baseColor[3] = 0.35f;
        mat.translucent = true;
        mat.doubleSided = true;
        int cmIdx = g.AddMesh(cm, 0, {g.AddMaterial(mat)}, false);
        int cn = g.AddNode("Collision", -1);
        int un = g.AddNode(cm.name, cmIdx, nullptr, cn);
        g.SetNodeExtras(un, Fmt("{\"collision\":true,\"boxes\":%d,\"spheres\":%d,\"capsules\":%d,\"convex\":%d}",
                                opt.collision->Count(CollisionShape::Box), opt.collision->Count(CollisionShape::Sphere),
                                opt.collision->Count(CollisionShape::Capsule), opt.collision->Count(CollisionShape::Convex)));
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
    return ExportAnimationsGlb(cache, {&anim}, skeleton, mesh, opt, out, error);
}

bool ExportAnimationsGlb(PackageCache& cache, const std::vector<const AnimSequenceData*>& anims, const std::vector<Bone>& skeleton, const MeshData* mesh,
                         const MeshExportOptions& opt, Bytes& out, std::string* error) {
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
    int clips = 0;
    for (const AnimSequenceData* anim : anims) {   // one glTF animation (Blender action) per sequence
        std::vector<float> times(anim->numFrames);
        for (int f = 0; f < anim->numFrames; f++) times[f] = anim->numFrames > 1 ? f * anim->length / (anim->numFrames - 1) : 0.f;
        std::vector<GltfBuilder::Track> tracks;
        for (auto& t : anim->tracks) {
            if (t.bone < 0 || t.bone >= (i32)toJoint.size() || toJoint[t.bone] < 0) continue;
            GltfBuilder::Track tr;
            tr.node = boneNodes[toJoint[t.bone]];
            tr.times = times;
            tr.translations = t.translation;
            tr.rotations = t.rotation;
            tr.scales = t.scale;
            tracks.push_back(std::move(tr));
        }
        if (tracks.empty()) continue;
        g.AddAnimation(anim->name, tracks);
        clips++;
    }
    if (!clips) { if (error) *error = "no animated bone matches the skeleton"; return false; }
    out = g.Finish();
    return true;
}

// ------------------------------------------------------------------ whole maps

// glTF node matrix (column-major) placing a light at `pos` shining along the UE direction `dir` (glTF lights shine along -Z)
static void LightMatrix(const Vec3& pos, const Vec3& dir, float out[16]) {
    Vec3 d = Normalize(GltfBuilder::ConvertDir(dir));
    Vec3 z = d * -1.f;
    Vec3 up = fabsf(z.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 x = Normalize(Cross(up, z));
    Vec3 y = Cross(z, x);
    Vec3 t = GltfBuilder::ConvertPos(pos);
    float m[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, t.x, t.y, t.z, 1};
    memcpy(out, m, sizeof m);
}

static std::string JsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}

static void ExportWorldEffects(GltfBuilder& g, const WorldData& w, const std::vector<int>& levelNodes) {
    auto parentOf = [&](u32 level) { return levelNodes[std::min<size_t>(level, levelNodes.size() - 1)]; };
    static const char* types[] = {"point", "spot", "directional"};
    int n = 0;
    for (auto& l : w.lights) {
        if (l.type == WorldLight::Sky) continue;
        float intensity = l.type == WorldLight::Directional ? l.intensity
                          : l.inverseSquared ? l.intensity / (4 * kPi)   // lumens -> candela
                                             : l.intensity * 100;          // unitless brightness
        float range = l.type == WorldLight::Directional ? 0 : l.radius * 0.01f;
        std::string name = Fmt("%s_%d", types[l.type], n++);
        int li = g.AddLight(name, types[l.type], l.color, intensity, range, l.innerCone * kPi / 180, std::max(l.outerCone, l.innerCone + 0.5f) * kPi / 180);
        float m[16];
        LightMatrix(l.position, l.direction, m);
        int node = g.AddNode(name, -1, m, parentOf(l.level));
        g.SetNodeLight(node, li);
    }
    for (auto& d : w.decals) {
        // an empty scaled to the decal box (half extents): Blender shows it as a cube
        Mat4 t = Mat4::FromTRS({}, {0, 0, 0, 1}, d.size * 0.01f) * d.transform;   // half extents in metres
        float m[16];
        GltfBuilder::ConvertMatrix(&t.m[0][0], m);
        std::string name = "Decal_" + d.material.substr(d.material.rfind('.') + 1);
        int node = g.AddNode(name, -1, m, parentOf(d.level));
        Vec3 ext{Length(d.transform.TransformDir({d.size.x, 0, 0})), Length(d.transform.TransformDir({0, d.size.y, 0})), Length(d.transform.TransformDir({0, 0, d.size.z}))};
        g.SetNodeExtras(node, "{\"decalMaterial\":" + JsonStr(d.material) + Fmt(",\"halfExtentsCm\":[%g,%g,%g]}", ext.x, ext.y, ext.z));
    }
    std::string extras;
    if (w.hasWater) extras += Fmt("\"waterLevel\":%g", w.waterLevel * 0.01f);
    for (auto& f : w.fogs) {
        if (!extras.empty()) extras += ",";
        extras += Fmt("\"%s\":{\"density\":%g,\"heightFalloff\":%g,\"fogHeight\":%g,\"startDistance\":%g,\"maxOpacity\":%g,\"color\":[%g,%g,%g]}",
                      f.underwater ? "underwaterFog" : "heightFog", f.density, f.heightFalloff, f.position.z * 0.01f, f.startDistance * 0.01f, f.maxOpacity,
                      f.color.x, f.color.y, f.color.z);
    }
    if (!extras.empty()) g.SetSceneExtras("{" + extras + "}");
}

static Vec3 VMin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
static Vec3 VMax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

// The part of a world within the clip sphere, shifted so the centre sits at the origin when asked.
static WorldData ClipWorld(const WorldData& w, const WorldExportOptions& wo) {
    Vec3 c = wo.clip ? wo.clipCenter : (w.boundsMin + w.boundsMax) * 0.5f;
    Vec3 off = wo.recenter ? c * -1.f : Vec3{};
    float r2 = wo.clipRadius * wo.clipRadius;
    auto inside = [&](const Vec3& p) { return !wo.clip || Dot(p - c, p - c) <= r2; };
    auto shift = [&](Mat4 m) { m.m[3][0] += off.x; m.m[3][1] += off.y; m.m[3][2] += off.z; return m; };
    WorldData o;
    o.root = w.root;
    o.levels = w.levels;
    o.actors = w.actors;
    o.unresolved = w.unresolved;
    o.hasWater = w.hasWater;
    o.waterLevel = w.waterLevel + off.z;
    std::string filter = ToLower(wo.meshFilter);
    for (auto& g : w.groups) {
        if (!filter.empty() && ToLower(g.mesh).find(filter) == std::string::npos) continue;
        WorldMeshGroup ng = g;
        ng.instances.clear();
        for (auto& inst : g.instances)
            if (inside(inst.transform.Translation())) {
                WorldInstance ni = inst;
                ni.transform = shift(inst.transform);
                ng.instances.push_back(ni);
                Vec3 p = ni.transform.Translation();
                o.boundsMin = VMin(o.boundsMin, p);
                o.boundsMax = VMax(o.boundsMax, p);
            }
        if (!ng.instances.empty()) o.groups.push_back(std::move(ng));
    }
    for (auto& l : w.landscapes) {   // patches touching the sphere
        Vec3 q = VMax(l.boundsMin, VMin(c, l.boundsMax));
        if (wo.clip && Dot(q - c, q - c) > r2) continue;
        WorldLandscape nl = l;
        for (auto& p : nl.mesh.positions) p = p + off;
        nl.boundsMin = l.boundsMin + off;
        nl.boundsMax = l.boundsMax + off;
        o.landscapes.push_back(std::move(nl));
    }
    for (auto& l : w.lights)
        if (l.type == WorldLight::Directional || l.type == WorldLight::Sky || inside(l.position)) {
            WorldLight nl = l;
            nl.position = l.position + off;
            o.lights.push_back(nl);
        }
    for (auto& d : w.decals)
        if (inside(d.transform.Translation())) {
            WorldDecal nd = d;
            nd.transform = shift(d.transform);
            o.decals.push_back(nd);
        }
    for (auto& f : w.fogs) {
        WorldFog nf = f;
        nf.position = f.position + off;
        o.fogs.push_back(nf);
    }
    return o;
}

bool ExportWorldGlb(PackageCache& cache, const WorldData& world, const WorldExportOptions& wo, Bytes& out, std::string* error,
                    const std::function<void(int, int)>& progress, WorldExportStats* stats) {
    WorldData clipped;
    bool part = wo.clip || wo.recenter || !wo.meshFilter.empty();
    if (part) clipped = ClipWorld(world, wo);
    const WorldData& w = part ? clipped : world;
    if (part && w.groups.empty() && w.landscapes.empty()) { if (error) *error = "nothing matches (radius / mesh filter)"; return false; }
    GltfBuilder g;
    MeshExportOptions mo;
    mo.embedTextures = wo.textures;
    mo.maxTextureSize = wo.maxTextureSize;
    mo.lod = wo.lod;
    GltfMaterialResolver mats(cache, g, mo);
    // one node per level, meshes underneath (instances share their glTF mesh)
    std::vector<int> levelNodes;
    for (auto& l : w.levels) {
        std::string name = l.path.substr(l.path.rfind('/') + 1);
        levelNodes.push_back(g.AddNode(name.substr(0, name.rfind('.')), -1));
    }
    if (levelNodes.empty()) levelNodes.push_back(g.AddNode("World", -1));
    int done = 0, total = (int)w.groups.size() + (wo.landscape ? 1 : 0), meshes = 0;
    for (auto& grp : w.groups) {
        if (progress) progress(done, total);
        done++;
        std::string err;
        auto pkg = cache.LoadPath(grp.mesh, &err);
        MeshData mesh;
        if (!pkg || !pkg->MainExport() || !ReadMesh(*pkg, pkg->MainExport() - 1, mesh, &err) || mesh.lods.empty()) {
            LOGW("map export: %s: %s", grp.mesh.c_str(), err.c_str());
            continue;
        }
        for (size_t s = 0; s < grp.materials.size() && s < mesh.materials.size(); s++)   // per-instance material overrides
            if (!grp.materials[s].empty()) mesh.materials[s].materialPath = grp.materials[s];
        std::vector<int> map = mats.ResolveAll(mesh);
        int lod = std::clamp(wo.lod, 0, (int)mesh.lods.size() - 1);
        int meshIdx = g.AddMesh(mesh, lod, map, false);
        meshes++;
        std::string shortName = grp.mesh.substr(grp.mesh.rfind('.') + 1);
        for (auto& inst : grp.instances) {
            float m[16];
            GltfBuilder::ConvertMatrix(&inst.transform.m[0][0], m);
            g.AddNode(shortName, meshIdx, m, levelNodes[std::min<size_t>(inst.level, levelNodes.size() - 1)]);
        }
    }
    if (wo.landscape && !w.landscapes.empty()) {
        // terrain patches are already in world space; vertex colours carry the paint layers
        GltfBuilder::MaterialDesc land;
        land.name = "Landscape";
        land.baseColor[0] = land.baseColor[1] = land.baseColor[2] = 1;
        int landMat = g.AddMaterial(land);
        int landRoot = g.AddNode("Landscape", -1);
        for (auto& l : w.landscapes) {
            if (l.mesh.positions.empty()) continue;
            MeshData md;
            md.name = l.name;
            md.lods.push_back(l.mesh);
            if (md.lods[0].sections.empty()) md.lods[0].sections.push_back({0, 0, (u32)(l.mesh.indices.size() / 3), 0, (u32)l.mesh.positions.size() - 1, true, 0});
            int mi = g.AddMesh(md, 0, {landMat}, false);
            g.AddNode(l.name, mi, nullptr, landRoot);
        }
    }
    if (wo.lights) ExportWorldEffects(g, w, levelNodes);
    if (progress) progress(total, total);
    if (!meshes && w.landscapes.empty()) { if (error) *error = "the map has no exportable meshes"; return false; }
    if (stats) *stats = {(size_t)meshes, w.InstanceCount(), wo.landscape ? w.landscapes.size() : 0};
    out = g.Finish();
    return true;
}