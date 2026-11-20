#pragma once
#include "render/GpuAssets.h"

// Headless rendering for the AI tools (preview_mesh): its own D3D11 device (hardware, WARP fallback), created lazily.
struct PreviewRequest {
    int width = 768, height = 768;
    float yaw = -135, pitch = -20;   // camera orbit angles (degrees)
    int lod = 0;
    DrawOptions draw;
    bool fixedBounds = false;   // frame the camera on these bounds instead of the mesh's
    Vec3 boundsMin, boundsMax;
};

bool RenderMeshPreview(PackageCache& cache, const MeshData& mesh, const PreviewRequest& req, Image& out, std::string* error);
// Several meshes (e.g. animation frames) with one camera; textures are decoded once.
bool RenderMeshPreviews(PackageCache& cache, const std::vector<MeshData>& meshes, const PreviewRequest& req, std::vector<Image>& out, std::string* error);
