#pragma once
#include "render/GpuAssets.h"
#include "ue/World.h"

// GPU side of a loaded map: one instanced mesh per (mesh, materials) group, landscape patches and deformed spline meshes,
// drawn with per-instance frustum, draw-distance and screen-size culling.
struct WorldDrawOptions {
    bool meshes = true, landscape = true, splines = true;
    float drawDistanceScale = 1;   // multiplies the game's per-mesh draw distances (0 = ignore them)
    float minScreenSize = 0.0015f; // cull instances whose bounding sphere covers less than this fraction of the view height
    bool lights = true;            // light the scene with the map's sun / sky light / point and spot lights
    bool fog = true;               // the map's exponential height fog
    bool water = true;             // sea plane at the map's water level
    bool gizmos = false;           // light and decal outlines
};

struct WorldDrawStats {
    u32 meshesDrawn = 0, instancesDrawn = 0, landscapesDrawn = 0;
};

class WorldScene {
public:
    WorldScene(Renderer& r, GpuAssets& assets, PackageCache& cache) : m_r(r), m_assets(assets), m_cache(cache) {}
    // Worker thread: uploads everything (meshes are decoded once per unique asset). progress(done, total).
    void Build(const WorldData& wd, const std::function<bool()>& cancel = {}, const std::function<void(int, int)>& progress = {});
    // Main thread, between Renderer::BeginFrame and EndFrame.
    void Draw(const Camera& cam, float viewHeightPx, const WorldDrawOptions& opt);
    // The map's lighting for a camera (sun, sky light, the lights nearest the eye, the height fog above / below the water).
    // Pass it in DrawOptions::lighting to Renderer::BeginFrame.
    void Lighting(const Camera& cam, const WorldDrawOptions& opt, SceneLighting& out) const;
    // Water plane and gizmos: after Draw.
    void DrawOverlays(const Camera& cam, const WorldDrawOptions& opt);
    float WaterLevel() const { return m_waterLevel; }
    size_t LightCount() const { return m_lights.size(); }
    size_t DecalCount() const { return m_decals.size(); }
    const WorldDrawStats& Stats() const { return m_stats; }
    size_t GpuBytes() const { return m_gpuBytes; }
    u32 MissingMeshes() const { return m_missing; }
    Vec3 BoundsMin() const { return m_bmin; }
    Vec3 BoundsMax() const { return m_bmax; }
    // bounds of the bulk of the scene (2nd..98th percentile of instance centres): frames the camera without far outliers
    Vec3 FocusMin() const { return m_fmin; }
    Vec3 FocusMax() const { return m_fmax; }

private:
    struct Item {
        std::shared_ptr<GpuMesh> mesh;
        std::vector<Mat4> transforms;
        std::vector<Vec4> spheres;   // world-space bounding sphere per instance (xyz centre, w radius)
        float drawDistance = 0;
        bool landscape = false;
    };
    Renderer& m_r;
    GpuAssets& m_assets;
    PackageCache& m_cache;
    std::vector<Item> m_items;
    std::vector<Mat4> m_visible;
    WorldDrawStats m_stats;
    size_t m_gpuBytes = 0;
    u32 m_missing = 0;
    Vec3 m_bmin, m_bmax, m_fmin, m_fmax;
    std::vector<WorldLight> m_lights;
    std::vector<WorldDecal> m_decals;
    std::vector<WorldFog> m_fogs;
    float m_waterLevel = 0;
    std::shared_ptr<GpuMesh> m_water;
};

// CPU deformation of a mesh LOD along a SplineMeshComponent's Hermite curve (component space).
void DeformSplineMesh(MeshLod& lod, const Vec3& boundsMin, const Vec3& boundsMax, const SplineMeshParams& p);
