#include "ue/MeshExtras.h"

namespace {

Vec3 V3(const PropValue* v, Vec3 def = {}) {
    if (!v || !v->Fields()) return def;
    auto f = [&](const char* n, float d) { auto x = v->Field(n); return x ? (float)x->AsFloat(d) : d; };
    return {f("X", def.x), f("Y", def.y), f("Z", def.z)};
}
Vec3 Rot(const PropValue* v) {
    if (!v || !v->Fields()) return {};
    auto f = [&](const char* n) { auto x = v->Field(n); return x ? (float)x->AsFloat() : 0.f; };
    return {f("Pitch"), f("Yaw"), f("Roll")};
}

void ReadSocketList(const Package& pkg, const PropertyList& props, const char* source, std::vector<MeshSocket>& out) {
    const PropValue* arr = FindValue(props, "Sockets");
    if (!arr || arr->type != PropType::Array) return;
    for (auto& it : arr->items) {
        i32 idx = it.ObjIndex();
        if (idx <= 0 || (size_t)idx > pkg.Exports().size()) continue;
        PropertyList p = pkg.ReadProperties((size_t)idx - 1);
        MeshSocket s;
        s.name = PropStr(p, "SocketName");
        if (s.name.empty()) s.name = pkg.Exports()[idx - 1].objectName;
        s.bone = PropStr(p, "BoneName");
        s.location = V3(FindValue(p, "RelativeLocation"));
        s.rotation = Rot(FindValue(p, "RelativeRotation"));
        s.scale = V3(FindValue(p, "RelativeScale"), {1, 1, 1});
        s.source = source;
        bool dup = false;
        for (auto& o : out) dup |= IEquals(o.name, s.name);
        if (!dup) out.push_back(std::move(s));
    }
}

}   // namespace

bool ReadMeshSockets(PackageCache& cache, const Package& pkg, size_t meshExport, const MeshData& mesh, std::vector<MeshSocket>& out) {
    out.clear();
    ReadSocketList(pkg, pkg.ReadProperties(meshExport), "mesh", out);
    if (mesh.skeletal && !mesh.skeletonPath.empty())
        if (auto sk = cache.LoadPath(mesh.skeletonPath))
            if (i32 main = sk->MainExport()) ReadSocketList(*sk, sk->ReadProperties((size_t)main - 1), "skeleton", out);
    // bone reference pose (component space)
    std::vector<Mat4> pose(mesh.skeleton.size(), Mat4::Identity());
    for (size_t b = 0; b < mesh.skeleton.size(); b++) {
        const Bone& bn = mesh.skeleton[b];
        Mat4 local = Mat4::FromTRS(bn.translation, bn.rotation, bn.scale);
        pose[b] = bn.parent >= 0 && (size_t)bn.parent < b ? local * pose[bn.parent] : local;
    }
    for (auto& s : out) {
        Mat4 local = Mat4::FromTRS(s.location, RotatorToQuat(s.rotation.x, s.rotation.y, s.rotation.z), s.scale);
        Mat4 parent = Mat4::Identity();
        if (!s.bone.empty())
            for (size_t b = 0; b < mesh.skeleton.size(); b++)
                if (IEquals(mesh.skeleton[b].name, s.bone)) { parent = pose[b]; break; }
        s.transform = local * parent;
    }
    return !out.empty();
}

bool ReadMorphTargets(const Package& pkg, size_t meshExport, std::vector<MorphTarget>& out, std::string* error) {
    out.clear();
    PropertyList mp = pkg.ReadProperties(meshExport);
    const PropValue* arr = FindValue(mp, "MorphTargets");
    std::vector<i32> exports;
    if (arr && arr->type == PropType::Array)
        for (auto& it : arr->items)
            if (it.ObjIndex() > 0) exports.push_back(it.ObjIndex());
    if (exports.empty())   // fall back to the MorphTarget exports of the package
        for (size_t i = 0; i < pkg.Exports().size(); i++)
            if (pkg.ExportClass(i) == "MorphTarget") exports.push_back((i32)i + 1);
    for (i32 idx : exports) {
        if ((size_t)idx > pkg.Exports().size()) continue;
        MorphTarget mt;
        mt.name = pkg.Exports()[idx - 1].objectName;
        try {
            size_t end = 0;
            pkg.ReadProperties((size_t)idx - 1, &end);
            Reader r = pkg.ExportReader((size_t)idx - 1);
            r.Seek(end);
            if (r.U32()) r.Skip(16);   // UObject: serialized object guid?
            r.Skip(2);                  // FStripDataFlags
            i32 nl = r.Count(8);
            for (i32 l = 0; l < nl; l++) {
                i32 nv = r.Count(28);
                std::vector<MorphDelta> d((size_t)nv);
                for (auto& x : d) {
                    x.position = {r.F32(), r.F32(), r.F32()};
                    x.normal = {r.F32(), r.F32(), r.F32()};
                    x.vertex = r.U32();
                }
                r.I32();   // NumBaseMeshVerts
                mt.lods.push_back(std::move(d));
            }
        } catch (const std::exception& e) {
            if (error) *error = mt.name + ": " + e.what();
            continue;
        }
        out.push_back(std::move(mt));
    }
    return !out.empty();
}

void ApplyMorphs(const MeshLod& base, int lod, const std::vector<MorphTarget>& morphs, const std::vector<float>& weights,
                 std::vector<Vec3>& positions, std::vector<Vec3>& normals) {
    positions = base.positions;
    normals = base.normals;
    if (normals.size() != positions.size()) normals.assign(positions.size(), Vec3{0, 0, 1});
    for (size_t m = 0; m < morphs.size() && m < weights.size(); m++) {
        float w = weights[m];
        if (w == 0 || lod < 0 || (size_t)lod >= morphs[m].lods.size()) continue;
        for (auto& d : morphs[m].lods[lod]) {
            if (d.vertex >= positions.size()) continue;
            positions[d.vertex] = positions[d.vertex] + d.position * w;
            normals[d.vertex] = normals[d.vertex] + d.normal * w;
        }
    }
    for (auto& n : normals) n = Normalize(n);
}

void SocketAxes(const std::vector<MeshSocket>& sockets, float size, std::vector<Vec3>& x, std::vector<Vec3>& y, std::vector<Vec3>& z) {
    for (auto& s : sockets) {
        Vec3 o = s.transform.Translation();
        x.push_back(o); x.push_back(o + Normalize(s.transform.TransformDir({1, 0, 0})) * size);
        y.push_back(o); y.push_back(o + Normalize(s.transform.TransformDir({0, 1, 0})) * size);
        z.push_back(o); z.push_back(o + Normalize(s.transform.TransformDir({0, 0, 1})) * size);
    }
}
