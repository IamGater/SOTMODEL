#pragma once
#include "export/Gltf.h"
#include "ue/Material.h"
#include "ue/Animation.h"
#include <map>

struct MeshExportOptions {
    int lod = 0;
    bool embedTextures = true;
    int maxTextureSize = 0;   // 0 = full resolution
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
