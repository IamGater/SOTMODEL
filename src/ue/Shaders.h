#pragma once
#include "ue/Package.h"

// Compiled shaders (D3D11 SM5 DXBC) of a cooked material's inline shader maps or of the GlobalShaderCache-*.bin file.
// UE 4.10 FShader records: ... VFType, VFSourceHash, Type, SourceHash, Target, uniform buffer structs, then the inline
// FShaderResource (SpecificType, Target, Code, OutputHash, NumInstructions, NumTextureSamplers); Sea of Thieves stores
// the code LZ4-compressed and appends its uncompressed size.
struct ShaderEntry {
    std::string type;            // FShaderType, e.g. TBasePassPSFNoLightMapPolicy
    std::string vertexFactory;   // FLocalVertexFactory, ... (empty for material / global shaders without one)
    std::string usage;           // shader map description ("Compiling M: Platform=..., Usage=...") when known
    int frequency = 0;           // EShaderFrequency: 0 vertex, 1 hull, 2 domain, 3 pixel, 4 geometry, 5 compute
    u32 platform = 0;
    std::vector<std::string> uniformBuffers;   // FViewUniformShaderParameters, ...
    std::vector<int> uniformBufferSlots;       // their constant buffer registers (bN)
    int materialBufferSlot = -1;               // FMaterialShader::MaterialUniformBuffer register (-1 unknown / unbound)
    int uniformSet = -1;                       // index into the package's MaterialUniformSets (the shader map it belongs to)
    u32 numInstructions = 0, numTextureSamplers = 0;
    size_t offset = 0;           // of the code array in the source data
    Bytes dxbc;                  // decompressed bytecode
};

const char* ShaderFrequencyName(int frequency);   // "vertex", "pixel", ...
const char* ShaderStagePrefix(int frequency);     // "vs", "ps", ...

// Scans raw data for shader records. `names` resolves FNames (packages); without it names are read as FStrings
// (the global shader cache is written by a memory archive).
void ScanShaders(const Bytes& data, const std::vector<std::string>* names, std::vector<ShaderEntry>& out);
bool ReadPackageShaders(const Package& pkg, std::vector<ShaderEntry>& out, std::string* error = nullptr);
bool IsShaderCacheFile(std::string_view path);

// FUniformExpressionSet of a material shader map: what the Material uniform buffer holds. Its layout (UE 4.10
// CreateBufferStruct): one float4 per vector expression, then the scalar expressions packed 4 per float4; textures
// are bound as t-registers in Uniform2DTextureExpressions order (then the cube ones).
struct MaterialUniformSet {
    size_t offset = 0;
    std::vector<std::string> vectors, scalars, textures2D, texturesCube;   // readable expressions
    std::vector<std::string> perFrameScalars, perFrameVectors;
    // "cbN[i].c" -> expression for this set (slot = the Material buffer register)
    std::string Describe(int vectorIndex, int component) const;   // component -1 = whole float4
};
bool ReadMaterialUniformSets(const Package& pkg, std::vector<MaterialUniformSet>& out);
// Assigns ShaderEntry::uniformSet by position (the last set before each shader).
void AssignUniformSets(std::vector<ShaderEntry>& shaders, const std::vector<MaterialUniformSet>& sets);
// Comments every line of decompiled HLSL that reads cb<slot>_cb[i] with the material parameters it reads, and returns
// a legend of the whole buffer.
std::string AnnotateMaterialBuffer(std::string& hlsl, int slot, const MaterialUniformSet& set);   // GlobalShaderCache-*.bin / ShaderCache files
