#include "ue/Collision.h"
#include <array>
#include <cmath>
#include <map>

int CollisionData::Count(CollisionShape::Kind k) const {
    int n = 0;
    for (auto& s : shapes) n += s.kind == k;
    return n;
}

namespace {

Vec3 V3(const PropValue* v, Vec3 def = {}) {
    if (!v || !v->Fields()) return def;
    auto f = [&](const char* n, float d) { auto x = v->Field(n); return x ? (float)x->AsFloat(d) : d; };
    return {f("X", def.x), f("Y", def.y), f("Z", def.z)};
}

// an element's rotation: FQuat "Orientation" (4.10) or FRotator "Rotation"
Vec4 RotationOf(const PropValue& e) {
    if (auto q = e.Field("Orientation"); q && q->Fields()) {
        auto f = [&](const char* n, float d) { auto x = q->Field(n); return x ? (float)x->AsFloat(d) : d; };
        return {f("X", 0), f("Y", 0), f("Z", 0), f("W", 1)};
    }
    if (auto r = e.Field("Rotation"); r && r->Fields()) {
        auto f = [&](const char* n) { auto x = r->Field(n); return x ? (float)x->AsFloat() : 0.f; };
        return RotatorToQuat(f("Pitch"), f("Yaw"), f("Roll"));
    }
    return {0, 0, 0, 1};
}

void ReadAggGeom(const PropValue& agg, const Mat4& body, const std::string& bone, CollisionData& out) {
    auto each = [&](const char* name, auto fn) {
        if (auto a = agg.Field(name); a && a->type == PropType::Array)
            for (auto& e : a->items)
                if (e.Fields()) fn(e);
    };
    each("SphereElems", [&](const PropValue& e) {
        CollisionShape s;
        s.kind = CollisionShape::Sphere;
        s.radius = e.Field("Radius") ? (float)e.Field("Radius")->AsFloat() : 0;
        s.transform = Mat4::FromTRS(V3(e.Field("Center")), {0, 0, 0, 1}, {1, 1, 1}) * body;
        s.bone = bone;
        out.shapes.push_back(std::move(s));
    });
    each("BoxElems", [&](const PropValue& e) {
        CollisionShape s;
        s.kind = CollisionShape::Box;
        auto f = [&](const char* n) { auto x = e.Field(n); return x ? (float)x->AsFloat() * 0.5f : 0.f; };   // X / Y / Z are full sizes
        s.extent = {f("X"), f("Y"), f("Z")};
        s.transform = Mat4::FromTRS(V3(e.Field("Center")), RotationOf(e), {1, 1, 1}) * body;
        s.bone = bone;
        out.shapes.push_back(std::move(s));
    });
    each("SphylElems", [&](const PropValue& e) {
        CollisionShape s;
        s.kind = CollisionShape::Capsule;
        s.radius = e.Field("Radius") ? (float)e.Field("Radius")->AsFloat() : 0;
        s.length = e.Field("Length") ? (float)e.Field("Length")->AsFloat() : 0;
        s.transform = Mat4::FromTRS(V3(e.Field("Center")), RotationOf(e), {1, 1, 1}) * body;
        s.bone = bone;
        out.shapes.push_back(std::move(s));
    });
    each("ConvexElems", [&](const PropValue& e) {
        CollisionShape s;
        s.kind = CollisionShape::Convex;
        if (auto vd = e.Field("VertexData"); vd && vd->type == PropType::Array)
            for (auto& v : vd->items) s.vertices.push_back(V3(&v));
        if (s.vertices.size() < 4) return;
        Mat4 local = Mat4::Identity();
        if (auto t = e.Field("Transform"); t && t->Fields()) {
            Vec4 q{0, 0, 0, 1};
            if (auto r = t->Field("Rotation"); r && r->Fields()) {
                auto f = [&](const char* n, float d) { auto x = r->Field(n); return x ? (float)x->AsFloat(d) : d; };
                q = {f("X", 0), f("Y", 0), f("Z", 0), f("W", 1)};
            }
            local = Mat4::FromTRS(V3(t->Field("Translation")), q, V3(t->Field("Scale3D"), {1, 1, 1}));
        }
        s.transform = local * body;
        ConvexHull(s.vertices, s.indices);
        s.bone = bone;
        out.shapes.push_back(std::move(s));
    });
}

bool ReadBodySetup(const Package& pkg, size_t exportIdx, const Mat4& body, const std::string& bone, CollisionData& out) {
    PropertyList p = pkg.ReadProperties(exportIdx);
    if (out.traceFlag.empty()) out.traceFlag = PropStr(p, "CollisionTraceFlag");
    const PropValue* agg = FindValue(p, "AggGeom");
    if (!agg || !agg->Fields()) return false;
    size_t before = out.shapes.size();
    ReadAggGeom(*agg, body, bone, out);
    return out.shapes.size() > before;
}

}   // namespace

bool ReadMeshCollision(PackageCache& cache, const Package& pkg, size_t meshExport, const MeshData& mesh, CollisionData& out, std::string* error) {
    out = CollisionData{};
    PropertyList mp = pkg.ReadProperties(meshExport);
    if (!mesh.skeletal) {
        i32 bs = PropObj(mp, "BodySetup");
        if (bs <= 0 || (size_t)bs > pkg.Exports().size()) {   // 4.10 cooks it natively: the BodySetup export under the mesh
            bs = 0;
            for (size_t i = 0; i < pkg.Exports().size() && !bs; i++)
                if (pkg.ExportClass(i) == "BodySetup" && (pkg.Exports()[i].outerIndex == (i32)meshExport + 1 || pkg.Exports()[i].outerIndex == 0)) bs = (i32)i + 1;
        }
        if (bs <= 0) { if (error) *error = "no BodySetup"; return false; }
        out.source = pkg.Exports()[bs - 1].objectName;
        ReadBodySetup(pkg, (size_t)bs - 1, Mat4::Identity(), {}, out);
        if (out.shapes.empty() && error) *error = out.traceFlag == "CTF_UseComplexAsSimple" ? "complex collision only (uses the render mesh)" : "no simple shapes";
        return !out.shapes.empty();
    }
    // skeletal: the PhysicsAsset's bodies at their bones (reference pose, component space)
    i32 pa = PropObj(mp, "PhysicsAsset");
    if (pa == 0) { if (error) *error = "no PhysicsAsset"; return false; }
    std::shared_ptr<Package> holder;
    const Package* ppkg = &pkg;
    i32 paExport = pa;
    if (pa < 0) {
        std::string path = pkg.FullPath(pa);
        holder = cache.LoadPath(path, error);
        if (!holder) return false;
        ppkg = holder.get();
        paExport = holder->MainExport();
        out.source = path;
    } else out.source = pkg.Exports()[pa - 1].objectName;
    if (paExport <= 0) { if (error) *error = "PhysicsAsset export not found"; return false; }
    std::vector<Mat4> pose(mesh.skeleton.size(), Mat4::Identity());
    for (size_t b = 0; b < mesh.skeleton.size(); b++) {
        const Bone& bn = mesh.skeleton[b];
        Mat4 local = Mat4::FromTRS(bn.translation, bn.rotation, bn.scale);
        pose[b] = bn.parent >= 0 && (size_t)bn.parent < b ? local * pose[bn.parent] : local;
    }
    PropertyList pp = ppkg->ReadProperties((size_t)paExport - 1);
    const PropValue* bodies = FindValue(pp, "BodySetup");
    if (!bodies) bodies = FindValue(pp, "SkeletalBodySetups");
    if (!bodies || bodies->type != PropType::Array) { if (error) *error = "PhysicsAsset has no bodies"; return false; }
    for (auto& b : bodies->items) {
        i32 idx = b.ObjIndex();
        if (idx <= 0 || (size_t)idx > ppkg->Exports().size()) continue;
        PropertyList bp = ppkg->ReadProperties((size_t)idx - 1);
        std::string bone = PropStr(bp, "BoneName");
        Mat4 at = Mat4::Identity();
        for (size_t k = 0; k < mesh.skeleton.size(); k++)
            if (IEquals(mesh.skeleton[k].name, bone)) { at = pose[k]; break; }
        out.bodies++;
        ReadBodySetup(*ppkg, (size_t)idx - 1, at, bone, out);
    }
    if (out.shapes.empty() && error) *error = "the PhysicsAsset bodies have no shapes";
    return !out.shapes.empty();
}

// ------------------------------------------------------------------ convex hull

bool ConvexHull(const std::vector<Vec3>& p, std::vector<u32>& tris) {
    tris.clear();
    size_t n = p.size();
    if (n < 4) return false;
    Vec3 mn = p[0], mx = p[0];
    for (auto& v : p) {
        mn = {std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z)};
        mx = {std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z)};
    }
    float eps = std::max(1e-6f, Length(mx - mn) * 1e-5f);
    // initial tetrahedron: two far points, the farthest from their line, the farthest from that plane
    size_t a = 0, b = 0;
    for (size_t i = 1; i < n; i++) { if (p[i].x < p[a].x) a = i; if (p[i].x > p[b].x) b = i; }
    if (Length(p[b] - p[a]) < eps) return false;
    size_t c = n, d = n;
    float best = eps;
    for (size_t i = 0; i < n; i++) {
        float dist = Length(Cross(p[b] - p[a], p[i] - p[a]));
        if (dist > best) { best = dist; c = i; }
    }
    if (c == n) return false;
    Vec3 nrm = Normalize(Cross(p[b] - p[a], p[c] - p[a]));
    best = eps;
    for (size_t i = 0; i < n; i++) {
        float dist = fabsf(Dot(nrm, p[i] - p[a]));
        if (dist > best) { best = dist; d = i; }
    }
    if (d == n) return false;
    struct Face { u32 v[3]; Vec3 n; float off; };
    std::vector<Face> faces;
    auto make = [&](u32 i, u32 j, u32 k) {
        Face f{{i, j, k}, Normalize(Cross(p[j] - p[i], p[k] - p[i])), 0};
        f.off = Dot(f.n, p[i]);
        return f;
    };
    u32 A = (u32)a, B = (u32)b, C = (u32)c, D = (u32)d;
    if (Dot(nrm, p[d] - p[a]) > 0) std::swap(B, C);   // D behind ABC
    faces = {make(A, B, C), make(A, D, B), make(B, D, C), make(C, D, A)};
    for (u32 i = 0; i < (u32)n; i++) {
        if (i == A || i == B || i == C || i == D) continue;
        std::vector<char> visible(faces.size(), 0);
        bool any = false;
        for (size_t f = 0; f < faces.size(); f++)
            if (Dot(faces[f].n, p[i]) - faces[f].off > eps) { visible[f] = 1; any = true; }
        if (!any) continue;
        // horizon: directed edges of visible faces whose reverse is not on another visible face
        std::map<std::pair<u32, u32>, int> edges;
        for (size_t f = 0; f < faces.size(); f++)
            if (visible[f])
                for (int e = 0; e < 3; e++) edges[{faces[f].v[e], faces[f].v[(e + 1) % 3]}]++;
        std::vector<Face> next;
        for (size_t f = 0; f < faces.size(); f++)
            if (!visible[f]) next.push_back(faces[f]);
        for (auto& [e, cnt] : edges)
            if (!edges.count({e.second, e.first})) next.push_back(make(e.first, e.second, i));
        faces.swap(next);
        if (faces.size() > 4096) break;   // degenerate input safety
    }
    for (auto& f : faces) tris.insert(tris.end(), {f.v[0], f.v[1], f.v[2]});
    return true;
}

// ------------------------------------------------------------------ drawing / export geometry

namespace {

void Circle(std::vector<Vec3>& out, const Mat4& m, const Vec3& c, const Vec3& u, const Vec3& v, float r, int seg, float a0 = 0, float a1 = 2 * kPi) {
    for (int k = 0; k < seg; k++) {
        float t0 = a0 + (a1 - a0) * k / seg, t1 = a0 + (a1 - a0) * (k + 1) / seg;
        out.push_back(m.TransformPoint(c + u * (cosf(t0) * r) + v * (sinf(t0) * r)));
        out.push_back(m.TransformPoint(c + u * (cosf(t1) * r) + v * (sinf(t1) * r)));
    }
}

}   // namespace

void CollisionLines(const CollisionData& c, std::vector<Vec3>& out) {
    const Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    for (auto& s : c.shapes) {
        const Mat4& m = s.transform;
        switch (s.kind) {
        case CollisionShape::Box: {
            Vec3 e = s.extent, k[8];
            for (int i = 0; i < 8; i++) k[i] = m.TransformPoint({(i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z});
            static const int ed[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (auto& x : ed) { out.push_back(k[x[0]]); out.push_back(k[x[1]]); }
            break;
        }
        case CollisionShape::Sphere:
            Circle(out, m, {}, X, Y, s.radius, 32);
            Circle(out, m, {}, X, Z, s.radius, 32);
            Circle(out, m, {}, Y, Z, s.radius, 32);
            break;
        case CollisionShape::Capsule: {
            float h = s.length * 0.5f;
            Circle(out, m, {0, 0, h}, X, Y, s.radius, 32);
            Circle(out, m, {0, 0, -h}, X, Y, s.radius, 32);
            for (Vec3 d : {X, Y, X * -1.f, Y * -1.f}) { out.push_back(m.TransformPoint(d * s.radius + Z * h)); out.push_back(m.TransformPoint(d * s.radius - Z * h)); }
            Circle(out, m, {0, 0, h}, X, Z, s.radius, 16, 0, kPi);
            Circle(out, m, {0, 0, h}, Y, Z, s.radius, 16, 0, kPi);
            Circle(out, m, {0, 0, -h}, X, Z, s.radius, 16, kPi, 2 * kPi);
            Circle(out, m, {0, 0, -h}, Y, Z, s.radius, 16, kPi, 2 * kPi);
            break;
        }
        case CollisionShape::Convex: {
            std::map<std::pair<u32, u32>, int> seen;
            for (size_t t = 0; t + 2 < s.indices.size(); t += 3)
                for (int e = 0; e < 3; e++) {
                    u32 i = s.indices[t + e], j = s.indices[t + (e + 1) % 3];
                    if (!seen.emplace(std::make_pair(std::min(i, j), std::max(i, j)), 1).second) continue;
                    out.push_back(m.TransformPoint(s.vertices[i]));
                    out.push_back(m.TransformPoint(s.vertices[j]));
                }
            break;
        }
        }
    }
}

MeshLod CollisionMesh(const CollisionData& c) {
    MeshLod lod;
    auto add = [&](const Mat4& m, const std::vector<Vec3>& pos, const std::vector<u32>& idx) {
        u32 base = (u32)lod.positions.size();
        for (auto& v : pos) lod.positions.push_back(m.TransformPoint(v));
        for (u32 i : idx) lod.indices.push_back(base + i);
    };
    // lat-long sphere / capsule (rings stretched apart by `half` along Z)
    auto roundShape = [&](const Mat4& m, float r, float half) {
        const int seg = 16, rings = 8;
        std::vector<Vec3> pos;
        std::vector<u32> idx;
        for (int y = 0; y <= rings; y++) {
            float t = kPi * y / rings;
            float z = cosf(t) * r + (y <= rings / 2 ? half : -half);
            for (int x = 0; x <= seg; x++) {
                float ph = 2 * kPi * x / seg;
                pos.push_back({sinf(t) * cosf(ph) * r, sinf(t) * sinf(ph) * r, z});
            }
        }
        for (int y = 0; y < rings; y++)
            for (int x = 0; x < seg; x++) {
                u32 i0 = y * (seg + 1) + x, i1 = i0 + 1, i2 = i0 + seg + 1, i3 = i2 + 1;
                idx.insert(idx.end(), {i0, i2, i1, i1, i2, i3});
            }
        add(m, pos, idx);
    };
    for (auto& s : c.shapes) {
        switch (s.kind) {
        case CollisionShape::Box: {
            Vec3 e = s.extent;
            std::vector<Vec3> pos;
            for (int i = 0; i < 8; i++) pos.push_back({(i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z});
            add(s.transform, pos, {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5});
            break;
        }
        case CollisionShape::Sphere: roundShape(s.transform, s.radius, 0); break;
        case CollisionShape::Capsule: roundShape(s.transform, s.radius, s.length * 0.5f); break;
        case CollisionShape::Convex: add(s.transform, s.vertices, s.indices); break;
        }
    }
    // flat-ish normals: per vertex from the adjacent faces
    lod.normals.assign(lod.positions.size(), Vec3{});
    for (size_t t = 0; t + 2 < lod.indices.size(); t += 3) {
        u32 i = lod.indices[t], j = lod.indices[t + 1], k = lod.indices[t + 2];
        Vec3 n = Cross(lod.positions[j] - lod.positions[i], lod.positions[k] - lod.positions[i]);
        lod.normals[i] = lod.normals[i] + n; lod.normals[j] = lod.normals[j] + n; lod.normals[k] = lod.normals[k] + n;
    }
    for (auto& n : lod.normals) n = Normalize(n);
    if (!lod.indices.empty()) {
        MeshSection sec;
        sec.numTriangles = (u32)lod.indices.size() / 3;
        sec.maxVertex = (u32)lod.positions.size() - 1;
        lod.sections.push_back(sec);
    }
    return lod;
}
