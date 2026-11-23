// RTPC curves of Wwise hierarchy objects (bank v134): which game parameter drives which property, and the curve.
#include "audio/Wwise.h"
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace {

u16 Rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
u32 Rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
float RdF(const u8* p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}

bool IsNodeType(u8 t) { return t == 2 || t == 5 || t == 6 || t == 7 || t == 9 || t == 10 || t == 11 || t == 12 || t == 13; }

// One InitialRTPC block at q: u16 count, curves. Strictly validated (it is found by scanning the node body).
bool ParseRtpcBlock(const u8* d, size_t size, size_t q, std::vector<WwiseRtpcCurve>& out) {
    if (q + 2 > size) return false;
    u16 n = Rd16(d + q);
    if (n < 1 || n > 32) return false;
    size_t p = q + 2;
    std::vector<WwiseRtpcCurve> curves;
    for (u16 k = 0; k < n; k++) {
        if (p + 6 > size) return false;
        WwiseRtpcCurve c;
        c.rtpc = Rd32(d + p);
        c.rtpcType = d[p + 4];
        c.accum = d[p + 5];
        p += 6;
        if (!c.rtpc || c.rtpcType > 2 || c.accum < 1 || c.accum > 6) return false;
        u32 prop = 0;   // AkVarLenInt
        int bytes = 0;
        for (; bytes < 4 && p < size; bytes++) {
            u8 b = d[p++];
            prop = (prop << 7) | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        if (bytes >= 4 || prop > 1000 || p + 7 > size) return false;
        c.property = prop;
        u32 curveId = Rd32(d + p);
        c.scaling = d[p + 4];
        u16 pts = Rd16(d + p + 5);
        p += 7;
        if (!curveId || c.scaling > 4 || pts < 1 || pts > 256 || p + (size_t)pts * 12 > size) return false;
        for (u16 i = 0; i < pts; i++, p += 12) {
            WwiseRtpcPoint pt{RdF(d + p), RdF(d + p + 4), Rd32(d + p + 8)};
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || std::fabs(pt.x) > 1e7f || std::fabs(pt.y) > 1e7f || pt.shape > 9) return false;
            if (!c.points.empty() && pt.x < c.points.back().x) return false;
            c.points.push_back(pt);
        }
        curves.push_back(std::move(c));
    }
    for (auto& c : curves) out.push_back(std::move(c));
    return true;
}

float Shape(u32 s, float t) {
    const float pi = 3.14159265f;
    switch (s) {
    case 0: return 1 - powf(1 - t, 3);          // Log3
    case 1: return sinf(t * pi * 0.5f);         // Sine
    case 2: return 1 - powf(1 - t, 1.41f);      // Log1
    case 3: return 0.5f + asinf(2 * t - 1) / pi;   // inverted S
    case 5: return t * t * (3 - 2 * t);         // S curve
    case 6: return powf(t, 1.41f);              // Exp1
    case 7: return 1 - cosf(t * pi * 0.5f);     // reciprocal sine
    case 8: return t * t * t;                   // Exp3
    case 9: return 0;                           // constant: holds the left value
    default: return t;                          // linear
    }
}

}   // namespace

float WwiseRtpcCurve::Eval(float x) const {
    if (points.empty()) return 0;
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x) return points.back().y;
    for (size_t i = 0; i + 1 < points.size(); i++) {
        const auto &a = points[i], &b = points[i + 1];
        if (x > b.x) continue;
        float t = b.x > a.x ? (x - a.x) / (b.x - a.x) : 1.f;
        return a.y + (b.y - a.y) * Shape(a.shape, t);
    }
    return points.back().y;
}

const char* WwiseRtpcPropertyName(u32 p) {
    static const char* names[] = {"Volume", "LFE", "Pitch", "LPF", "HPF", "BusVolume", "InitialDelay", "MakeUpGain",
                                  "FeedbackVolume", "FeedbackLowpass", "FeedbackPitch", "MidiTransposition", "MidiVelocityOffset",
                                  "PlaybackSpeed", "MuteRatio", "PlayMechanismSpecialTransitions", "MaxNumInstances", "Priority",
                                  "PanX2D", "PanY2D", "PanX3D", "PanY3D", "PanZ3D", "PositioningTypeBlend", "DivergenceCenter",
                                  "ConeAttenuationOnOff", "ConeAttenuation", "ConeLPF", "ConeHPF", "BypassFX0", "BypassFX1", "BypassFX2",
                                  "BypassFX3", "BypassAllFX", "HDRBusThreshold", "HDRBusReleaseTime", "HDRBusRatio", "HDRActiveRange",
                                  "GameAuxSendVolume", "UserAuxSendVolume0", "UserAuxSendVolume1", "UserAuxSendVolume2",
                                  "UserAuxSendVolume3", "OutputBusVolume", "OutputBusHPF", "OutputBusLPF", "EnableAttenuation"};
    return p < sizeof(names) / sizeof(names[0]) ? names[p] : "";
}

std::vector<WwiseRtpcCurve> FindWwiseRtpcs(const u8* data, const BankInfo& bank, const std::vector<u32>& nodes) {
    std::unordered_map<u32, const HircObject*> byId;
    for (auto& o : bank.hirc) byId[o.id] = &o;
    auto isNode = [&](u32 id) {
        auto it = byId.find(id);
        return it != byId.end() && IsNodeType(it->second->type);
    };
    // parents: containers / actor-mixers list their children as "u32 count + ids" (the first such run in the body)
    std::unordered_map<u32, u32> parentOf;
    for (auto& o : bank.hirc) {
        if (o.type == 2 || o.type == 11 || !IsNodeType(o.type)) continue;
        const u8* d = data + o.offset;
        for (size_t q = 0; q + 8 <= o.size; q++) {
            u32 n = Rd32(d + q);
            bool run = n >= 1 && n <= 512 && q + 4 + (size_t)n * 4 <= o.size;
            for (u32 k = 0; run && k < n; k++) {
                u32 c = Rd32(d + q + 4 + k * 4);
                run = c != o.id && isNode(c);
            }
            if (!run) continue;
            for (u32 k = 0; k < n; k++) parentOf.try_emplace(Rd32(d + q + 4 + k * 4), o.id);
            break;
        }
    }
    std::vector<u32> all;
    std::unordered_set<u32> seen;
    for (u32 n : nodes)
        for (u32 id = n; id && seen.insert(id).second;) {
            all.push_back(id);
            auto p = parentOf.find(id);
            id = p == parentOf.end() ? 0 : p->second;
        }
    std::vector<WwiseRtpcCurve> out;
    for (u32 id : all) {
        auto it = byId.find(id);
        if (it == byId.end()) continue;
        const HircObject& o = *it->second;
        const u8* d = data + o.offset;
        size_t before = out.size();
        for (size_t q = 0; q + 2 < o.size; q++)
            if (ParseRtpcBlock(d, o.size, q, out)) break;
        for (size_t i = before; i < out.size(); i++) {
            out[i].node = id;
            out[i].nodeType = o.type;
        }
    }
    return out;
}

std::unordered_map<u32, float> EvalWwiseRtpcs(const std::vector<WwiseRtpcCurve>& curves, const std::unordered_map<u32, float>& values) {
    std::unordered_map<u32, float> out;
    for (auto& c : curves) {
        if (c.points.empty()) continue;
        auto v = values.find(c.rtpc);
        float y = c.Eval(v == values.end() ? c.points.front().x : v->second);
        auto [it, fresh] = out.try_emplace(c.property, c.accum == 3 ? 1.f : 0.f);
        if (c.accum == 3) it->second *= y;
        else if (c.accum == 5 || c.accum == 1) it->second = fresh ? y : std::max(it->second, y);
        else it->second += y;
    }
    return out;
}
