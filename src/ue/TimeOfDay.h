#pragma once
#include "ue/Package.h"
#include "render/Math.h"

// Sea of Thieves lights its seas with a day cycle driven by curves over the hour (Athena/Content/Curves/Lighting/<region>):
// sun colour and intensity, sky light, fog and sky dome colours. Island maps carry no directional or sky light of their
// own, so the viewer evaluates these curves for a chosen hour.

struct CurveKey {
    float time = 0, value = 0;
    u8 interp = 0;   // ERichCurveInterpMode: 0 linear, 1 constant, 2 cubic
    float arrive = 0, leave = 0;
};

// One float channel; Eval wraps around the 24 hour day.
struct FloatCurveData {
    std::vector<CurveKey> keys;
    bool Empty() const { return keys.empty(); }
    float Eval(float hour, float def = 0) const;
};

// Reads a CurveFloat (channels[0]) or a CurveLinearColor (r, g, b, a) package.
bool ReadCurveAsset(PackageCache& cache, const std::string& path, std::vector<FloatCurveData>& channels, std::string* error = nullptr);

struct TimeOfDayCurves {
    std::string region;   // folder under Curves/Lighting ("WSP", "Wilds", "Standard", ...)
    FloatCurveData sunIntensity, skyIntensity, fogDensity, fogStart;
    FloatCurveData sunColor[3], skyColor[3], horizon[3], zenith[3];
    float sunrise = 5.2f, sunset = 22.2f;   // hours, where the sun intensity crosses half its peak
};

// The evaluated lighting at an hour, in the viewer's units (linear colours ready for the shaders).
struct TimeOfDayLight {
    float hour = 12;
    bool moon = false;    // night: the light comes from the moon
    Vec3 toSun{0, 0, 1};  // unit vector towards the sun (or the moon)
    Vec3 sunColor{1, 1, 1}, ambient{1, 1, 1};
    Vec3 horizon, zenith;   // sky dome colours (exposed)
    Vec3 fogColor;
    float fogDensity = 0, fogStart = 0;
};

// The lighting regions present in the game (folders with a sun intensity curve), sorted.
std::vector<std::string> TimeOfDayRegions(const PackageCache& cache);
// The region a map belongs to, from its path (seas map to their curve sets; "Standard" otherwise).
std::string TimeOfDayRegionFor(std::string_view mapPath, const std::vector<std::string>& regions);
bool LoadTimeOfDay(PackageCache& cache, const std::string& region, TimeOfDayCurves& out, std::string* error = nullptr);
TimeOfDayLight EvaluateTimeOfDay(const TimeOfDayCurves& c, float hour);
