#pragma once
#include "ue/Package.h"
#include <array>

struct Vec2 { float x = 0, y = 0; };
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct MeshSection {
    i32 materialIndex = 0;
    u32 firstIndex = 0;
    u32 numTriangles = 0;
    u32 minVertex = 0, maxVertex = 0;
    bool castShadow = true;
    u32 baseVertex = 0;   // skeletal: chunk base
};

// Renderable LOD shared by static and skeletal meshes (Unreal space, cm).
struct MeshLod {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec4> tangents;            // xyz + bitangent sign
    std::vector<Vec2> uvs;                 // numUVs per vertex, interleaved [vertex * numUVs + set]
    int numUVs = 0;
    std::vector<u32> colors;               // RGBA8 (optional)
    std::vector<u32> indices;
    std::vector<MeshSection> sections;
    // skinning (skeletal meshes): up to 8 influences per vertex
    int influences = 0;
    std::vector<u16> boneIndices;          // vertex * influences, already mapped to skeleton bone index
    std::vector<u8> boneWeights;           // vertex * influences, sum 255
    u32 VertexCount() const { return (u32)positions.size(); }
};

struct MeshMaterialSlot {
    std::string name;
    std::string materialPath;   // /Game/... object path, empty if none
};

struct Bone {
    std::string name;
    i32 parent = -1;
    Vec4 rotation{0, 0, 0, 1};   // local, quaternion
    Vec3 translation;
    Vec3 scale{1, 1, 1};
};

struct MeshData {
    std::string name;
    bool skeletal = false;
    std::vector<MeshLod> lods;
    std::vector<MeshMaterialSlot> materials;
    std::vector<Bone> skeleton;      // skeletal meshes: reference skeleton
    std::string skeletonPath;        // USkeleton asset
    Vec3 boundsMin, boundsMax;
    void ComputeBounds();
};

bool IsStaticMeshClass(std::string_view cls);
bool IsSkeletalMeshClass(std::string_view cls);

bool ReadStaticMesh(const Package& pkg, size_t exportIdx, MeshData& out, std::string* error = nullptr);
bool ReadSkeletalMesh(const Package& pkg, size_t exportIdx, MeshData& out, std::string* error = nullptr);
bool ReadMesh(const Package& pkg, size_t exportIdx, MeshData& out, std::string* error = nullptr);

// helpers shared by the mesh readers
Vec3 UnpackNormal(u32 packed);
void SkipBulkArray(Reader& r);
template <typename T> std::vector<T> ReadBulkArray(Reader& r) {
    i32 elemSize = r.I32();
    i32 count = r.I32();
    if (count < 0 || elemSize < 0 || (size_t)count * elemSize > r.Remaining()) throw ParseError(Fmt("bad bulk array %d x %d at %zu", count, elemSize, r.Pos()));
    if (count && elemSize != (i32)sizeof(T)) throw ParseError(Fmt("bulk element size %d, expected %zu", elemSize, sizeof(T)));
    std::vector<T> v(count);
    if (count) r.ReadInto(v.data(), (size_t)count * sizeof(T));
    return v;
}
