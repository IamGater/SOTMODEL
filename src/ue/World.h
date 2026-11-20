#pragma once
#include "ue/Package.h"
#include "render/Math.h"
#include <functional>
#include <deque>

// Static scene of a map (.umap): every placed static / skeletal mesh with its world transform, Blueprint component templates
// resolved underneath the level's overrides, instanced meshes expanded, and the world-composition sublevels of the map.

struct SplineMeshParams {
    Vec3 startPos, startTangent, endPos, endTangent;
    Vec2 startScale{1, 1}, endScale{1, 1};
    Vec2 startOffset, endOffset;
    float startRoll = 0, endRoll = 0;
    Vec3 upDir{0, 0, 1};
    int forwardAxis = 0;   // ESplineMeshAxis: 0 X, 1 Y, 2 Z
};

struct WorldInstance {
    Mat4 transform;   // mesh -> world (UE space, row vectors)
    u32 level = 0;    // index into WorldData::levels
};

// All instances of one mesh with one set of materials.
struct WorldMeshGroup {
    std::string mesh;                     // object path "/Game/Models/x.x"
    std::vector<std::string> materials;   // per-slot overrides ("" = the mesh's own); empty = none
    bool skeletal = false;
    float drawDistance = 0;               // largest draw distance of the instances (cm, 0 = unlimited)
    std::vector<WorldInstance> instances;
};

struct WorldSpline {
    std::string mesh;
    std::vector<std::string> materials;
    Mat4 transform;   // component -> world
    SplineMeshParams params;
    u32 level = 0;
};

// One LandscapeComponent as a world-space mesh (vertex colours blended from its paint layers).
struct WorldLandscape {
    std::string name;   // Landscape actor . component
    u32 level = 0;
    MeshLod mesh;
    Vec3 boundsMin, boundsMax;
    std::vector<std::string> layers;   // paint layer names
};

struct WorldLevel {
    std::string path;    // game path of the .umap
    Mat4 transform = Mat4::Identity();   // level -> world
    u32 actors = 0, meshComponents = 0, instances = 0, splines = 0, unresolved = 0, landscapeComponents = 0;
};

struct WorldData {
    std::string root;   // the map that was opened
    std::vector<WorldLevel> levels;
    std::vector<WorldMeshGroup> groups;
    std::vector<WorldSpline> splines;
    std::vector<WorldLandscape> landscapes;
    std::vector<std::string> unresolved;   // "level: Actor.Component" of mesh components whose mesh could not be found
    Vec3 boundsMin{1e30f, 1e30f, 1e30f}, boundsMax{-1e30f, -1e30f, -1e30f};   // of instance origins
    size_t InstanceCount() const;
};

struct WorldOptions {
    bool sublevels = true;       // a persistent map also loads the .umap files under its folder (world composition)
    bool skipNonVisual = true;   // skip audio / tall-tale / arena sublevels and helper meshes (shapes, clouds, volumes)
    bool skeletal = true;        // include skeletal mesh components (shown in their reference pose)
    bool landscape = true;       // build Landscape terrain meshes from the heightmaps
    int landscapeStep = 1;
    // mesh path filter: comma separated substrings (case insensitive); a mesh must match one of the plain terms (if any)
    // and none of the '-' terms, e.g. "rock,-small"
    std::string meshFilter;       // landscape vertex spacing in quads (2 or 4 = coarser, less memory)
    std::function<bool()> cancel;
    std::function<void(int done, int total, const std::string& level)> progress;
};

class WorldLoader {
public:
    explicit WorldLoader(PackageCache& cache);
    ~WorldLoader();
    bool Load(std::string_view mapPath, const WorldOptions& opt, WorldData& out, std::string* error = nullptr);
    // Adds the placements of one level package with the given level -> world transform.
    void AddLevel(const Package& level, const Mat4& transform, const WorldOptions& opt, WorldData& out);
    // The .umap files that make up a map (itself first), following the Athena world-composition layout.
    std::vector<std::string> MapLevels(std::string_view mapPath, bool skipNonVisual) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

bool IsWorldClass(std::string_view cls);
