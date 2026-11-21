#include "render/Renderer.h"
#include <d3dcompiler.h>
#include <chrono>

static const char* kMeshShader = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) {
    float4x4 ViewProj;
    float4 EyePos;
    float4 LightDir;
    float4 Flags;   // x textures, y normal maps, z vertex colours
    float4 SunColor;    // rgb, w > 0: the map's sun replaces the key light
    float4 Ambient;     // rgb scale of the hemisphere ambient
    float4 FogColor;    // rgb inscattering, w density (already / 1000)
    float4 FogParams;   // x height falloff (/ 1000), y fog height, z start distance, w max opacity (0 = no fog)
    float4 LightCount;
    float4 Lights[96];  // per light: (pos, radius) (colour, cos inner or -2 = point) (direction, cos outer)
};
cbuffer Material : register(b1) {
    float4 BaseColor;
    float4 MatFlags;   // x has diffuse, y has normal, z masked
};
Texture2D DiffuseTex : register(t0);
Texture2D NormalTex : register(t1);
SamplerState Samp : register(s0);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float4 tan : TANGENT; float2 uv : TEXCOORD0; float4 col : COLOR0;
    float4 r0 : WORLD0; float4 r1 : WORLD1; float4 r2 : WORLD2; float4 r3 : WORLD3;
};
struct PSIn {
    float4 pos : SV_Position; float3 wpos : WPOS; float3 nrm : NORMAL; float4 tan : TANGENT; float2 uv : TEXCOORD0; float4 col : COLOR0;
};
PSIn VSMain(VSIn i) {
    float4x4 W = float4x4(i.r0, i.r1, i.r2, i.r3);
    PSIn o;
    float4 wp = mul(float4(i.pos, 1), W);
    o.wpos = wp.xyz;
    o.pos = mul(wp, ViewProj);
    o.nrm = mul(float4(i.nrm, 0), W).xyz;
    o.tan = float4(mul(float4(i.tan.xyz, 0), W).xyz, i.tan.w);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}
float4 PSMain(PSIn i, bool front : SV_IsFrontFace) : SV_Target {
    float4 base = BaseColor;
    if (Flags.x > 0 && MatFlags.x > 0) {
        float4 t = DiffuseTex.Sample(Samp, i.uv);
        base.rgb = t.rgb;
        if (MatFlags.z > 0) { clip(t.a - 0.33); }
    }
    if (Flags.z > 0) base.rgb *= i.col.rgb;
    else if (MatFlags.w > 0) base.rgb *= pow(abs(i.col.rgb), 2.2);
    float3 n = normalize(i.nrm);
    if (Flags.y > 0 && MatFlags.y > 0 && dot(i.tan.xyz, i.tan.xyz) > 1e-6) {
        float3 t = normalize(i.tan.xyz - n * dot(n, i.tan.xyz));
        float3 b = cross(n, t) * i.tan.w;
        float2 xy = NormalTex.Sample(Samp, i.uv).rg * 2 - 1;
        float3 tn = float3(xy, sqrt(saturate(1 - dot(xy, xy))));
        n = normalize(tn.x * t + tn.y * b + tn.z * n);
    }
    if (!front) n = -n;
    float3 v = normalize(EyePos.xyz - i.wpos);
    float3 l = normalize(-LightDir.xyz);
    float ndl = saturate(dot(n, l));
    float hemi = 0.5 + 0.5 * n.z;
    float3 ambient = lerp(float3(0.16, 0.15, 0.14), float3(0.34, 0.37, 0.42), hemi) * Ambient.rgb;
    float3 h = normalize(l + v);
    float3 sun = SunColor.w > 0 ? SunColor.rgb : float3(0.95, 0.92, 0.85);
    float spec = pow(saturate(dot(n, h)), 32) * 0.12;
    float3 c = base.rgb * (ambient + ndl * sun) + spec * sun;
    for (int k = 0; k < (int)LightCount.x; k++) {
        float4 a = Lights[k * 3], b = Lights[k * 3 + 1], d = Lights[k * 3 + 2];
        float3 L = a.xyz - i.wpos;
        float dist = length(L);
        if (dist >= a.w) continue;
        L /= max(dist, 1e-3);
        float win = saturate(1 - pow(dist / a.w, 4));
        float att = win * win / (1 + dist * dist * 0.0001);
        if (b.w > -1.5) att *= saturate((dot(-L, d.xyz) - d.w) / max(b.w - d.w, 1e-3));
        c += base.rgb * b.rgb * saturate(dot(n, L)) * att;
    }
    float rim = pow(1 - saturate(dot(n, v)), 4) * 0.08;
    c += rim;
    if (FogParams.w > 0) {   // UE 4.10 exponential height fog
        float3 ray = i.wpos - EyePos.xyz;
        float len = length(ray);
        float f0 = FogColor.w * exp2(-FogParams.x * (EyePos.z - FogParams.y));
        float fx = max(-127, FogParams.x * ray.z);
        float lineInt = abs(fx) > 0.01 ? (1 - exp2(-fx)) / fx : 0.6931 - 0.2402 * fx;
        float amount = f0 * lineInt * max(len - FogParams.z, 0);
        float fog = min(1 - saturate(exp2(-amount)), FogParams.w);
        c = lerp(c, FogColor.rgb, fog);
    }
    return float4(c, 1);
}
)";

static const char* kLineShader = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) { float4x4 ViewProj; float4 EyePos; float4 LightDir; float4 Flags; };
cbuffer Line : register(b1) { float4 Scale; };
struct VSIn { float3 pos : POSITION; float4 col : COLOR0; };
struct PSIn { float4 pos : SV_Position; float4 col : COLOR0; };
PSIn VSMain(VSIn i) {
    PSIn o;
    float3 p = i.pos * Scale.x + Scale.yzw;
    o.pos = mul(float4(p, 1), ViewProj);
    o.col = i.col;
    return o;
}
float4 PSMain(PSIn i) : SV_Target { return i.col; }
)";

struct MeshVertex {
    float pos[3], nrm[3], tan[4], uv[2];
    u32 color;
};
struct LineVertex {
    float pos[3];
    u32 color;
};
struct FrameConstants {
    Mat4 viewProj;
    float eye[4], light[4], flags[4];
    float sun[4], ambient[4], fogColor[4], fogParams[4], lightCount[4];
    float lights[kMaxSceneLights * 3][4];
};
struct MaterialConstants {
    float color[4];
    float flags[4];
};

static bool Compile(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string* error) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr)) {
        if (error) *error = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : Fmt("D3DCompile 0x%08x", (unsigned)hr);
        return false;
    }
    return true;
}

Vec3 Camera::Forward() const {
    float y = yaw * kPi / 180, p = pitch * kPi / 180;
    return {cosf(p) * cosf(y), cosf(p) * sinf(y), sinf(p)};
}

void Camera::Frame(const Vec3& mn, const Vec3& mx) {
    target = (mn + mx) * 0.5f;
    float r = std::max(1.f, Length(mx - mn) * 0.5f);
    distance = r / tanf(fovY * kPi / 360) * 1.1f;
    nearZ = std::max(0.5f, r * 0.002f);
    flySpeed = std::max(200.f, r * 2);
}

bool Renderer::Init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* error) {
    m_dev = device;
    m_ctx = ctx;
    ComPtr<ID3DBlob> vs, ps, lvs, lps;
    if (!Compile(kMeshShader, "VSMain", "vs_5_0", vs, error) || !Compile(kMeshShader, "PSMain", "ps_5_0", ps, error) ||
        !Compile(kLineShader, "VSMain", "vs_5_0", lvs, error) || !Compile(kLineShader, "PSMain", "ps_5_0", lps, error))
        return false;
    m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_meshVs);
    m_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_meshPs);
    m_dev->CreateVertexShader(lvs->GetBufferPointer(), lvs->GetBufferSize(), nullptr, &m_lineVs);
    m_dev->CreatePixelShader(lps->GetBufferPointer(), lps->GetBufferSize(), nullptr, &m_linePs);

    D3D11_INPUT_ELEMENT_DESC meshEl[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"WORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"WORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"WORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"WORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    if (FAILED(m_dev->CreateInputLayout(meshEl, 9, vs->GetBufferPointer(), vs->GetBufferSize(), &m_meshLayout))) { if (error) *error = "mesh input layout"; return false; }
    D3D11_INPUT_ELEMENT_DESC lineEl[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(m_dev->CreateInputLayout(lineEl, 2, lvs->GetBufferPointer(), lvs->GetBufferSize(), &m_lineLayout))) { if (error) *error = "line input layout"; return false; }

    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = sizeof(FrameConstants);
    m_dev->CreateBuffer(&cb, nullptr, &m_frameCb);
    cb.ByteWidth = sizeof(MaterialConstants);
    m_dev->CreateBuffer(&cb, nullptr, &m_materialCb);
    cb.ByteWidth = 32;
    m_dev->CreateBuffer(&cb, nullptr, &m_graphCb);
    m_startTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_dev->CreateSamplerState(&sd, &m_sampler);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = TRUE;   // UE front faces wind counter-clockwise on screen
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    rd.AntialiasedLineEnable = TRUE;
    m_dev->CreateRasterizerState(&rd, &m_rsSolid[0]);
    rd.CullMode = D3D11_CULL_NONE;
    m_dev->CreateRasterizerState(&rd, &m_rsSolid[1]);
    rd.FillMode = D3D11_FILL_WIREFRAME;
    m_dev->CreateRasterizerState(&rd, &m_rsWire);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;   // reversed Z
    m_dev->CreateDepthStencilState(&dd, &m_depth);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m_dev->CreateDepthStencilState(&dd, &m_depthNoWrite);
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m_dev->CreateDepthStencilState(&dd, &m_noDepth);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_dev->CreateBlendState(&bd, &m_opaqueBlend);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    m_dev->CreateBlendState(&bd, &m_lineBlend);
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    m_dev->CreateBlendState(&bd, &m_addBlend);
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    m_dev->CreateBlendState(&bd, &m_modBlend);

    UINT q = 0;
    if (FAILED(m_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, &q)) || !q) m_samples = 1;

    Image white;
    white.width = white.height = 1;
    white.rgba = {255, 255, 255, 255};
    m_white = CreateTexture(white, false, false);
    white.rgba = {128, 128, 255, 255};
    m_flatNormal = CreateTexture(white, false, false);
    EnsureInstanceCapacity(1024);
    return true;
}

std::vector<Image> BuildMips(const Image& img) {
    std::vector<Image> mips;
    mips.push_back(img);
    while (mips.back().width > 1 || mips.back().height > 1) {
        const Image& s = mips.back();
        Image d;
        d.width = std::max(1, s.width / 2);
        d.height = std::max(1, s.height / 2);
        d.rgba.resize((size_t)d.width * d.height * 4);
        for (int y = 0; y < d.height; y++)
            for (int x = 0; x < d.width; x++) {
                int x0 = std::min(x * 2, s.width - 1), x1 = std::min(x * 2 + 1, s.width - 1);
                int y0 = std::min(y * 2, s.height - 1), y1 = std::min(y * 2 + 1, s.height - 1);
                for (int c = 0; c < 4; c++) {
                    int v = s.rgba[((size_t)y0 * s.width + x0) * 4 + c] + s.rgba[((size_t)y0 * s.width + x1) * 4 + c] +
                            s.rgba[((size_t)y1 * s.width + x0) * 4 + c] + s.rgba[((size_t)y1 * s.width + x1) * 4 + c];
                    d.rgba[((size_t)y * d.width + x) * 4 + c] = (u8)((v + 2) / 4);
                }
            }
        mips.push_back(std::move(d));
    }
    return mips;
}

std::shared_ptr<GpuTexture> Renderer::CreateTexture(const Image& img, bool srgb, bool mips) const {
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() < (size_t)img.width * img.height * 4) return nullptr;
    std::vector<Image> levels;
    if (mips) levels = BuildMips(img);
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    if (mips)
        for (auto& l : levels) init.push_back({l.rgba.data(), (UINT)l.width * 4, 0});
    else
        init.push_back({img.rgba.data(), (UINT)img.width * 4, 0});
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = img.width;
    td.Height = img.height;
    td.MipLevels = (UINT)init.size();
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_dev->CreateTexture2D(&td, init.data(), &tex))) return nullptr;
    auto t = std::make_shared<GpuTexture>();
    t->width = img.width;
    t->height = img.height;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = td.MipLevels;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_dev->CreateShaderResourceView(tex.Get(), &sv, &t->srvRaw);
    if (srgb) {
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        m_dev->CreateShaderResourceView(tex.Get(), &sv, &t->srv);
    } else {
        t->srv = t->srvRaw;
    }
    return t;
}

std::shared_ptr<GpuMesh> Renderer::CreateMesh(const MeshLod& lod, const Vec3& bmin, const Vec3& bmax, bool dynamic) const {
    u32 n = lod.VertexCount();
    if (!n || lod.indices.empty()) return nullptr;
    std::vector<MeshVertex> v(n);
    for (u32 i = 0; i < n; i++) {
        auto& o = v[i];
        o.pos[0] = lod.positions[i].x; o.pos[1] = lod.positions[i].y; o.pos[2] = lod.positions[i].z;
        Vec3 nr = i < lod.normals.size() ? lod.normals[i] : Vec3{0, 0, 1};
        o.nrm[0] = nr.x; o.nrm[1] = nr.y; o.nrm[2] = nr.z;
        Vec4 t = i < lod.tangents.size() ? lod.tangents[i] : Vec4{};
        o.tan[0] = t.x; o.tan[1] = t.y; o.tan[2] = t.z; o.tan[3] = t.w;
        if (lod.numUVs) { o.uv[0] = lod.uvs[(size_t)i * lod.numUVs].x; o.uv[1] = lod.uvs[(size_t)i * lod.numUVs].y; }
        else o.uv[0] = o.uv[1] = 0;
        o.color = i < lod.colors.size() ? lod.colors[i] : 0xffffffffu;
    }
    auto m = std::make_shared<GpuMesh>();
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
    bd.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(v.size() * sizeof(MeshVertex));
    D3D11_SUBRESOURCE_DATA sd = {v.data()};
    if (FAILED(m_dev->CreateBuffer(&bd, &sd, &m->vb))) return nullptr;
    if (dynamic) {
        m->cpuVertices.resize(v.size() * sizeof(MeshVertex));
        memcpy(m->cpuVertices.data(), v.data(), m->cpuVertices.size());
    }
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.CPUAccessFlags = 0;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(lod.indices.size() * 4);
    sd.pSysMem = lod.indices.data();
    if (FAILED(m_dev->CreateBuffer(&bd, &sd, &m->ib))) return nullptr;
    for (auto& s : lod.sections) m->sections.push_back({s.firstIndex, s.numTriangles * 3, s.materialIndex});
    if (m->sections.empty()) m->sections.push_back({0, (u32)lod.indices.size(), 0});
    m->boundsMin = bmin;
    m->boundsMax = bmax;
    m->center = (bmin + bmax) * 0.5f;
    m->radius = Length(bmax - bmin) * 0.5f;
    m->vertexCount = n;
    m->triangleCount = (u32)(lod.indices.size() / 3);
    m->gpuBytes = v.size() * sizeof(MeshVertex) + lod.indices.size() * 4;
    return m;
}

void Renderer::EnsureInstanceCapacity(u32 count) {
    if (count <= m_instanceCap) return;
    u32 cap = std::max(count, m_instanceCap * 2);
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = cap * sizeof(Mat4);
    m_instanceVb.Reset();
    if (SUCCEEDED(m_dev->CreateBuffer(&bd, nullptr, &m_instanceVb))) m_instanceCap = cap;
}

void Renderer::EnsureTargets(int w, int h) {
    if (w == m_w && h == m_h && m_msaaRtv) return;
    m_w = w;
    m_h = h;
    m_msaaColor.Reset(); m_out.Reset(); m_msaaDepth.Reset(); m_msaaRtv.Reset(); m_dsv.Reset(); m_outSrv.Reset();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = m_samples;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    m_dev->CreateTexture2D(&td, nullptr, &m_msaaColor);
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rv.ViewDimension = m_samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    m_dev->CreateRenderTargetView(m_msaaColor.Get(), &rv, &m_msaaRtv);
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    m_dev->CreateTexture2D(&td, nullptr, &m_out);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // shown gamma-encoded by ImGui
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    m_dev->CreateShaderResourceView(m_out.Get(), &sv, &m_outSrv);
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = m_samples;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    m_dev->CreateTexture2D(&td, nullptr, &m_msaaDepth);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = m_samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    m_dev->CreateDepthStencilView(m_msaaDepth.Get(), &dv, &m_dsv);
}

void Renderer::BeginFrame(int width, int height, const Camera& cam, const DrawOptions& opt) {
    EnsureTargets(std::max(1, width), std::max(1, height));
    m_opt = opt;
    m_drawnTris = 0;
    float clear[4] = {0.011f, 0.012f, 0.016f, 1};   // linear (the target is sRGB)
    if (opt.lighting && opt.lighting->fog) {         // the sky behind fogged geometry
        float o = std::min(1.f, opt.lighting->fogMaxOpacity);
        clear[0] = clear[0] * (1 - o) + opt.lighting->fogColor.x * o;
        clear[1] = clear[1] * (1 - o) + opt.lighting->fogColor.y * o;
        clear[2] = clear[2] * (1 - o) + opt.lighting->fogColor.z * o;
    }
    m_ctx->ClearRenderTargetView(m_msaaRtv.Get(), clear);
    m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    ID3D11RenderTargetView* rtv = m_msaaRtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, m_dsv.Get());
    D3D11_VIEWPORT vp = {0, 0, (float)m_w, (float)m_h, 0, 1};
    m_ctx->RSSetViewports(1, &vp);

    Vec3 eye = cam.Eye();
    Mat4 view = Mat4::LookAt(eye, cam.target, {0, 0, 1});
    Mat4 proj = Mat4::PerspectiveReversed(cam.fovY * kPi / 180, (float)m_w / m_h, cam.nearZ);
    m_viewProj = view * proj;
    m_frustum.FromViewProj(m_viewProj);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(m_ctx->Map(m_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        FrameConstants fc{};
        fc.viewProj = m_viewProj;
        fc.eye[0] = eye.x; fc.eye[1] = eye.y; fc.eye[2] = eye.z; fc.eye[3] = 1;
        // key light from over the camera's shoulder
        Vec3 f = cam.Forward();
        Vec3 l = Normalize(Vec3{f.x * 0.6f + f.y * 0.4f, f.y * 0.6f - f.x * 0.4f, -0.8f});
        fc.light[0] = l.x; fc.light[1] = l.y; fc.light[2] = l.z; fc.light[3] = 0;
        fc.flags[0] = opt.textures ? 1.f : 0.f;
        fc.flags[1] = opt.normalMaps ? 1.f : 0.f;
        fc.flags[2] = opt.vertexColors ? 1.f : 0.f;
        fc.flags[3] = 0;
        fc.ambient[0] = fc.ambient[1] = fc.ambient[2] = 1;
        if (const SceneLighting* sl = opt.lighting) {
            if (sl->sun) {
                Vec3 d = Normalize(sl->sunDir);
                fc.light[0] = d.x; fc.light[1] = d.y; fc.light[2] = d.z;
                fc.sun[0] = sl->sunColor.x; fc.sun[1] = sl->sunColor.y; fc.sun[2] = sl->sunColor.z; fc.sun[3] = 1;
            }
            fc.ambient[0] = sl->ambientScale.x; fc.ambient[1] = sl->ambientScale.y; fc.ambient[2] = sl->ambientScale.z;
            if (sl->fog) {
                fc.fogColor[0] = sl->fogColor.x; fc.fogColor[1] = sl->fogColor.y; fc.fogColor[2] = sl->fogColor.z;
                fc.fogColor[3] = sl->fogDensity / 1000;
                fc.fogParams[0] = sl->fogFalloff / 1000;
                fc.fogParams[1] = sl->fogHeight;
                fc.fogParams[2] = sl->fogStart;
                fc.fogParams[3] = std::max(1e-4f, sl->fogMaxOpacity);
            }
            int nl = (int)std::min<size_t>(sl->lights.size(), kMaxSceneLights);
            fc.lightCount[0] = (float)nl;
            for (int k = 0; k < nl; k++) {
                const SceneLight& L = sl->lights[k];
                float* a = fc.lights[k * 3];
                float* b = fc.lights[k * 3 + 1];
                float* d = fc.lights[k * 3 + 2];
                a[0] = L.position.x; a[1] = L.position.y; a[2] = L.position.z; a[3] = L.radius;
                b[0] = L.color.x; b[1] = L.color.y; b[2] = L.color.z; b[3] = L.spot ? L.cosInner : -2.f;
                Vec3 dir = Normalize(L.direction);
                d[0] = dir.x; d[1] = dir.y; d[2] = dir.z; d[3] = L.cosOuter;
            }
        }
        memcpy(ms.pData, &fc, sizeof(fc));
        m_ctx->Unmap(m_frameCb.Get(), 0);
    }
    if (SUCCEEDED(m_ctx->Map(m_graphCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        float g[8] = {opt.time >= 0 ? opt.time : (float)(now - m_startTime), 0, 0, 0, (float)m_w, (float)m_h, 1.f / m_w, 1.f / m_h};
        memcpy(ms.pData, g, sizeof(g));
        m_ctx->Unmap(m_graphCb.Get(), 0);
    }
    m_ctx->OMSetDepthStencilState(m_depth.Get(), 0);
    m_ctx->OMSetBlendState(m_opaqueBlend.Get(), nullptr, 0xffffffff);
    ID3D11SamplerState* s = m_sampler.Get();
    m_ctx->PSSetSamplers(0, 1, &s);
}

bool Renderer::CreatePixelShader(const std::string& source, const char* entry, ComPtr<ID3D11PixelShader>& out, std::string* error) const {
    ComPtr<ID3DBlob> blob;
    if (!Compile(source.c_str(), entry, "ps_5_0", blob, error)) return false;
    return SUCCEEDED(m_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
}

void Renderer::DrawMesh(const GpuMesh& mesh, const Mat4* instances, u32 count) {
    if (!count || !mesh.vb) return;
    EnsureInstanceCapacity(count);
    if (count > m_instanceCap) count = m_instanceCap;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(m_ctx->Map(m_instanceVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, instances, count * sizeof(Mat4));
    m_ctx->Unmap(m_instanceVb.Get(), 0);

    m_ctx->IASetInputLayout(m_meshLayout.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* vbs[2] = {mesh.vb.Get(), m_instanceVb.Get()};
    UINT strides[2] = {sizeof(MeshVertex), sizeof(Mat4)}, offs[2] = {0, 0};
    m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
    m_ctx->IASetIndexBuffer(mesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->VSSetShader(m_meshVs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_meshPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[3] = {m_frameCb.Get(), m_materialCb.Get(), m_graphCb.Get()};
    m_ctx->VSSetConstantBuffers(0, 2, cbs);
    m_ctx->PSSetConstantBuffers(0, 3, cbs);

    for (auto& s : mesh.sections) {
        if (!s.indexCount) continue;
        const GpuMaterial* mat = s.material >= 0 && s.material < (int)mesh.materials.size() ? mesh.materials[s.material].get() : nullptr;
        MaterialConstants mc = {};
        static const GpuMaterial kDefault;
        if (!mat) mat = &kDefault;
        memcpy(mc.color, mat->color, sizeof(mc.color));
        mc.flags[0] = mat->diffuse ? 1.f : 0.f;
        mc.flags[1] = mat->normal ? 1.f : 0.f;
        mc.flags[2] = mat->masked ? 1.f : 0.f;
        mc.flags[3] = mat->vertexColor ? 1.f : 0.f;
        if (SUCCEEDED(m_ctx->Map(m_materialCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            memcpy(ms.pData, &mc, sizeof(mc));
            m_ctx->Unmap(m_materialCb.Get(), 0);
        }
        const GpuGraphShader* g = m_opt.graphMaterials && mat->graph && mat->graph->ps ? mat->graph.get() : nullptr;
        if (g) {
            ID3D11ShaderResourceView* srvs[32] = {};
            for (size_t k = 0; k < g->textures.size() && k < 31; k++) srvs[k] = g->textures[k] ? g->textures[k]->srv.Get() : nullptr;
            srvs[31] = m_white->srv.Get();
            m_ctx->PSSetShaderResources(0, 32, srvs);
            m_ctx->PSSetShader(g->ps.Get(), nullptr, 0);
            ID3D11BlendState* bs = g->blend == 2 ? m_lineBlend.Get() : g->blend == 3 ? m_addBlend.Get() : g->blend == 4 ? m_modBlend.Get() : m_opaqueBlend.Get();
            m_ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
            m_ctx->OMSetDepthStencilState(g->blend >= 2 ? m_depthNoWrite.Get() : m_depth.Get(), 0);
        } else {
            ID3D11ShaderResourceView* srvs[2] = {(mat->diffuse ? mat->diffuse : m_white)->srv.Get(), (mat->normal ? mat->normal : m_flatNormal)->srv.Get()};
            m_ctx->PSSetShaderResources(0, 2, srvs);
            m_ctx->PSSetShader(m_meshPs.Get(), nullptr, 0);
        }
        bool noCull = mat->twoSided || mat->masked || !m_opt.backfaceCulling || (g && g->blend >= 2);
        m_ctx->RSSetState(m_opt.wireframe ? m_rsWire.Get() : m_rsSolid[noCull ? 1 : 0].Get());
        m_ctx->DrawIndexedInstanced(s.indexCount, count, s.firstIndex, 0, 0);
        if (g) {
            m_ctx->OMSetBlendState(m_opaqueBlend.Get(), nullptr, 0xffffffff);
            m_ctx->OMSetDepthStencilState(m_depth.Get(), 0);
        }
        m_drawnTris += (u64)s.indexCount / 3 * count;
    }
}

void Renderer::DrawGrid(float cellSize, int cells) {
    if (cells != m_gridCells || !m_gridVb) {
        std::vector<LineVertex> v;
        for (int i = -cells; i <= cells; i++) {
            u32 c = i == 0 ? 0x90ffffffu : (i % 10 == 0 ? 0x50ffffffu : 0x26ffffffu);
            u32 cx = i == 0 ? 0xc04040e0u : c, cy = i == 0 ? 0xc040e040u : c;   // ABGR: X axis red, Y axis green
            v.push_back({{(float)i, (float)-cells, 0}, cy});
            v.push_back({{(float)i, (float)cells, 0}, cy});
            v.push_back({{(float)-cells, (float)i, 0}, cx});
            v.push_back({{(float)cells, (float)i, 0}, cx});
        }
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.ByteWidth = (UINT)(v.size() * sizeof(LineVertex));
        D3D11_SUBRESOURCE_DATA sd = {v.data()};
        m_gridVb.Reset();
        if (FAILED(m_dev->CreateBuffer(&bd, &sd, &m_gridVb))) return;
        m_gridVerts = (u32)v.size();
        m_gridCells = cells;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    float scale[4] = {cellSize, 0, 0, -cellSize * 0.02f};   // slightly below z=0 to avoid z-fighting with mesh bases
    if (FAILED(m_ctx->Map(m_materialCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, scale, sizeof(scale));
    m_ctx->Unmap(m_materialCb.Get(), 0);
    m_ctx->IASetInputLayout(m_lineLayout.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D11Buffer* vb = m_gridVb.Get();
    UINT stride = sizeof(LineVertex), off = 0;
    m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    m_ctx->VSSetShader(m_lineVs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_linePs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {m_frameCb.Get(), m_materialCb.Get()};
    m_ctx->VSSetConstantBuffers(0, 2, cbs);
    m_ctx->RSSetState(m_rsSolid[1].Get());
    m_ctx->OMSetBlendState(m_lineBlend.Get(), nullptr, 0xffffffff);
    m_ctx->Draw(m_gridVerts, 0);
    m_ctx->OMSetBlendState(m_opaqueBlend.Get(), nullptr, 0xffffffff);
}

void Renderer::UpdateMeshVertices(GpuMesh& mesh, const std::vector<Vec3>& pos, const std::vector<Vec3>& nrm) {
    if (mesh.cpuVertices.empty() || pos.size() != mesh.vertexCount) return;
    MeshVertex* v = (MeshVertex*)mesh.cpuVertices.data();
    for (u32 i = 0; i < mesh.vertexCount; i++) {
        v[i].pos[0] = pos[i].x; v[i].pos[1] = pos[i].y; v[i].pos[2] = pos[i].z;
        if (i < nrm.size()) { v[i].nrm[0] = nrm[i].x; v[i].nrm[1] = nrm[i].y; v[i].nrm[2] = nrm[i].z; }
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(m_ctx->Map(mesh.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, mesh.cpuVertices.data(), mesh.cpuVertices.size());
        m_ctx->Unmap(mesh.vb.Get(), 0);
    }
}

void Renderer::DrawLines(const std::vector<Vec3>& segments, u32 color) {
    if (segments.size() < 2) return;
    u32 n = (u32)(segments.size() & ~(size_t)1);
    if (n > m_lineCap) {
        m_lineVb.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = std::max(n, 1024u) * sizeof(LineVertex);
        if (FAILED(m_dev->CreateBuffer(&bd, nullptr, &m_lineVb))) return;
        m_lineCap = std::max(n, 1024u);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(m_ctx->Map(m_lineVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    LineVertex* lv = (LineVertex*)ms.pData;
    for (u32 i = 0; i < n; i++) lv[i] = {{segments[i].x, segments[i].y, segments[i].z}, color};
    m_ctx->Unmap(m_lineVb.Get(), 0);
    float scale[4] = {1, 0, 0, 0};
    if (FAILED(m_ctx->Map(m_materialCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, scale, sizeof(scale));
    m_ctx->Unmap(m_materialCb.Get(), 0);
    m_ctx->IASetInputLayout(m_lineLayout.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D11Buffer* vb = m_lineVb.Get();
    UINT stride = sizeof(LineVertex), off = 0;
    m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    m_ctx->VSSetShader(m_lineVs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_linePs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {m_frameCb.Get(), m_materialCb.Get()};
    m_ctx->VSSetConstantBuffers(0, 2, cbs);
    m_ctx->RSSetState(m_rsSolid[1].Get());
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);   // bones are drawn on top of the mesh
    m_ctx->Draw(n, 0);
    m_ctx->OMSetDepthStencilState(m_depth.Get(), 0);
}

bool Renderer::ReadOutput(Image& out) {
    if (!m_out) return false;
    D3D11_TEXTURE2D_DESC td;
    m_out->GetDesc(&td);
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    m_ctx->CopyResource(staging.Get(), m_out.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
    out.width = td.Width;
    out.height = td.Height;
    out.rgba.resize((size_t)td.Width * td.Height * 4);
    for (UINT y = 0; y < td.Height; y++) memcpy(&out.rgba[(size_t)y * td.Width * 4], (u8*)ms.pData + (size_t)y * ms.RowPitch, td.Width * 4);
    m_ctx->Unmap(staging.Get(), 0);
    return true;
}

void Renderer::EndFrame() {
    if (m_samples > 1) m_ctx->ResolveSubresource(m_out.Get(), 0, m_msaaColor.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    else m_ctx->CopyResource(m_out.Get(), m_msaaColor.Get());
    ID3D11RenderTargetView* none = nullptr;
    m_ctx->OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* nulls[2] = {};
    m_ctx->PSSetShaderResources(0, 2, nulls);
}
