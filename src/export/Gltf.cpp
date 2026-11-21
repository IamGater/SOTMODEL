#include "export/Gltf.h"
#include "core/JsonValue.h"
#include <cmath>

static std::string Q(const std::string& s) {
    JsonValue v;
    v.type = JsonValue::Type::String;
    v.str = s;
    return v.Dump();
}
static std::string F(float f) {
    if (!std::isfinite(f)) f = 0;
    char b[32];
    snprintf(b, sizeof(b), "%.7g", f);
    return b;
}
static void Append(std::string& list, const std::string& item) {
    if (!list.empty()) list += ",";
    list += item;
}

void GltfBuilder::ConvertMatrix(const float m[16], float o[16]) {
    // UE: row vectors, M[row][col], translation in row 3. Swap Y/Z on both sides, scale translation to metres.
    int map[3] = {0, 2, 1};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) o[c * 4 + r] = m[map[c] * 4 + map[r]];   // gltf column c row r = UE row c' col r'
    o[3] = o[7] = o[11] = 0;
    o[12] = m[12] * 0.01f;
    o[13] = m[14] * 0.01f;
    o[14] = m[13] * 0.01f;
    o[15] = 1;
}

int GltfBuilder::AddView(const void* data, size_t size, int target) {
    while (m_bin.size() % 4) m_bin.push_back(0);
    size_t off = m_bin.size();
    m_bin.insert(m_bin.end(), (const u8*)data, (const u8*)data + size);
    std::string v = "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(size);
    if (target) v += ",\"target\":" + std::to_string(target);
    v += "}";
    Append(m_views, v);
    return m_viewCount++;
}

int GltfBuilder::AddAccessor(int view, int ct, size_t count, const char* type, const float* mn, const float* mx, int comps, bool normalized) {
    std::string a = "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(ct) + ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
    if (normalized) a += ",\"normalized\":true";
    if (mn && mx) {
        a += ",\"min\":[";
        for (int i = 0; i < comps; i++) a += (i ? "," : "") + F(mn[i]);
        a += "],\"max\":[";
        for (int i = 0; i < comps; i++) a += (i ? "," : "") + F(mx[i]);
        a += "]";
    }
    a += "}";
    Append(m_accessors, a);
    return m_accessorCount++;
}

int GltfBuilder::AddImagePng(const Bytes& png, const std::string& name) {
    int view = AddView(png.data(), png.size());
    Append(m_images, "{\"bufferView\":" + std::to_string(view) + ",\"mimeType\":\"image/png\",\"name\":" + Q(name) + "}");
    Append(m_textures, "{\"source\":" + std::to_string(m_imageCount) + "}");
    return m_imageCount++;
}

int GltfBuilder::AddMaterial(const MaterialDesc& d) {
    float c[4] = {d.baseColor[0], d.baseColor[1], d.baseColor[2], d.baseColor[3]};
    if (d.baseColorImage >= 0) c[0] = c[1] = c[2] = c[3] = 1.f;   // the texture carries the colour
    std::string m = "{\"name\":" + Q(d.name) + ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + F(c[0]) + "," + F(c[1]) + "," + F(c[2]) + "," + F(c[3]) + "]";
    if (d.baseColorImage >= 0) m += ",\"baseColorTexture\":{\"index\":" + std::to_string(d.baseColorImage) + "}";
    m += ",\"metallicFactor\":0,\"roughnessFactor\":0.8}";
    if (d.normalImage >= 0) m += ",\"normalTexture\":{\"index\":" + std::to_string(d.normalImage) + "}";
    if (d.doubleSided) m += ",\"doubleSided\":true";
    if (d.masked) m += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5";
    else if (d.translucent) m += ",\"alphaMode\":\"BLEND\"";
    m += "}";
    Append(m_materials, m);
    return m_materialCount++;
}

int GltfBuilder::AddMesh(const MeshData& mesh, int lodIndex, const std::vector<int>& materialMap, bool withSkin) {
    const MeshLod& lod = mesh.lods.at(lodIndex);
    size_t nv = lod.positions.size();
    std::vector<Vec3> pos(nv), nrm;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < nv; i++) {
        pos[i] = ConvertPos(lod.positions[i]);
        const float* p = &pos[i].x;
        for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
    }
    int posAcc = AddAccessor(AddView(pos.data(), nv * 12, 34962), 5126, nv, "VEC3", mn, mx, 3);
    std::string attrs = "\"POSITION\":" + std::to_string(posAcc);
    if (lod.normals.size() == nv) {
        nrm.resize(nv);
        for (size_t i = 0; i < nv; i++) {
            Vec3 n = ConvertDir(lod.normals[i]);
            float l = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
            nrm[i] = l > 1e-6f ? n * (1.f / l) : Vec3{0, 1, 0};
        }
        attrs += ",\"NORMAL\":" + std::to_string(AddAccessor(AddView(nrm.data(), nv * 12, 34962), 5126, nv, "VEC3"));
    }
    if (lod.tangents.size() == nv) {
        std::vector<Vec4> tan(nv);
        for (size_t i = 0; i < nv; i++) {
            Vec3 t = ConvertDir({lod.tangents[i].x, lod.tangents[i].y, lod.tangents[i].z});
            float l = sqrtf(t.x * t.x + t.y * t.y + t.z * t.z);
            if (l < 1e-6f) t = {1, 0, 0}; else t = t * (1.f / l);
            tan[i] = {t.x, t.y, t.z, -lod.tangents[i].w};   // the axis swap mirrors the basis
        }
        attrs += ",\"TANGENT\":" + std::to_string(AddAccessor(AddView(tan.data(), nv * 16, 34962), 5126, nv, "VEC4"));
    }
    for (int u = 0; u < lod.numUVs && u < 4; u++) {
        std::vector<Vec2> uv(nv);
        for (size_t i = 0; i < nv; i++) uv[i] = lod.uvs[i * lod.numUVs + u];
        attrs += ",\"TEXCOORD_" + std::to_string(u) + "\":" + std::to_string(AddAccessor(AddView(uv.data(), nv * 8, 34962), 5126, nv, "VEC2"));
    }
    if (lod.colors.size() == nv) attrs += ",\"COLOR_0\":" + std::to_string(AddAccessor(AddView(lod.colors.data(), nv * 4, 34962), 5121, nv, "VEC4", nullptr, nullptr, 0, true));
    if (withSkin && lod.influences > 0 && lod.boneIndices.size() == nv * lod.influences) {
        int sets = (lod.influences + 3) / 4;
        for (int s = 0; s < sets; s++) {
            std::vector<u16> j(nv * 4, 0);
            std::vector<u8> w(nv * 4, 0);
            for (size_t i = 0; i < nv; i++)
                for (int k = 0; k < 4; k++) {
                    int inf = s * 4 + k;
                    if (inf >= lod.influences) break;
                    j[i * 4 + k] = lod.boneIndices[i * lod.influences + inf];
                    w[i * 4 + k] = lod.boneWeights[i * lod.influences + inf];
                }
            attrs += ",\"JOINTS_" + std::to_string(s) + "\":" + std::to_string(AddAccessor(AddView(j.data(), j.size() * 2, 34962), 5123, nv, "VEC4"));
            attrs += ",\"WEIGHTS_" + std::to_string(s) + "\":" + std::to_string(AddAccessor(AddView(w.data(), w.size(), 34962), 5121, nv, "VEC4", nullptr, nullptr, 0, true));
        }
    }
    std::string prims;
    for (auto& s : lod.sections) {
        if (!s.numTriangles) continue;
        size_t first = s.firstIndex, count = (size_t)s.numTriangles * 3;
        if (first + count > lod.indices.size()) continue;
        std::vector<u32> idx(lod.indices.begin() + first, lod.indices.begin() + first + count);
        for (auto& i : idx) i += s.baseVertex;
        int acc = AddAccessor(AddView(idx.data(), idx.size() * 4, 34963), 5125, idx.size(), "SCALAR");
        std::string p = "{\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(acc);
        int mat = s.materialIndex >= 0 && s.materialIndex < (int)materialMap.size() ? materialMap[s.materialIndex] : -1;
        if (mat >= 0) p += ",\"material\":" + std::to_string(mat);
        p += "}";
        Append(prims, p);
    }
    Append(m_meshes, "{\"name\":" + Q(mesh.name) + ",\"primitives\":[" + prims + "]}");
    return m_meshCount++;
}

int GltfBuilder::AddNode(const std::string& name, int mesh, const float* matrix, int parent) {
    std::string n = "\"name\":" + Q(name);
    if (mesh >= 0) n += ",\"mesh\":" + std::to_string(mesh);
    if (matrix) {
        bool identity = true;
        for (int i = 0; i < 16; i++) if (fabsf(matrix[i] - ((i % 5 == 0) ? 1.f : 0.f)) > 1e-6f) identity = false;
        if (!identity) {
            n += ",\"matrix\":[";
            for (int i = 0; i < 16; i++) n += (i ? "," : "") + F(matrix[i]);
            n += "]";
        }
    }
    m_nodeJson.push_back(n);
    m_nodeChildren.emplace_back();
    m_nodeParent.push_back(parent);
    if (parent >= 0) m_nodeChildren[parent].push_back(m_nodeCount);
    else m_roots.push_back(m_nodeCount);
    return m_nodeCount++;
}

int GltfBuilder::AddLight(const std::string& name, const char* type, const Vec3& color, float intensity, float range, float innerCone, float outerCone) {
    std::string l = "{\"name\":" + Q(name) + ",\"type\":\"" + type + "\",\"color\":[" + F(color.x) + "," + F(color.y) + "," + F(color.z) + "],\"intensity\":" + F(intensity);
    if (range > 0) l += ",\"range\":" + F(range);
    if (std::string(type) == "spot") l += ",\"spot\":{\"innerConeAngle\":" + F(innerCone) + ",\"outerConeAngle\":" + F(outerCone) + "}";
    Append(m_lights, l + "}");
    return m_lightCount++;
}

void GltfBuilder::SetNodeLight(int node, int light) {
    m_nodeJson[node] += ",\"extensions\":{\"KHR_lights_punctual\":{\"light\":" + std::to_string(light) + "}}";
}

void GltfBuilder::SetNodeExtras(int node, const std::string& extras) { m_nodeJson[node] += ",\"extras\":" + extras; }

void GltfBuilder::SetNodeSkin(int node, int skin) { m_nodeJson[node] += ",\"skin\":" + std::to_string(skin); }

int GltfBuilder::AddSkin(const std::vector<Bone>& bones, int parentNode, std::vector<int>* boneNodes) {
    std::vector<int> nodes(bones.size());
    // world matrices for inverse bind (gltf space)
    struct M { float m[16]; };
    std::vector<M> world(bones.size());
    auto compose = [](const Vec4& q, const Vec3& t, const Vec3& s, float* o) {   // column-major TRS
        float x = q.x, y = q.y, z = q.z, w = q.w;
        float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                      2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                      2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
        for (int c = 0; c < 3; c++) {
            float sc = c == 0 ? s.x : c == 1 ? s.y : s.z;
            for (int rr = 0; rr < 3; rr++) o[c * 4 + rr] = r[c * 3 + rr] * sc;
            o[c * 4 + 3] = 0;
        }
        o[12] = t.x; o[13] = t.y; o[14] = t.z; o[15] = 1;
    };
    auto mul = [](const float* a, const float* b, float* o) {
        float t[16];
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
        memcpy(o, t, sizeof(t));
    };
    for (size_t i = 0; i < bones.size(); i++) {
        const Bone& b = bones[i];
        Vec4 q = ConvertQuat(b.rotation);
        Vec3 t = ConvertPos(b.translation);
        Vec3 s = {b.scale.x, b.scale.z, b.scale.y};
        float local[16];
        compose(q, t, s, local);
        int parent = b.parent >= 0 && b.parent < (int)i ? nodes[b.parent] : parentNode;
        std::string n = "\"name\":" + Q(b.name) + ",\"rotation\":[" + F(q.x) + "," + F(q.y) + "," + F(q.z) + "," + F(q.w) + "],\"translation\":[" + F(t.x) + "," + F(t.y) + "," + F(t.z) + "]";
        if (fabsf(s.x - 1) > 1e-5f || fabsf(s.y - 1) > 1e-5f || fabsf(s.z - 1) > 1e-5f) n += ",\"scale\":[" + F(s.x) + "," + F(s.y) + "," + F(s.z) + "]";
        m_nodeJson.push_back(n);
        m_nodeChildren.emplace_back();
        m_nodeParent.push_back(parent);
        if (parent >= 0) m_nodeChildren[parent].push_back(m_nodeCount);
        else m_roots.push_back(m_nodeCount);
        nodes[i] = m_nodeCount++;
        if (b.parent >= 0 && b.parent < (int)i) mul(world[b.parent].m, local, world[i].m);
        else memcpy(world[i].m, local, sizeof(local));
    }
    // inverse bind matrices (rigid inverse with scale support via general 4x4 inverse)
    std::vector<float> ibm(bones.size() * 16);
    for (size_t i = 0; i < bones.size(); i++) {
        const float* m = world[i].m;
        float inv[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        float id = fabsf(det) > 1e-12f ? 1.f / det : 0.f;
        for (int k = 0; k < 16; k++) ibm[i * 16 + k] = inv[k] * id;
        ibm[i * 16 + 3] = ibm[i * 16 + 7] = ibm[i * 16 + 11] = 0;
        ibm[i * 16 + 15] = 1;
    }
    int acc = AddAccessor(AddView(ibm.data(), ibm.size() * 4), 5126, bones.size(), "MAT4");
    std::string joints;
    for (int n : nodes) Append(joints, std::to_string(n));
    Append(m_skins, "{\"inverseBindMatrices\":" + std::to_string(acc) + ",\"joints\":[" + joints + "]" + (nodes.empty() ? "" : ",\"skeleton\":" + std::to_string(nodes[0])) + "}");
    if (boneNodes) *boneNodes = nodes;
    return m_skinCount++;
}

void GltfBuilder::AddAnimation(const std::string& name, const std::vector<Track>& tracks) {
    std::string samplers, channels;
    int si = 0;
    for (auto& t : tracks) {
        if (t.times.empty()) continue;
        float tmin = t.times.front(), tmax = t.times.back();
        int timeAcc = AddAccessor(AddView(t.times.data(), t.times.size() * 4), 5126, t.times.size(), "SCALAR", &tmin, &tmax, 1);
        auto addChannel = [&](const char* path, const void* data, size_t bytes, const char* type) {
            int acc = AddAccessor(AddView(data, bytes), 5126, t.times.size(), type);
            Append(samplers, "{\"input\":" + std::to_string(timeAcc) + ",\"output\":" + std::to_string(acc) + ",\"interpolation\":\"LINEAR\"}");
            Append(channels, "{\"sampler\":" + std::to_string(si++) + ",\"target\":{\"node\":" + std::to_string(t.node) + ",\"path\":\"" + path + "\"}}");
        };
        if (t.translations.size() == t.times.size()) {
            std::vector<Vec3> v(t.translations.size());
            for (size_t i = 0; i < v.size(); i++) v[i] = ConvertPos(t.translations[i]);
            addChannel("translation", v.data(), v.size() * 12, "VEC3");
        }
        if (t.rotations.size() == t.times.size()) {
            std::vector<Vec4> v(t.rotations.size());
            for (size_t i = 0; i < v.size(); i++) v[i] = ConvertQuat(t.rotations[i]);
            addChannel("rotation", v.data(), v.size() * 16, "VEC4");
        }
        if (t.scales.size() == t.times.size()) {
            std::vector<Vec3> v(t.scales.size());
            for (size_t i = 0; i < v.size(); i++) v[i] = {t.scales[i].x, t.scales[i].z, t.scales[i].y};
            addChannel("scale", v.data(), v.size() * 12, "VEC3");
        }
    }
    if (channels.empty()) return;
    Append(m_animations, "{\"name\":" + Q(name) + ",\"samplers\":[" + samplers + "],\"channels\":[" + channels + "]}");
    m_animCount++;
}

Bytes GltfBuilder::Finish() {
    std::string nodes;
    for (int i = 0; i < m_nodeCount; i++) {
        std::string n = "{" + m_nodeJson[i];
        if (!m_nodeChildren[i].empty()) {
            n += ",\"children\":[";
            for (size_t k = 0; k < m_nodeChildren[i].size(); k++) n += (k ? "," : "") + std::to_string(m_nodeChildren[i][k]);
            n += "]";
        }
        Append(nodes, n + "}");
    }
    std::string roots;
    for (int r : m_roots) Append(roots, std::to_string(r));
    while (m_bin.size() % 4) m_bin.push_back(0);
    std::string j = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"SOTModel\"},\"scene\":0,\"scenes\":[{\"nodes\":[" + roots + "]" +
                    (m_sceneExtras.empty() ? std::string() : ",\"extras\":" + m_sceneExtras) + "}]";
    if (m_lightCount)
        j += ",\"extensionsUsed\":[\"KHR_lights_punctual\"],\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[" + m_lights + "]}}";
    if (m_nodeCount) j += ",\"nodes\":[" + nodes + "]";
    if (m_meshCount) j += ",\"meshes\":[" + m_meshes + "]";
    if (m_materialCount) j += ",\"materials\":[" + m_materials + "]";
    if (m_imageCount) j += ",\"images\":[" + m_images + "],\"textures\":[" + m_textures + "]";
    if (m_skinCount) j += ",\"skins\":[" + m_skins + "]";
    if (m_animCount) j += ",\"animations\":[" + m_animations + "]";
    if (m_accessorCount) j += ",\"accessors\":[" + m_accessors + "]";
    if (m_viewCount) j += ",\"bufferViews\":[" + m_views + "]";
    if (!m_bin.empty()) j += ",\"buffers\":[{\"byteLength\":" + std::to_string(m_bin.size()) + "}]";
    j += "}";
    while (j.size() % 4) j += ' ';
    Bytes out;
    auto put = [&](u32 v) { out.insert(out.end(), (u8*)&v, (u8*)&v + 4); };
    u32 total = 12 + 8 + (u32)j.size() + (m_bin.empty() ? 0 : 8 + (u32)m_bin.size());
    put(0x46546C67);
    put(2);
    put(total);
    put((u32)j.size());
    put(0x4E4F534A);
    out.insert(out.end(), j.begin(), j.end());
    if (!m_bin.empty()) {
        put((u32)m_bin.size());
        put(0x004E4942);
        out.insert(out.end(), m_bin.begin(), m_bin.end());
    }
    return out;
}
