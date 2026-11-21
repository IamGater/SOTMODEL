// Particle system view: simulated sprite preview, emitter list and module curves.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "imgui.h"
#include <cmath>

namespace {

ImU32 Col(const Vec4& c, float alphaScale = 1) {
    auto ch = [](float v) { return (int)std::clamp(v * 255.f, 0.f, 255.f); };
    return IM_COL32(ch(c.x), ch(c.y), ch(c.z), ch(c.w * alphaScale));
}

}   // namespace

void App::DrawParticlePanel(Document& d) {
    ParticleSystemInfo& ps = *d.particles;
    ParticleSim& sim = *d.psim;
    float fs = ImGui::GetFontSize();
    // transport
    if (ImGui::Button(d.psPlaying ? "Pause" : "Play")) d.psPlaying = !d.psPlaying;
    ImGui::SameLine();
    if (ImGui::Button("Restart")) { sim.Reset(); d.psTime = 0; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x - fs * 16));
    float t = d.psTime;
    if (ImGui::SliderFloat("##pst", &t, 0, std::max(d.psLength, t), "%.2f s")) { d.psTime = t; d.psPlaying = false; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 6);
    ImGui::SliderFloat("##pss", &d.psSpeed, 0.05f, 2.f, "speed %.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &d.psGrid);
    if (d.psPlaying) {
        d.psTime += ImGui::GetIO().DeltaTime * d.psSpeed;
        if (d.psLoop && d.psLength > 0 && d.psTime > d.psLength * 1.5f && sim.Count() == 0) { sim.Reset(); d.psTime = 0; }
        m_busyFrames = 3;
    }
    sim.AdvanceTo(d.psTime);
    // viewport
    float listH = std::min(fs * 16, ImGui::GetContentRegionAvail().y * 0.42f);
    ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(fs * 10, ImGui::GetContentRegionAvail().y - listH));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##psview", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemActive()) {
        ImVec2 md = ImGui::GetIO().MouseDelta;
        d.psYaw += md.x * 0.4f;
        d.psPitch = std::clamp(d.psPitch - md.y * 0.4f, -89.f, 89.f);
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) d.psDist *= powf(0.88f, ImGui::GetIO().MouseWheel);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(44, 46, 54, 255), IM_COL32(44, 46, 54, 255), IM_COL32(24, 25, 30, 255),
                                IM_COL32(24, 25, 30, 255));
    Vec3 eye, right, up, fwd;
    ParticleCamera(d.psCentre, d.psDist, d.psYaw, d.psPitch, eye, right, up, fwd);
    float focal = size.y / (2 * tanf(30 * kPi / 180));
    auto project = [&](Vec3 w, ImVec2& out, float& depth) {
        Vec3 v = w - eye;
        depth = Dot(v, fwd);
        if (depth < 1) return false;
        out = ImVec2(p.x + size.x / 2 + Dot(v, right) / depth * focal, p.y + size.y / 2 - Dot(v, up) / depth * focal);
        return true;
    };
    if (d.psGrid) {   // ground grid around the origin
        float step = std::max(10.f, powf(10, floorf(log10f(std::max(10.f, d.psDist * 0.3f)))));
        for (int i = -10; i <= 10; i++) {
            ImVec2 a, b;
            float da, db;
            ImU32 c = i == 0 ? IM_COL32(110, 110, 120, 160) : IM_COL32(80, 82, 90, 110);
            if (project({i * step, -10 * step, 0}, a, da) && project({i * step, 10 * step, 0}, b, db)) dl->AddLine(a, b, c);
            if (project({-10 * step, i * step, 0}, a, da) && project({10 * step, i * step, 0}, b, db)) dl->AddLine(a, b, c);
        }
    }
    std::vector<ParticleSprite> sprites;
    sim.Sprites(sprites);
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < sprites.size(); i++) {
        int e = sprites[i].emitter;
        if (e < (int)d.psVisible.size() && !d.psVisible[e]) continue;
        if (d.psSel >= 0 && d.psSolo && e != d.psSel) continue;
        order.push_back({Dot(sprites[i].pos - eye, fwd), i});
    }
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [depth, i] : order) {
        const ParticleSprite& s = sprites[i];
        const ParticleEmitterInfo& e = ps.emitters[s.emitter];
        ImVec2 c;
        float dz;
        if (!project(s.pos, c, dz)) continue;
        float hx = s.size.x * 0.5f / dz * focal, hy = s.size.y * 0.5f / dz * focal;
        float ax = cosf(s.rotation), ay = sinf(s.rotation);
        if (e.screenAlignment.find("Velocity") != std::string::npos && Length(s.velocity) > 1e-3f) {
            float vx = Dot(s.velocity, right), vy = -Dot(s.velocity, up), l = sqrtf(vx * vx + vy * vy);
            if (l > 1e-4f) { ax = vy / l; ay = -vx / l; }
        }
        ImVec2 ux(ax * hx, ay * hx), uy(-ay * hy, ax * hy);
        GpuTexture* tex = s.emitter < (int)d.psTextures.size() ? d.psTextures[s.emitter].get() : nullptr;
        bool additive = e.blendMode.find("Additive") != std::string::npos;
        ImU32 col = Col(s.color, additive ? 0.7f : 1.f);
        if (tex) {
            int cells = e.subH * e.subV, cell = ((int)s.subImage % cells + cells) % cells;
            float u0 = (float)(cell % e.subH) / e.subH, v0 = (float)(cell / e.subH) / e.subV, du = 1.f / e.subH, dv = 1.f / e.subV;
            dl->AddImageQuad((ImTextureID)(intptr_t)tex->srvRaw.Get(), ImVec2(c.x - ux.x - uy.x, c.y - ux.y - uy.y), ImVec2(c.x + ux.x - uy.x, c.y + ux.y - uy.y),
                             ImVec2(c.x + ux.x + uy.x, c.y + ux.y + uy.y), ImVec2(c.x - ux.x + uy.x, c.y - ux.y + uy.y), ImVec2(u0, v0), ImVec2(u0 + du, v0),
                             ImVec2(u0 + du, v0 + dv), ImVec2(u0, v0 + dv), col);
        } else {
            dl->AddCircleFilled(c, std::max(1.f, (hx + hy) * 0.5f), Col(s.color, 0.5f));
        }
    }
    dl->AddText(ImVec2(p.x + 6, p.y + 4), IM_COL32(200, 200, 200, 200), Fmt("%d particles | drag orbits, wheel zooms", (int)order.size()).c_str());
    dl->PopClipRect();
    // emitters | modules | curve
    ImGui::BeginChild("##pslist", ImVec2(0, 0), ImGuiChildFlags_None);
    float colW = ImGui::GetContentRegionAvail().x / 3;
    ImGui::BeginChild("##pse", ImVec2(colW, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("Emitters (%zu)", ps.emitters.size());
    ImGui::SameLine();
    ImGui::Checkbox("solo", &d.psSolo);
    for (size_t i = 0; i < ps.emitters.size(); i++) {
        auto& e = ps.emitters[i];
        ImGui::PushID((int)i);
        bool vis = i < d.psVisible.size() ? (bool)d.psVisible[i] : true;
        if (ImGui::Checkbox("##v", &vis) && i < d.psVisible.size()) d.psVisible[i] = vis;
        ImGui::SameLine();
        if (ImGui::Selectable(Fmt("%s  [%s]", e.name.c_str(), e.type.c_str()).c_str(), d.psSel == (int)i)) { d.psSel = (int)i; d.psModule = -1; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nmaterial %s (%s)\n%s%s\nduration %.2f s, loops %d%s\nspawn rate %s, %zu burst(s)", e.cls.c_str(), e.material.c_str(),
                              e.blendMode.c_str(), e.screenAlignment.c_str(), e.mesh.empty() ? "" : ("\nmesh " + e.mesh).c_str(), e.duration, e.loops,
                              e.subH * e.subV > 1 ? Fmt(", subUV %dx%d", e.subH, e.subV).c_str() : "", e.rate.Describe().c_str(), e.bursts.size());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##psm", ImVec2(colW, 0), ImGuiChildFlags_Borders);
    if (d.psSel >= 0 && d.psSel < (int)ps.emitters.size()) {
        auto& e = ps.emitters[d.psSel];
        if (!e.material.empty() && ImGui::SmallButton("Open material")) {
            i64 f = m_api->ResolveFile(e.material);
            if (f >= 0) OpenFile((u32)f, true);
        }
        if (!e.mesh.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open mesh")) {
                i64 f = m_api->ResolveFile(e.mesh);
                if (f >= 0) OpenFile((u32)f, true);
            }
        }
        for (size_t i = 0; i < e.modules.size(); i++) {
            auto& m = e.modules[i];
            std::string label = m.cls.rfind("ParticleModule", 0) == 0 ? m.cls.substr(14) : m.cls;
            if (!m.enabled) label += " (off)";
            if (!m.simulated) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::Selectable(Fmt("%s##%zu", label.c_str(), i).c_str(), d.psModule == (int)i)) d.psModule = (int)i;
            if (!m.simulated) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not simulated by the preview");
            }
        }
    } else {
        ImGui::TextDisabled("Select an emitter");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##psc", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (d.psSel >= 0 && d.psSel < (int)ps.emitters.size() && d.psModule >= 0 && d.psModule < (int)ps.emitters[d.psSel].modules.size()) {
        auto& m = ps.emitters[d.psSel].modules[d.psModule];
        ImGui::TextUnformatted(m.cls.c_str());
        static const ImU32 chCol[3] = {IM_COL32(230, 90, 90, 255), IM_COL32(90, 210, 90, 255), IM_COL32(100, 140, 240, 255)};
        for (auto& [n, c] : m.curves) {
            ImGui::TextDisabled("%s", n.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(c.Describe().c_str());
            if (c.count <= 1 && c.op != 2) continue;
            // plot every channel (min and max for random ranges) over 0..1
            ImVec2 a = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x, h = fs * 4;
            ImGui::Dummy(ImVec2(w, h));
            ImDrawList* cd = ImGui::GetWindowDrawList();
            cd->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(24, 26, 30, 255));
            float mn = 1e30f, mx = -1e30f;
            for (int ch = 0; ch < c.dims; ch++) {
                float lo, hi;
                c.Range(ch, lo, hi);
                mn = std::min(mn, lo);
                mx = std::max(mx, hi);
            }
            if (mx - mn < 1e-6f) { mx += 0.5f; mn -= 0.5f; }
            for (int ch = 0; ch < c.dims; ch++)
                for (int r = 0; r < (c.op >= 2 ? 2 : 1); r++) {
                    ImVec2 prev;
                    for (int k = 0; k <= 48; k++) {
                        float x = k / 48.f, v = c.Eval(x, ch, (float)r);
                        ImVec2 q(a.x + x * w, a.y + h - (v - mn) / (mx - mn) * (h - 4) - 2);
                        if (k) cd->AddLine(prev, q, c.dims == 1 ? IM_COL32(230, 200, 90, 255) : chCol[ch % 3], 1.5f);
                        prev = q;
                    }
                }
            cd->AddText(ImVec2(a.x + 2, a.y), IM_COL32(150, 150, 150, 255), Fmt("%g", mx).c_str());
            cd->AddText(ImVec2(a.x + 2, a.y + h - fs), IM_COL32(150, 150, 150, 255), Fmt("%g", mn).c_str());
        }
        for (auto& [n, v] : m.values) {
            ImGui::TextDisabled("%s", n.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(v.c_str());
        }
    } else {
        ImGui::TextDisabled("Select a module for its values and curves");
    }
    ImGui::EndChild();
    ImGui::EndChild();
}
