#include "ue/AnimAssets.h"
#include <cmath>
#include <deque>

namespace {

float F(const PropValue* v, float def = 0) { return v ? (float)v->AsFloat(def) : def; }
float F(const PropValue& s, const char* n, float def = 0) { return F(s.Field(n), def); }
std::string S(const PropValue& s, const char* n) { auto v = s.Field(n); return v ? v->str : std::string(); }
std::string Obj(const Package& pkg, const PropValue* v) {
    if (!v) return {};
    i32 i = v->ObjIndex();
    if (i == 0) return {};
    return i > 0 ? pkg.PackageName() + "." + pkg.ObjectName(i) : pkg.FullPath(i);
}
std::string EnumTail(const std::string& e) {
    size_t k = e.rfind("::");
    return k == std::string::npos ? e : e.substr(k + 2);
}

}   // namespace

bool ReadAnimNotifies(const Package& pkg, const PropertyList& props, std::vector<AnimNotifyInfo>& out) {
    const PropValue* arr = FindValue(props, "Notifies");
    if (!arr || arr->type != PropType::Array) return false;
    for (auto& n : arr->items) {
        if (!n.Fields()) continue;
        AnimNotifyInfo a;
        a.name = S(n, "NotifyName");
        a.time = n.Field("LinkValue") ? F(n, "LinkValue") : F(n, "DisplayTime_DEPRECATED");
        a.duration = F(n, "Duration");
        a.track = (int)F(n, "TrackIndex");
        i32 obj = n.Field("Notify") ? n.Field("Notify")->ObjIndex() : 0;
        if (!obj && n.Field("NotifyStateClass")) obj = n.Field("NotifyStateClass")->ObjIndex();
        if (obj) {
            a.cls = pkg.ClassName(obj);
            if (obj > 0) {   // a few telling properties of the notify object
                PropertyList np = pkg.ReadProperties((size_t)obj - 1);
                for (auto& p : np) {
                    if (p.name == "WwiseEvent" || p.name == "Sound" || p.name == "AkEvent") a.sound = Obj(pkg, &p.value);
                    else if (p.name == "PSTemplate") a.particle = Obj(pkg, &p.value);
                    else if (p.name == "SocketName") a.socket = p.value.str == "None" ? "" : p.value.str;
                    else if (p.name == "Attached") a.attached = p.value.b;
                    else if ((p.name == "LocationOffset" || p.name == "RotationOffset") && p.value.Fields()) {
                        const PropValue& s = p.value;
                        Vec3 v = p.name == "LocationOffset" ? Vec3{F(s, "X"), F(s, "Y"), F(s, "Z")} : Vec3{F(s, "Pitch"), F(s, "Yaw"), F(s, "Roll")};
                        (p.name == "LocationOffset" ? a.locationOffset : a.rotationOffset) = v;
                    }
                    std::string v = p.value.type == PropType::Object ? pkg.ObjectName(p.value.ObjIndex()) : p.value.str;
                    if (v.empty() || v == "None") continue;
                    if (!a.detail.empty()) a.detail += ", ";
                    a.detail += p.name + "=" + v;
                    if (a.detail.size() > 160) break;
                }
            }
        }
        if (a.name.empty()) a.name = a.cls;
        out.push_back(std::move(a));
    }
    return !out.empty();
}

float MontageInfo::Segment::Length() const { return rate != 0 ? (animEnd - animStart) * std::max(1, loops) / fabsf(rate) : 0; }

bool ReadMontage(const Package& pkg, size_t exportIdx, MontageInfo& m, std::string* error) {
    m = MontageInfo{};
    PropertyList p = pkg.ReadProperties(exportIdx);
    m.skeleton = Obj(pkg, FindValue(p, "Skeleton"));
    m.length = (float)PropFloat(p, "SequenceLength");
    m.rateScale = (float)PropFloat(p, "RateScale", 1);
    if (auto b = FindValue(p, "BlendIn"); b && b->Fields()) m.blendIn = F(*b, "BlendTime");
    else m.blendIn = (float)PropFloat(p, "BlendInTime", 0.25);
    if (auto b = FindValue(p, "BlendOut"); b && b->Fields()) m.blendOut = F(*b, "BlendTime");
    else m.blendOut = (float)PropFloat(p, "BlendOutTime", 0.25);
    if (auto cs = FindValue(p, "CompositeSections"); cs && cs->type == PropType::Array)
        for (auto& s : cs->items)
            if (s.Fields()) m.sections.push_back({S(s, "SectionName"), S(s, "NextSectionName"), F(s, "LinkValue")});
    if (auto st = FindValue(p, "SlotAnimTracks"); st && st->type == PropType::Array)
        for (auto& t : st->items) {
            if (!t.Fields()) continue;
            MontageInfo::Slot slot;
            slot.name = S(t, "SlotName");
            if (auto tr = t.Field("AnimTrack"); tr && tr->Fields())
                if (auto segs = tr->Field("AnimSegments"); segs && segs->type == PropType::Array)
                    for (auto& g : segs->items) {
                        if (!g.Fields()) continue;
                        MontageInfo::Segment sg;
                        sg.anim = Obj(pkg, g.Field("AnimReference"));
                        sg.startPos = F(g, "StartPos");
                        sg.animStart = F(g, "AnimStartTime");
                        sg.animEnd = F(g, "AnimEndTime");
                        sg.rate = F(g, "AnimPlayRate", 1);
                        sg.loops = (int)F(g, "LoopingCount", 1);
                        slot.segments.push_back(sg);
                    }
            m.slots.push_back(std::move(slot));
        }
    ReadAnimNotifies(pkg, p, m.notifies);
    if (m.slots.empty() && error) *error = "no slot tracks";
    return true;
}

// ------------------------------------------------------------------ blend spaces

bool IsBlendSpaceClass(std::string_view c) { return c == "BlendSpace" || c == "BlendSpace1D" || c == "AimOffsetBlendSpace" || c == "AimOffsetBlendSpace1D"; }

bool ReadBlendSpace(const Package& pkg, size_t exportIdx, BlendSpaceInfo& b, std::string* error) {
    b = BlendSpaceInfo{};
    std::string cls = pkg.ExportClass(exportIdx);
    b.dims = cls.find("1D") != std::string::npos ? 1 : 2;
    PropertyList p = pkg.ReadProperties(exportIdx);
    b.skeleton = Obj(pkg, FindValue(p, "Skeleton"));
    for (int a = 0; a < 2; a++) {
        const PropValue* v = FindValue(p, "BlendParameters", a);
        if (!v || !v->Fields()) continue;
        b.axes[a].name = S(*v, "DisplayName");
        b.axes[a].min = F(*v, "Min", 0);
        b.axes[a].max = F(*v, "Max", 100);
        b.axes[a].grid = std::max(1, (int)F(*v, "GridNum", 4));
    }
    if (b.dims == 1) b.axes[1] = {"", 0, 1, 0};
    if (auto sd = FindValue(p, "SampleData"); sd && sd->type == PropType::Array)
        for (auto& s : sd->items) {
            if (!s.Fields()) continue;
            BlendSpaceInfo::Sample smp;
            smp.anim = Obj(pkg, s.Field("Animation"));
            if (auto sv = s.Field("SampleValue"); sv && sv->Fields()) { smp.x = F(*sv, "X"); smp.y = F(*sv, "Y"); }
            b.samples.push_back(smp);
        }
    if (auto gs = FindValue(p, "GridSamples"); gs && gs->type == PropType::Array)
        for (auto& g : gs->items) {
            if (!g.Fields()) continue;
            BlendSpaceInfo::Grid e;
            for (int k = 0; k < 3; k++) {   // static arrays: one tagged property per element
                const PropValue* iv = FindValue(*g.Fields(), "Indices", k);
                e.index[k] = iv ? (int)iv->AsInt(-1) : -1;
                e.weight[k] = F(FindValue(*g.Fields(), "Weights", k));
            }
            b.grid.push_back(e);
        }
    if (b.samples.empty() && error) *error = "no samples";
    return !b.samples.empty();
}

void BlendSpaceInfo::Weights(float x, float y, std::vector<float>& out) const {
    out.assign(samples.size(), 0.f);
    int gx = axes[0].grid, gy = dims == 2 ? axes[1].grid : 0;
    auto norm = [](float v, const Axis& a, int n) { return a.max > a.min ? std::clamp((v - a.min) / (a.max - a.min), 0.f, 1.f) * n : 0.f; };
    float fx = norm(x, axes[0], gx), fy = dims == 2 ? norm(y, axes[1], gy) : 0.f;
    if ((int)grid.size() >= (gx + 1) * (gy + 1)) {
        int ix = std::min((int)fx, gx - 1), iy = gy > 0 ? std::min((int)fy, gy - 1) : 0;
        float rx = fx - ix, ry = gy > 0 ? fy - iy : 0;
        auto add = [&](int cx, int cy, float w) {
            if (w <= 0) return;
            const Grid& g = grid[(size_t)cx * (gy + 1) + cy];   // UBlendSpace::GetEditorElement
            for (int k = 0; k < 3; k++)
                if (g.index[k] >= 0 && (size_t)g.index[k] < out.size()) out[g.index[k]] += g.weight[k] * w;
        };
        if (gy > 0) {
            add(ix, iy, (1 - rx) * (1 - ry));
            add(ix + 1, iy, rx * (1 - ry));
            add(ix, iy + 1, (1 - rx) * ry);
            add(ix + 1, iy + 1, rx * ry);
        } else {
            add(ix, 0, 1 - rx);
            add(ix + 1, 0, rx);
        }
    } else {   // no grid: inverse distance to the samples
        float sum = 0;
        for (size_t i = 0; i < samples.size(); i++) {
            float dx = (samples[i].x - x) / std::max(1e-6f, axes[0].max - axes[0].min);
            float dy = dims == 2 ? (samples[i].y - y) / std::max(1e-6f, axes[1].max - axes[1].min) : 0;
            out[i] = 1.f / (dx * dx + dy * dy + 1e-4f);
            sum += out[i];
        }
        for (auto& w : out) w /= sum;
    }
    float sum = 0;
    for (float w : out) sum += w;
    if (sum > 0) for (auto& w : out) w /= sum;
}

// ------------------------------------------------------------------ animation blueprints

bool ReadAnimBlueprint(const Package& pkg, AnimBlueprintInfo& out, std::string* error) {
    out = AnimBlueprintInfo{};
    size_t cls = (size_t)-1, cdo = (size_t)-1;
    for (size_t i = 0; i < pkg.Exports().size(); i++) {
        std::string c = pkg.ExportClass(i);
        if (c == "AnimBlueprintGeneratedClass") cls = i;
        else if (pkg.Exports()[i].objectFlags & RF_ClassDefaultObject) cdo = i;
    }
    if (cls == (size_t)-1) { if (error) *error = "no AnimBlueprintGeneratedClass"; return false; }
    PropertyList p = pkg.ReadProperties(cls);
    out.skeleton = Obj(pkg, FindValue(p, "TargetSkeleton"));
    if (auto sms = FindValue(p, "BakedStateMachines"); sms && sms->type == PropType::Array)
        for (auto& m : sms->items) {
            if (!m.Fields()) continue;
            StateMachineInfo sm;
            sm.name = S(m, "MachineName");
            sm.initial = (int)F(m, "InitialState");
            if (auto st = m.Field("States"); st && st->type == PropType::Array)
                for (auto& s : st->items) {
                    if (!s.Fields()) continue;
                    StateMachineInfo::State state;
                    state.name = S(s, "StateName");
                    state.conduit = s.Field("bIsAConduit") && s.Field("bIsAConduit")->AsBool();
                    if (auto pl = s.Field("PlayerNodeIndices"); pl && pl->type == PropType::Array)
                        for (auto& x : pl->items) state.players.push_back((int)x.AsInt());
                    if (auto tr = s.Field("Transitions"); tr && tr->type == PropType::Array)
                        for (auto& t : tr->items)
                            if (t.Fields()) state.transitions.push_back((int)F(t, "TransitionIndex", -1));
                    sm.states.push_back(std::move(state));
                }
            if (auto tr = m.Field("Transitions"); tr && tr->type == PropType::Array)
                for (auto& t : tr->items) {
                    if (!t.Fields()) continue;
                    StateMachineInfo::Transition x;
                    x.from = (int)F(t, "PreviousState");
                    x.to = (int)F(t, "NextState");
                    x.crossfade = F(t, "CrossfadeDuration");
                    x.blendMode = EnumTail(S(t, "BlendMode"));
                    x.logic = EnumTail(S(t, "LogicType"));
                    x.priority = (int)F(t, "PriorityOrder");
                    x.bidirectional = t.Field("bBidirectional") && t.Field("bBidirectional")->AsBool();
                    sm.transitions.push_back(x);
                }
            out.machines.push_back(std::move(sm));
        }
    if (auto an = FindValue(p, "AnimNotifies"); an && an->type == PropType::Array)
        for (auto& n : an->items)
            if (n.Fields()) out.notifies.push_back(S(n, "NotifyName"));
    if (cdo != (size_t)-1) {
        PropertyList dp = pkg.ReadProperties(cdo);
        for (auto& prop : dp) {
            if (!prop.value.Fields() || prop.name.rfind("AnimGraphNode_", 0) != 0) continue;
            for (const char* f : {"Sequence", "BlendSpace", "Montage"})
                if (auto v = prop.value.Field(f)) {
                    std::string a = Obj(pkg, v);
                    if (!a.empty()) out.players.push_back({prop.name, a});
                }
        }
    }
    return true;
}

void LayoutStateMachine(const StateMachineInfo& sm, std::vector<Vec2>& pos) {
    size_t n = sm.states.size();
    pos.assign(n, Vec2{});
    if (!n) return;
    // breadth-first depth from the initial state; columns by depth, rows in visiting order
    std::vector<int> depth(n, -1);
    std::deque<int> q;
    int start = std::clamp(sm.initial, 0, (int)n - 1);
    depth[start] = 0;
    q.push_back(start);
    while (!q.empty()) {
        int s = q.front();
        q.pop_front();
        for (auto& t : sm.transitions)
            if (t.from == s && t.to >= 0 && (size_t)t.to < n && depth[t.to] < 0) { depth[t.to] = depth[s] + 1; q.push_back(t.to); }
    }
    int maxd = 0;
    for (int d : depth) maxd = std::max(maxd, d);
    for (auto& d : depth) if (d < 0) d = ++maxd;   // unreachable states at the end
    std::vector<int> rows(maxd + 2, 0);
    for (size_t i = 0; i < n; i++) {
        int d = depth[i];
        pos[i] = {d * 260.f, rows[d]++ * 120.f};
    }
    // centre every column vertically
    for (size_t i = 0; i < n; i++) pos[i].y -= (rows[depth[i]] - 1) * 60.f;
}

void BlendPoses(const std::vector<std::vector<Bone>>& poses, const std::vector<float>& weights, std::vector<Bone>& out) {
    out.clear();
    size_t first = poses.size();
    for (size_t i = 0; i < poses.size(); i++)
        if (i < weights.size() && weights[i] > 0) { first = i; break; }
    if (first == poses.size()) { if (!poses.empty()) out = poses[0]; return; }
    out = poses[first];
    for (size_t b = 0; b < out.size(); b++) {
        Vec3 t{}, s{};
        Vec4 q{0, 0, 0, 0};
        const Vec4& ref = poses[first][b].rotation;
        float wsum = 0;
        for (size_t i = 0; i < poses.size() && i < weights.size(); i++) {
            float w = weights[i];
            if (w <= 0 || b >= poses[i].size()) continue;
            const Bone& x = poses[i][b];
            t = t + x.translation * w;
            s = s + x.scale * w;
            float sg = ref.x * x.rotation.x + ref.y * x.rotation.y + ref.z * x.rotation.z + ref.w * x.rotation.w < 0 ? -w : w;
            q = {q.x + x.rotation.x * sg, q.y + x.rotation.y * sg, q.z + x.rotation.z * sg, q.w + x.rotation.w * sg};
            wsum += w;
        }
        if (wsum <= 0) continue;
        float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        out[b].translation = t * (1 / wsum);
        out[b].scale = s * (1 / wsum);
        if (len > 1e-8f) out[b].rotation = {q.x / len, q.y / len, q.z / len, q.w / len};
    }
}
