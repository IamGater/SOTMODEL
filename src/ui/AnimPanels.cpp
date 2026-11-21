// Montage timeline, blend space canvas and animation blueprint state machine graph.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "imgui.h"
#include <cmath>

namespace {

std::string ShortName(const std::string& path) {
    size_t s = path.find_last_of("/.");
    return s == std::string::npos ? path : path.substr(s + 1);
}

const ImU32 kSegCol[4] = {IM_COL32(70, 110, 170, 255), IM_COL32(80, 140, 110, 255), IM_COL32(150, 110, 70, 255), IM_COL32(120, 90, 150, 255)};

// Mesh-space local pose of an animation at `seconds` (clamped).
void SamplePose(const AnimPoser& poser, const AnimSequenceData& a, float seconds, std::vector<Bone>& pose) {
    float fps = a.FrameRate();
    float f = std::clamp(seconds * fps, 0.f, (float)std::max(0, a.numFrames - 1));
    poser.MeshPose(a, f, pose);
}

}   // namespace

// Pose of a montage / blend space document at d.animTime. False for plain animation sequences.
bool App::AnimAssetPose(Document& d, std::vector<Bone>& pose) {
    if (!d.poser) return false;
    if (d.montage) {
        const MontageInfo& m = *d.montage;
        if (m.slots.empty()) { pose = d.poser->MeshBones(); return true; }
        const auto& segs = m.slots[0].segments;
        size_t si = 0;
        for (size_t i = 0; i < segs.size(); i++)
            if (d.animTime >= segs[i].startPos) si = i;
        if (si >= segs.size() || si >= d.subAnims.size() || !d.subAnims[si]) { pose = d.poser->MeshBones(); return true; }
        const auto& g = segs[si];
        float span = g.animEnd - g.animStart, local = (d.animTime - g.startPos) * fabsf(g.rate);
        if (span > 0) local = std::min(fmodf(std::max(0.f, local), span) + (local >= span * std::max(1, g.loops) ? span : 0.f), span);
        float t = g.rate < 0 ? g.animEnd - local : g.animStart + local;
        SamplePose(*d.poser, *d.subAnims[si], t, pose);
        return true;
    }
    if (d.blendSpace) {
        std::vector<float> w;
        d.blendSpace->Weights(d.bsX, d.bsY, w);
        d.bsWeights = w;
        std::vector<std::vector<Bone>> poses(w.size());
        float phase = d.anim && d.anim->length > 0 ? d.animTime / d.anim->length : 0;   // samples play in sync (normalised time)
        for (size_t i = 0; i < w.size(); i++) {
            if (w[i] <= 0 || i >= d.subAnims.size() || !d.subAnims[i]) { w[i] = 0; continue; }
            SamplePose(*d.poser, *d.subAnims[i], phase * d.subAnims[i]->length, poses[i]);
        }
        BlendPoses(poses, w, pose);
        if (pose.empty()) pose = d.poser->MeshBones();
        return true;
    }
    return false;
}

// Montage: sections / slot segments / notifies over time, click or drag to scrub.
// Blend space: the sample grid with a draggable input point.
void App::DrawAnimAssetHeader(Document& d) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fs = ImGui::GetFontSize();
    float W = ImGui::GetContentRegionAvail().x;
    if (d.montage) {
        const MontageInfo& m = *d.montage;
        float len = std::max(0.01f, m.length);
        int rows = 2 + (int)m.slots.size();
        float rowH = fs * 1.4f, H = rowH * rows + 4;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##timeline", ImVec2(W, H));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive()) {
            d.animTime = std::clamp((ImGui::GetIO().MousePos.x - p.x) / W * len, 0.f, len);
            d.animPlaying = false;
        }
        auto X = [&](float t) { return p.x + t / len * W; };
        dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(28, 30, 34, 255));
        // sections
        float y = p.y;
        for (size_t i = 0; i < m.sections.size(); i++) {
            const auto& s = m.sections[i];
            float x0 = X(s.start), x1 = i + 1 < m.sections.size() ? X(m.sections[i + 1].start) : p.x + W;
            dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1 - 1, y + rowH - 1), IM_COL32(60, 60, 70, 255));
            std::string label = s.name + (s.next.empty() || s.next == "None" ? "" : " > " + s.next);
            dl->PushClipRect(ImVec2(x0, y), ImVec2(x1 - 2, y + rowH), true);
            dl->AddText(ImVec2(x0 + 4, y + rowH * 0.15f), IM_COL32(230, 230, 230, 255), label.c_str());
            dl->PopClipRect();
            if (hovered && ImGui::GetIO().MousePos.x >= x0 && ImGui::GetIO().MousePos.x < x1 && ImGui::GetIO().MousePos.y < y + rowH)
                ImGui::SetTooltip("Section %s at %.3f s%s%s", s.name.c_str(), s.start, s.next.empty() || s.next == "None" ? "" : "\nnext: ", s.next.c_str());
        }
        y += rowH;
        // slot tracks
        for (size_t si = 0; si < m.slots.size(); si++, y += rowH) {
            const auto& slot = m.slots[si];
            for (size_t i = 0; i < slot.segments.size(); i++) {
                const auto& g = slot.segments[i];
                float x0 = X(g.startPos), x1 = X(g.startPos + g.Length());
                dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(std::max(x0 + 2, x1 - 1), y + rowH - 1), kSegCol[i % 4]);
                dl->PushClipRect(ImVec2(x0, y), ImVec2(x1 - 2, y + rowH), true);
                dl->AddText(ImVec2(x0 + 4, y + rowH * 0.15f), IM_COL32(240, 240, 240, 255), ShortName(g.anim).c_str());
                dl->PopClipRect();
                ImVec2 mp = ImGui::GetIO().MousePos;
                if (hovered && mp.x >= x0 && mp.x < x1 && mp.y >= y && mp.y < y + rowH)
                    ImGui::SetTooltip("%s\nslot %s\n%.3f .. %.3f s (anim %.3f .. %.3f, rate %.2f, loops %d)", g.anim.c_str(), slot.name.c_str(), g.startPos,
                                      g.startPos + g.Length(), g.animStart, g.animEnd, g.rate, g.loops);
            }
            dl->AddText(ImVec2(p.x + W - ImGui::CalcTextSize(slot.name.c_str()).x - 4, y + rowH * 0.15f), IM_COL32(160, 160, 160, 200), slot.name.c_str());
        }
        // notifies
        for (auto& n : m.notifies) {
            float x = X(n.time), cy = y + rowH * 0.5f, r = rowH * 0.3f;
            if (n.duration > 0) dl->AddRectFilled(ImVec2(x, cy - r * 0.5f), ImVec2(X(n.time + n.duration), cy + r * 0.5f), IM_COL32(200, 160, 60, 120));
            dl->AddQuadFilled(ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy), IM_COL32(240, 190, 60, 255));
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (hovered && fabsf(mp.x - x) <= r + 2 && mp.y >= y && mp.y < y + rowH)
                ImGui::SetTooltip("%s\n%s at %.3f s%s\n%s", n.name.c_str(), n.cls.c_str(), n.time, n.duration > 0 ? Fmt(" for %.3f s", n.duration).c_str() : "",
                                  n.detail.c_str());
        }
        float cx = X(d.animTime);
        dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + H), IM_COL32(255, 80, 80, 255), 2);
        ImGui::TextDisabled("%.3f / %.3f s | %zu sections, %zu slot(s), %zu notifies | blend in %.2f out %.2f", d.animTime, m.length, m.sections.size(),
                            m.slots.size(), m.notifies.size(), m.blendIn, m.blendOut);
        return;
    }
    if (!d.blendSpace) return;
    const BlendSpaceInfo& b = *d.blendSpace;
    bool two = b.dims == 2;
    float H = two ? std::min(fs * 10, W * 0.45f) : fs * 3.2f, padL = fs * 3, padB = fs * 1.4f;
    float CW = two ? std::max(fs * 8, std::min(W - padL - fs * 15, H * 1.4f)) : W - padL - 4;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 c0(p.x + padL, p.y + 2), c1(c0.x + CW, p.y + H - padB);
    ImGui::InvisibleButton("##bs", ImVec2(W, H));
    const auto& ax = b.axes[0];
    const auto& ay = b.axes[1];
    auto toScreen = [&](float x, float y) {
        float u = ax.max > ax.min ? (x - ax.min) / (ax.max - ax.min) : 0, v = two && ay.max > ay.min ? (y - ay.min) / (ay.max - ay.min) : 0.5f;
        return ImVec2(c0.x + u * (c1.x - c0.x), c1.y - v * (c1.y - c0.y));
    };
    if (ImGui::IsItemActive()) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        d.bsX = ax.min + std::clamp((mp.x - c0.x) / (c1.x - c0.x), 0.f, 1.f) * (ax.max - ax.min);
        if (two) d.bsY = ay.min + std::clamp((c1.y - mp.y) / (c1.y - c0.y), 0.f, 1.f) * (ay.max - ay.min);
    }
    dl->AddRectFilled(c0, c1, IM_COL32(28, 30, 34, 255));
    for (int i = 0; i <= ax.grid; i++) {
        float x = c0.x + (c1.x - c0.x) * i / ax.grid;
        dl->AddLine(ImVec2(x, c0.y), ImVec2(x, c1.y), IM_COL32(60, 64, 72, 255));
    }
    if (two)
        for (int i = 0; i <= ay.grid; i++) {
            float y = c0.y + (c1.y - c0.y) * i / ay.grid;
            dl->AddLine(ImVec2(c0.x, y), ImVec2(c1.x, y), IM_COL32(60, 64, 72, 255));
        }
    dl->AddRect(c0, c1, IM_COL32(110, 110, 120, 255));
    dl->AddText(ImVec2(c0.x, c1.y + 2), IM_COL32(170, 170, 170, 255), Fmt("%g", ax.min).c_str());
    std::string xl = Fmt("%s  %g", ax.name.c_str(), ax.max);
    dl->AddText(ImVec2(c1.x - ImGui::CalcTextSize(xl.c_str()).x, c1.y + 2), IM_COL32(170, 170, 170, 255), xl.c_str());
    if (two) {
        dl->AddText(ImVec2(p.x, c0.y), IM_COL32(170, 170, 170, 255), Fmt("%g", ay.max).c_str());
        dl->AddText(ImVec2(p.x, c1.y - fs), IM_COL32(170, 170, 170, 255), Fmt("%g", ay.min).c_str());
        dl->AddText(ImVec2(p.x, (c0.y + c1.y) / 2 - fs / 2), IM_COL32(170, 170, 170, 255), ay.name.c_str());
    }
    ImVec2 mp = ImGui::GetIO().MousePos;
    int hover = -1;
    for (size_t i = 0; i < b.samples.size(); i++) {
        ImVec2 s = toScreen(b.samples[i].x, b.samples[i].y);
        float w = i < d.bsWeights.size() ? d.bsWeights[i] : 0;
        dl->AddCircleFilled(s, fs * 0.25f + w * fs * 0.6f, w > 0 ? IM_COL32(90, 200, 120, 255) : IM_COL32(150, 150, 160, 255));
        if (fabsf(mp.x - s.x) < fs * 0.5f && fabsf(mp.y - s.y) < fs * 0.5f) hover = (int)i;
    }
    if (hover >= 0 && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s\n%s %g%s\nweight %.3f", b.samples[hover].anim.c_str(), ax.name.c_str(), b.samples[hover].x,
                          two ? Fmt(", %s %g", ay.name.c_str(), b.samples[hover].y).c_str() : "", hover < (int)d.bsWeights.size() ? d.bsWeights[hover] : 0.f);
    ImVec2 ip = toScreen(d.bsX, d.bsY);
    dl->AddCircle(ip, fs * 0.45f, IM_COL32(255, 90, 90, 255), 0, 2);
    dl->AddLine(ImVec2(ip.x - fs * 0.6f, ip.y), ImVec2(ip.x + fs * 0.6f, ip.y), IM_COL32(255, 90, 90, 255));
    dl->AddLine(ImVec2(ip.x, ip.y - fs * 0.6f), ImVec2(ip.x, ip.y + fs * 0.6f), IM_COL32(255, 90, 90, 255));
    // blended samples, strongest first
    float lx = c1.x + fs, ly = c0.y;
    if (two && lx < p.x + W - fs * 6) {
        std::vector<int> order;
        for (size_t i = 0; i < d.bsWeights.size(); i++)
            if (d.bsWeights[i] > 0.001f) order.push_back((int)i);
        std::sort(order.begin(), order.end(), [&](int a, int c) { return d.bsWeights[a] > d.bsWeights[c]; });
        dl->AddText(ImVec2(lx, ly), IM_COL32(170, 170, 170, 255), Fmt("%s %.2f, %s %.2f", ax.name.c_str(), d.bsX, ay.name.c_str(), d.bsY).c_str());
        for (int i : order) {
            ly += fs * 1.2f;
            dl->AddText(ImVec2(lx, ly), IM_COL32(220, 220, 220, 255), Fmt("%3.0f%%  %s", d.bsWeights[i] * 100, ShortName(b.samples[i].anim).c_str()).c_str());
        }
    } else {
        ImGui::TextDisabled("%s %.2f", ax.name.c_str(), d.bsX);
    }
}

void App::DrawStateMachinePanel(Document& d) {
    const AnimBlueprintInfo& bp = *d.animBp;
    float fs = ImGui::GetFontSize();
    if (!bp.skeleton.empty()) {
        ImGui::TextDisabled("Skeleton");
        ImGui::SameLine();
        ImGui::TextUnformatted(bp.skeleton.c_str());
    }
    if (bp.machines.empty()) ImGui::TextDisabled("No baked state machines.");
    else {
        d.smSel = std::clamp(d.smSel, 0, (int)bp.machines.size() - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##sm", Fmt("%s  (%zu states)", bp.machines[d.smSel].name.c_str(), bp.machines[d.smSel].states.size()).c_str())) {
            for (int i = 0; i < (int)bp.machines.size(); i++)
                if (ImGui::Selectable(Fmt("%s  (%zu states)##%d", bp.machines[i].name.c_str(), bp.machines[i].states.size(), i).c_str(), d.smSel == i)) {
                    d.smSel = i;
                    d.smState = -1;
                    d.graphZoom = 0;
                }
            ImGui::EndCombo();
        }
    }
    float listH = std::min(fs * 12, ImGui::GetContentRegionAvail().y * 0.4f);
    ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - listH);
    if (!bp.machines.empty() && size.y > fs * 4) {
        const StateMachineInfo& sm = bp.machines[d.smSel];
        std::vector<Vec2> pos;
        LayoutStateMachine(sm, pos);
        Vec2 mn{1e9f, 1e9f}, mx{-1e9f, -1e9f};
        for (auto& q : pos) {
            mn = {std::min(mn.x, q.x), std::min(mn.y, q.y)};
            mx = {std::max(mx.x, q.x + 240), std::max(mx.y, q.y + 50)};
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##smcanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered = ImGui::IsItemHovered();
        if (d.graphZoom <= 0 && !pos.empty()) {   // fit
            float zx = (size.x - fs * 2) / std::max(1.f, mx.x - mn.x), zy = (size.y - fs * 2) / std::max(1.f, mx.y - mn.y);
            d.graphZoom = std::clamp(std::min(zx, zy), 0.2f, 1.5f);
            d.graphPanX = (size.x - (mx.x - mn.x) * d.graphZoom) / 2 - mn.x * d.graphZoom;
            d.graphPanY = (size.y - (mx.y - mn.y) * d.graphZoom) / 2 - mn.y * d.graphZoom;
        }
        if (hovered && ImGui::GetIO().MouseWheel != 0) {
            float z0 = d.graphZoom, z1 = std::clamp(z0 * powf(1.15f, ImGui::GetIO().MouseWheel), 0.1f, 4.f);
            ImVec2 m = ImGui::GetIO().MousePos;
            d.graphPanX = m.x - p.x - (m.x - p.x - d.graphPanX) * z1 / z0;
            d.graphPanY = m.y - p.y - (m.y - p.y - d.graphPanY) * z1 / z0;
            d.graphZoom = z1;
        }
        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                      ImGui::IsMouseDragging(ImGuiMouseButton_Left, fs * 0.3f))) {
            d.graphPanX += ImGui::GetIO().MouseDelta.x;
            d.graphPanY += ImGui::GetIO().MouseDelta.y;
        }
        float z = d.graphZoom;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(24, 26, 30, 255));
        ImVec2 bs(200 * z, 44 * z);
        auto centre = [&](int i) { return ImVec2(p.x + d.graphPanX + pos[i].x * z + bs.x / 2, p.y + d.graphPanY + pos[i].y * z + bs.y / 2); };
        // transitions
        int hoverT = -1;
        ImVec2 mp = ImGui::GetIO().MousePos;
        for (size_t ti = 0; ti < sm.transitions.size(); ti++) {
            const auto& t = sm.transitions[ti];
            if (t.from < 0 || t.to < 0 || t.from >= (int)pos.size() || t.to >= (int)pos.size()) continue;
            ImVec2 a = centre(t.from), b = centre(t.to);
            float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
            if (l < 1) continue;
            float nx = dx / l, ny = dy / l, ox = -ny * 6 * z, oy = nx * 6 * z;   // offset so A->B and B->A don't overlap
            // clip to the boxes
            auto clip = [&](ImVec2 c, float sx, float sy) {
                float tx = fabsf(nx) > 1e-4f ? (bs.x / 2) / fabsf(nx) : 1e9f, ty = fabsf(ny) > 1e-4f ? (bs.y / 2) / fabsf(ny) : 1e9f;
                float k = std::min(tx, ty);
                return ImVec2(c.x + sx * nx * k, c.y + sy * ny * k);
            };
            ImVec2 a2 = clip(a, 1, 1), b2 = clip(b, -1, -1);
            a2.x += ox; a2.y += oy; b2.x += ox; b2.y += oy;
            bool sel = d.smState >= 0 && (t.from == d.smState || t.to == d.smState);
            float hx = mp.x - a2.x, hy = mp.y - a2.y, along = hx * nx + hy * ny, across = fabsf(hx * -ny + hy * nx);
            bool hov = hovered && along > 0 && along < l && across < 5;
            if (hov) hoverT = (int)ti;
            ImU32 col = hov ? IM_COL32(255, 255, 255, 255) : sel ? IM_COL32(240, 200, 90, 255) : IM_COL32(150, 155, 165, 255);
            dl->AddLine(a2, b2, col, sel || hov ? 2.5f : 1.5f);
            float ah = 9 * std::max(0.6f, z);
            dl->AddTriangleFilled(b2, ImVec2(b2.x - nx * ah + ny * ah * 0.5f, b2.y - ny * ah - nx * ah * 0.5f),
                                  ImVec2(b2.x - nx * ah - ny * ah * 0.5f, b2.y - ny * ah + nx * ah * 0.5f), col);
        }
        // states
        int hoverS = -1;
        for (size_t i = 0; i < sm.states.size(); i++) {
            ImVec2 c = centre((int)i), a(c.x - bs.x / 2, c.y - bs.y / 2), b(c.x + bs.x / 2, c.y + bs.y / 2);
            bool hov = hovered && mp.x >= a.x && mp.x < b.x && mp.y >= a.y && mp.y < b.y;
            if (hov) hoverS = (int)i;
            const auto& s = sm.states[i];
            ImU32 fill = (int)i == d.smState ? IM_COL32(150, 110, 40, 255) : (int)i == sm.initial ? IM_COL32(40, 80, 50, 255) : s.conduit ? IM_COL32(60, 80, 60, 255) : IM_COL32(52, 58, 72, 255);
            dl->AddRectFilled(a, b, fill, s.conduit ? bs.y / 2 : 6 * z);
            dl->AddRect(a, b, (int)i == sm.initial ? IM_COL32(120, 220, 120, 255) : hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(100, 105, 120, 255),
                        s.conduit ? bs.y / 2 : 6 * z, 0, (int)i == sm.initial ? 1.5f : 1.f);
            if (z > 0.35f) {
                ImVec2 ts = ImGui::CalcTextSize(s.name.c_str());
                dl->PushClipRect(a, b, true);
                dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(235, 235, 235, 255), s.name.c_str());
                dl->PopClipRect();
            }
        }
        dl->PopClipRect();
        auto sname = [&](int i) { return i >= 0 && i < (int)sm.states.size() ? sm.states[i].name : Fmt("#%d", i); };
        if (hoverS >= 0) {
            const auto& s = sm.states[hoverS];
            ImGui::SetTooltip("%s%s%s\n%zu player node(s), %zu outgoing transition(s)", s.name.c_str(), s.conduit ? " (conduit)" : "",
                              hoverS == sm.initial ? " (initial)" : "", s.players.size(), s.transitions.size());
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x == 0) d.smState = hoverS;
        } else if (hoverT >= 0) {
            const auto& t = sm.transitions[hoverT];
            ImGui::SetTooltip("%s -> %s\ncrossfade %.2f s, %s, %s%s", sname(t.from).c_str(), sname(t.to).c_str(), t.crossfade, t.blendMode.c_str(),
                              t.logic.c_str(), t.priority ? Fmt(", priority %d", t.priority).c_str() : "");
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoverS < 0) { d.smState = -1; d.graphZoom = 0; }
    }
    // details: the selected state's transitions, then the asset players
    ImGui::BeginChild("##smlist", ImVec2(0, 0), ImGuiChildFlags_None);
    if (!bp.machines.empty() && d.smState >= 0 && d.smState < (int)bp.machines[d.smSel].states.size()) {
        const auto& sm = bp.machines[d.smSel];
        ImGui::Text("%s", sm.states[d.smState].name.c_str());
        for (auto& t : sm.transitions)
            if (t.from == d.smState || t.to == d.smState) {
                bool out = t.from == d.smState;
                int other = out ? t.to : t.from;
                ImGui::TextDisabled("  %s %s  %.2f s %s", out ? "->" : "<-", other >= 0 && other < (int)sm.states.size() ? sm.states[other].name.c_str() : "?",
                                    t.crossfade, t.blendMode.c_str());
            }
        ImGui::Separator();
    } else {
        ImGui::TextDisabled("Click a state for its transitions; wheel zooms, drag pans, double-click fits.");
    }
    if (!bp.players.empty()) {
        ImGui::TextDisabled("Animation players (%zu)", bp.players.size());
        for (size_t i = 0; i < bp.players.size(); i++) {
            ImGui::PushID((int)i);
            if (ImGui::Selectable(ShortName(bp.players[i].asset).c_str())) {
                i64 e = m_api->ResolveFile(bp.players[i].asset);
                if (e >= 0) OpenFile((u32)e, true);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\nclick to open", bp.players[i].node.c_str(), bp.players[i].asset.c_str());
            ImGui::PopID();
        }
    }
    if (!bp.notifies.empty()) {
        ImGui::TextDisabled("Notify events (%zu)", bp.notifies.size());
        for (auto& n : bp.notifies) ImGui::BulletText("%s", n.c_str());
    }
    ImGui::EndChild();
}
