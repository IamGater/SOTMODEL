#include "render/ParticleDraw.h"
#include <algorithm>
#include <cmath>

std::vector<ParticleLook> ParticleLooks(const ParticleSystemInfo& ps, const std::vector<std::shared_ptr<GpuTexture>>& textures,
                                        const std::vector<std::shared_ptr<GpuMesh>>& meshes) {
    std::vector<ParticleLook> looks(ps.emitters.size());
    for (size_t i = 0; i < ps.emitters.size(); i++) {
        auto& e = ps.emitters[i];
        auto& l = looks[i];
        l.texture = i < textures.size() ? textures[i] : nullptr;
        l.mesh = i < meshes.size() ? meshes[i] : nullptr;
        l.additive = e.blendMode.find("Additive") != std::string::npos;
        l.velocityAligned = e.screenAlignment.find("Velocity") != std::string::npos;
        l.subH = std::max(1, e.subH);
        l.subV = std::max(1, e.subV);
        l.pivot = ParticlePivot(e);
        l.frameBlend = e.subUVMethod.find("Blend") != std::string::npos;
    }
    return looks;
}

int DrawParticleSprites(Renderer& r, const Camera& cam, const std::vector<ParticleSprite>& sprites, const std::vector<ParticleLook>& looks,
                        const Mat4& toWorld) {
    Vec3 eye = cam.Eye(), fwd = Normalize(cam.target - eye);
    Vec3 right = Normalize(Cross({0, 0, 1}, fwd)), up = Cross(fwd, right);   // screen axes (as Renderer::BeginFrame)
    float scale = Length(toWorld.TransformDir({1, 0, 0}));
    // mesh emitters first (opaque or masked meshes under the translucent sprites): an instance per particle,
    // scaled by its size and turned by ParticleModuleMeshRotation (roll / pitch / yaw in turns)
    int drawnMeshes = 0;
    std::vector<Mat4> inst;
    for (size_t e = 0; e < looks.size(); e++) {
        if (!looks[e].mesh || !looks[e].visible) continue;
        inst.clear();
        for (auto& s : sprites)
            if (s.emitter == (int)e) {
                Vec4 q = RotatorToQuat(s.meshRot.y * 360.f, s.meshRot.z * 360.f, s.meshRot.x * 360.f);
                inst.push_back(Mat4::FromTRS(s.pos, q, {s.size.x, s.size.y, s.sizeZ}) * toWorld);
            }
        if (!inst.empty()) r.DrawMesh(*looks[e].mesh, inst.data(), (u32)inst.size());
        drawnMeshes += (int)inst.size();
    }
    struct Item { float depth; size_t i; Vec3 pos; };
    std::vector<Item> items;
    items.reserve(sprites.size());
    for (size_t i = 0; i < sprites.size(); i++) {
        int e = sprites[i].emitter;
        if (e < 0 || e >= (int)looks.size() || !looks[e].visible || looks[e].mesh) continue;
        Vec3 p = toWorld.TransformPoint(sprites[i].pos);
        items.push_back({Dot(p - eye, fwd), i, p});
    }
    // back to front; quads of one emitter that follow each other in this order are drawn in one call
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });
    std::vector<SpriteVertex> batch;
    int batchEmitter = -1;
    auto flush = [&] {
        if (batchEmitter >= 0 && !batch.empty()) r.DrawSprites(batch.data(), (u32)batch.size(), looks[batchEmitter].texture.get(), looks[batchEmitter].additive);
        batch.clear();
    };
    for (auto& it : items) {
        const ParticleSprite& s = sprites[it.i];
        const ParticleLook& l = looks[s.emitter];
        if (s.emitter != batchEmitter) {
            flush();
            batchEmitter = s.emitter;
        }
        float ax = cosf(s.rotation), ay = sinf(s.rotation);
        Vec3 ux = right * ax + up * ay, uy = up * ax - right * ay;   // rotated in the screen plane
        if (l.velocityAligned) {
            Vec3 v = toWorld.TransformDir(s.velocity);
            Vec3 vs = v - fwd * Dot(v, fwd);   // velocity projected on the screen plane
            if (Length(vs) > 1e-4f) {
                uy = Normalize(vs);
                ux = Normalize(Cross(fwd, uy));
            }
        }
        ux = ux * (s.size.x * 0.5f * scale);
        uy = uy * (s.size.y * 0.5f * scale);
        int cells = l.subH * l.subV, whole = (int)floorf(s.subImage), cell = (whole % cells + cells) % cells, next = (cell + 1) % cells;
        float u0 = (float)(cell % l.subH) / l.subH, v0 = (float)(cell / l.subH) / l.subV, du = 1.f / l.subH, dv = 1.f / l.subV;
        // *_Blend flipbooks cross-fade into the next frame by the fraction of the index
        float blend = l.frameBlend && cells > 1 ? s.subImage - floorf(s.subImage) : 0.f;
        float nu = (float)(next % l.subH) / l.subH - u0, nv = (float)(next / l.subH) / l.subV - v0;   // offset to the next frame
        if (!l.texture) u0 = v0 = 0, du = dv = 1, blend = 0;
        Vec3 o = it.pos + ux * (2 * l.pivot.x) - uy * (2 * l.pivot.y);   // pivot offset: (0, -0.5) puts the bottom edge on the particle
        Vec3 c[4] = {o - ux + uy, o + ux + uy, o + ux - uy, o - ux - uy};   // top-left, top-right, bottom-right, bottom-left
        float uv[4][2] = {{u0, v0}, {u0 + du, v0}, {u0 + du, v0 + dv}, {u0, v0 + dv}};
        static const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k : idx)
            batch.push_back({{c[k].x, c[k].y, c[k].z}, {uv[k][0], uv[k][1]}, {s.color.x, s.color.y, s.color.z, std::clamp(s.color.w, 0.f, 1.f)},
                             {uv[k][0] + nu, uv[k][1] + nv, blend}});
    }
    flush();
    return (int)items.size() + drawnMeshes;
}
