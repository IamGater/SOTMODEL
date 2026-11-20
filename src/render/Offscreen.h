#pragma once
#include "render/GpuAssets.h"

// Headless rendering for the AI tools (preview_mesh): its own D3D11 device (hardware, WARP fallback), created lazily.
struct PreviewRequest {
    int width = 768, height = 768;
    float yaw = -135, pitch = -20;   // camera orbit angles (degrees)
    int lod = 0;
    DrawOptions draw;
};

bool RenderMeshPreview(PackageCache& cache, const MeshData& mesh, const PreviewRequest& req, Image& out, std::string* error);
