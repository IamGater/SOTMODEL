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
bool IsShaderCacheFile(std::string_view path);   // GlobalShaderCache-*.bin / ShaderCache files
