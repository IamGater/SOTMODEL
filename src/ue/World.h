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
    u32 actor = UINT32_MAX;   // index into WorldData::actors (the component's owner)
};

// The actor owning placed components (picking shows its name, class and Blueprint package).
struct WorldActor {
    std::string name, cls;   // object name, class ("BP_Barrel_C")
    std::string blueprint;   // /Game/... package of a Blueprint class, empty for native classes
    u32 level = 0;
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
    u32 actor = UINT32_MAX;
};

// One LandscapeComponent as a world-space mesh (vertex colours blended from its paint layers).
struct WorldLandscape {
    std::string name;   // Landscape actor . component
    u32 level = 0;
    MeshLod mesh;
    Vec3 boundsMin, boundsMax;
    std::vector<std::string> layers;   // paint layer names
};

// A light component (colour linear 0..1 from the FColor, intensity in the engine's units).
struct WorldLight {
    enum Type : u8 { Point, Spot, Directional, Sky } type = Point;
    Vec3 position, direction{1, 0, 0};   // direction = the component's +X
    Vec3 color{1, 1, 1};
    float intensity = 5000, radius = 1000, innerCone = 0, outerCone = 44;   // radius = AttenuationRadius (cm), cones in degrees
    bool inverseSquared = true, castShadows = true;
    u32 level = 0;
};

// A deferred decal: a unit box (+-1) through the component transform, projected along X. In UE 4.10 the extent is the
// component scale (a DecalActor starts at 128 x 256 x 256); DecalSize (4.11+) multiplies it when present.
struct WorldDecal {
    std::string material;
    Mat4 transform;   // component -> world
    Vec3 size{1, 1, 1};
    u32 level = 0;
};

// ExponentialHeightFogComponent (UE 4.10 units: density and falloff are divided by 1000 when rendering).
struct WorldFog {
    Vec3 position;
    float density = 0.02f, heightFalloff = 0.2f, maxOpacity = 1, startDistance = 0;
    Vec3 color{0.447f, 0.639f, 1.0f};   // FogInscatteringColor
    bool underwater = false;            // SoT's second fog component ("UnderwaterFog"), used below the water surface
    u32 level = 0;
};

struct WorldLevel {
    std::string path;    // game path of the .umap
    Mat4 transform = Mat4::Identity();   // level -> world
    std::string island;                  // world layout: the island the level belongs to
    u32 actors = 0, meshComponents = 0, instances = 0, splines = 0, unresolved = 0, landscapeComponents = 0;
};

struct WorldData {
    std::string root;   // the map that was opened
    std::vector<WorldLevel> levels;
    std::vector<WorldMeshGroup> groups;
    std::vector<WorldActor> actors;
    std::vector<WorldSpline> splines;
    std::vector<WorldLandscape> landscapes;
    std::vector<WorldLight> lights;
    std::vector<WorldDecal> decals;
    std::vector<WorldFog> fogs;
    bool hasWater = false;
    float waterLevel = 0;   // Z of the FFT water plane (FFTWaterComponent); 0 when the map has none
    std::vector<std::string> unresolved;   // "level: Actor.Component" of mesh components whose mesh could not be found
    Vec3 boundsMin{1e30f, 1e30f, 1e30f}, boundsMax{-1e30f, -1e30f, -1e30f};   // of instance origins
    size_t InstanceCount() const;
};

struct WorldOptions {
    bool sublevels = true;       // a persistent map also loads the .umap files under its folder (world composition)
    bool skipNonVisual = true;   // skip audio / tall-tale / arena sublevels and helper meshes (shapes, clouds, volumes)
    bool skeletal = true;        // include skeletal mesh components (shown in their reference pose)
    bool landscape = true;       // build Landscape terrain meshes from the heightmaps
    bool lights = true;          // collect lights, decals, height fog and the water plane
    int landscapeStep = 1;
    // mesh path filter: comma separated substrings (case insensitive); a mesh must match one of the plain terms (if any)
    // and none of the '-' terms, e.g. "rock,-small"
    std::string meshFilter;
    // Opening sot_world_layout_2 loads the whole open world: every island placed by the world layout (see WorldLayout.h).
    std::string islands;          // island filter for the world layout, same syntax as meshFilter (name / folder / sea)
    bool proxies = false;         // world layout: use the islands' merged LOD proxy levels (one cheap mesh per sublevel)
    bool region = false;          // world layout: only islands whose centre lies in [regionMin, regionMax] (world XY, cm)
    Vec2 regionMin, regionMax;
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
