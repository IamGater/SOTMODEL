#include "render/WorldScene.h"
#include "ue/Mesh.h"

static Vec4 SphereOf(const GpuMesh& m, const Mat4& w) {
    Vec3 c = w.TransformPoint(m.center);
    return {c.x, c.y, c.z, m.radius * w.MaxScale()};
}

// shallow copy of a cached mesh with some material slots replaced (vertex / index buffers are shared)
static std::shared_ptr<GpuMesh> WithMaterials(GpuAssets& assets, const std::shared_ptr<GpuMesh>& base, const std::vector<std::string>& mats) {
    if (mats.empty() || !base) return base;
    auto copy = std::make_shared<GpuMesh>(*base);
    copy->cpuVertices.clear();
    copy->gpuBytes = 0;
    for (size_t i = 0; i < mats.size() && i < copy->materials.size(); i++)
        if (!mats[i].empty()) copy->materials[i] = assets.Material(mats[i]);
    return copy;
}

void DeformSplineMesh(MeshLod& lod, const Vec3& bmin, const Vec3& bmax, const SplineMeshParams& p) {
    int ax = std::clamp(p.forwardAxis, 0, 2);
    auto get = [](const Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; };
    float lo = get(bmin, ax), hi = get(bmax, ax);
    float len = hi - lo;
    if (len < 1e-4f) len = 1;
    auto hermite = [&](float t) {
        float t2 = t * t, t3 = t2 * t;
        return p.startPos * (2 * t3 - 3 * t2 + 1) + p.startTangent * (t3 - 2 * t2 + t) + p.endPos * (-2 * t3 + 3 * t2) + p.endTangent * (t3 - t2);
    };
    auto hermiteDir = [&](float t) {
        float t2 = t * t;
        Vec3 d = p.startPos * (6 * t2 - 6 * t) + p.startTangent * (3 * t2 - 4 * t + 1) + p.endPos * (-6 * t2 + 6 * t) + p.endTangent * (3 * t2 - 2 * t);
        return Length(d) < 1e-6f ? Normalize(p.endPos - p.startPos) : Normalize(d);
    };
    for (size_t i = 0; i < lod.positions.size(); i++) {
        Vec3 v = lod.positions[i];
        float t = (get(v, ax) - lo) / len;
        Vec3 pos = hermite(t), dir = hermiteDir(t);
        Vec3 bx = Normalize(Cross(p.upDir, dir)), by = Normalize(Cross(dir, bx));
        Vec2 off{p.startOffset.x + (p.endOffset.x - p.startOffset.x) * t, p.startOffset.y + (p.endOffset.y - p.startOffset.y) * t};
        pos = pos + bx * off.x + by * off.y;
        float roll = p.startRoll + (p.endRoll - p.startRoll) * t;
        float c = cosf(roll), s = sinf(roll);
        Vec3 xv = bx * c - by * s, yv = by * c + bx * s;
        Vec2 sc{p.startScale.x + (p.endScale.x - p.startScale.x) * t, p.startScale.y + (p.endScale.y - p.startScale.y) * t};
        // the two cross-section axes of the mesh, per forward axis (USplineMeshComponent::CalcSliceTransform)
        float a, b;
        if (ax == 0) { a = v.y; b = v.z; }
        else if (ax == 1) { a = v.z; b = v.x; }
        else { a = v.x; b = v.y; }
        lod.positions[i] = pos + xv * (a * sc.x) + yv * (b * sc.y);
        if (i < lod.normals.size()) {
            Vec3 n = lod.normals[i];
            float nf = get(n, ax), na, nb;
            if (ax == 0) { na = n.y; nb = n.z; }
            else if (ax == 1) { na = n.z; nb = n.x; }
            else { na = n.x; nb = n.y; }
            lod.normals[i] = Normalize(dir * nf + xv * na + yv * nb);
        }
    }
    lod.tangents.clear();   // no longer valid; the shader skips normal mapping without tangents
}

void WorldScene::Build(const WorldData& wd, const std::function<bool()>& cancel, const std::function<void(int, int)>& progress) {
    m_items.clear();
    m_lights = wd.lights;
    m_decals = wd.decals;
    m_fogs = wd.fogs;
    m_waterLevel = wd.waterLevel;
    {   // sea plane (unit quad, scaled when drawn)
        MeshLod quad;
        quad.positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
        quad.normals.assign(4, Vec3{0, 0, 1});
        quad.indices = {0, 2, 1, 0, 3, 2};
        MeshSection sec;
        sec.numTriangles = 2;
        sec.maxVertex = 3;
        quad.sections.push_back(sec);
        m_water = m_r.CreateMesh(quad, {-1, -1, 0}, {1, 1, 0});
        if (m_water) {
            auto wm = std::make_shared<GpuMaterial>();
            wm->color[0] = 0.02f; wm->color[1] = 0.11f; wm->color[2] = 0.16f;
            wm->twoSided = true;
            m_water->materials = {wm};
        }
    }
    m_gpuBytes = 0;
    m_missing = 0;
    m_bmin = {1e30f, 1e30f, 1e30f};
    m_bmax = {-1e30f, -1e30f, -1e30f};
    int total = (int)(wd.groups.size() + wd.landscapes.size() + wd.splines.size()), done = 0;
    auto grow = [&](const Vec4& s) {
        m_bmin = {std::min(m_bmin.x, s.x - s.w), std::min(m_bmin.y, s.y - s.w), std::min(m_bmin.z, s.z - s.w)};
        m_bmax = {std::max(m_bmax.x, s.x + s.w), std::max(m_bmax.y, s.y + s.w), std::max(m_bmax.z, s.z + s.w)};
    };
    for (auto& g : wd.groups) {
        if (cancel && cancel()) return;
        if (progress && (done++ & 7) == 0) progress(done, total);
        auto mesh = WithMaterials(m_assets, m_assets.MeshByPath(g.mesh, 0), g.materials);
        if (!mesh) { m_missing++; continue; }
        Item it;
        it.mesh = mesh;
        it.drawDistance = g.drawDistance;
        it.transforms.reserve(g.instances.size());
        for (auto& in : g.instances) {
            it.transforms.push_back(in.transform);
            it.spheres.push_back(SphereOf(*mesh, in.transform));
            grow(it.spheres.back());
        }
        m_items.push_back(std::move(it));
    }
    for (auto& l : wd.landscapes) {
        if (cancel && cancel()) return;
        if (progress && (done++ & 7) == 0) progress(done, total);
        auto mesh = m_r.CreateMesh(l.mesh, l.boundsMin, l.boundsMax);
        if (!mesh) continue;
        auto mat = std::make_shared<GpuMaterial>();
        mat->color[0] = mat->color[1] = mat->color[2] = 1;
        mat->vertexColor = true;
        mesh->materials = {mat};
        m_gpuBytes += mesh->gpuBytes;
        Item it;
        it.mesh = mesh;
        it.landscape = true;
        it.transforms.push_back(Mat4::Identity());
        it.spheres.push_back(SphereOf(*mesh, Mat4::Identity()));
        grow(it.spheres.back());
        m_items.push_back(std::move(it));
    }
    for (auto& s : wd.splines) {
        if (cancel && cancel()) return;
        if (progress && (done++ & 7) == 0) progress(done, total);
        std::string err;
        auto pkg = m_cache.LoadPath(s.mesh, &err);
        MeshData md;
        size_t dot = s.mesh.rfind('.');
        i32 ex = pkg ? pkg->FindExport(dot == std::string::npos ? std::string_view{} : std::string_view(s.mesh).substr(dot + 1)) : 0;
        if (pkg && !ex) ex = pkg->MainExport();
        if (!ex || !ReadMesh(*pkg, ex - 1, md, &err) || md.lods.empty()) { m_missing++; continue; }
        MeshLod lod = md.lods[0];
        DeformSplineMesh(lod, md.boundsMin, md.boundsMax, s.params);
        Vec3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
        for (auto& p : lod.positions) {
            bmin = {std::min(bmin.x, p.x), std::min(bmin.y, p.y), std::min(bmin.z, p.z)};
            bmax = {std::max(bmax.x, p.x), std::max(bmax.y, p.y), std::max(bmax.z, p.z)};
        }
        auto mesh = m_r.CreateMesh(lod, bmin, bmax);
        if (!mesh) continue;
        for (size_t i = 0; i < md.materials.size(); i++)
            mesh->materials.push_back(m_assets.Material(i < s.materials.size() && !s.materials[i].empty() ? s.materials[i] : md.materials[i].materialPath));
        m_gpuBytes += mesh->gpuBytes;
        Item it;
        it.mesh = mesh;
        it.transforms.push_back(s.transform);
        it.spheres.push_back(SphereOf(*mesh, s.transform));
        grow(it.spheres.back());
        m_items.push_back(std::move(it));
    }
    if (m_items.empty()) m_bmin = m_bmax = {};
    // focus bounds: percentiles of the instance centres (landscape patches count by their area)
    std::vector<float> xs, ys, zs;
    for (auto& it : m_items)
        for (auto& sp : it.spheres) {
            int w = it.landscape ? 16 : 1;
            for (int k = 0; k < w; k++) { xs.push_back(sp.x); ys.push_back(sp.y); zs.push_back(sp.z); }
        }
    m_fmin = m_bmin;
    m_fmax = m_bmax;
    if (xs.size() > 20) {
        auto pct = [](std::vector<float>& v, float p) {
            size_t k = std::min(v.size() - 1, (size_t)(p * (v.size() - 1)));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        };
        m_fmin = {pct(xs, 0.02f), pct(ys, 0.02f), pct(zs, 0.02f)};
        m_fmax = {pct(xs, 0.98f), pct(ys, 0.98f), pct(zs, 0.98f)};
    }
    if (progress) progress(total, total);
}

void WorldScene::Draw(const Camera& cam, float viewHeightPx, const WorldDrawOptions& opt) {
    m_stats = {};
    const Frustum& fr = m_r.ViewFrustum();
    Vec3 eye = cam.Eye();
    float proj = 1.f / tanf(cam.fovY * kPi / 360);   // sphere radius / distance -> fraction of half the view height
    float minSize = opt.minScreenSize * 2;
    (void)viewHeightPx;
    for (auto& it : m_items) {
        if (it.landscape ? !opt.landscape : !opt.meshes) continue;
        m_visible.clear();
        float maxDist = it.drawDistance > 0 && opt.drawDistanceScale > 0 ? it.drawDistance * opt.drawDistanceScale : 0;
        for (size_t i = 0; i < it.spheres.size(); i++) {
            const Vec4& s = it.spheres[i];
            Vec3 c{s.x, s.y, s.z};
            if (!fr.SphereVisible(c, s.w)) continue;
            float d = Length(c - eye);
            if (maxDist > 0 && d - s.w > maxDist) continue;
            if (!it.landscape && d > s.w && s.w / d * proj < minSize) continue;
            m_visible.push_back(it.transforms[i]);
        }
        if (m_visible.empty()) continue;
        m_r.DrawMesh(*it.mesh, m_visible.data(), (u32)m_visible.size());
        if (it.landscape) m_stats.landscapesDrawn++;
        else {
            m_stats.meshesDrawn++;
            m_stats.instancesDrawn += (u32)m_visible.size();
        }
    }
}

void WorldScene::Lighting(const Camera& cam, const WorldDrawOptions& opt, SceneLighting& out) const {
    out = SceneLighting{};
    Vec3 eye = cam.Eye();
    if (opt.lights) {
        for (auto& l : m_lights) {
            if (l.type == WorldLight::Directional && !out.sun && l.intensity > 0) {
                out.sun = true;
                out.sunDir = l.direction;
                out.sunColor = l.color * (0.95f * std::clamp(l.intensity / 2.5f, 0.3f, 1.6f));
            } else if (l.type == WorldLight::Sky && l.intensity > 0) {
                float k = std::clamp(l.intensity / 1.2f, 0.3f, 2.f);
                out.ambientScale = Vec3{0.6f + 0.4f * l.color.x, 0.6f + 0.4f * l.color.y, 0.6f + 0.4f * l.color.z} * k;
            }
        }
        // the local lights whose range is nearest the eye
        std::vector<std::pair<float, const WorldLight*>> closest;
        for (auto& l : m_lights) {
            if (l.type > WorldLight::Spot || l.intensity <= 0 || l.radius <= 0) continue;
            float d = Length(l.position - eye) - l.radius;
            if (d < 30000) closest.push_back({d, &l});
        }
        size_t n = std::min<size_t>(closest.size(), kMaxSceneLights);
        std::partial_sort(closest.begin(), closest.begin() + n, closest.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < n; i++) {
            const WorldLight& l = *closest[i].second;
            SceneLight s;
            s.position = l.position;
            s.radius = l.radius;
            float k = l.inverseSquared ? std::clamp(l.intensity / 1000.f, 0.f, 20.f) : std::clamp(l.intensity, 0.f, 10.f);
            s.color = l.color * k;
            s.spot = l.type == WorldLight::Spot;
            s.direction = l.direction;
            s.cosOuter = cosf(std::clamp(l.outerCone, 1.f, 89.f) * kPi / 180);
            s.cosInner = cosf(std::clamp(std::min(l.innerCone, l.outerCone - 0.5f), 0.f, 89.f) * kPi / 180);
            out.lights.push_back(s);
        }
    }
    if (opt.fog) {
        bool under = eye.z < m_waterLevel;
        const WorldFog* pick = nullptr;
        for (auto& f : m_fogs)
            if (f.underwater == under) { pick = &f; break; }
        if (!pick && !under && !m_fogs.empty() && !m_fogs[0].underwater) pick = &m_fogs[0];
        if (pick) {
            out.fog = true;
            out.fogColor = pick->color;
            out.fogDensity = pick->density;
            out.fogFalloff = pick->heightFalloff;
            out.fogHeight = pick->position.z;
            out.fogStart = pick->startDistance;
            out.fogMaxOpacity = pick->maxOpacity;
        }
    }
}

void WorldScene::DrawOverlays(const Camera& cam, const WorldDrawOptions& opt) {
    Vec3 eye = cam.Eye();
    if (opt.water && m_water) {
        float half = std::max(Length(m_bmax - m_bmin), 2e6f);
        Mat4 m = Mat4::FromTRS({eye.x, eye.y, m_waterLevel}, {0, 0, 0, 1}, {half, half, 1});
        m_r.DrawMesh(*m_water, &m, 1);
    }
    if (!opt.gizmos) return;
    const float range = 30000;
    std::vector<Vec3> pts, spots, decals;
    auto circle = [](std::vector<Vec3>& v, const Vec3& c, const Vec3& u, const Vec3& w, float r, int seg) {
        for (int k = 0; k < seg; k++) {
            float a0 = 2 * kPi * k / seg, a1 = 2 * kPi * (k + 1) / seg;
            v.push_back(c + u * (cosf(a0) * r) + w * (sinf(a0) * r));
            v.push_back(c + u * (cosf(a1) * r) + w * (sinf(a1) * r));
        }
    };
    auto basis = [](const Vec3& d, Vec3& u, Vec3& w) {
        Vec3 up = fabsf(d.z) < 0.95f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        u = Normalize(Cross(d, up));
        w = Cross(d, u);
    };
    for (auto& l : m_lights) {
        if (l.type > WorldLight::Spot || Length(l.position - eye) > range) continue;
        float s = 20;
        for (Vec3 a : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) { pts.push_back(l.position - a * s); pts.push_back(l.position + a * s); }
        if (l.type == WorldLight::Point) {
            circle(pts, l.position, {1, 0, 0}, {0, 1, 0}, l.radius, 32);
        } else {
            Vec3 u, w;
            basis(l.direction, u, w);
            float len = std::min(l.radius, 2000.f);
            float r = len * tanf(std::clamp(l.outerCone, 1.f, 89.f) * kPi / 180);
            Vec3 end = l.position + l.direction * len;
            circle(spots, end, u, w, r, 24);
            for (int k = 0; k < 4; k++) {
                float a = kPi * 0.5f * k;
                spots.push_back(l.position);
                spots.push_back(end + u * (cosf(a) * r) + w * (sinf(a) * r));
            }
        }
    }
    for (auto& d : m_decals) {
        if (Length(d.transform.Translation() - eye) > range) continue;
        Vec3 c[8];
        for (int k = 0; k < 8; k++)
            c[k] = d.transform.TransformPoint({(k & 1) ? d.size.x : -d.size.x, (k & 2) ? d.size.y : -d.size.y, (k & 4) ? d.size.z : -d.size.z});
        static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& ed : e) { decals.push_back(c[ed[0]]); decals.push_back(c[ed[1]]); }
    }
    if (!pts.empty()) m_r.DrawLines(pts, 0xff40e0ffu);
    if (!spots.empty()) m_r.DrawLines(spots, 0xff40a0ffu);
    if (!decals.empty()) m_r.DrawLines(decals, 0xffffc040u);
}
