#include "ue/AnimPose.h"
#include "core/Vfs.h"
#include <unordered_map>

bool AnimPoser::Init(const MeshData& mesh, const std::vector<Bone>& skeleton, std::string* error) {
    if (mesh.skeleton.empty()) {
        if (error) *error = "mesh has no skeleton";
        return false;
    }
    m_meshBones = mesh.skeleton;
    m_skeleton = skeleton;
    std::unordered_map<std::string, int> byName;
    for (size_t i = 0; i < skeleton.size(); i++) byName[ToLower(skeleton[i].name)] = (int)i;
    m_meshToSkel.assign(m_meshBones.size(), -1);
    m_matched = 0;
    for (size_t i = 0; i < m_meshBones.size(); i++) {
        auto it = byName.find(ToLower(m_meshBones[i].name));
        if (it != byName.end()) { m_meshToSkel[i] = it->second; m_matched++; }
    }
    std::vector<Mat4> ref;
    WorldMatrices(m_meshBones, ref);
    m_invRef.resize(ref.size());
    for (size_t i = 0; i < ref.size(); i++) m_invRef[i] = ref[i].Inverse();
    if (!m_matched && error) *error = "no mesh bone matches the animation skeleton";
    return m_matched > 0;
}

void AnimPoser::MeshPose(const AnimSequenceData& anim, float frame, std::vector<Bone>& meshPose) const {
    std::vector<Bone> skelPose;
    SampleAnimation(anim, m_skeleton, frame, skelPose);
    std::vector<char> animated(m_skeleton.size(), 0);
    for (auto& t : anim.tracks) if (t.bone >= 0 && t.bone < (i32)animated.size()) animated[t.bone] = 1;
    meshPose = m_meshBones;
    for (size_t i = 0; i < meshPose.size(); i++) {
        int s = m_meshToSkel[i];
        if (s < 0 || !animated[s]) continue;
        meshPose[i].rotation = skelPose[s].rotation;
        meshPose[i].translation = skelPose[s].translation;
        meshPose[i].scale = skelPose[s].scale;
    }
}

void AnimPoser::WorldMatrices(const std::vector<Bone>& bones, std::vector<Mat4>& world) {
    world.resize(bones.size());
    for (size_t i = 0; i < bones.size(); i++) {
        Mat4 local = Mat4::FromTRS(bones[i].translation, bones[i].rotation, bones[i].scale);
        int p = bones[i].parent;
        world[i] = p >= 0 && p < (int)i ? local * world[p] : local;
    }
}

void AnimPoser::Skin(const MeshLod& lod, const std::vector<Bone>& meshPose, std::vector<Vec3>& pos, std::vector<Vec3>& nrm) const {
    std::vector<Mat4> world, skin;
    WorldMatrices(meshPose, world);
    skin.resize(world.size());
    for (size_t i = 0; i < world.size(); i++) skin[i] = m_invRef[i] * world[i];
    u32 n = lod.VertexCount();
    pos.resize(n);
    nrm.resize(n);
    int infl = lod.influences;
    for (u32 v = 0; v < n; v++) {
        Vec3 p{}, q{};
        float total = 0;
        for (int k = 0; k < infl; k++) {
            u8 w = lod.boneWeights[(size_t)v * infl + k];
            if (!w) continue;
            u16 b = lod.boneIndices[(size_t)v * infl + k];
            if (b >= skin.size()) continue;
            float f = w / 255.f;
            p = p + skin[b].TransformPoint(lod.positions[v]) * f;
            if (v < lod.normals.size()) q = q + skin[b].TransformDir(lod.normals[v]) * f;
            total += f;
        }
        if (total <= 0) { pos[v] = lod.positions[v]; nrm[v] = v < lod.normals.size() ? lod.normals[v] : Vec3{0, 0, 1}; continue; }
        pos[v] = p * (1.f / total);
        nrm[v] = Normalize(q);
    }
}

std::string FindMeshForSkeleton(PackageCache& cache, const std::string& skeletonPath) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> memo;
    {
        std::lock_guard lk(mutex);
        auto it = memo.find(skeletonPath);
        if (it != memo.end()) return it->second;
    }
    const Vfs& vfs = cache.FileSystem();
    // /Game/Models/Characters/NPCs/npc_generic_Skeleton.npc_generic_Skeleton -> package + folder
    std::string pkgPath = skeletonPath;
    size_t dot = pkgPath.rfind('.');
    if (dot != std::string::npos && pkgPath.find('/', dot) == std::string::npos) pkgPath.resize(dot);
    std::string skelName = pkgPath.substr(pkgPath.rfind('/') + 1);
    std::string folder = ToLower(Vfs::NormalizePath(pkgPath.substr(0, pkgPath.rfind('/') + 1)));
    std::string result;
    // widen the search one folder level at a time (max 2 levels up)
    for (int up = 0; up < 3 && result.empty(); up++) {
        std::vector<u32> candidates;
        for (u32 i = 0; i < vfs.FileCount(); i++) {
            std::string_view p = vfs.Path(i);
            if (p.size() < folder.size() || !IEquals(p.substr(0, folder.size()), folder) || !IEndsWith(p, ".uasset")) continue;
            candidates.push_back(i);
        }
        int checked = 0;
        for (u32 c : candidates) {
            if (++checked > 2000) break;
            auto pkg = cache.Load(c);
            if (!pkg) continue;
            i32 main = pkg->MainExport();
            if (!main || !IsSkeletalMeshClass(pkg->ExportClass(main - 1))) continue;
            bool uses = false;
            for (auto& im : pkg->Imports())
                if (im.className == "Skeleton" && IEquals(im.objectName, skelName)) { uses = true; break; }
            if (uses) { result = pkg->FullPath(main); break; }
        }
        if (folder.size() < 2) break;
        size_t s = folder.rfind('/', folder.size() - 2);
        if (s == std::string::npos) break;
        folder.resize(s + 1);
    }
    std::lock_guard lk(mutex);
    memo[skeletonPath] = result;
    return result;
}
