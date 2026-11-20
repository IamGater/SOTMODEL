#pragma once
#include "render/Math.h"
#include "ue/Texture.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>
#include <map>

using Microsoft::WRL::ComPtr;

// GPU texture: `srv` samples with the asset's colour space (sRGB or linear), `srvRaw` shows the stored values as-is (viewer).
struct GpuTexture {
    ComPtr<ID3D11ShaderResourceView> srv, srvRaw;
    int width = 0, height = 0;
};

struct GpuMaterial {
    std::shared_ptr<GpuTexture> diffuse, normal;
    float color[4] = {0.75f, 0.75f, 0.75f, 1};
    bool twoSided = false;
    bool masked = false;
};

struct GpuSection {
    u32 firstIndex = 0, indexCount = 0;
    int material = 0;
};

struct GpuMesh {
    ComPtr<ID3D11Buffer> vb, ib;
    std::vector<GpuSection> sections;
    std::vector<std::shared_ptr<GpuMaterial>> materials;   // per material slot
    Vec3 boundsMin, boundsMax, center;
    float radius = 0;
    u32 vertexCount = 0, triangleCount = 0;
    size_t gpuBytes = 0;
};

struct Camera {
    Vec3 target;
    float yaw = -135, pitch = -20;   // degrees
    float distance = 500;
    float fovY = 60;
    float nearZ = 1;
    float flySpeed = 1000;          // cm/s
    Vec3 Forward() const;
    Vec3 Eye() const { return target - Forward() * distance; }
    void Frame(const Vec3& mn, const Vec3& mx);
};

struct DrawOptions {
    bool wireframe = false;
    bool textures = true;
    bool normalMaps = true;
    bool vertexColors = false;
    bool grid = true;
    bool backfaceCulling = true;
};

class Renderer {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* error);
    ID3D11Device* Device() const { return m_dev.Get(); }

    // Creation functions only touch the device (free threaded): safe to call from worker threads.
    std::shared_ptr<GpuTexture> CreateTexture(const Image& img, bool srgb, bool mips) const;
    std::shared_ptr<GpuMesh> CreateMesh(const MeshLod& lod, const Vec3& bmin, const Vec3& bmax) const;

    // Viewport rendering into an offscreen target (main thread).
    void BeginFrame(int width, int height, const Camera& cam, const DrawOptions& opt);
    void DrawMesh(const GpuMesh& mesh, const Mat4* instances, u32 count);
    void DrawGrid(float cellSize, int cells);
    void EndFrame();
    ID3D11ShaderResourceView* Output() const { return m_outSrv.Get(); }
    bool ReadOutput(Image& out);   // CPU readback of the last frame (RGBA8, gamma encoded)
    const Frustum& ViewFrustum() const { return m_frustum; }
    const Mat4& ViewProj() const { return m_viewProj; }
    u64 DrawnTriangles() const { return m_drawnTris; }

private:
    void EnsureTargets(int w, int h);
    void EnsureInstanceCapacity(u32 count);

    ComPtr<ID3D11Device> m_dev;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11VertexShader> m_meshVs, m_lineVs;
    ComPtr<ID3D11PixelShader> m_meshPs, m_linePs;
    ComPtr<ID3D11InputLayout> m_meshLayout, m_lineLayout;
    ComPtr<ID3D11Buffer> m_frameCb, m_materialCb, m_instanceVb, m_gridVb;
    u32 m_instanceCap = 0, m_gridVerts = 0;
    int m_gridCells = 0;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_rsSolid[2], m_rsWire;   // [cull back, none]
    ComPtr<ID3D11DepthStencilState> m_depth;
    ComPtr<ID3D11BlendState> m_opaqueBlend, m_lineBlend;
    std::shared_ptr<GpuTexture> m_white, m_flatNormal;

    // targets
    int m_w = 0, m_h = 0;
    ComPtr<ID3D11Texture2D> m_msaaColor, m_out, m_msaaDepth;
    ComPtr<ID3D11RenderTargetView> m_msaaRtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11ShaderResourceView> m_outSrv;
    UINT m_samples = 4;

    Mat4 m_viewProj;
    Frustum m_frustum;
    DrawOptions m_opt;
    u64 m_drawnTris = 0;
};

// Builds box-filtered mip levels of an RGBA8 image (level 0 is the input).
std::vector<Image> BuildMips(const Image& img);
