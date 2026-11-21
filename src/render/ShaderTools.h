#pragma once
#include "core/Common.h"

// DXBC inspection with d3dcompiler_47 (D3DDisassemble / D3DReflect) and a DXBC -> HLSL translator.
struct ShaderVariable {
    std::string name, type;
    u32 offset = 0, size = 0;
};
struct ShaderConstantBuffer {
    std::string name;
    u32 slot = 0, size = 0;   // register bN, bytes
    std::vector<ShaderVariable> variables;
};
struct ShaderBinding {
    std::string name;
    std::string kind;   // texture, sampler, cbuffer, uav, structured, ...
    std::string dimension;   // 2d, 3d, cube, buffer, ...
    u32 slot = 0, count = 1;
};
struct ShaderSignatureElement {
    std::string semantic;
    u32 index = 0, reg = 0;
    std::string sysValue;   // SV_Position, ...
    std::string type;       // float, uint, int
    u8 mask = 0;            // components used
};
struct ShaderReflectionInfo {
    std::string target;     // ps_5_0
    u32 instructions = 0, tempRegisters = 0;
    std::vector<ShaderConstantBuffer> cbuffers;
    std::vector<ShaderBinding> bindings;
    std::vector<ShaderSignatureElement> inputs, outputs;
};

bool DisassembleShader(const Bytes& dxbc, std::string& out, std::string* error = nullptr);
bool ReflectShader(const Bytes& dxbc, ShaderReflectionInfo& out, std::string* error = nullptr);
std::string ReflectionToText(const ShaderReflectionInfo& r);

// Readable HLSL from the bytecode: declarations from the reflection data (constant buffers with their members,
// textures, samplers, inputs / outputs), then the instructions translated one by one. `bitExact` keeps every temp a
// uint4 of raw lanes and prints literals as exact bit patterns; otherwise temps are float4, literals are rounded
// decimals and only integer ops convert. Unsupported instructions are kept as comments.
bool DecompileShader(const Bytes& dxbc, std::string& hlsl, bool bitExact = false, std::string* error = nullptr);
