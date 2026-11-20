#pragma once
#include "render/Renderer.h"
#include "ue/Material.h"
#include <unordered_map>

// Decodes and uploads textures / materials / meshes for the viewport, cached by object path. Thread safe.
class GpuAssets {
public:
    GpuAssets(Renderer& r, PackageCache& cache) : m_r(r), m_cache(cache) {}
    void SetMaxTextureSize(int s) { m_maxTex = s; }

    std::shared_ptr<GpuTexture> Texture(const std::string& path, bool forceLinear = false);
    std::shared_ptr<GpuMaterial> Material(const std::string& path);
    // Uploads one LOD of a decoded mesh with its material slots resolved.
    std::shared_ptr<GpuMesh> Mesh(const MeshData& mesh, int lod, bool loadMaterials = true, bool dynamic = false);
    // Loads a mesh asset by object path (cached; nullptr if it can't be decoded).
    std::shared_ptr<GpuMesh> MeshByPath(const std::string& path, int lod = 0);

    void Clear();
    void Trim();   // drops cached assets nobody else references (call after closing views)
    size_t TextureBytes() const { return m_texBytes; }

private:
    Renderer& m_r;
    PackageCache& m_cache;
    int m_maxTex = 1024;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<GpuTexture>> m_textures;
    std::unordered_map<std::string, std::shared_ptr<GpuMaterial>> m_materials;
    std::unordered_map<std::string, std::shared_ptr<GpuMesh>> m_meshes;
    std::atomic<size_t> m_texBytes{0};
};
