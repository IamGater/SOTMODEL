#include "ue/Particles.h"
#include "ue/Texture.h"
#include "ue/Material.h"
#include "core/Common.h"
#include <algorithm>
#include <cmath>

namespace {

std::string Obj(const Package& pkg, const PropValue* v) {
    if (!v) return {};
    i32 i = v->ObjIndex();
    if (i == 0) return {};
    return i > 0 ? pkg.PackageName() + "." + pkg.ObjectName(i) : pkg.FullPath(i);
}
float F(const PropValue* v, float def = 0) { return v ? (float)v->AsFloat(def) : def; }
Vec3 V3(const PropValue* v, Vec3 def = {}) {
    if (!v || !v->Fields()) return def;
    return {F(v->Field("X"), def.x), F(v->Field("Y"), def.y), F(v->Field("Z"), def.z)};
}
std::string EnumTail(const std::string& e) {
    size_t k = e.rfind("::");
    return k == std::string::npos ? e : e.substr(k + 2);
}
std::string ShortObj(const std::string& path) {
    size_t s = path.find_last_of("./");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Values of a curve point's OutVal: float, vector, or {v1, v2} (uniform curves).
void PointValues(const PropValue* out, std::vector<float>& lo, std::vector<float>& hi) {
    lo.clear();
    hi.clear();
    if (!out) return;
    if (!out->Fields()) { lo.push_back(F(out)); hi = lo; return; }
    auto vec = [&](const PropValue* v, std::vector<float>& dst) {
        if (!v) return;
        if (!v->Fields()) { dst.push_back(F(v)); return; }
        if (v->Field("X") || v->Field("Y") || v->Field("Z")) {
            dst.push_back(F(v->Field("X")));
            dst.push_back(F(v->Field("Y")));
            if (v->Field("Z") || v->str == "Vector") dst.push_back(F(v->Field("Z")));
        }
    };
    if (out->Field("v1") || out->Field("v2")) {
        vec(out->Field("v1"), lo);
        vec(out->Field("v2"), hi);
        if (lo.empty()) lo.assign(hi.size(), 0.f);
        if (hi.empty()) hi.assign(lo.size(), 0.f);
    } else {
        vec(out, lo);
        hi = lo;
    }
}

// Fallback when a raw distribution has no baked table: read the distribution object.
bool CurveFromDistribution(const Package& pkg, i32 obj, ParticleCurve& c) {
    if (obj <= 0) return false;
    std::string cls = pkg.ClassName(obj);
    PropertyList p = pkg.ReadProperties((size_t)obj - 1);
    bool vector = cls.find("Vector") != std::string::npos;
    int dims = vector ? 3 : 1;
    c = ParticleCurve{};
    c.source = cls;
    c.dims = dims;
    auto put = [&](const PropValue* v, std::vector<float>& dst) {
        if (vector) { Vec3 x = V3(v); dst.insert(dst.end(), {x.x, x.y, x.z}); }
        else dst.push_back(F(v));
    };
    if (cls.find("Uniform") != std::string::npos && cls.find("Curve") == std::string::npos) {
        c.op = 2;
        c.count = 1;
        std::vector<float> lo, hi;
        put(FindValue(p, "Min"), lo);
        put(FindValue(p, "Max"), hi);
        c.values = lo;
        c.values.insert(c.values.end(), hi.begin(), hi.end());
        return true;
    }
    if (cls.find("Curve") != std::string::npos) {
        const PropValue* curve = FindValue(p, "ConstantCurve");
        const PropValue* pts = curve && curve->Fields() ? curve->Field("Points") : nullptr;
        bool uniform = cls.find("Uniform") != std::string::npos;
        c.op = uniform ? 2 : 1;
        if (!pts || pts->type != PropType::Array || pts->items.empty()) {
            c.count = 1;
            c.values.assign((size_t)dims * (uniform ? 2 : 1), 0.f);
            return true;
        }
        // resample the (linear) keys into 16 entries over the key range
        std::vector<float> in;
        std::vector<std::vector<float>> lo, hi;
        for (auto& k : pts->items) {
            if (!k.Fields()) continue;
            in.push_back(F(k.Field("InVal")));
            std::vector<float> a, b;
            PointValues(k.Field("OutVal"), a, b);
            a.resize(dims, a.empty() ? 0.f : a.back());
            b.resize(dims, b.empty() ? 0.f : b.back());
            lo.push_back(a);
            hi.push_back(b);
        }
        if (in.empty()) return false;
        float t0 = in.front(), t1 = in.back();
        int n = in.size() == 1 || t1 <= t0 ? 1 : 16;
        c.count = n;
        c.timeBias = t0;
        c.timeScale = n > 1 ? (n - 1) / (t1 - t0) : 0;
        for (int e = 0; e < n; e++) {
            float t = n > 1 ? t0 + (t1 - t0) * e / (n - 1) : t0;
            size_t k = 0;
            while (k + 1 < in.size() && in[k + 1] < t) k++;
            size_t k2 = std::min(k + 1, in.size() - 1);
            float a = k2 != k && in[k2] > in[k] ? std::clamp((t - in[k]) / (in[k2] - in[k]), 0.f, 1.f) : 0.f;
            for (int d = 0; d < dims; d++) c.values.push_back(lo[k][d] + (lo[k2][d] - lo[k][d]) * a);
            if (uniform)
                for (int d = 0; d < dims; d++) c.values.push_back(hi[k][d] + (hi[k2][d] - hi[k][d]) * a);
        }
        return true;
    }
    // constants (and particle parameters: their default)
    c.op = 1;
    c.count = 1;
    const PropValue* v = FindValue(p, "Constant");
    if (!v) v = FindValue(p, "MinOutput");
    put(v, c.values);
    return true;
}

bool ReadCurve(const Package& pkg, const PropValue& raw, ParticleCurve& c) {
    const PropValue* t = raw.Field("Table");
    if (t && t->Fields()) {
        c = ParticleCurve{};
        c.source = "table";
        c.op = (int)F(t->Field("Op"));
        c.count = (int)F(t->Field("EntryCount"));
        int stride = (int)F(t->Field("EntryStride")), sub = (int)F(t->Field("SubEntryStride"));
        c.timeScale = F(t->Field("TimeScale"));
        c.timeBias = F(t->Field("TimeBias"));
        if (auto vs = t->Field("Values"); vs && vs->type == PropType::Array)
            for (auto& x : vs->items) c.values.push_back((float)x.AsFloat());
        c.dims = c.op == 1 ? stride : sub;
        if (c.op >= 2 && c.dims <= 0) c.dims = stride / 2;
        if (c.count > 0 && c.dims > 0 && (int)c.values.size() >= c.count * stride) {
            if (stride != (c.op == 1 ? c.dims : 2 * c.dims)) {   // repack
                std::vector<float> v;
                for (int e = 0; e < c.count; e++)
                    for (int k = 0; k < (c.op == 1 ? c.dims : 2 * c.dims); k++) v.push_back(c.values[(size_t)e * stride + k]);
                c.values = v;
            }
            return true;
        }
    }
    const PropValue* d = raw.Field("Distribution");
    return d && CurveFromDistribution(pkg, d->ObjIndex(), c);
}

bool IsRawDistribution(const PropValue& v) { return v.Fields() && (v.Field("Table") || v.Field("Distribution")); }

std::string SimpleValue(const Package& pkg, const PropValue& v) {
    switch (v.type) {
    case PropType::Object: return ShortObj(Obj(pkg, &v));
    case PropType::Bool: return v.b ? "true" : "false";
    case PropType::Array: return Fmt("[%zu]", v.items.size());
    default: break;
    }
    if (v.Fields()) {
        if (v.Field("X") && v.Field("Y")) {
            Vec3 x = V3(&v);
            return Fmt("(%g, %g, %g)", x.x, x.y, x.z);
        }
        return "{...}";
    }
    if (!v.str.empty()) return EnumTail(v.str);
    return Fmt("%g", v.AsFloat());
}

const char* kSimulated[] = {"ParticleModuleRequired", "ParticleModuleSpawn", "ParticleModuleLifetime", "ParticleModuleSize", "ParticleModuleSizeScale",
                            "ParticleModuleSizeMultiplyLife", "ParticleModuleColor", "ParticleModuleColorOverLife", "ParticleModuleColorScaleOverLife",
                            "ParticleModuleVelocity", "ParticleModuleVelocityCone", "ParticleModuleAcceleration", "ParticleModuleAccelerationConstant",
                            "ParticleModuleAccelerationDrag", "ParticleModuleLocation", "ParticleModuleLocationPrimitiveSphere",
                            "ParticleModuleLocationPrimitiveCylinder", "ParticleModuleRotation", "ParticleModuleRotationRate", "ParticleModuleSubUV",
                            "ParticleModuleMeshRotation", "ParticleModuleTypeDataGpu", "ParticleModuleTypeDataMesh"};

ParticleModuleInfo ReadModule(const Package& pkg, i32 obj) {
    ParticleModuleInfo m;
    m.cls = pkg.ClassName(obj);
    m.name = obj > 0 ? pkg.Exports()[obj - 1].objectName : pkg.ObjectName(obj);
    for (auto* s : kSimulated) m.simulated |= m.cls == s;
    if (obj <= 0) return m;
    for (auto& p : pkg.ReadProperties((size_t)obj - 1)) {
        std::string key = p.arrayIndex ? Fmt("%s[%d]", p.name.c_str(), p.arrayIndex) : p.name;
        if (p.name == "bEnabled") { m.enabled = p.value.AsBool(true); continue; }
        if (p.name == "LODValidity") continue;
        if (IsRawDistribution(p.value)) {
            if (p.name.rfind("Additional", 0) == 0) continue;   // SoT per-group overrides (groups B..F)
            ParticleCurve c;
            if (ReadCurve(pkg, p.value, c)) m.curves.push_back({key, std::move(c)});
            continue;
        }
        m.values.push_back({key, SimpleValue(pkg, p.value)});
    }
    return m;
}

}   // namespace

// ------------------------------------------------------------------ curves

float ParticleCurve::Eval(float t, int ch, float rnd) const {
    if (!Valid() || ch >= dims) return 0;
    int stride = op == 1 ? dims : 2 * dims;
    float x = std::max(0.f, (t - timeBias) * timeScale);
    int i = std::min((int)x, count - 1), j = std::min(i + 1, count - 1);
    float a = count > 1 ? std::clamp(x - (int)x, 0.f, 1.f) : 0;
    auto at = [&](int e, int k) { size_t idx = (size_t)e * stride + k; return idx < values.size() ? values[idx] : 0.f; };
    float lo = at(i, ch) + (at(j, ch) - at(i, ch)) * a;
    if (op == 1) return lo;
    float hi = at(i, dims + ch) + (at(j, dims + ch) - at(i, dims + ch)) * a;
    if (op == 3) return rnd < 0.5f ? lo : hi;
    return lo + (hi - lo) * rnd;
}

Vec3 ParticleCurve::Eval3(float t, float rnd) const {
    if (dims == 1) { float v = Eval(t, 0, rnd); return {v, v, v}; }
    return {Eval(t, 0, rnd), Eval(t, 1, rnd), Eval(t, 2, rnd)};
}

void ParticleCurve::Range(int ch, float& mn, float& mx) const {
    mn = 1e30f;
    mx = -1e30f;
    int stride = op == 1 ? dims : 2 * dims;
    for (int e = 0; e < count; e++)
        for (int k = ch; k < stride; k += dims) {
            size_t idx = (size_t)e * stride + k;
            if (idx < values.size()) { mn = std::min(mn, values[idx]); mx = std::max(mx, values[idx]); }
        }
    if (mn > mx) mn = mx = 0;
}

std::string ParticleCurve::Describe() const {
    if (!Valid()) return "?";
    std::string s;
    for (int ch = 0; ch < dims; ch++) {
        float mn, mx;
        Range(ch, mn, mx);
        if (!s.empty()) s += ", ";
        s += mn == mx ? Fmt("%g", mn) : Fmt("%g..%g", mn, mx);
    }
    if (dims > 1) s = "(" + s + ")";
    if (count > 1 && timeScale > 0) {
        float t0 = timeBias, t1 = timeBias + (count - 1) / timeScale;   // key range, when it is not the usual 0..1
        s = (fabsf(t0) > 1e-3f || fabsf(t1 - 1) > 1e-3f ? Fmt("curve %d over %g..%g: ", count, t0, t1) : Fmt("curve %d: ", count)) + s;
    } else if (count > 1) s = Fmt("curve %d: ", count) + s;
    else if (op == 2) s = "random " + s;
    else if (op == 3) s = "min or max " + s;
    return s;
}

const ParticleCurve* ParticleModuleInfo::Curve(std::string_view n) const {
    for (auto& c : curves)
        if (c.first == n) return &c.second;
    return nullptr;
}
std::string ParticleModuleInfo::Value(std::string_view n) const {
    for (auto& v : values)
        if (v.first == n) return v.second;
    return {};
}

// ------------------------------------------------------------------ reading

bool ReadParticleSystem(PackageCache& cache, const Package& pkg, size_t exportIdx, ParticleSystemInfo& ps, std::string* error) {
    ps = ParticleSystemInfo{};
    ps.path = pkg.FullPath((i32)exportIdx + 1);
    PropertyList p = pkg.ReadProperties(exportIdx);
    if (auto ld = FindValue(p, "LODDistances"); ld && ld->type == PropType::Array)
        for (auto& x : ld->items) ps.lodDistances.push_back((float)x.AsFloat());
    ps.warmup = (float)PropFloat(p, "WarmupTime");
    if (auto b = FindValue(p, "FixedRelativeBoundingBox"); b && b->Fields()) {
        ps.boundsMin = V3(b->Field("Min"));
        ps.boundsMax = V3(b->Field("Max"));
        ps.fixedBounds = PropBool(p, "bUseFixedRelativeBoundingBox");
    }
    const PropValue* em = FindValue(p, "Emitters");
    if (!em || em->type != PropType::Array) { if (error) *error = "no emitters"; return false; }
    for (auto& ev : em->items) {
        i32 eo = ev.ObjIndex();
        if (eo <= 0) continue;
        ParticleEmitterInfo e;
        e.cls = pkg.ClassName(eo);
        e.type = e.cls.find("Mesh") != std::string::npos ? "Mesh" : "Sprite";
        PropertyList ep = pkg.ReadProperties((size_t)eo - 1);
        e.name = PropStr(ep, "EmitterName");
        if (e.name.empty()) e.name = pkg.Exports()[eo - 1].objectName;
        const PropValue* lods = FindValue(ep, "LODLevels");
        i32 lo = lods && lods->type == PropType::Array && !lods->items.empty() ? lods->items[0].ObjIndex() : 0;
        if (lo <= 0) continue;
        PropertyList lp = pkg.ReadProperties((size_t)lo - 1);
        e.enabled = PropBool(lp, "bEnabled", true);
        e.peakParticles = (int)PropInt(lp, "PeakActiveParticles");
        std::vector<i32> mods;
        for (const char* n : {"RequiredModule", "SpawnModule", "TypeDataModule"})
            if (i32 o = PropObj(lp, n)) mods.push_back(o);
        if (auto ms = FindValue(lp, "Modules"); ms && ms->type == PropType::Array)
            for (auto& x : ms->items)
                if (x.ObjIndex()) mods.push_back(x.ObjIndex());
        for (i32 o : mods) {
            ParticleModuleInfo m = ReadModule(pkg, o);
            if (m.cls == "ParticleModuleRequired") {
                PropertyList rp = pkg.ReadProperties((size_t)o - 1);
                e.material = Obj(pkg, FindValue(rp, "Material"));
                e.screenAlignment = EnumTail(PropStr(rp, "ScreenAlignment"));
                if (e.screenAlignment.empty()) e.screenAlignment = "PSA_Square";
                e.localSpace = PropBool(rp, "bUseLocalSpace");
                e.loops = (int)PropInt(rp, "EmitterLoops", 0);
                e.duration = (float)PropFloat(rp, "EmitterDuration", 1);
                if (auto c = m.Curve("LoopTime")) e.loopTime = *c;
                if (auto c = m.Curve("LoopTimeForSubUV"); c && PropBool(rp, "bSeparateLoopTimeForSubUV")) e.subUVLoopTime = *c;
                e.origin = V3(FindValue(rp, "EmitterOrigin"));
                e.delay = (float)PropFloat(rp, "EmitterDelay");
                e.subH = std::max(1, (int)PropInt(rp, "SubImages_Horizontal", 1));
                e.subV = std::max(1, (int)PropInt(rp, "SubImages_Vertical", 1));
                e.subUVMethod = EnumTail(PropStr(rp, "InterpolationMethod"));
                if (auto c = m.Curve("SpawnRate")) e.rate = *c;
            } else if (m.cls == "ParticleModuleSpawn") {
                PropertyList sp = pkg.ReadProperties((size_t)o - 1);
                if (auto c = m.Curve("Rate")) {
                    // Required.SpawnRate and Spawn.Rate both spawn; keep the larger constant one
                    if (!e.rate.Valid() || c->Eval(0) > e.rate.Eval(0)) e.rate = *c;
                }
                if (auto c = m.Curve("RateScale")) e.rateScale = *c;
                if (auto bl = FindValue(sp, "BurstList"); bl && bl->type == PropType::Array)
                    for (auto& b : bl->items)
                        if (b.Fields()) e.bursts.push_back({(int)F(b.Field("Count")), (int)F(b.Field("CountLow"), -1), F(b.Field("Time"))});
                std::sort(e.bursts.begin(), e.bursts.end(), [](auto& a, auto& b) { return a.time < b.time; });
            } else if (m.cls.rfind("ParticleModuleTypeData", 0) == 0) {
                std::string t = m.cls.substr(22);
                PropertyList tp = pkg.ReadProperties((size_t)o - 1);
                if (t == "Mesh") { e.type = "Mesh"; e.mesh = Obj(pkg, FindValue(tp, "Mesh")); }
                else if (t == "Gpu") e.type = "GPU sprites";
                else if (t.find("Beam") == 0) e.type = "Beam";
                else if (t.find("Ribbon") == 0) e.type = "Ribbon";
                else if (t.find("AnimTrail") == 0) e.type = "AnimTrail";
                else e.type = t;
            }
            e.modules.push_back(std::move(m));
        }
        if (!e.material.empty()) {
            MaterialInfo mi;
            if (ReadMaterial(cache, e.material, mi)) e.blendMode = EnumTail(mi.blendMode);
        }
        ps.emitters.push_back(std::move(e));
    }
    if (ps.emitters.empty() && error) *error = "no readable emitters";
    return !ps.emitters.empty();
}

// ------------------------------------------------------------------ simulation

float ParticleSim::Rnd() { return std::uniform_real_distribution<float>(0.f, 1.f)(m_rng); }

void ParticleSim::Init(const ParticleSystemInfo& ps, u32 seed) {
    m_ps = &ps;
    m_seed = seed;
    Reset();
}

void ParticleSim::Reset() {
    m_rng.seed(m_seed);
    m_time = 0;
    m_emitters.clear();
    if (!m_ps) return;
    for (auto& e : m_ps->emitters) {
        Emitter x;
        x.info = &e;
        x.done = !e.enabled;
        m_emitters.push_back(std::move(x));
    }
}

int ParticleSim::Count() const {
    int n = 0;
    for (auto& e : m_emitters) n += (int)e.parts.size();
    return n;
}

void ParticleSim::Spawn(Emitter& em, float et, int n) {
    const ParticleEmitterInfo& e = *em.info;
    for (int k = 0; k < n && em.parts.size() < 4000; k++) {
        Particle p;
        p.baseSize = {1, 1, 1};
        p.baseColor = {1, 1, 1};
        p.rnd = Rnd();
        p.subRnd = Rnd();
        p.pos = e.origin;
        if (e.loopTime.Valid()) p.loopTime = e.loopTime.Eval(et, 0, Rnd());
        p.subLoopTime = p.loopTime;   // SubImageIndex tables run over the particle loop (indices past the grid wrap)
        for (auto& m : e.modules) {
            if (!m.enabled) continue;
            const std::string& c = m.cls;
            auto cv = [&](const char* name) { return m.Curve(name); };
            if (c == "ParticleModuleLifetime") {
                if (auto x = cv("Lifetime")) {
                    p.life = x->Eval(et, 0, Rnd());
                    if (p.life <= 0) p.life = 1e9f;   // 0 = lives until the emitter dies
                }
            } else if (c == "ParticleModuleSize") {
                if (auto x = cv("StartSize")) p.baseSize = x->Eval3(et, Rnd());
            } else if (c == "ParticleModuleColor") {
                if (auto x = cv("StartColor")) p.baseColor = x->Eval3(et, Rnd());
                if (auto x = cv("StartAlpha")) p.baseAlpha = x->Eval(et, 0, Rnd());
            } else if (c == "ParticleModuleVelocity") {
                if (auto x = cv("StartVelocity")) p.vel = p.vel + x->Eval3(et, Rnd());
                if (auto x = cv("StartVelocityRadial")) {
                    float r = x->Eval(et, 0, Rnd()), l = Length(p.pos);
                    if (l > 1e-3f) p.vel = p.vel + p.pos * (r / l);
                }
            } else if (c == "ParticleModuleVelocityCone") {
                float ang = cv("Angle") ? cv("Angle")->Eval(et, 0, Rnd()) * kPi : 0;   // 0..1 = 0..180 degrees
                float speed = cv("Velocity") ? cv("Velocity")->Eval(et, 0, Rnd()) : 0;
                Vec3 dir{0, 0, 1};
                std::string d = m.Value("Direction");
                float dx = 0, dy = 0, dz = 1;
                if (!d.empty() && sscanf(d.c_str(), "(%f, %f, %f)", &dx, &dy, &dz) == 3 && (dx || dy || dz)) dir = Normalize(Vec3{dx, dy, dz});
                Vec3 a = fabsf(dir.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                Vec3 u = Normalize(Cross(dir, a)), v = Cross(dir, u);
                float th = Rnd() * ang, ph = Rnd() * 2 * kPi;
                Vec3 out = dir * cosf(th) + (u * cosf(ph) + v * sinf(ph)) * sinf(th);
                p.vel = p.vel + out * speed;
            } else if (c == "ParticleModuleAcceleration") {
                if (auto x = cv("Acceleration")) p.accel = p.accel + x->Eval3(et, Rnd());
            } else if (c == "ParticleModuleAccelerationConstant") {
                std::string a = m.Value("Acceleration");
                float ax = 0, ay = 0, az = 0;
                if (sscanf(a.c_str(), "(%f, %f, %f)", &ax, &ay, &az) == 3) p.accel = p.accel + Vec3{ax, ay, az};
            } else if (c == "ParticleModuleAccelerationDrag") {
                if (auto x = cv("DragCoefficientRaw")) p.drag = x->Eval(et, 0, Rnd());
                else if (auto x2 = cv("DragCoefficient")) p.drag = x2->Eval(et, 0, Rnd());
            } else if (c == "ParticleModuleLocation") {
                if (auto x = cv("StartLocation")) p.pos = p.pos + x->Eval3(et, Rnd());
            } else if (c == "ParticleModuleLocationPrimitiveSphere" || c == "ParticleModuleLocationPrimitiveCylinder") {
                bool sphere = c.find("Sphere") != std::string::npos;
                float r = cv("StartRadius") ? cv("StartRadius")->Eval(et, 0, Rnd()) : 50;
                Vec3 off;
                if (sphere) {
                    Vec3 d;
                    do d = {Rnd() * 2 - 1, Rnd() * 2 - 1, Rnd() * 2 - 1};
                    while (Dot(d, d) > 1 || Dot(d, d) < 1e-6f);
                    off = m.Value("SurfaceOnly") == "true" ? Normalize(d) * r : d * r;
                } else {
                    float h = cv("StartHeight") ? cv("StartHeight")->Eval(et, 0, Rnd()) : 50;
                    float a = Rnd() * 2 * kPi, rr = r * sqrtf(Rnd());
                    off = {cosf(a) * rr, sinf(a) * rr, (Rnd() - 0.5f) * h};
                }
                if (auto x = cv("StartLocation")) off = off + x->Eval3(et, Rnd());
                p.pos = p.pos + off;
                if (m.Value("Velocity") == "true") {
                    float vs = cv("VelocityScale") ? cv("VelocityScale")->Eval(et, 0, Rnd()) : 1;
                    p.vel = p.vel + off * vs;
                }
            } else if (c == "ParticleModuleRotation") {
                if (auto x = cv("StartRotation")) p.rot = x->Eval(et, 0, Rnd()) * 2 * kPi;   // turns
            } else if (c == "ParticleModuleRotationRate") {
                if (auto x = cv("StartRotationRate")) p.rotRate = x->Eval(et, 0, Rnd()) * 2 * kPi;
            }
        }
        em.parts.push_back(p);
    }
}

void ParticleSim::Step(float dt) {
    m_time += dt;
    for (auto& em : m_emitters) {
        const ParticleEmitterInfo& e = *em.info;
        // update particles
        for (size_t i = 0; i < em.parts.size();) {
            Particle& p = em.parts[i];
            p.age += dt;
            if (p.age >= p.life) { em.parts[i] = em.parts.back(); em.parts.pop_back(); continue; }
            p.vel = p.vel + p.accel * dt;
            if (p.drag > 0) p.vel = p.vel * std::max(0.f, 1 - p.drag * dt);
            p.pos = p.pos + p.vel * dt;
            p.rot += p.rotRate * dt;
            i++;
        }
        if (em.done) continue;
        // emitter time / loops
        float local = m_time - e.delay;
        if (local < 0) continue;
        float dur = std::max(0.01f, e.duration);
        int loop = (int)(local / dur);
        while (em.loop < loop && !em.done) {   // finish the bursts of the loops passed in this step (short / zero duration emitters)
            while (em.burstNext < e.bursts.size()) {
                const auto& b = e.bursts[em.burstNext++];
                Spawn(em, b.time, b.countLow >= 0 ? b.countLow + (int)(Rnd() * (b.count - b.countLow + 1)) : b.count);
            }
            em.loop++;
            em.burstNext = 0;
            if (e.loops > 0 && em.loop >= e.loops) em.done = true;
        }
        if (em.done) continue;
        float et = fmodf(local, dur) / dur;
        // bursts (Time is 0..1 of the loop)
        while (em.burstNext < e.bursts.size() && e.bursts[em.burstNext].time <= et) {
            const auto& b = e.bursts[em.burstNext++];
            int n = b.countLow >= 0 ? b.countLow + (int)(Rnd() * (b.count - b.countLow + 1)) : b.count;
            Spawn(em, et, n);
        }
        float rate = e.rate.Valid() ? e.rate.Eval(et, 0, Rnd()) : 0;
        if (e.rateScale.Valid()) rate *= e.rateScale.Eval(et, 0, 0.5f);
        em.spawnAcc += std::max(0.f, rate) * dt;
        int n = (int)em.spawnAcc;
        em.spawnAcc -= n;
        if (n > 0) Spawn(em, et, n);
    }
}

void ParticleSim::AdvanceTo(float seconds) {
    if (seconds < m_time) Reset();
    const float dt = 1.f / 30;
    while (m_time + dt <= seconds + 1e-4f) Step(dt);
}

void ParticleSim::Sprites(std::vector<ParticleSprite>& out) const {
    out.clear();
    for (size_t ei = 0; ei < m_emitters.size(); ei++) {
        const Emitter& em = m_emitters[ei];
        const ParticleEmitterInfo& e = *em.info;
        for (auto& p : em.parts) {
            float rel = p.life < 1e8f ? std::clamp(p.age / p.life, 0.f, 1.f) : p.loopTime > 0 ? fmodf(p.age, p.loopTime) / p.loopTime : 0.f;
            float subRel = p.life >= 1e8f && p.subLoopTime > 0 ? fmodf(p.age, p.subLoopTime) / p.subLoopTime : rel;
            Vec3 size = p.baseSize, col = p.baseColor;
            float alpha = p.baseAlpha, sub = 0;
            for (auto& m : e.modules) {
                if (!m.enabled) continue;
                const std::string& c = m.cls;
                if (c == "ParticleModuleSizeScale") {
                    if (auto x = m.Curve("SizeScale")) { Vec3 s = x->Eval3(rel, p.rnd); size = {size.x * s.x, size.y * s.y, size.z * s.z}; }
                } else if (c == "ParticleModuleSizeMultiplyLife") {
                    if (auto x = m.Curve("LifeMultiplier")) { Vec3 s = x->Eval3(rel, p.rnd); size = {size.x * s.x, size.y * s.y, size.z * s.z}; }
                } else if (c == "ParticleModuleColorOverLife") {
                    if (auto x = m.Curve("ColorOverLife")) col = x->Eval3(rel, p.rnd);
                    if (auto x = m.Curve("AlphaOverLife")) alpha = x->Eval(rel, 0, p.rnd);
                } else if (c == "ParticleModuleColorScaleOverLife") {
                    if (auto x = m.Curve("ColorScaleOverLife")) { Vec3 s = x->Eval3(rel, p.rnd); col = {col.x * s.x, col.y * s.y, col.z * s.z}; }
                    if (auto x = m.Curve("AlphaScaleOverLife")) alpha *= x->Eval(rel, 0, p.rnd);
                } else if (c == "ParticleModuleSubUV") {
                    if (auto x = m.Curve("SubImageIndex")) sub = x->Eval(subRel, 0, p.subRnd);
                }
            }
            ParticleSprite s;
            s.pos = p.pos;
            s.velocity = p.vel;
            s.size = {fabsf(size.x), fabsf(size.y) > 0 ? fabsf(size.y) : fabsf(size.x)};
            s.color = {std::max(0.f, col.x), std::max(0.f, col.y), std::max(0.f, col.z), std::clamp(alpha, 0.f, 1.f)};
            s.rotation = p.rot;
            s.subImage = e.subUVMethod == "PSUVIM_Random" || e.subUVMethod == "PSUVIM_Random_Blend" ? floorf(p.subRnd * e.subH * e.subV) : sub;
            s.emitter = (int)ei;
            out.push_back(s);
        }
    }
}

// ------------------------------------------------------------------ software sprite rendering

void ParticleCamera(Vec3 target, float distance, float yawDeg, float pitchDeg, Vec3& eye, Vec3& right, Vec3& up, Vec3& fwd) {
    float y = yawDeg * kPi / 180, p = pitchDeg * kPi / 180;
    fwd = {cosf(p) * cosf(y), cosf(p) * sinf(y), sinf(p)};
    right = {-sinf(y), cosf(y), 0};
    up = Cross(fwd, right);
    eye = target - fwd * distance;
}

void RenderParticleSprites(const std::vector<ParticleSprite>& sprites, const std::vector<ParticleRenderEmitter>& emitters, Vec3 target, float distance,
                           float yawDeg, float pitchDeg, int W, int H, Image& out) {
    out.width = W;
    out.height = H;
    std::vector<float> acc((size_t)W * H * 3);
    for (int y = 0; y < H; y++)   // dark gradient background
        for (int x = 0; x < W; x++) {
            float g = 0.10f + 0.06f * (1 - (float)y / H);
            float* px = &acc[((size_t)y * W + x) * 3];
            px[0] = g; px[1] = g * 1.05f; px[2] = g * 1.2f;
        }
    Vec3 eye, right, up, fwd;
    ParticleCamera(target, distance, yawDeg, pitchDeg, eye, right, up, fwd);
    float focal = H / (2 * tanf(30 * kPi / 180));
    // back to front
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < sprites.size(); i++) order.push_back({Dot(sprites[i].pos - eye, fwd), i});
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [depth, i] : order) {
        if (depth < 1) continue;
        const ParticleSprite& s = sprites[i];
        const ParticleRenderEmitter* re = s.emitter < (int)emitters.size() ? &emitters[s.emitter] : nullptr;
        Vec3 v = s.pos - eye;
        float cx = W / 2 + Dot(v, right) / depth * focal, cy = H / 2 - Dot(v, up) / depth * focal;
        float hx = s.size.x * 0.5f / depth * focal, hy = s.size.y * 0.5f / depth * focal;
        // screen axes of the quad: rotated, or along the velocity for velocity-aligned sprites
        float ax = cosf(s.rotation), ay = sinf(s.rotation);
        if (re && re->alignment.find("Velocity") != std::string::npos && Length(s.velocity) > 1e-3f) {
            float vx = Dot(s.velocity, right), vy = -Dot(s.velocity, up), l = sqrtf(vx * vx + vy * vy);
            if (l > 1e-4f) { ax = vy / l; ay = -vx / l; }
        }
        float r = sqrtf(hx * hx + hy * hy) + 1;
        int x0 = std::max(0, (int)(cx - r)), x1 = std::min(W - 1, (int)(cx + r)), y0 = std::max(0, (int)(cy - r)), y1 = std::min(H - 1, (int)(cy + r));
        if (x0 > x1 || y0 > y1 || hx < 0.05f) continue;
        int sh = re ? re->subH : 1, sv = re ? re->subV : 1, cells = sh * sv;
        int cell = ((int)s.subImage % cells + cells) % cells;
        float cu = (float)(cell % sh) / sh, cvv = (float)(cell / sh) / sv;
        const Image* tex = re ? re->texture : nullptr;
        bool add = re && re->additive;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float u = (dx * ax + dy * ay) / (2 * hx) + 0.5f, w = (-dx * ay + dy * ax) / (2 * std::max(hy, 0.05f)) + 0.5f;
                if (u < 0 || u >= 1 || w < 0 || w >= 1) continue;
                float tr = 1, tg = 1, tb = 1, ta;
                if (tex && tex->width > 0) {
                    int tx = std::clamp((int)((cu + u / sh) * tex->width), 0, tex->width - 1), ty = std::clamp((int)((cvv + w / sv) * tex->height), 0, tex->height - 1);
                    const u8* t = &tex->rgba[((size_t)ty * tex->width + tx) * 4];
                    tr = t[0] / 255.f; tg = t[1] / 255.f; tb = t[2] / 255.f; ta = t[3] / 255.f;
                } else {
                    float d = (u - 0.5f) * (u - 0.5f) + (w - 0.5f) * (w - 0.5f);   // soft disc
                    ta = std::max(0.f, 1 - d * 4);
                }
                float a = ta * s.color.w;
                float* px = &acc[((size_t)y * W + x) * 3];
                float sr = tr * s.color.x, sg = tg * s.color.y, sb = tb * s.color.z;
                if (add) { px[0] += sr * a; px[1] += sg * a; px[2] += sb * a; }
                else { px[0] += (sr - px[0]) * a; px[1] += (sg - px[1]) * a; px[2] += (sb - px[2]) * a; }
            }
    }
    out.rgba.resize((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        for (int k = 0; k < 3; k++) out.rgba[i * 4 + k] = (u8)std::clamp(powf(std::max(0.f, acc[i * 3 + k]), 1 / 1.0f) * 255.f, 0.f, 255.f);
        out.rgba[i * 4 + 3] = 255;
    }
}

std::string ParticleTexturePath(PackageCache& cache, const std::string& material) {
    MaterialInfo mi;
    if (material.empty() || !ReadMaterial(cache, material, mi)) return {};
    if (!mi.diffuse.empty()) return mi.diffuse;
    if (!mi.emissive.empty()) return mi.emissive;
    for (auto& t : mi.textures)
        if (!t.texturePath.empty() && ClassifyTexture(t.name, t.texturePath) != "normal") return t.texturePath;
    return {};
}

bool LoadParticleTexture(PackageCache& cache, const std::string& material, int maxDim, Image& out) {
    std::string path = ParticleTexturePath(cache, material);
    if (path.empty()) return false;
    auto pkg = cache.LoadPath(path);
    i32 main = pkg ? pkg->MainExport() : 0;
    TextureInfo ti;
    if (!main || !ReadTextureInfo(*pkg, main - 1, ti) || !DecodeTexture(*pkg, ti, -1, maxDim, out)) return false;
    // VFX masks usually have no alpha: use the brightness as coverage and let the particle colour tint it
    bool opaque = true;
    for (size_t i = 3; i < out.rgba.size() && opaque; i += 4) opaque = out.rgba[i] >= 250;
    if (opaque)
        for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
            u8 l = std::max({out.rgba[i], out.rgba[i + 1], out.rgba[i + 2]});
            out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 255;
            out.rgba[i + 3] = l;
        }
    return true;
}

// Camera framing: bounds of the sprites over a few sample times.
void ParticleFrame(const ParticleSystemInfo& ps, float upTo, Vec3& centre, float& dist) {
    ParticleSim sim;
    sim.Init(ps);
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    std::vector<ParticleSprite> sp;
    int n = 0;
    for (int i = 1; i <= 12; i++) {
        sim.AdvanceTo(upTo * i / 12);
        sim.Sprites(sp);
        for (auto& s : sp) {
            float r = std::max(s.size.x, s.size.y) * 0.5f;
            mn = {std::min(mn.x, s.pos.x - r), std::min(mn.y, s.pos.y - r), std::min(mn.z, s.pos.z - r)};
            mx = {std::max(mx.x, s.pos.x + r), std::max(mx.y, s.pos.y + r), std::max(mx.z, s.pos.z + r)};
            n++;
        }
    }
    if (!n) { centre = {}; dist = 300; return; }
    centre = (mn + mx) * 0.5f;
    dist = std::max(40.f, Length(mx - mn) * 0.8f);
}
