#pragma once
#include "render/Renderer.h"
#include "ue/Particles.h"

// Simulated Cascade sprites (ParticleSim) drawn by the viewport renderer as camera-facing quads.
struct ParticleLook {   // per emitter
    std::shared_ptr<GpuTexture> texture;   // null: soft dots
    bool additive = false, velocityAligned = false, visible = true;
    bool frameBlend = false;   // SubUV PSUVIM_*_Blend: cross-fade between flipbook frames
    int subH = 1, subV = 1;
    int axisLock = 0;   // ParticleEmitterInfo::axisLock
    Vec2 pivot;   // ParticlePivot
    std::shared_ptr<GpuMesh> mesh;   // mesh emitters (TypeDataMesh): one instance per particle; null draws sprites
};
// `meshes` (optional, per emitter): the loaded TypeDataMesh meshes
std::vector<ParticleLook> ParticleLooks(const ParticleSystemInfo& ps, const std::vector<std::shared_ptr<GpuTexture>>& textures,
                                        const std::vector<std::shared_ptr<GpuMesh>>& meshes = {});

// `toWorld` places the system (system space -> world, e.g. a socket). Translucent emitters are sorted back to front.
// Returns the number of sprites drawn.
int DrawParticleSprites(Renderer& r, const Camera& cam, const std::vector<ParticleSprite>& sprites, const std::vector<ParticleLook>& looks,
                        const Mat4& toWorld);
