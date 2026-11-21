#include "render/Offscreen.h"
#include "ue/Mesh.h"

namespace {
struct Headless {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Renderer renderer;
    std::unique_ptr<GpuAssets> assets;
    PackageCache* cache = nullptr;
    std::string error;
    bool ok = false;
};
std::mutex g_mutex;
std::unique_ptr<Headless> g_headless;

Headless* GetHeadless(PackageCache& cache) {
    if (!g_headless) {
        g_headless = std::make_unique<Headless>();
        auto& h = *g_headless;
        D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &h.dev, nullptr, &h.ctx);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &h.dev, nullptr, &h.ctx);
        if (FAILED(hr)) h.error = Fmt("D3D11CreateDevice failed 0x%08x", (unsigned)hr);
        else h.ok = h.renderer.Init(h.dev.Get(), h.ctx.Get(), &h.error);
    }
    auto& h = *g_headless;
    if (h.ok && h.cache != &cache) {
        h.assets = std::make_unique<GpuAssets>(h.renderer, cache);
        h.cache = &cache;
    }
    return &h;
}
}

bool RenderMeshPreviews(PackageCache& cache, const std::vector<MeshData>& meshes, const PreviewRequest& req, std::vector<Image>& out, std::string* error) {
    std::lock_guard lk(g_mutex);
    Headless* h = GetHeadless(cache);
    if (!h->ok) { if (error) *error = h->error; return false; }
    if (meshes.empty()) { if (error) *error = "nothing to render"; return false; }
    Vec3 bmin = req.fixedBounds ? req.boundsMin : meshes[0].boundsMin, bmax = req.fixedBounds ? req.boundsMax : meshes[0].boundsMax;
    bool ok = true;
    std::vector<std::shared_ptr<GpuMesh>> keep;   // keeps materials/textures alive across frames
    out.clear();
    for (auto& mesh : meshes) {
        auto gm = h->assets->Mesh(mesh, req.lod, req.draw.textures);
        if (!gm) { if (error) *error = "mesh has no renderable geometry"; ok = false; break; }
        keep.push_back(gm);
        if (req.draw.graphMaterials) {
            h->assets->EnsureGraphShaders(*gm);
            if (req.graphNotes && out.empty())
                for (auto& m : gm->materials)
                    if (m && m->graph)
                        req.graphNotes->push_back(m->path + ": " + (m->graph->ps ? Fmt("compiled (%.0f ms, %zu textures)", m->graph->compileMs, m->graph->textures.size()) : m->graph->error));
        }
        Camera cam;
        cam.Frame(bmin, bmax);
        cam.yaw = req.yaw;
        cam.pitch = req.pitch;
        int w = std::clamp(req.width, 16, 4096), hgt = std::clamp(req.height, 16, 4096);
        h->renderer.BeginFrame(w, hgt, cam, req.draw);
        if (req.draw.grid) {
            float r = std::max(1.f, Length(bmax - bmin) * 0.5f);
            float cell = powf(10.f, floorf(log10f(r)));
            h->renderer.DrawGrid(cell / 10, 50);
        }
        Mat4 id = Mat4::Identity();
        h->renderer.DrawMesh(*gm, &id, 1);
        for (auto& o : req.overlays) h->renderer.DrawLines(o.segments, o.color);
        h->renderer.EndFrame();
        Image img;
        if (!h->renderer.ReadOutput(img)) { if (error) *error = "readback failed"; ok = false; break; }
        for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        out.push_back(std::move(img));
    }
    keep.clear();
    h->assets->Trim();   // the MCP server is long-lived: don't keep every previewed texture on the GPU
    return ok;
}

bool RenderMeshPreview(PackageCache& cache, const MeshData& mesh, const PreviewRequest& req, Image& out, std::string* error) {
    std::vector<MeshData> one{mesh};
    std::vector<Image> imgs;
    if (!RenderMeshPreviews(cache, one, req, imgs, error)) return false;
    out = std::move(imgs[0]);
    return true;
}

bool RenderWorldPreview(PackageCache& cache, const WorldData& world, const WorldPreviewRequest& req, Image& out, std::string* error) {
    std::lock_guard lk(g_mutex);
    Headless* h = GetHeadless(cache);
    if (!h->ok) { if (error) *error = h->error; return false; }
    bool ok = true;
    {
        WorldScene scene(h->renderer, *h->assets, cache);
        scene.Build(world);
        Camera cam;
        Vec3 bmin = scene.FocusMin(), bmax = scene.FocusMax();
        if (Length(bmax - bmin) < 1) { bmin = world.boundsMin; bmax = world.boundsMax; }
        cam.Frame(bmin, bmax);
        cam.yaw = req.yaw;
        cam.pitch = req.pitch;
        cam.distance *= std::max(0.01f, req.zoom);
        if (req.useEye) {
            Vec3 d = req.target - req.eye;
            cam.target = req.target;
            cam.distance = std::max(1.f, Length(d));
            cam.yaw = atan2f(d.y, d.x) * 180 / kPi;
            cam.pitch = asinf(std::clamp(d.z / cam.distance, -1.f, 1.f)) * 180 / kPi;
        }
        cam.nearZ = std::max(1.f, cam.distance * 0.0005f);
        int w = std::clamp(req.width, 16, 4096), hgt = std::clamp(req.height, 16, 4096);
        SceneLighting lighting;
        scene.Lighting(cam, req.world, lighting);
        DrawOptions dopt = req.draw;
        dopt.lighting = &lighting;
        h->renderer.BeginFrame(w, hgt, cam, dopt);
        scene.Draw(cam, (float)hgt, req.world);
        scene.DrawOverlays(cam, req.world);
        h->renderer.EndFrame();
        if (!h->renderer.ReadOutput(out)) { if (error) *error = "readback failed"; ok = false; }
        else for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    }
    h->assets->Trim();
    return ok;
}

MeshLod MakePreviewSphere(float radius, int seg, int rings) {
    MeshLod l;
    l.numUVs = 1;
    for (int r = 0; r <= rings; r++) {
        float v = (float)r / rings, th = v * kPi;
        for (int s = 0; s <= seg; s++) {
            float u = (float)s / seg, ph = u * 2 * kPi;
            Vec3 n{sinf(th) * cosf(ph), sinf(th) * sinf(ph), cosf(th)};
            l.positions.push_back(n * radius);
            l.normals.push_back(n);
            l.tangents.push_back({-sinf(ph), cosf(ph), 0, 1});
            l.uvs.push_back({u * 2, v});
        }
    }
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < seg; s++) {
            u32 a = r * (seg + 1) + s, b = a + seg + 1;
            l.indices.insert(l.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    l.sections.push_back({0, 0, (u32)(l.indices.size() / 3), 0, (u32)l.positions.size() - 1, true, 0});
    return l;
}
