#include "render/GpuAssets.h"
#include "render/GraphShader.h"
#include "ue/MaterialGraph.h"
#include <chrono>
#include "ue/Mesh.h"

static std::string ObjectNameOf(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot != std::string::npos && path.find('/', dot) == std::string::npos) return path.substr(dot + 1);
    return {};
}

std::shared_ptr<GpuTexture> GpuAssets::Texture(const std::string& path, bool forceLinear) {
    if (path.empty()) return nullptr;
    std::string key = path + (forceLinear ? "#lin" : "");
    {
        std::lock_guard lk(m_mutex);
        auto it = m_textures.find(key);
        if (it != m_textures.end()) return it->second;
    }
    std::shared_ptr<GpuTexture> result;
    std::string err;
    if (auto pkg = m_cache.LoadPath(path, &err)) {
        std::string obj = ObjectNameOf(path);
        i32 ex = obj.empty() ? 0 : pkg->FindExport(obj);
        if (!ex) ex = pkg->MainExport();
        TextureInfo info;
        Image img;
        if (ex && ReadTextureInfo(*pkg, ex - 1, info, &err) && DecodeTexture(*pkg, info, -1, m_maxTex, img, &err)) {
            result = m_r.CreateTexture(img, info.srgb && !forceLinear, true);
            if (result) m_texBytes += (size_t)img.width * img.height * 4 * 4 / 3;
        }
    }
    if (!result) LOGW("texture %s: %s", path.c_str(), err.empty() ? "unsupported" : err.c_str());
    std::lock_guard lk(m_mutex);
    m_textures[key] = result;
    return result;
}

std::shared_ptr<GpuMaterial> GpuAssets::Material(const std::string& path) {
    {
        std::lock_guard lk(m_mutex);
        auto it = m_materials.find(path);
        if (it != m_materials.end()) return it->second;
    }
    auto gm = std::make_shared<GpuMaterial>();
    gm->path = path;
    MaterialInfo mi;
    std::string err;
    if (!path.empty() && ReadMaterial(m_cache, path, mi, &err)) {
        gm->twoSided = mi.twoSided;
        gm->masked = mi.blendMode == "BLEND_Masked";
        gm->diffuse = Texture(mi.diffuse);
        gm->normal = Texture(mi.normal, true);
        if (!gm->diffuse)
            for (auto& v : mi.vectors) {
                std::string n = ToLower(v.name);
                if (n.find("colo") != std::string::npos || n.find("tint") != std::string::npos || n.find("albedo") != std::string::npos) {
                    gm->color[0] = v.r; gm->color[1] = v.g; gm->color[2] = v.b;
                    break;
                }
            }
    } else if (!path.empty()) {
        LOGW("material %s: %s", path.c_str(), err.c_str());
    }
    std::lock_guard lk(m_mutex);
    m_materials[path] = gm;
    return gm;
}

std::shared_ptr<GpuMesh> GpuAssets::Mesh(const MeshData& mesh, int lod, bool loadMaterials, bool dynamic) {
    if (mesh.lods.empty()) return nullptr;
    lod = std::clamp(lod, 0, (int)mesh.lods.size() - 1);
    auto gm = m_r.CreateMesh(mesh.lods[lod], mesh.boundsMin, mesh.boundsMax, dynamic);
    if (!gm) return nullptr;
    for (auto& slot : mesh.materials) gm->materials.push_back(loadMaterials ? Material(slot.materialPath) : std::make_shared<GpuMaterial>());
    return gm;
}

std::shared_ptr<GpuMesh> GpuAssets::MeshByPath(const std::string& path, int lod) {
    std::string key = path + "#" + std::to_string(lod);
    {
        std::lock_guard lk(m_mutex);
        auto it = m_meshes.find(key);
        if (it != m_meshes.end()) return it->second;
    }
    std::shared_ptr<GpuMesh> result;
    std::string err;
    if (auto pkg = m_cache.LoadPath(path, &err)) {
        std::string obj = ObjectNameOf(path);
        i32 ex = obj.empty() ? 0 : pkg->FindExport(obj);
        if (!ex) ex = pkg->MainExport();
        MeshData md;
        if (ex && ReadMesh(*pkg, ex - 1, md, &err)) result = Mesh(md, lod);
    }
    if (!result) LOGW("mesh %s: %s", path.c_str(), err.c_str());
    std::lock_guard lk(m_mutex);
    m_meshes[key] = result;
    return result;
}

void GpuAssets::Clear() {
    std::lock_guard lk(m_mutex);
    m_textures.clear();
    m_materials.clear();
    m_meshes.clear();
    m_graphs.clear();
    m_texBytes = 0;
}

void GpuAssets::Trim() {
    std::lock_guard lk(m_mutex);
    // meshes first (they hold materials), then materials (they hold textures), then textures
    for (auto it = m_meshes.begin(); it != m_meshes.end();) it = it->second.use_count() <= 1 ? m_meshes.erase(it) : std::next(it);
    for (auto it = m_materials.begin(); it != m_materials.end();) it = it->second.use_count() <= 1 ? m_materials.erase(it) : std::next(it);
    for (auto it = m_graphs.begin(); it != m_graphs.end();) it = it->second.use_count() <= 1 ? m_graphs.erase(it) : std::next(it);
    size_t bytes = 0;
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        if (it->second.use_count() <= 1) it = m_textures.erase(it);
        else {
            bytes += (size_t)it->second->width * it->second->height * 4 * 4 / 3;
            ++it;
        }
    }
    m_texBytes = bytes;
}

std::shared_ptr<GpuGraphShader> GpuAssets::GraphShader(const std::string& path) {
    {
        std::lock_guard lk(m_mutex);
        auto it = m_graphs.find(path);
        if (it != m_graphs.end()) return it->second;
    }
    auto g = std::make_shared<GpuGraphShader>();
    auto t0 = std::chrono::steady_clock::now();
    MaterialInfo mi;
    std::string hlsl, err;
    GraphShaderSource src;
    if (path.empty()) g->error = "no material";
    else if (!ReadMaterial(m_cache, path, mi, &err)) g->error = "material: " + err;
    else if (!MaterialToHlsl(m_cache, path, hlsl, nullptr, &err)) g->error = "translate: " + err;
    else if (!BuildGraphShaderSource(hlsl, mi.blendMode, mi.shadingModel, src, &err)) g->error = err;
    else if (!m_r.CreatePixelShader(src.hlsl, "GraphPSMain", g->ps, &err)) {
        std::string errs;   // error lines only (drop warnings and the shader file prefix)
        size_t s = 0;
        while (s < err.size()) {
            size_t e = err.find('\n', s);
            std::string l = err.substr(s, e == std::string::npos ? std::string::npos : e - s);
            s = e == std::string::npos ? err.size() : e + 1;
            size_t k = l.find("error X");
            if (k == std::string::npos) continue;
            size_t lp = l.find('(');
            errs += (errs.empty() ? "" : "; ") + (lp != std::string::npos && lp < k ? "line " + l.substr(lp + 1, l.find(',', lp) - lp - 1) + ": " : std::string()) + l.substr(k);
        }
        g->error = "compile: " + (errs.empty() ? err : errs).substr(0, 800);
    }
    else {
        g->blend = src.blend;
        for (auto& t : src.textures) {
            g->textureNames.push_back(t.ident);
            g->textures.push_back(t.cube || t.path.empty() ? nullptr : Texture(t.path));
        }
    }
    g->compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!g->error.empty()) LOGW("graph shader %s: %s", path.c_str(), g->error.substr(0, 300).c_str());
    std::lock_guard lk(m_mutex);
    m_graphs[path] = g;
    return g;
}

void GpuAssets::EnsureGraphShaders(const GpuMesh& mesh) {
    for (auto& m : mesh.materials)
        if (m && !m->graph) m->graph = GraphShader(m->path);
}
