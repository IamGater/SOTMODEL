#pragma once
#include "render/Math.h"
#include "ue/Texture.h"
#include "render/GraphShader.h"
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

// A material's translated graph compiled as a viewport pixel shader (GraphShader.h).
struct GpuGraphShader {
    ComPtr<ID3D11PixelShader> ps;
    std::vector<std::shared_ptr<GpuTexture>> textures;   // t0..tN (null for cubes / missing)
    std::vector<std::string> textureNames;
    int blend = 0;   // 0 opaque, 1 masked, 2 translucent, 3 additive, 4 modulate
    std::string error;
    double compileMs = 0;
    // editable scalar / vector parameters (b3, float4 each): change `values` on the main thread and bump `version`
    std::vector<GraphShaderParam> params;   // value = the material's own value
    std::vector<float> values;
    u32 version = 1;
    mutable ComPtr<ID3D11Buffer> paramCb;
    mutable u32 uploaded = 0;
};

struct GpuMaterial {
    std::string path;
    std::shared_ptr<GpuGraphShader> graph;   // set when graph shading is requested (GpuAssets::GraphShader)
    std::shared_ptr<GpuTexture> diffuse, normal;
    float color[4] = {0.75f, 0.75f, 0.75f, 1};
    bool twoSided = false;
    bool masked = false;
    bool vertexColor = false;   // multiply by the (sRGB) vertex colour, e.g. landscape paint layers
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
    Bytes cpuVertices;   // dynamic meshes (animation playback): CPU copy of the vertex buffer
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

// Lights / fog of a map applied by the mesh shader (all in UE world space).
struct SceneLight {
    Vec3 position; float radius = 1000;
    Vec3 color;                 // linear colour already scaled by the brightness
    bool spot = false;
    Vec3 direction{1, 0, 0};
    float cosOuter = -1, cosInner = 1;
};
constexpr int kMaxSceneLights = 32;
struct SceneLighting {
    bool sun = false;
    Vec3 sunDir{0, 0, -1}, sunColor{1, 1, 1}, ambientScale{1, 1, 1};
    bool fog = false;                       // UE exponential height fog
    Vec3 fogColor;
    float fogDensity = 0, fogFalloff = 0;   // as on the component (divided by 1000 in the shader math)
    float fogHeight = 0, fogStart = 0, fogMaxOpacity = 1;
    std::vector<SceneLight> lights;         // the first kMaxSceneLights are used
};

struct DrawOptions {
    bool wireframe = false;
    bool textures = true;
    bool normalMaps = true;
    bool vertexColors = false;
    bool grid = true;
    bool backfaceCulling = true;
    const SceneLighting* lighting = nullptr;   // map lights / fog (null: the viewer's key light)
    bool graphMaterials = false;               // draw with each material's translated graph shader where compiled
    float time = -1;                           // material time (s); < 0 = wall clock
};

// A particle sprite corner (DrawSprites takes triangle lists, 6 vertices per quad). Colour is linear rgb + alpha.
struct SpriteVertex {
    float pos[3];
    float uv[2];
    float color[4];
};

class Renderer {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* error);
    ID3D11Device* Device() const { return m_dev.Get(); }

    // Creation functions only touch the device (free threaded): safe to call from worker threads.
    std::shared_ptr<GpuTexture> CreateTexture(const Image& img, bool srgb, bool mips) const;
    std::shared_ptr<GpuMesh> CreateMesh(const MeshLod& lod, const Vec3& bmin, const Vec3& bmax, bool dynamic = false) const;
    // Dynamic meshes only (main thread): replace positions / normals, e.g. after CPU skinning.
    void UpdateMeshVertices(GpuMesh& mesh, const std::vector<Vec3>& positions, const std::vector<Vec3>& normals);
    // Compiles a pixel shader (free threaded).
    bool CreatePixelShader(const std::string& source, const char* entry, ComPtr<ID3D11PixelShader>& out, std::string* error) const;

    // Viewport rendering into an offscreen target (main thread).
    void BeginFrame(int width, int height, const Camera& cam, const DrawOptions& opt);
    void DrawMesh(const GpuMesh& mesh, const Mat4* instances, u32 count);
    void DrawGrid(float cellSize, int cells);
    void DrawLines(const std::vector<Vec3>& segments, u32 colorAbgr);   // pairs of points, drawn on top
    // Translucent particle quads: depth tested but not written; texture null = a soft round dot. Draw after the opaque scene.
    void DrawSprites(const SpriteVertex* verts, u32 count, const GpuTexture* texture, bool additive);
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
    ComPtr<ID3D11DepthStencilState> m_depth, m_noDepth;
    ComPtr<ID3D11Buffer> m_lineVb;
    u32 m_lineCap = 0;
    ComPtr<ID3D11VertexShader> m_spriteVs;
    ComPtr<ID3D11PixelShader> m_spritePs;
    ComPtr<ID3D11InputLayout> m_spriteLayout;
    ComPtr<ID3D11Buffer> m_spriteVb;
    u32 m_spriteCap = 0;
    ComPtr<ID3D11BlendState> m_opaqueBlend, m_lineBlend, m_addBlend, m_modBlend;
    ComPtr<ID3D11DepthStencilState> m_depthNoWrite;
    ComPtr<ID3D11Buffer> m_graphCb;
    double m_startTime = 0;
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
