// USkeletalMesh (UE 4.10 FStaticLODModel with chunks) + Sea of Thieves extras, after CUE4Parse's GAME_SeaOfThieves path.
#include "ue/Mesh.h"
#include "ue/TextureDecode.h"

namespace {
struct StripFlags {
    u8 global = 0, cls = 0;
    void Read(Reader& r) { global = r.U8(); cls = r.U8(); }
    bool EditorStripped() const { return global & 1; }
    bool AVStripped() const { return global & 2; }
    bool ClassStripped(u8 f) const { return cls & f; }
};

struct SkelSection {
    i16 material = 0;
    u16 chunk = 0;
    u32 baseIndex = 0, numTriangles = 0;
};

struct SkelChunk {
    i32 baseVertex = 0;
    std::vector<u16> boneMap;
    i32 numRigid = 0, numSoft = 0;
    i32 maxInfluences = 0;
    bool cloth = false;
};

std::vector<u32> ReadMultisizeIndices(Reader& r) {
    u8 size = r.U8();
    i32 elemSize = r.I32();
    i32 count = r.I32();
    if (count < 0 || (count && elemSize != size) || (size != 2 && size != 4)) throw ParseError(Fmt("bad index container (%u, %d x %d)", size, elemSize, count));
    r.Need((size_t)count * elemSize);
    std::vector<u32> idx(count);
    for (i32 i = 0; i < count; i++) idx[i] = size == 2 ? r.U16() : r.U32();
    return idx;
}

template <typename T> void SkipArray(Reader& r) {
    i32 n = r.Count(sizeof(T));
    r.Skip((i64)n * sizeof(T));
}

void ReadSection(Reader& r, SkelSection& s) {
    StripFlags sf;
    sf.Read(r);
    s.material = (i16)r.U16();
    s.chunk = r.U16();
    if (!sf.AVStripped()) {
        s.baseIndex = r.U32();
        s.numTriangles = r.U32();
    }
    r.U8();    // triangle sorting
    r.U32();   // bDisabled (legacy clothing section)
    r.U16();   // CorrespondClothSectionIndex
    r.U8();    // bEnableClothLOD
}

void ReadChunk(Reader& r, SkelChunk& c, bool filterEditorOnly) {
    StripFlags sf;
    sf.Read(r);
    if (!sf.AVStripped()) c.baseVertex = r.I32();
    if (!sf.EditorStripped() && !filterEditorOnly) throw ParseError("skeletal mesh has editor vertex data (uncooked)");
    i32 nb = r.Count(2);
    c.boneMap.resize(nb);
    for (auto& b : c.boneMap) b = r.U16();
    c.numRigid = r.I32();
    c.numSoft = r.I32();
    c.maxInfluences = r.I32();
    i32 clothMap = r.Count(64);   // FMeshToMeshVertData
    r.Skip((i64)clothMap * 64);
    SkipArray<Vec3>(r);   // physical mesh vertices
    SkipArray<Vec3>(r);   // physical mesh normals
    r.Skip(4);            // CorrespondClothAssetIndex, ClothAssetSubmeshIndex
    c.cloth = clothMap > 0;
}

void ReadLod(Reader& r, MeshLod& lod, bool hasVertexColors, bool filterEditorOnly) {
    r.Skip(4);   // Athena
    StripFlags sf;
    sf.Read(r);
    r.Skip(4);   // Athena
    i32 ns = r.Count(16);
    std::vector<SkelSection> sections(ns);
    for (auto& s : sections) ReadSection(r, s);
    std::vector<u32> indices = ReadMultisizeIndices(r);
    SkipArray<i16>(r);   // active bone indices
    i32 nc = r.Count(16);
    std::vector<SkelChunk> chunks(nc);
    for (auto& c : chunks) ReadChunk(r, c, filterEditorOnly);
    r.I32();   // size
    i32 numVertices = sf.AVStripped() ? 0 : r.I32();
    SkipArray<i16>(r);   // required bones
    if (!sf.EditorStripped()) throw ParseError("skeletal LOD has raw point indices (uncooked)");
    SkipArray<i32>(r);   // MeshToImportVertexMap
    r.I32();             // MaxImportVertex
    if (sf.AVStripped()) return;
    i32 numTexCoords = r.I32();

    // FSkeletalMeshVertexBuffer
    StripFlags vsf;
    vsf.Read(r);
    i32 vbTexCoords = r.I32();
    bool fullUV = r.Bool32();
    bool extraInfluences = r.Bool32();
    Vec3 meshExtension{r.F32(), r.F32(), r.F32()};
    Vec3 meshOrigin{r.F32(), r.F32(), r.F32()};
    (void)meshExtension; (void)meshOrigin;
    if (vbTexCoords < 1 || vbTexCoords > 4) throw ParseError(Fmt("bad skeletal NumTexCoords %d", vbTexCoords));
    int infl = extraInfluences ? 8 : 4;
    i32 elemSize = r.I32();
    i32 count = r.I32();
    i32 expect = 8 + infl * 2 + 12 + vbTexCoords * (fullUV ? 8 : 4);
    if (count < 0 || (count && elemSize != expect)) throw ParseError(Fmt("skeletal vertex buffer %d x %d, expected stride %d", count, elemSize, expect));
    if (count != numVertices) throw ParseError(Fmt("skeletal vertex count %d != %d", count, numVertices));
    r.Need((size_t)count * elemSize);
    (void)numTexCoords;

    lod.numUVs = vbTexCoords;
    lod.influences = infl;
    lod.positions.resize(count);
    lod.normals.resize(count);
    lod.tangents.resize(count);
    lod.uvs.resize((size_t)count * vbTexCoords);
    lod.boneIndices.resize((size_t)count * infl);
    lod.boneWeights.resize((size_t)count * infl);
    for (i32 v = 0; v < count; v++) {
        u32 tx = r.U32(), tz = r.U32();
        Vec3 t = UnpackNormal(tx), n = UnpackNormal(tz);
        float sign = ((tz >> 24) & 255) / 127.5f - 1.0f;
        lod.normals[v] = n;
        lod.tangents[v] = {t.x, t.y, t.z, sign < 0 ? -1.f : 1.f};
        for (int i = 0; i < infl; i++) lod.boneIndices[(size_t)v * infl + i] = r.U8();
        for (int i = 0; i < infl; i++) lod.boneWeights[(size_t)v * infl + i] = r.U8();
        lod.positions[v] = {r.F32(), r.F32(), r.F32()};
        for (i32 u = 0; u < vbTexCoords; u++) {
            Vec2& uv = lod.uvs[(size_t)v * vbTexCoords + u];
            if (fullUV) { uv.x = r.F32(); uv.y = r.F32(); }
            else { uv.x = HalfToFloat(r.U16()); uv.y = HalfToFloat(r.U16()); }
        }
    }
    if (hasVertexColors) {
        StripFlags csf;
        csf.Read(r);
        if (!csf.AVStripped()) {
            auto c = ReadBulkArray<u32>(r);   // FColor BGRA
            if ((i32)c.size() == count) {
                lod.colors.resize(c.size());
                for (size_t i = 0; i < c.size(); i++) {
                    u32 b = c[i] & 255, g = (c[i] >> 8) & 255, rr = (c[i] >> 16) & 255, a = c[i] >> 24;
                    lod.colors[i] = rr | (g << 8) | (b << 16) | (a << 24);
                }
            }
        }
    }
    // Athena: an array of 44-byte records, four int arrays and 13 bytes (CUE4Parse GAME_SeaOfThieves)
    i32 extra = r.Count(44);
    r.Skip((i64)extra * 44);
    for (int i = 0; i < 4; i++) SkipArray<i32>(r);
    r.Skip(13);
    if (!sf.ClassStripped(1)) ReadMultisizeIndices(r);   // adjacency
    bool cloth = std::any_of(chunks.begin(), chunks.end(), [](auto& c) { return c.cloth; });
    if (cloth) {
        StripFlags clsf;
        clsf.Read(r);
        if (!clsf.AVStripped()) SkipBulkArray(r);
    }
    ReadMultisizeIndices(r);   // Athena: extra index container

    // chunk-local bone indices -> skeleton bones
    for (auto& c : chunks) {
        i32 end = std::min(count, c.baseVertex + c.numRigid + c.numSoft);
        for (i32 v = std::max(0, c.baseVertex); v < end; v++)
            for (int i = 0; i < infl; i++) {
                u16& b = lod.boneIndices[(size_t)v * infl + i];
                b = b < c.boneMap.size() ? c.boneMap[b] : 0;
            }
    }
    lod.indices = std::move(indices);
    for (auto& s : sections) {
        MeshSection ms;
        ms.materialIndex = s.material;
        ms.firstIndex = s.baseIndex;
        ms.numTriangles = s.numTriangles;
        if ((u64)ms.firstIndex + (u64)ms.numTriangles * 3 > lod.indices.size()) throw ParseError("section outside index buffer");
        lod.sections.push_back(ms);
    }
}
}

void ReadReferenceSkeleton(const Package& pkg, Reader& r, std::vector<Bone>& bones) {
    bool filterEditorOnly = (pkg.Summary().packageFlags & 0x80000000u) != 0;
    i32 nb = r.Count(12);
    bones.resize(nb);
    for (auto& b : bones) {
        b.name = pkg.ReadName(r);
        b.parent = r.I32();
        if (!filterEditorOnly) r.FString();   // export name
    }
    i32 np = r.Count(40);
    if (np != nb) throw ParseError(Fmt("bone pose count %d != bone count %d", np, nb));
    for (auto& b : bones) {
        b.rotation = {r.F32(), r.F32(), r.F32(), r.F32()};
        b.translation = {r.F32(), r.F32(), r.F32()};
        b.scale = {r.F32(), r.F32(), r.F32()};
    }
    i32 nmap = r.Count(12);
    r.Skip((i64)nmap * 12);
    for (i32 i = 0; i < nb; i++)
        if (bones[i].parent >= i) throw ParseError("bad bone hierarchy");
    if (nb) bones[0].parent = -1;
}

bool ReadSkeleton(const Package& pkg, size_t exportIdx, std::vector<Bone>& bones, std::string* error) {
    try {
        size_t end = 0;
        pkg.ReadProperties(exportIdx, &end);
        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32()) r.Skip(16);
        ReadReferenceSkeleton(pkg, r, bones);
        if (bones.empty()) throw ParseError("skeleton has no bones");
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

bool ReadSkeletalMesh(const Package& pkg, size_t exportIdx, MeshData& m, std::string* error) {
    try {
        m.name = pkg.Exports()[exportIdx].objectName;
        m.skeletal = true;
        size_t end = 0;
        PropertyList props = pkg.ReadProperties(exportIdx, &end);
        bool hasVertexColors = PropBool(props, "bHasVertexColors", false);
        if (i32 sk = PropObj(props, "Skeleton")) m.skeletonPath = pkg.FullPath(sk);
        bool filterEditorOnly = (pkg.Summary().packageFlags & 0x80000000u) != 0;

        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32()) r.Skip(16);
        StripFlags sf;
        sf.Read(r);
        r.Skip(28);   // FBoxSphereBounds
        i32 nm = r.Count(8);
        for (i32 i = 0; i < nm; i++) {
            i32 mat = r.I32();
            r.U32();   // bEnableShadowCasting
            MeshMaterialSlot s;
            s.name = mat ? pkg.ObjectName(mat) : Fmt("material_%d", i);
            if (mat) s.materialPath = pkg.FullPath(mat);
            m.materials.push_back(s);
        }
        ReadReferenceSkeleton(pkg, r, m.skeleton);

        i32 nl = r.Count(32);
        m.lods.resize(nl);
        for (i32 i = 0; i < nl; i++) ReadLod(r, m.lods[i], hasVertexColors, filterEditorOnly);
        for (auto& l : m.lods)
            for (u16 b : l.boneIndices)
                if (b >= m.skeleton.size()) throw ParseError("bone index outside skeleton");
        // per-LOD material remapping (LODInfo[i].LODMaterialMap: section material -> slot)
        if (auto info = FindValue(props, "LODInfo"))
            for (size_t li = 0; li < info->items.size() && li < m.lods.size(); li++)
                if (auto map = info->items[li].Field("LODMaterialMap"))
                    for (auto& s : m.lods[li].sections)
                        if (s.materialIndex >= 0 && s.materialIndex < (i32)map->items.size()) {
                            i64 to = map->items[s.materialIndex].AsInt();
                            if (to >= 0 && to < (i64)m.materials.size()) s.materialIndex = (i32)to;
                        }
        m.lods.erase(std::remove_if(m.lods.begin(), m.lods.end(), [](const MeshLod& l) { return l.positions.empty() || l.indices.empty(); }), m.lods.end());
        if (m.lods.empty()) throw ParseError("no LOD with geometry");
        m.ComputeBounds();
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}
