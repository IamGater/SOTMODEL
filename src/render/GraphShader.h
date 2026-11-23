#pragma once
#include "core/Common.h"
#include <string>
#include <vector>

// Turns a material's translated graph HLSL (MaterialToHlsl) into a pixel shader for the viewport: the UE material
// environment (FMaterialPixelParameters, View, Primitive, engine helper functions) is stubbed, parameters become
// static constants, texture parameters get registers, and GraphPSMain lights the resulting material attributes with
// the viewport's lights / fog (same Frame constants as the mesh shader).
struct GraphShaderTexture {
    std::string ident;   // Tex_*
    std::string path;    // texture object path from the declaration comment
    bool cube = false;
    int slot = 0;        // t register
};
// A scalar / vector parameter read from the GraphParams constant buffer (b3, one float4 per parameter) so it can be edited
// without recompiling.
struct GraphShaderParam {
    std::string ident;   // HLSL identifier
    std::string name;    // parameter name in the material
    int components = 1;  // 1 scalar, 2..4 vector
    float value[4] = {0, 0, 0, 0};   // the material's value
};
struct GraphShaderSource {
    std::string hlsl;                         // complete shader, entry GraphPSMain (ps_5_0)
    std::vector<GraphShaderTexture> textures;
    std::vector<GraphShaderParam> params;     // GraphParam[i] in the shader
    int blend = 0;                            // 0 opaque, 1 masked, 2 translucent, 3 additive, 4 modulate
    bool unlit = false;
};
// blendMode / shadingModel as in MaterialInfo (BLEND_*, MSM_*).
bool BuildGraphShaderSource(const std::string& translated, const std::string& blendMode, const std::string& shadingModel, GraphShaderSource& out,
                            std::string* error = nullptr);

// The scene behind a material preview, shared by the graph shaders (SceneColor / SceneDepth / DepthFade read it) and the
// renderer's backdrop pass (which draws it, so translucency and refraction show what those nodes read): a checker floor at
// scene.x (world z) fading into a dim gradient, everything else at distance scene.y. scene.z = 0: no backdrop drawn (the flat
// clear colour is returned, depth stays at scene.y).
#define GRAPH_BACKDROP_HLSL R"HLSL(
float3 BackdropRayDir(float4x4 invViewProj, float3 eye, float2 uv) {
    float4 w = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, 0.5, 1), invViewProj);
    return normalize(w.xyz / w.w - eye);
}
float BackdropDistance(float3 eye, float3 dir, float4 scene) {
    float t = scene.y;
    if (scene.z > 0 && dir.z < -1e-4) {
        float f = (scene.x - eye.z) / dir.z;
        if (f > 0) t = min(t, f);
    }
    return t;
}
float3 BackdropColor(float3 eye, float3 dir, float4 scene) {
    if (scene.z <= 0) return float3(0.011, 0.012, 0.016);
    float3 sky = lerp(float3(0.020, 0.022, 0.030), float3(0.075, 0.085, 0.110), saturate(dir.z * 0.5 + 0.5));
    if (dir.z < -1e-4) {
        float f = (scene.x - eye.z) / dir.z;
        if (f > 0 && f < scene.y) {
            float3 p = eye + dir * f;
            float2 cell = floor(p.xy / 25);
            float k = fmod(abs(cell.x + cell.y), 2);
            float3 floorC = lerp(float3(0.055, 0.057, 0.065), float3(0.11, 0.105, 0.095), k);
            return lerp(floorC, sky, saturate(f / scene.y));
        }
    }
    return sky;
}
)HLSL"