#pragma once
#include "render/GpuAssets.h"
#include "render/WorldScene.h"

// Headless rendering for the AI tools (preview_mesh): its own D3D11 device (hardware, WARP fallback), created lazily.
struct PreviewRequest {
    int width = 768, height = 768;
    float yaw = -135, pitch = -20;   // camera orbit angles (degrees)
    int lod = 0;
    DrawOptions draw;
    bool fixedBounds = false;   // frame the camera on these bounds instead of the mesh's
    Vec3 boundsMin, boundsMax;
    struct Overlay { u32 color; std::vector<Vec3> segments; };   // pairs of points, mesh space, drawn on top
    std::vector<Overlay> overlays;
};

bool RenderMeshPreview(PackageCache& cache, const MeshData& mesh, const PreviewRequest& req, Image& out, std::string* error);
// Several meshes (e.g. animation frames) with one camera; textures are decoded once.
bool RenderMeshPreviews(PackageCache& cache, const std::vector<MeshData>& meshes, const PreviewRequest& req, std::vector<Image>& out, std::string* error);

// A whole map: the camera orbits the scene bounds (or looks from `eye` at `target` when useEye is set).
struct WorldPreviewRequest {
    int width = 1280, height = 720;
    float yaw = -135, pitch = -35, zoom = 1;   // zoom < 1 moves the orbit camera closer
    bool useEye = false;
    Vec3 eye, target;
    DrawOptions draw;
    WorldDrawOptions world;
};
bool RenderWorldPreview(PackageCache& cache, const WorldData& world, const WorldPreviewRequest& req, Image& out, std::string* error);
