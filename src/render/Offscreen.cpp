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

bool RenderMeshPreview(PackageCache& cache, const MeshData& mesh, const PreviewRequest& req, Image& out, std::string* error) {
    std::lock_guard lk(g_mutex);
    Headless* h = GetHeadless(cache);
    if (!h->ok) { if (error) *error = h->error; return false; }
    auto gm = h->assets->Mesh(mesh, req.lod, req.draw.textures);
    if (!gm) { if (error) *error = "mesh has no renderable geometry"; return false; }
    Camera cam;
    cam.Frame(mesh.boundsMin, mesh.boundsMax);
    cam.yaw = req.yaw;
    cam.pitch = req.pitch;
    int w = std::clamp(req.width, 16, 4096), hgt = std::clamp(req.height, 16, 4096);
    h->renderer.BeginFrame(w, hgt, cam, req.draw);
    if (req.draw.grid) {
        float r = std::max(1.f, gm->radius);
        float cell = powf(10.f, floorf(log10f(r)));
        h->renderer.DrawGrid(cell / 10, 50);
    }
    Mat4 id = Mat4::Identity();
    h->renderer.DrawMesh(*gm, &id, 1);
    h->renderer.EndFrame();
    bool ok = h->renderer.ReadOutput(out);
    gm.reset();
    h->assets->Trim();   // the MCP server is long-lived: don't keep every previewed texture on the GPU
    if (!ok) { if (error) *error = "readback failed"; return false; }
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    return true;
}
