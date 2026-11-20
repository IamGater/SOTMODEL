#pragma once
#include "ue/Package.h"

struct MaterialTextureParam {
    std::string name;          // parameter name (or "<referenced>" for textures only listed as dependencies)
    std::string texturePath;   // /Game/...
    std::string definedIn;     // material that set it
};
struct MaterialScalarParam { std::string name; float value; std::string definedIn; };
struct MaterialVectorParam { std::string name; float r, g, b, a; std::string definedIn; };

struct MaterialInfo {
    std::string path;                    // the material asked for
    std::vector<std::string> chain;      // instance -> parent -> ... -> base material
    std::string baseMaterial;
    std::string blendMode = "BLEND_Opaque";
    std::string shadingModel;
    bool twoSided = false;
    std::vector<MaterialTextureParam> textures;
    std::vector<MaterialScalarParam> scalars;
    std::vector<MaterialVectorParam> vectors;
    std::vector<std::string> staticSwitches;

    // best guesses for previews / exports (texture object paths, empty if none)
    std::string diffuse, normal, packed, emissive;
};

// Resolves the instance chain (child parameters override parents). `path` is an object or package path.
bool ReadMaterial(PackageCache& cache, const std::string& path, MaterialInfo& out, std::string* error = nullptr);
bool IsMaterialClass(std::string_view cls);
std::string ClassifyTexture(std::string_view paramName, std::string_view texturePath);   // "diffuse", "normal", "packed", "emissive", "mask", ""
