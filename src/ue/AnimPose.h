#pragma once
#include "ue/Animation.h"
#include "render/Math.h"

// Poses a skeletal mesh with an animation (CPU skinning). Mesh bones are matched to skeleton bones by name.
class AnimPoser {
public:
    bool Init(const MeshData& mesh, const std::vector<Bone>& skeleton, std::string* error = nullptr);
    // Local pose of the mesh bones at `frame` (unanimated / unmatched bones keep the mesh reference pose).
    void MeshPose(const AnimSequenceData& anim, float frame, std::vector<Bone>& meshPose) const;
    static void WorldMatrices(const std::vector<Bone>& bones, std::vector<Mat4>& world);
    // Skins one LOD with a mesh-space local pose: writes positions and normals.
    void Skin(const MeshLod& lod, const std::vector<Bone>& meshPose, std::vector<Vec3>& positions, std::vector<Vec3>& normals) const;
    const std::vector<Bone>& MeshBones() const { return m_meshBones; }
    int MatchedBones() const { return m_matched; }

private:
    std::vector<Bone> m_meshBones, m_skeleton;
    std::vector<int> m_meshToSkel;
    std::vector<Mat4> m_invRef;
    int m_matched = 0;
};

// Looks for a skeletal mesh using `skeletonPath` near the skeleton asset (same folder tree). Empty if none.
std::string FindMeshForSkeleton(PackageCache& cache, const std::string& skeletonPath);
