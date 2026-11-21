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
