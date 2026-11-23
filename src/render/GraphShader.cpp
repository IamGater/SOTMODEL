#include "render/GraphShader.h"
#include <regex>
#include <sstream>
#include <cstring>

namespace {

// The UE 4.10 material shader environment, reduced to what the translated graphs reference.
const char* kPrelude = R"HLSL(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) {
    float4x4 ViewProj;
    float4 EyePos;
    float4 LightDir;
    float4 Flags;
    float4 SunColor;
    float4 FrameAmbient;
    float4 FogColor;
    float4 FogParams;
    float4 LightCount;
    float4 Lights[96];
};
cbuffer GraphFrame : register(b2) {
    float4 GraphTime;   // x game time (s)
    float4 ViewSize;    // w, h, 1/w, 1/h
    float4x4 GraphInvViewProj;
    float4 SceneParams;   // backdrop: x floor z, y far distance, z drawn (GRAPH_BACKDROP_HLSL)
};
)HLSL" GRAPH_BACKDROP_HLSL R"HLSL(
SamplerState MatSampler : register(s0);
Texture2D StubTexture : register(t31);

struct FViewStub {
    float GameTime, RealTime, DeltaTime, MaterialTextureMipBias;
    float4 ViewSizeAndInvSize, BufferSizeAndInvSize, ViewRectMin;
    float3 WorldCameraOrigin, WorldViewOrigin, PreViewTranslation, ViewForward, ViewOrigin, DirectionalLightDirection, DirectionalLightColor;
    float4x4 ViewToClip, ClipToTranslatedWorld, TranslatedWorldToClip, WorldToClip, TranslatedWorldToView, ViewToTranslatedWorld, ScreenToWorld, ClipToView;
    float4 ScreenPositionScaleBias, TemporalAAParams, ExposureScale;
    float2 RenderTargetSize;
    float4 DiffuseOverrideParameter, SpecularOverrideParameter;
    float UnlitViewmodeMask, OutOfBoundsMask;
    float4 UserClipPlane, InvDeviceZToWorldZTransform;
    float3 PrevViewOrigin, PrevPreViewTranslation, PrevWorldCameraOrigin;
    float4x4 PrevTranslatedWorldToClip, PrevViewToClip, PrevClipToView;
};
static FViewStub View;
#define ResolvedView View
#define MaterialFloat float
#define MaterialFloat2 float2
#define MaterialFloat3 float3
#define MaterialFloat4 float4
#define MaterialFloat3x3 float3x3
#define MaterialFloat4x4 float4x4
struct FPrimitiveStub {
    float4x4 LocalToWorld, WorldToLocal;
    float4 ObjectWorldPositionAndRadius;
    float3 ObjectBounds, ActorWorldPosition, LocalObjectBoundsMin, LocalObjectBoundsMax;
    float DecalReceiverMask, PerObjectGBufferData, UseSingleSampleShadowFromStationaryLights;
};
static FPrimitiveStub Primitive;
FPrimitiveStub GetPrimitiveData() { return Primitive; }

struct FMaterialParticleParameters {
    float4 Color;
    float4 SubUVCoords[2];
    float SubUVLerp, RelativeTime, MotionBlurFade, Random;
    float4 PositionAndSize, DynamicParameter, MacroUV, VelocityAndSpeed;
    float2 Size;
    float3 Velocity;
    float4x4 LocalToWorld;
};
struct FMaterialPixelParameters {
    float2 TexCoords[8];
    float4 VertexColor;
    float3 WorldNormal, ReflectionVector, CameraVector, LightVector;
    float4 SvPosition, ScreenPosition;
    float UnMirrored, TwoSidedSign;
    float3x3 TangentToWorld;
    float3 AbsoluteWorldPosition, WorldPosition_CamRelative, WorldPosition_NoOffsets, WorldPosition_NoOffsets_CamRelative, LightingPositionOffset;
    float3 PreSkinnedPosition, PreSkinnedNormal;
    float4 LayerWeights;
    float PerInstanceRandom, PerInstanceFadeAmount;
    FMaterialParticleParameters Particle;
};
struct FMaterialVertexParameters {
    float3 WorldPosition, PreSkinnedPosition, PreSkinnedNormal;
    float3x3 TangentToWorld;
    float4 VertexColor;
    float2 TexCoords[8];
    float PerInstanceRandom;
    FMaterialParticleParameters Particle;
};
struct FPixelMaterialInputs {
    float3 EmissiveColor, BaseColor, Normal, SubsurfaceColor;
    float Opacity, OpacityMask, Metallic, Specular, Roughness, AmbientOcclusion, PixelDepthOffset, ClearCoat, ClearCoatRoughness;
    float2 Refraction;
};
struct FMaterialAttributes {
    float3 BaseColor, EmissiveColor, Normal, WorldPositionOffset, WorldDisplacement, SubsurfaceColor;
    float Metallic, Specular, Roughness, Opacity, OpacityMask, AmbientOcclusion, PixelDepthOffset, TessellationMultiplier, ClearCoat, ClearCoatRoughness;
    float2 Refraction;
    float2 CustomizedUVs0, CustomizedUVs1, CustomizedUVs2, CustomizedUVs3, CustomizedUVs4, CustomizedUVs5, CustomizedUVs6, CustomizedUVs7;
};

float4 Texture2DSample(Texture2D t, SamplerState s, float2 uv) { return t.Sample(s, uv); }
float4 Texture2DSampleLevel(Texture2D t, SamplerState s, float2 uv, float l) { return t.SampleLevel(s, uv, l); }
float4 Texture2DSampleBias(Texture2D t, SamplerState s, float2 uv, float b) { return t.SampleBias(s, uv, b); }
float4 Texture2DSampleGrad(Texture2D t, SamplerState s, float2 uv, float2 dx, float2 dy) { return t.SampleGrad(s, uv, dx, dy); }
float4 TextureCubeSample(TextureCube t, SamplerState s, float3 uv) { return t.Sample(s, uv); }
float4 TextureCubeSampleLevel(TextureCube t, SamplerState s, float3 uv, float l) { return t.SampleLevel(s, uv, l); }
float4 ProcessMaterialColorTextureLookup(float4 c) { return c; }
float4 ProcessMaterialLinearColorTextureLookup(float4 c) { return c; }
float ProcessMaterialGreyscaleTextureLookup(float c) { return c; }
float4 ProcessMaterialGreyscaleTextureLookup(float4 c) { return c; }
float4 ProcessMaterialLinearGreyscaleTextureLookup(float4 c) { return c; }
float4 UnpackNormalMap(float4 t) {
    float2 xy = t.rg * 2 - 1;
    return float4(xy, sqrt(saturate(1 - dot(xy, xy))), 1);
}
float ClampedPow(float x, float e) { return pow(max(abs(x), 1e-6), e); }
float2 ClampedPow(float2 x, float2 e) { return pow(max(abs(x), 1e-6), e); }
float3 ClampedPow(float3 x, float3 e) { return pow(max(abs(x), 1e-6), e); }
float4 ClampedPow(float4 x, float4 e) { return pow(max(abs(x), 1e-6), e); }
float PositiveClampedPow(float x, float e) { return pow(max(x, 1e-6), e); }
float3 GetWorldPosition(FMaterialPixelParameters P) { return P.AbsoluteWorldPosition; }
float3 GetWorldPosition(FMaterialVertexParameters P) { return P.WorldPosition; }
float3 GetWorldPosition_NoMaterialOffsets(FMaterialPixelParameters P) { return P.AbsoluteWorldPosition; }
float3 GetWorldPosition_NoMaterialOffsets(FMaterialVertexParameters P) { return P.WorldPosition; }
float3 GetObjectWorldPosition(FMaterialPixelParameters P) { return Primitive.ObjectWorldPositionAndRadius.xyz; }
float3 GetObjectWorldPosition(FMaterialVertexParameters P) { return Primitive.ObjectWorldPositionAndRadius.xyz; }
float3 GetActorWorldPosition() { return Primitive.ActorWorldPosition; }
float3 GetObjectOrientation() { return float3(0, 0, 1); }
float GetPixelDepth(FMaterialPixelParameters P) { return length(P.WorldPosition_CamRelative); }
float2 GetViewportUV(FMaterialPixelParameters P) { return P.SvPosition.xy * ViewSize.zw; }
float2 GetPixelPosition(FMaterialPixelParameters P) { return P.SvPosition.xy; }
float2 GetDefaultSceneTextureUV(FMaterialPixelParameters P) { return P.SvPosition.xy * ViewSize.zw; }
float2 ScreenAlignedPosition(float4 sp) { return float2(sp.x / sp.w * 0.5 + 0.5, 0.5 - sp.y / sp.w * 0.5); }
float4 ScreenAlignedPosition4(float4 sp) { return float4(ScreenAlignedPosition(sp), 0, 0); }
float2 CalcScreenUVFromOffsetFraction(float4 sp, float2 off) { return ScreenAlignedPosition(sp) + off; }
// the scene behind the preview: the backdrop the renderer draws (floor, far gradient), traced per screen uv
float CalcSceneDepth(float2 uv) { return BackdropDistance(EyePos.xyz, BackdropRayDir(GraphInvViewProj, EyePos.xyz, uv), SceneParams); }
float3 DecodeSceneColorForMaterialNode(float2 uv) { return BackdropColor(EyePos.xyz, BackdropRayDir(GraphInvViewProj, EyePos.xyz, uv), SceneParams); }
float4 SceneTextureLookup(float2 uv, int id) {   // 1 PPI_SceneDepth, 13 PPI_CustomDepth
    return id == 1 || id == 13 ? CalcSceneDepth(uv).xxxx : float4(DecodeSceneColorForMaterialNode(uv), 1);
}
float4 SampleLowResClouds(float2 uv) { return 0; }
float4 SampleUnderwaterTranslucency(float2 uv) { return 0; }
float GetDistanceCullFade() { return 1; }
#define BRANCH [branch]
#define FLATTEN [flatten]
#define UNROLL [unroll]
#define LOOP [loop]
float3 ApplyColorVisionDeficiencyCorrection(float mode, float preview, float strength, float3 c) { return c; }
float4 ApplyColorVisionDeficiencyCorrection(float mode, float preview, float strength, float4 c) { return c; }
struct FGBuffersStub {   // the deferred G-buffer: unbound in the viewer (reads 0)
    Texture2D GBufferATexture, GBufferBTexture, GBufferCTexture, GBufferDTexture, GBufferETexture, GBufferVelocityTexture;
    SamplerState GBufferATextureSampler, GBufferBTextureSampler, GBufferCTextureSampler, GBufferDTextureSampler, GBufferETextureSampler, GBufferVelocityTextureSampler;
};
FGBuffersStub GBuffers;
float3 ScaleSaturationByFactor(float3 c, float f) { float l = dot(c, float3(0.3, 0.59, 0.11)); return lerp(l.xxx, c, f); }
#define GetMaterialSharedSampler(materialSampler, sharedSampler) sharedSampler
struct FRopeStub { float3 Start, End, Roughness, Thickness, Length, Tension, Colour, Color; };   // RopeParameter (SoT)
static FRopeStub Rope;
Texture2D SceneDepthTexture;   // no scene depth in the viewer (unbound: reads 0)
SamplerState SceneDepthTextureSampler;
float ConvertFromDeviceZ(float d) { return SceneParams.y; }
float4 SceneTextureLookup(float2 uv, int id, bool filtered) { return SceneTextureLookup(uv, id); }
float CheaperSmoothStep(float a, float b, float x) { x = saturate((x - a) / (b - a)); return x * x * (3 - 2 * x); }
float CheaperSmoothStep(float x) { x = saturate(x); return x * x * (3 - 2 * x); }
float2 CheaperSmoothStep(float2 x) { x = saturate(x); return x * x * (3 - 2 * x); }
float3 CheaperSmoothStep(float3 x) { x = saturate(x); return x * x * (3 - 2 * x); }
float4 CheaperSmoothStep(float4 x) { x = saturate(x); return x * x * (3 - 2 * x); }
float3 LinearToSRGB(float3 c) { return pow(saturate(c), 1 / 2.2); }
float4 LinearToSRGB(float4 c) { return float4(pow(saturate(c.rgb), 1 / 2.2), c.a); }
float3 SRGBToLinear(float3 c) { return pow(saturate(c), 2.2); }
float4 SRGBToLinear(float4 c) { return float4(pow(saturate(c.rgb), 2.2), c.a); }
float EyeAdaptationLookup() { return 1; }
float AthenaLumaAvg() { return 0.18; }   // SoT engine nodes: neutral values in the viewer
float2 AthenaCausticTextureCoordinate(FMaterialPixelParameters P) { return 0; }
float2 AthenaCausticTextureCoordinate(FMaterialVertexParameters P) { return 0; }
float3 AthenaCausticTexture(float2 uv) { return 0; }
static const int STG_Branch = 0, STG_Frond = 1, STG_Leaf = 2, STG_FacingLeaf = 3, STG_Billboard = 4;
static const int STW_None = 0, STW_Fastest = 1, STW_Fast = 2, STW_Better = 3, STW_Best = 4, STW_Palm = 5, STW_BestPlus = 6;
static const int STLOD_Pop = 0, STLOD_Smooth = 1;
float3 GetSpeedTreeVertexOffset(FMaterialVertexParameters P, int g, int w, int l, float t, bool a) { return 0; }   // no wind
float3 GetSpeedTreeVertexOffset(FMaterialPixelParameters P, int g, int w, int l, float t, bool a) { return 0; }
bool GetGIReplaceState() { return false; }
bool IsShadowDepthShader() { return false; }
float GetPerInstanceRandom(FMaterialPixelParameters P) { return P.PerInstanceRandom; }
float GetPerInstanceRandom(FMaterialVertexParameters P) { return P.PerInstanceRandom; }
float GetPerInstanceFadeAmount(FMaterialPixelParameters P) { return 1; }
float3 TransformWorldVectorToTangent(float3x3 tangentToWorld, float3 v) { return mul(tangentToWorld, v); }
float3 TransformTangentVectorToWorld(float3x3 tangentToWorld, float3 v) { return mul(v, tangentToWorld); }
float3 ReflectionAboutCustomWorldNormal(FMaterialPixelParameters P, float3 n) { return -P.CameraVector + 2 * dot(n, P.CameraVector) * n; }
float3 RotateAboutAxis(float4 axisAngle, float3 pivot, float3 pos) {
    float3 c = pivot + normalize(axisAngle.xyz) * dot(normalize(axisAngle.xyz), pos - pivot);
    float3 u = pos - c, v = cross(normalize(axisAngle.xyz), u);
    return c + u * cos(axisAngle.w) + v * sin(axisAngle.w) - pos;
}
float3 TransformLocalToTranslatedWorld(float3 p) { return p; }
float CheaperSmoothStepIn01(float x) { return x * x * (3 - 2 * x); }
half4 CalculateVertexHeightFog(float3 wp, float4 cam) { return half4(0, 0, 0, 1); }
float3 TransformLocalToWorld(float3 p) { return p; }
float3 TransformLocalPositionToWorld(FMaterialPixelParameters P, float3 p) { return p; }
float3 TransformLocalPositionToWorld(FMaterialVertexParameters P, float3 p) { return p; }
float3 TransformLocalVectorToWorld(FMaterialPixelParameters P, float3 v) { return v; }
float3 TransformLocalVectorToWorld(FMaterialVertexParameters P, float3 v) { return v; }
float3 TransformWorldVectorToLocal(float3 v) { return v; }
static const int PPI_SceneColor = 0, PPI_SceneDepth = 1, PPI_DiffuseColor = 2, PPI_SpecularColor = 3, PPI_SubsurfaceColor = 4, PPI_BaseColor = 5,
    PPI_Specular = 6, PPI_Metallic = 7, PPI_WorldNormal = 8, PPI_SeparateTranslucency = 9, PPI_Opacity = 10, PPI_Roughness = 11, PPI_MaterialAO = 12,
    PPI_CustomDepth = 13, PPI_PostProcessInput0 = 14, PPI_PostProcessInput1 = 15, PPI_PostProcessInput2 = 16, PPI_PostProcessInput3 = 17,
    PPI_PostProcessInput4 = 18, PPI_PostProcessInput5 = 19, PPI_PostProcessInput6 = 20, PPI_DecalMask = 21, PPI_ShadingModel = 22,
    PPI_AmbientOcclusion = 23, PPI_CustomStencil = 24;
float StubHash(float3 p) { return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453); }
float StubValueNoise(float3 p) {
    float3 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    float a = lerp(lerp(StubHash(i), StubHash(i + float3(1, 0, 0)), f.x), lerp(StubHash(i + float3(0, 1, 0)), StubHash(i + float3(1, 1, 0)), f.x), f.y);
    float b = lerp(lerp(StubHash(i + float3(0, 0, 1)), StubHash(i + float3(1, 0, 1)), f.x), lerp(StubHash(i + float3(0, 1, 1)), StubHash(i + float3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z);
}
float FastGradientPerlinNoise3D_TEX(float3 p) { return StubValueNoise(p) * 2 - 1; }
float GradientNoise3D_TEX(float3 p) { return StubValueNoise(p) * 2 - 1; }
float SimplexNoise3D_TEX(float3 p) { return StubValueNoise(p) * 2 - 1; }
float MaterialExpressionNoise(float3 pos, float scale, int quality, int func, bool turb, int levels, float outMin, float outMax, float levelScale, float filterWidth) {
    float v = 0, amp = 1, sum = 0;
    float3 p = pos * scale;
    for (int k = 0; k < clamp(levels, 1, 6); k++) {
        float n = StubValueNoise(p) * 2 - 1;
        v += (turb ? abs(n) : n) * amp;
        sum += amp;
        amp *= 0.5;
        p *= levelScale;
    }
    v /= max(sum, 1e-4);
    return lerp(outMin, outMax, turb ? v : v * 0.5 + 0.5);
}
)HLSL";

const char* kMain = R"HLSL(
struct GraphPSIn {
    float4 pos : SV_Position; float3 wpos : WPOS; float3 nrm : NORMAL; float4 tan : TANGENT; float2 uv : TEXCOORD0; float4 col : COLOR0;
    float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2; float2 uv3 : TEXCOORD3;
};
float4 GraphPSMain(GraphPSIn i, bool front : SV_IsFrontFace) : SV_Target {
    View = (FViewStub)0;
    View.GameTime = GraphTime.x;
    View.RealTime = GraphTime.x;
    View.DeltaTime = 1.0 / 60;
    View.ViewSizeAndInvSize = ViewSize;
    View.RenderTargetSize = ViewSize.xy;
    View.BufferSizeAndInvSize = ViewSize;
    View.WorldCameraOrigin = EyePos.xyz;
    View.WorldViewOrigin = EyePos.xyz;
    View.DiffuseOverrideParameter = float4(0, 0, 0, 1);
    View.SpecularOverrideParameter = float4(0, 0, 0, 1);
    View.OutOfBoundsMask = 1;
    Primitive = (FPrimitiveStub)0;
    Primitive.LocalToWorld = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    Primitive.WorldToLocal = Primitive.LocalToWorld;
    Primitive.ObjectWorldPositionAndRadius = float4(0, 0, 0, 100);
    Primitive.ObjectBounds = float3(100, 100, 100);

    FMaterialPixelParameters Parameters = (FMaterialPixelParameters)0;
    [unroll] for (int k = 0; k < 8; k++) Parameters.TexCoords[k] = i.uv;
    Parameters.TexCoords[1] = i.uv1;   // liveries, damage masks
    Parameters.TexCoords[2] = i.uv2;   // decals
    Parameters.TexCoords[3] = i.uv3;   // sets past the mesh's last one repeat set 0
    [unroll] for (int cuv = 0; cuv < 8; cuv++) if (GRAPH_CUSTOM_UVS & (1u << cuv)) Parameters.TexCoords[cuv] = 0;
    Parameters.VertexColor = i.col;
    float3 n = normalize(i.nrm);
    if (!front) n = -n;
    float3 t = dot(i.tan.xyz, i.tan.xyz) > 1e-6 ? normalize(i.tan.xyz - n * dot(n, i.tan.xyz)) : normalize(cross(n, abs(n.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0)));
    float3 b = cross(n, t) * (i.tan.w < 0 ? -1 : 1);
    Parameters.TangentToWorld = float3x3(t, b, n);
    Parameters.WorldNormal = n;
    Parameters.CameraVector = normalize(EyePos.xyz - i.wpos);
    Parameters.ReflectionVector = reflect(-Parameters.CameraVector, n);
    Parameters.LightVector = -normalize(LightDir.xyz);
    Parameters.AbsoluteWorldPosition = i.wpos;
    Parameters.PreSkinnedPosition = i.wpos;
    Parameters.PreSkinnedNormal = n;
    Parameters.WorldPosition_NoOffsets = i.wpos;
    Parameters.WorldPosition_CamRelative = i.wpos - EyePos.xyz;
    Parameters.WorldPosition_NoOffsets_CamRelative = i.wpos - EyePos.xyz;
    Parameters.SvPosition = i.pos;
    float2 ndc = i.pos.xy * ViewSize.zw * float2(2, -2) + float2(-1, 1);
    Parameters.ScreenPosition = float4(ndc, i.pos.z, 1);
    Parameters.UnMirrored = 1;
    Parameters.TwoSidedSign = front ? 1 : -1;
    Parameters.PerInstanceRandom = 0.5;
    Parameters.PerInstanceFadeAmount = 1;
    Parameters.Particle.Color = float4(1, 1, 1, 1);
    Parameters.Particle.SubUVCoords[0] = float4(i.uv, i.uv);
    Parameters.Particle.SubUVCoords[1] = float4(i.uv, i.uv);
    Parameters.Particle.RelativeTime = frac(GraphTime.x * 0.25);
    Parameters.Particle.PositionAndSize = float4(i.wpos, 50);
    Parameters.Particle.Size = float2(50, 50);
    Parameters.Particle.DynamicParameter = float4(1, 1, 1, 1);
    Parameters.Particle.MacroUV = float4(0, 0, 1, 1);
    Parameters.Particle.Random = 0.5;

    FPixelMaterialInputs M = (FPixelMaterialInputs)0;
    M.Normal = float3(0, 0, 1);
    M.Opacity = 1;
    M.OpacityMask = 1;
    M.Specular = 0.5;
    M.Roughness = 0.5;
    M.AmbientOcclusion = 1;
    CalcPixelMaterialInputs(Parameters, M);

#if GRAPH_BLEND == 1
    clip(M.OpacityMask - 0.3333);
#endif
    float3 wn = normalize(mul(normalize(M.Normal), Parameters.TangentToWorld));
    if (dot(wn, wn) < 0.5 || any(isnan(wn))) wn = n;
#if GRAPH_UNLIT
    float3 c = M.EmissiveColor;
#else
    float3 v = Parameters.CameraVector;
    float3 l = normalize(-LightDir.xyz);
    float3 sun = SunColor.w > 0 ? SunColor.rgb : float3(0.95, 0.92, 0.85);
    float metal = saturate(M.Metallic);
    float3 diffuse = saturate(M.BaseColor) * (1 - metal);
    float3 specColor = lerp(0.08 * saturate(M.Specular).xxx, saturate(M.BaseColor), metal);
    float rough = clamp(M.Roughness, 0.05, 1);
    float hemi = 0.5 + 0.5 * wn.z;
    float3 ambient = lerp(float3(0.16, 0.15, 0.14), float3(0.34, 0.37, 0.42), hemi) * FrameAmbient.rgb * saturate(M.AmbientOcclusion);
    float3 h = normalize(l + v);
    float ndl = saturate(dot(wn, l));
    float specPow = exp2(10 * (1 - rough) + 1);
    float3 c = diffuse * (ambient + ndl * sun) + specColor * pow(saturate(dot(wn, h)), specPow) * ndl * sun * (specPow + 2) / 8 + specColor * ambient * 0.5;
    for (int q = 0; q < (int)LightCount.x; q++) {
        float4 a = Lights[q * 3], bb = Lights[q * 3 + 1], d = Lights[q * 3 + 2];
        float3 L = a.xyz - i.wpos;
        float dist = length(L);
        if (dist >= a.w) continue;
        L /= max(dist, 1e-3);
        float win = saturate(1 - pow(dist / a.w, 4));
        float att = win * win / (1 + dist * dist * 0.0001);
        if (bb.w > -1.5) att *= saturate((dot(-L, d.xyz) - d.w) / max(bb.w - d.w, 1e-3));
        c += diffuse * bb.rgb * saturate(dot(wn, L)) * att;
    }
    c += M.EmissiveColor;
#endif
    if (FogParams.w > 0) {
        float3 ray = i.wpos - EyePos.xyz;
        float len = length(ray);
        float f0 = FogColor.w * exp2(-FogParams.x * (EyePos.z - FogParams.y));
        float fx = max(-127, FogParams.x * ray.z);
        float lineInt = abs(fx) > 0.01 ? (1 - exp2(-fx)) / fx : 0.6931 - 0.2402 * fx;
        float fog = min(1 - saturate(exp2(-f0 * lineInt * max(len - FogParams.z, 0))), FogParams.w);
        c = lerp(c, FogColor.rgb, fog);
    }
    c = max(c, 0);
#if GRAPH_BLEND == 2
    return float4(c, saturate(M.Opacity));
#elif GRAPH_BLEND == 3
    return float4(c * saturate(M.Opacity), 1);
#elif GRAPH_BLEND == 4
    return float4(c, 1);
#else
    return float4(c, 1);
#endif
}
)HLSL";

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// TransformVector_<A>To<B> / TransformPosition_<A>To<B> (MaterialExpressionTransform*): tangent space uses the pixel's
// basis, the other spaces are treated as world space (the viewer has no object / view transforms per pixel).
std::string TransformFunctions() {
    static const char* vec[] = {"Tangent", "Local", "World", "View", "Camera", "ParticleWorld", "Instance"};
    static const char* pos[] = {"Local", "World", "TranslatedWorld", "View", "Camera", "Instance", "Particle"};
    std::string s;
    for (const char* a : vec)
        for (const char* b : vec) {
            std::string body = !strcmp(a, "Tangent") ? "mul(v, P.TangentToWorld)" : !strcmp(b, "Tangent") ? "mul(P.TangentToWorld, v)" : "v";
            for (const char* pt : {"FMaterialPixelParameters", "FMaterialVertexParameters"})
                s += Fmt("float3 TransformVector_%sTo%s(%s P, float3 v) { return %s; }\n", a, b, pt, body.c_str());
        }
    for (const char* a : pos)
        for (const char* b : pos) {
            for (const char* pt : {"FMaterialPixelParameters", "FMaterialVertexParameters"})
                s += Fmt("float3 TransformPosition_%sTo%s(%s P, float3 v) { return v; }\n", a, b, pt);
        }
    return s;
}

}   // namespace

bool BuildGraphShaderSource(const std::string& translated, const std::string& blendMode, const std::string& shadingModel, GraphShaderSource& out,
                            std::string* error) {
    out = GraphShaderSource{};
    size_t endPixel = translated.find("\nvoid CalcVertexMaterialInputs");
    std::string code = translated.substr(0, endPixel);
    if (code.find("void CalcPixelMaterialInputs") == std::string::npos) {
        if (error) *error = "no CalcPixelMaterialInputs (material functions can't be previewed)";
        return false;
    }
    out.blend = blendMode.find("Masked") != std::string::npos        ? 1
                : blendMode.find("Additive") != std::string::npos    ? 3
                : blendMode.find("Modulate") != std::string::npos    ? 4
                : blendMode.find("Translucent") != std::string::npos ? 2
                                                                     : 0;
    out.unlit = shadingModel.find("Unlit") != std::string::npos;
    // View resources the viewer doesn't have
    auto replaceAll = [&](const std::string& a, const std::string& b) {
        for (size_t k = 0; (k = code.find(a, k)) != std::string::npos; k += b.size()) code.replace(k, a.size(), b);
    };
    replaceAll("View.LocalDisturbanceTexture", "StubTexture");
    replaceAll("View.BilinearWrapSampler", "MatSampler");
    code = std::regex_replace(code, std::regex(R"(Material\.(?:Wrap|Clamp)_WorldGroupSettings)"), "MatSampler");   // shared samplers
    // Custom node code may return ScreenAlignedPosition from a float3 function (mt_unq_simple_flare): inside the custom
    // expressions it returns a float4 that narrows to any declared type (with a truncation warning only)
    if (size_t cs = code.find("// ---- custom expressions\n"); cs != std::string::npos) {
        size_t s = code.find('\n', cs) + 1, end = code.size();
        int d = 0;
        static const std::regex sigRe(R"(^\w+\s+CustomExpression\d+\s*\()");
        while (s < code.size()) {
            size_t e = code.find('\n', s);
            if (e == std::string::npos) e = code.size();
            std::string l = code.substr(s, e - s);
            bool skip = d > 0 || l.empty() || l.rfind("//", 0) == 0 || l[0] == '{' || l[0] == '}' || std::regex_search(l, sigRe);
            for (char c : l) d += c == '{' ? 1 : c == '}' ? -1 : 0;
            if (!skip) { end = s; break; }
            s = e + 1;
        }
        code.insert(end, "#undef ScreenAlignedPosition\n");
        code.insert(cs, "#define ScreenAlignedPosition ScreenAlignedPosition4\n");
    }

    static const std::regex texDecl(R"(^(Texture2D|TextureCube)\s+(\w+)\s*;(.*)$)");
    static const std::regex varDecl(R"(^((?:float|half|int|uint|bool)[234]?(?:x[234])?|FMaterialAttributes)\s+(\w+)\s*(=\s*([^;]*))?;(.*)$)");
    static const std::regex pathRe(R"(((?:/Game|/Engine)/[^\s()]+))");
    static const std::regex paramRe(R"((ScalarParameter|VectorParameter)\s+\"([^\"]*)\")");
    static const std::regex numRe(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)");
    std::istringstream in(code);
    std::string line, body;
    int depth = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (depth == 0 && std::regex_match(line, m, texDecl)) {
            GraphShaderTexture t;
            t.cube = m[1] == "TextureCube";
            t.ident = m[2];
            std::string comment = m[3];
            std::smatch pm;
            if (std::regex_search(comment, pm, pathRe)) t.path = pm[1];
            t.slot = (int)out.textures.size();
            if (t.slot >= 30) { if (error) *error = "too many textures"; return false; }
            line = Fmt("%s %s : register(t%d);%s\n#define %sSampler MatSampler", m[1].str().c_str(), t.ident.c_str(), t.slot, comment.c_str(), t.ident.c_str());
            out.textures.push_back(t);
        } else if (depth == 0 && std::regex_match(line, m, varDecl)) {
            std::string type = m[1], id = m[2], init = m[4], comment = m[5];
            std::smatch pm;
            int comps = type == "float" ? 1 : type == "float2" ? 2 : type == "float3" ? 3 : type == "float4" ? 4 : 0;
            if (comps && out.params.size() < 512 && std::regex_search(comment, pm, paramRe)) {
                GraphShaderParam p;
                p.ident = id;
                p.name = pm[2];
                p.components = comps;
                int n = 0;
                std::string vals = init.substr(init.find('(') == std::string::npos ? 0 : init.find('(') + 1);   // past "float4("
                bool literal = vals.find_first_not_of("0123456789.eE+-, )\t") == std::string::npos;
                for (std::sregex_iterator it(vals.begin(), vals.end(), numRe), e; literal && it != e && n < 5; ++it) {
                    if (n < 4) p.value[n] = strtof(it->str().c_str(), nullptr);
                    n++;
                }
                if (n == comps) {   // a literal value: read from the constant buffer instead (a static, so same-named locals still shadow it)
                    static const char* sw[] = {"", ".x", ".xy", ".xyz", ""};
                    line = Fmt("static %s %s = GraphParam[%zu]%s;%s", type.c_str(), id.c_str(), out.params.size(), sw[comps], comment.c_str());
                    out.params.push_back(p);
                    body += line + "\n";
                    continue;
                }
            }
            if (init.empty()) init = type == "FMaterialAttributes" ? "(FMaterialAttributes)0" : id.rfind("LayerWeight_", 0) == 0 ? "1" : "0";
            line = "static " + type + " " + id + " = " + init + ";" + comment;
        }
        for (char c : line) {
            if (c == '{') depth++;
            else if (c == '}') depth = std::max(0, depth - 1);
        }
        body += line + "\n";
    }
    std::string params = out.params.empty() ? "" : Fmt("cbuffer GraphParams : register(b3) { float4 GraphParam[%zu]; };\n", out.params.size());
    // customized UVs are computed by the vertex part, which the viewer doesn't run (SoT ships put fire charring in
    // CustomizedUVs4): those TexCoords read 0, the neutral value, instead of the mesh UVs
    unsigned customUvs = 0;
    if (endPixel != std::string::npos) {
        static const std::regex cuv(R"(CustomizedUVs\[(\d)\]\s*=)");
        std::string vertex = translated.substr(endPixel);
        for (std::sregex_iterator it(vertex.begin(), vertex.end(), cuv), e; it != e; ++it) customUvs |= 1u << ((*it)[1].str()[0] - '0');
    }
    out.hlsl = Fmt("#define GRAPH_BLEND %d\n#define GRAPH_UNLIT %d\n#define GRAPH_CUSTOM_UVS %u\n", out.blend, out.unlit ? 1 : 0, customUvs & 0xff) + kPrelude + TransformFunctions() +
               "\n// ---- translated material graph\n" + params + body + kMain;
    return true;
}
