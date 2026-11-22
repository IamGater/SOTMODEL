#pragma once
#include "ue/Mesh.h"
#include "render/Math.h"

// Sockets and morph targets of a mesh.

struct MeshSocket {
    std::string name, bone;   // bone: skeletal sockets ("" = mesh space)
    Vec3 location, rotation{0, 0, 0}, scale{1, 1, 1};   // relative to the bone / mesh; rotation = pitch, yaw, roll
    Mat4 transform;           // socket -> mesh space (skeletal: reference pose)
    std::string source;       // "mesh" or "skeleton"
};

// Static mesh Sockets, or a skeletal mesh's sockets plus those of its Skeleton asset (mesh ones win by name).
bool ReadMeshSockets(PackageCache& cache, const Package& pkg, size_t meshExport, const MeshData& mesh, std::vector<MeshSocket>& out);

struct MorphDelta {
    u32 vertex = 0;           // render vertex of the LOD
    Vec3 position, normal;    // deltas
};
struct MorphTarget {
    std::string name;
    std::vector<std::vector<MorphDelta>> lods;   // per LOD
    bool bodyPatch = false;   // SoT MeshPatchAsset (wardrobe body-shape patch: Female_N, Male_SE, ...)
};

// UMorphTarget exports listed in the skeletal mesh's MorphTargets array. With `mesh` (the decoded mesh of `meshExport`)
// the package's MeshPatchAsset exports are added too: SoT's body-shape patches of wardrobe items. Their native data is
// per LOD a bulk array of 24 byte entries (position delta 3 x half, normal delta 3 x half, two u16, u32 0, u32 index)
// plus a u32; the index is the imported (DCC) vertex, mapped to render vertices by the LOD's MeshToImportVertexMap.
bool ReadMorphTargets(const Package& pkg, size_t meshExport, std::vector<MorphTarget>& out, std::string* error = nullptr,
                      const MeshData* mesh = nullptr);

// base + sum(weight * delta) for one LOD; normals renormalised.
void ApplyMorphs(const MeshLod& base, int lod, const std::vector<MorphTarget>& morphs, const std::vector<float>& weights,
                 std::vector<Vec3>& positions, std::vector<Vec3>& normals);

// Socket axes as line segments (red X, green Y, blue Z handled by the caller's colours): pairs of points per axis.
void SocketAxes(const std::vector<MeshSocket>& sockets, float size, std::vector<Vec3>& x, std::vector<Vec3>& y, std::vector<Vec3>& z);
