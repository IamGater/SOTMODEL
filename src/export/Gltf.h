#pragma once
#include "ue/Mesh.h"

// Builds a binary glTF 2.0 (.glb). Input is in Unreal space (cm, Z up, left handed);
// output is glTF space (m, Y up, right handed): gltf = (x, z, y) / 100.
class GltfBuilder {
public:
    struct MaterialDesc {
        std::string name;
        int baseColorImage = -1, normalImage = -1;
        float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.f};
        bool doubleSided = false;
        bool masked = false, translucent = false;
    };

    int AddImagePng(const Bytes& png, const std::string& name);
    int AddMaterial(const MaterialDesc& m);
    // Adds a mesh (one primitive per section). materialMap: slot index -> gltf material (-1 none). Returns gltf mesh index.
    int AddMesh(const MeshData& mesh, int lod, const std::vector<int>& materialMap, bool withSkin);
    int AddNode(const std::string& name, int mesh, const float* matrix4x4 = nullptr, int parent = -1);   // column-major gltf matrix
    // Skeleton: creates one node per bone under `parent`; returns the skin index. Bones must be parents-first ordered.
    int AddSkin(const std::vector<Bone>& bones, int parentNode, std::vector<int>* boneNodes = nullptr);
    void SetNodeSkin(int node, int skin);
    // Animation channels for bone nodes: times (s) + per-key local transforms in Unreal space.
    struct Track { int node; std::vector<float> times; std::vector<Vec3> translations; std::vector<Vec4> rotations; std::vector<Vec3> scales; };
    void AddAnimation(const std::string& name, const std::vector<Track>& tracks);
    void AddRootNode(int node) { m_roots.push_back(node); }
    // KHR_lights_punctual: type "point" / "spot" / "directional"; intensity in candela (point, spot) or lux; range in m
    // (0 = infinite); cones in radians. The light shines along the node's -Z.
    int AddLight(const std::string& name, const char* type, const Vec3& color, float intensity, float range, float innerCone = 0, float outerCone = 0.785f);
    void SetNodeLight(int node, int light);
    void SetNodeExtras(int node, const std::string& extrasJson);   // a JSON object
    void SetSceneExtras(const std::string& extrasJson) { m_sceneExtras = extrasJson; }

    Bytes Finish();

    static Vec3 ConvertPos(const Vec3& p) { return {p.x * 0.01f, p.z * 0.01f, p.y * 0.01f}; }
    static Vec3 ConvertDir(const Vec3& p) { return {p.x, p.z, p.y}; }
    static Vec4 ConvertQuat(const Vec4& q) { return {-q.x, -q.z, -q.y, q.w}; }   // mirror (swap Y/Z) of a rotation
    static void ConvertMatrix(const float ue[16], float out[16]);                  // row-major UE (row vectors) -> column-major gltf

private:
    int AddView(const void* data, size_t size, int target = 0);
    int AddAccessor(int view, int componentType, size_t count, const char* type, const float* minv = nullptr, const float* maxv = nullptr, int comps = 0, bool normalized = false);
    Bytes m_bin;
    std::string m_views, m_accessors, m_meshes, m_nodes, m_materials, m_images, m_textures, m_skins, m_animations;
    int m_viewCount = 0, m_accessorCount = 0, m_meshCount = 0, m_nodeCount = 0, m_materialCount = 0, m_imageCount = 0, m_skinCount = 0, m_animCount = 0;
    std::vector<std::string> m_nodeJson;
    std::vector<std::vector<int>> m_nodeChildren;
    std::vector<int> m_nodeParent;
    std::vector<int> m_roots;
    std::string m_lights, m_sceneExtras;
    int m_lightCount = 0;
};
