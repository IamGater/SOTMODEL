#include "ue/TimeOfDay.h"
#include "core/Vfs.h"
#include <algorithm>
#include <cmath>

static const char* kLightingFolder = "Athena/Content/Curves/Lighting";

float FloatCurveData::Eval(float hour, float def) const {
    if (keys.empty()) return def;
    if (keys.size() == 1) return keys[0].value;
    // the day wraps: the last key leads into the first one of the next day
    float t = fmodf(hour, 24.f);
    if (t < 0) t += 24;
    const CurveKey* a = nullptr;
    const CurveKey* b = nullptr;
    float ta = 0, tb = 0;
    for (size_t i = 0; i + 1 < keys.size(); i++)
        if (t >= keys[i].time && t <= keys[i + 1].time) { a = &keys[i]; b = &keys[i + 1]; ta = a->time; tb = b->time; break; }
    if (!a) {   // between the last key and the first one (+24 h)
        a = &keys.back();
        b = &keys.front();
        ta = a->time;
        tb = b->time + 24;
        if (t < ta) t += 24;
    }
    float span = tb - ta;
    if (span <= 1e-5f) return b->value;
    float u = std::clamp((t - ta) / span, 0.f, 1.f);
    if (a->interp == 1) return a->value;   // constant
    if (a->interp == 2) {                  // cubic Hermite with the keys' tangents (per hour)
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        return h00 * a->value + h10 * span * a->leave + h01 * b->value + h11 * span * b->arrive;
    }
    return a->value + (b->value - a->value) * u;
}

static void ReadKeys(const PropValue* curve, FloatCurveData& out) {
    out.keys.clear();
    const PropValue* keys = curve ? curve->Field("Keys") : nullptr;
    if (!keys) return;
    for (auto& k : keys->items) {
        auto f = [&](const char* n) { auto x = k.Field(n); return x ? (float)x->AsFloat() : 0.f; };
        CurveKey key;
        key.time = f("Time");
        key.value = f("Value");
        key.arrive = f("ArriveTangent");
        key.leave = f("LeaveTangent");
        if (auto m = k.Field("InterpMode")) key.interp = (u8)m->AsInt();
        out.keys.push_back(key);
    }
    std::stable_sort(out.keys.begin(), out.keys.end(), [](auto& a, auto& b) { return a.time < b.time; });
}

bool ReadCurveAsset(PackageCache& cache, const std::string& path, std::vector<FloatCurveData>& channels, std::string* error) {
    channels.clear();
    auto pkg = cache.LoadPath(path, error);
    if (!pkg) return false;
    i32 ex = pkg->MainExport();
    if (!ex) { if (error) *error = "no curve in " + path; return false; }
    PropertyList props = pkg->ReadProperties(ex - 1);
    if (const PropValue* c = FindValue(props, "FloatCurve")) {
        channels.resize(1);
        ReadKeys(c, channels[0]);
    } else {
        channels.resize(4);
        for (int i = 0; i < 4; i++) ReadKeys(FindValue(props, "FloatCurves", i), channels[i]);
    }
    return true;
}

std::vector<std::string> TimeOfDayRegions(const PackageCache& cache) {
    std::vector<std::string> out;
    const Vfs& vfs = cache.FileSystem();
    for (u32 i = 0; i < vfs.Folders().size(); i++) {
        const VfsFolder& f = vfs.Folders()[i];
        if (f.parent == UINT32_MAX) continue;
        if (vfs.Find(std::string(kLightingFolder) + "/" + f.name + "/c_light_intensity.uasset") < 0) continue;
        if (std::find(out.begin(), out.end(), f.name) == out.end()) out.push_back(f.name);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return ToLower(a) < ToLower(b); });
    return out;
}

std::string TimeOfDayRegionFor(std::string_view mapPath, const std::vector<std::string>& regions) {
    std::string p = ToLower(mapPath);
    auto pick = [&](const char* r) -> std::string {
        for (auto& x : regions) if (IEquals(x, r)) return x;
        return {};
    };
    struct { const char* key; const char* region; } map[] = {
        {"wide_sea_of_plunder", "WSP"}, {"/wsp_", "WSP"}, {"the_wilds", "Wilds"}, {"/wld_", "Wilds"},
        {"devils_roar", "Volcanic"}, {"/dvr_", "Volcanic"}, {"hideout", "Hideout"}, {"skelly", "Skelly"},
    };
    for (auto& m : map)
        if (p.find(m.key) != std::string::npos)
            if (auto r = pick(m.region); !r.empty()) return r;
    if (auto r = pick("Standard"); !r.empty()) return r;
    return regions.empty() ? std::string() : regions.front();
}

bool LoadTimeOfDay(PackageCache& cache, const std::string& region, TimeOfDayCurves& out, std::string* error) {
    out = TimeOfDayCurves{};
    out.region = region;
    std::string base = std::string(kLightingFolder) + "/" + region + "/";
    std::vector<FloatCurveData> ch;
    auto scalar = [&](const char* name, FloatCurveData& dst) {
        if (ReadCurveAsset(cache, base + name + ".uasset", ch, nullptr) && !ch.empty()) dst = ch[0];
    };
    auto color = [&](const char* name, FloatCurveData* dst) {
        if (ReadCurveAsset(cache, base + name + ".uasset", ch, nullptr) && ch.size() >= 3)
            for (int i = 0; i < 3; i++) dst[i] = ch[i];
    };
    scalar("c_light_intensity", out.sunIntensity);
    if (out.sunIntensity.Empty()) {
        if (error) *error = "no c_light_intensity curve in " + base;
        return false;
    }
    scalar("c_sky_light_intensity", out.skyIntensity);
    scalar("c_fog_density", out.fogDensity);
    scalar("c_fog_start_distance", out.fogStart);
    color("c_light_sun_colour", out.sunColor);
    color("c_sky_light_colour", out.skyColor);
    color("c_skydome_horizon_colour", out.horizon);
    color("c_skydome_zenith_colour", out.zenith);
    // day = where the sun is above half its peak intensity
    float peak = 0;
    for (auto& k : out.sunIntensity.keys) peak = std::max(peak, k.value);
    float rise = -1, set = -1;
    for (float h = 0; h < 24; h += 0.05f) {
        bool day = out.sunIntensity.Eval(h) >= peak * 0.5f;
        bool before = out.sunIntensity.Eval(h - 0.05f) >= peak * 0.5f;
        if (day && !before && rise < 0) rise = h;
        if (!day && before) set = h;
    }
    if (rise >= 0 && set > rise) { out.sunrise = rise; out.sunset = set; }
    return true;
}

TimeOfDayLight EvaluateTimeOfDay(const TimeOfDayCurves& c, float hour) {
    TimeOfDayLight l;
    l.hour = fmodf(hour, 24.f);
    if (l.hour < 0) l.hour += 24;
    auto rgb = [&](const FloatCurveData* ch, Vec3 def) {
        return ch[0].Empty() ? def : Vec3{ch[0].Eval(l.hour), ch[1].Eval(l.hour), ch[2].Eval(l.hour)};
    };
    // the sun crosses the sky between sunrise and sunset; the moon takes the night
    float dayLen = c.sunset - c.sunrise, nightLen = 24 - dayLen;
    float a;
    if (l.hour >= c.sunrise && l.hour <= c.sunset) a = (l.hour - c.sunrise) / dayLen * kPi;
    else {
        l.moon = true;
        float t = l.hour - c.sunset;
        if (t < 0) t += 24;
        a = t / std::max(nightLen, 0.5f) * kPi;
    }
    // rises in +X, peaks 60 degrees up towards -Y, sets in -X; the moon follows the same arc
    float elev = sinf(a) * 60 * kPi / 180;
    l.toSun = Normalize(Vec3{cosf(a) * cosf(elev), -0.35f * cosf(elev), sinf(elev) + 0.08f});
    // colours: SoT exposes and colour grades afterwards; these scales match the viewer's plain shading
    float sunI = c.sunIntensity.Eval(l.hour, 2);
    Vec3 sc = rgb(c.sunColor, {1, 0.9f, 0.7f});
    float lum = sc.x * 0.3f + sc.y * 0.59f + sc.z * 0.11f;
    sc = sc * 0.6f + Vec3{lum, lum, lum} * 0.4f;   // the game's grading takes much of the yellow out
    l.sunColor = sc * (sunI * 0.55f);
    float skyI = c.skyIntensity.Eval(l.hour, 0.8f);
    l.ambient = rgb(c.skyColor, {0.86f, 0.97f, 1}) * (skyI * 1.3f);
    const float exposure = 5;
    l.horizon = rgb(c.horizon, {0.08f, 0.1f, 0.11f}) * exposure;
    l.zenith = rgb(c.zenith, {0.003f, 0.006f, 0.06f}) * exposure;
    l.fogColor = l.horizon;   // distant geometry fades into the sky behind it
    l.fogDensity = c.fogDensity.Eval(l.hour, 0.01f);
    l.fogStart = c.fogStart.Eval(l.hour, 2000);
    return l;
}
