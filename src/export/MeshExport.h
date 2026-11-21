#pragma once
#include "export/Gltf.h"
#include "ue/Material.h"
#include "ue/Animation.h"
#include "ue/World.h"
#include <map>

struct CollisionData;
struct MeshExportOptions {
    int lod = 0;
    bool embedTextures = true;
    int maxTextureSize = 0;   // 0 = full resolution
    const CollisionData* collision = nullptr;   // simple collision as a separate "Collision" node (UCX_ mesh)
};

// Shared helper: resolves mesh material slots into glTF materials (with decoded textures), caching per material path.
class GltfMaterialResolver {
public:
    GltfMaterialResolver(PackageCache& cache, GltfBuilder& gltf, const MeshExportOptions& opt) : m_cache(cache), m_gltf(gltf), m_opt(opt) {}
    int Resolve(const MeshMaterialSlot& slot);
    std::vector<int> ResolveAll(const MeshData& mesh);

private:
    int Texture(const std::string& path, bool normal);
    PackageCache& m_cache;
    GltfBuilder& m_gltf;
    MeshExportOptions m_opt;
    std::map<std::string, int> m_materials, m_images;
};

bool ExportMeshGlb(PackageCache& cache, const MeshData& mesh, const MeshExportOptions& opt, Bytes& out, std::string* error);
bool ExportMeshObj(const MeshData& mesh, int lod, std::string& obj, std::string& mtl, const std::string& mtlName);

// Decodes a texture asset by object path into PNG (downscaled to maxSize when > 0).
bool TextureToPng(PackageCache& cache, const std::string& path, int maxSize, Bytes& png, std::string* error = nullptr);

// Animation as glTF: skeleton joints (+ the skinned mesh when given) and one animation clip.
bool ExportAnimationGlb(PackageCache& cache, const AnimSequenceData& anim, const std::vector<Bone>& skeleton, const MeshData* mesh, const MeshExportOptions& opt,
                        Bytes& out, std::string* error);
// Several animations of one skeleton in one file: the skinned mesh once, one glTF animation (Blender action) per sequence.
bool ExportAnimationsGlb(PackageCache& cache, const std::vector<const AnimSequenceData*>& anims, const std::vector<Bone>& skeleton, const MeshData* mesh,
                         const MeshExportOptions& opt, Bytes& out, std::string* error);

// A loaded map as one glTF scene: a node per level, every mesh once with an instance node per placement (textures embedded,
// downscaled to maxTextureSize), landscape terrain as world-space meshes.
struct WorldExportOptions {
    int lod = 0;
    bool textures = true;
    int maxTextureSize = 1024;
    bool landscape = true;
    bool lights = true;   // lights (KHR_lights_punctual), decal boxes (empties with extras), fog + water level (scene extras)
};
bool ExportWorldGlb(PackageCache& cache, const WorldData& world, const WorldExportOptions& opt, Bytes& out, std::string* error = nullptr,
                    const std::function<void(int, int)>& progress = {});
