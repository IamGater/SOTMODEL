#include "ue/Mesh.h"
#include "ue/TextureDecode.h"
#include <cmath>

bool IsStaticMeshClass(std::string_view c) { return c == "StaticMesh" || c == "DestructibleMesh"; }
bool IsSkeletalMeshClass(std::string_view c) { return c == "SkeletalMesh"; }

Vec3 UnpackNormal(u32 p) {
    auto f = [](u32 b) { return (float)(b & 255) / 127.5f - 1.0f; };
    return {f(p), f(p >> 8), f(p >> 16)};
}

void SkipBulkArray(Reader& r) {
    i32 elemSize = r.I32();
    i32 count = r.I32();
    if (count < 0 || elemSize < 0) throw ParseError("bad bulk array");
    r.Skip((i64)elemSize * count);
}

void MeshData::ComputeBounds() {
    boundsMin = {1e30f, 1e30f, 1e30f};
    boundsMax = {-1e30f, -1e30f, -1e30f};
    if (lods.empty() || lods[0].positions.empty()) { boundsMin = boundsMax = {}; return; }
    for (auto& p : lods[0].positions) {
        boundsMin = {std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z)};
        boundsMax = {std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z)};
    }
}

namespace {
struct StripFlags {
    u8 global = 0, cls = 0;
    void Read(Reader& r) { global = r.U8(); cls = r.U8(); }
    bool EditorStripped() const { return global & 1; }
    bool AVStripped() const { return global & 2; }
    bool ClassStripped(u8 f) const { return cls & f; }
};

// FStaticMeshVertexBuffer (4.10): tangent basis + UVs interleaved per vertex
void ReadStaticVertexBuffer(Reader& r, MeshLod& lod) {
    StripFlags sf;
    sf.Read(r);
    i32 numUV = r.I32();
    r.I32();   // stride
    i32 numVerts = r.I32();
    bool fullUV = r.Bool32();
    if (numUV < 0 || numUV > 8) throw ParseError(Fmt("bad NumTexCoords %d", numUV));
    if (sf.AVStripped()) return;
    i32 elemSize = r.I32();
    i32 count = r.I32();
    i32 expect = 8 + numUV * (fullUV ? 8 : 4);
    if (count != numVerts || (count && elemSize != expect)) throw ParseError(Fmt("vertex buffer mismatch: %d x %d (expected %d x %d)", count, elemSize, numVerts, expect));
    r.Need((size_t)count * elemSize);
    lod.numUVs = numUV;
    lod.normals.resize(count);
    lod.tangents.resize(count);
    lod.uvs.resize((size_t)count * numUV);
    for (i32 v = 0; v < count; v++) {
        u32 tx = r.U32(), tz = r.U32();
        Vec3 t = UnpackNormal(tx), n = UnpackNormal(tz);
        float sign = ((tz >> 24) & 255) / 127.5f - 1.0f;
        lod.normals[v] = n;
        lod.tangents[v] = {t.x, t.y, t.z, sign < 0 ? -1.f : 1.f};
        for (i32 u = 0; u < numUV; u++) {
            Vec2& uv = lod.uvs[(size_t)v * numUV + u];
            if (fullUV) { uv.x = r.F32(); uv.y = r.F32(); }
            else { uv.x = HalfToFloat(r.U16()); uv.y = HalfToFloat(r.U16()); }
        }
    }
}

std::vector<u32> ReadIndexBuffer(Reader& r) {
    bool is32 = r.Bool32();
    auto bytes = ReadBulkArray<u8>(r);
    std::vector<u32> idx;
    if (is32) {
        idx.resize(bytes.size() / 4);
        memcpy(idx.data(), bytes.data(), idx.size() * 4);
    } else {
        idx.resize(bytes.size() / 2);
        for (size_t i = 0; i < idx.size(); i++) idx[i] = bytes[i * 2] | (bytes[i * 2 + 1] << 8);
    }
    return idx;
}

void ReadColorBuffer(Reader& r, MeshLod& lod) {
    StripFlags sf;
    sf.Read(r);
    r.I32();   // stride
    i32 n = r.I32();
    if (n > 0 && !sf.AVStripped()) {
        auto c = ReadBulkArray<u32>(r);   // FColor BGRA
        lod.colors.resize(c.size());
        for (size_t i = 0; i < c.size(); i++) {
            u32 b = c[i] & 255, g = (c[i] >> 8) & 255, rr = (c[i] >> 16) & 255, a = c[i] >> 24;
            lod.colors[i] = rr | (g << 8) | (b << 16) | (a << 24);
        }
    }
}

void ReadStaticLod(Reader& r, MeshLod& lod) {
    StripFlags sf;
    sf.Read(r);
    i32 ns = r.Count(28);
    lod.sections.resize(ns);
    for (auto& s : lod.sections) {
        s.materialIndex = r.I32();
        s.firstIndex = r.U32();
        s.numTriangles = r.U32();
        s.minVertex = r.U32();
        s.maxVertex = r.U32();
        r.Bool32();   // enable collision
        s.castShadow = r.Bool32();
    }
    r.F32();   // max deviation
    if (sf.AVStripped()) return;
    // position buffer
    r.I32();   // stride
    i32 nv = r.I32();
    lod.positions = ReadBulkArray<Vec3>(r);
    if ((i32)lod.positions.size() != nv) throw ParseError("position count mismatch");
    ReadStaticVertexBuffer(r, lod);
    ReadColorBuffer(r, lod);
    lod.indices = ReadIndexBuffer(r);
    ReadIndexBuffer(r);   // depth only
    if (!sf.EditorStripped()) ReadIndexBuffer(r);   // wireframe
    if (!sf.ClassStripped(1)) ReadIndexBuffer(r);   // adjacency
    r.Skip(17);   // Athena: extra per-LOD data (CUE4Parse)
}
}

bool ReadStaticMesh(const Package& pkg, size_t exportIdx, MeshData& m, std::string* error) {
    try {
        m.name = pkg.Exports()[exportIdx].objectName;
        m.skeletal = false;
        size_t end = 0;
        PropertyList props = pkg.ReadProperties(exportIdx, &end);
        if (auto mats = FindValue(props, "Materials")) {
            for (auto& v : mats->items) {
                MeshMaterialSlot s;
                i32 idx = v.ObjIndex();
                s.name = pkg.ObjectName(idx);
                if (idx) s.materialPath = pkg.FullPath(idx);
                m.materials.push_back(s);
            }
        }
        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32()) r.Skip(16);
        StripFlags sf;
        sf.Read(r);
        bool cooked = r.Bool32();
        r.I32();   // body setup
        r.I32();   // nav collision
        if (!sf.EditorStripped()) { r.FString(); r.U32(); }   // high res source mesh name/CRC
        r.Skip(16);   // lighting guid
        i32 sockets = r.Count(4);
        r.Skip((i64)sockets * 4);
        if (!cooked) throw ParseError("mesh has no cooked render data");
        i32 nl = r.Count(8);
        m.lods.resize(nl);
        for (i32 i = 0; i < nl; i++) ReadStaticLod(r, m.lods[i]);
        // sanity: render data continues with strip flags and one "has distance field" bool per LOD
        {
            StripFlags dsf;
            dsf.Read(r);
            for (i32 i = 0; i < nl; i++) {
                u32 valid = r.U32();
                if (valid > 1) throw ParseError(Fmt("render data desync after LODs (bool %u)", valid));
                if (valid) break;   // distance field payload follows; stop checking
            }
        }
        // drop LODs without geometry
        m.lods.erase(std::remove_if(m.lods.begin(), m.lods.end(), [](const MeshLod& l) { return l.positions.empty() || l.indices.empty(); }), m.lods.end());
        if (m.lods.empty()) throw ParseError("no LOD with geometry");
        m.ComputeBounds();
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

bool ReadMesh(const Package& pkg, size_t exportIdx, MeshData& out, std::string* error) {
    std::string cls = pkg.ExportClass(exportIdx);
    if (IsStaticMeshClass(cls)) return ReadStaticMesh(pkg, exportIdx, out, error);
    if (IsSkeletalMeshClass(cls)) return ReadSkeletalMesh(pkg, exportIdx, out, error);
    if (error) *error = "not a mesh: " + cls;
    return false;
}
