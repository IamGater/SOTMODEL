#pragma once
#include "ue/Package.h"

struct ExportOptions {
    std::wstring outputDir;
    int meshFormat = 0;      // 0 glb, 1 obj
    int textureFormat = 0;   // 0 png, 1 dds
    bool keepDirectories = true;
    bool raw = false;        // also write the .uasset itself
    bool json = true;
    bool embedTextures = true;   // glTF: embed material textures
    int maxTextureSize = 0;      // embedded textures: 0 = full resolution
};

std::wstring ExportPathFor(const ExportOptions& opt, const Package& pkg, const std::wstring& ext);

// Exports a package by its main asset type (texture -> image, mesh -> model, animation -> glb, ...) plus its JSON.
bool ExportPackage(PackageCache& cache, const Package& pkg, const ExportOptions& opt, std::vector<std::string>& written, std::string* error);
