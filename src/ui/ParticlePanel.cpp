// Particle system view: simulated sprite preview, emitter list and module curves.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "render/ParticleDraw.h"
#include "imgui.h"
#include <cmath>

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
    // viewport (the 3D renderer: sprites are depth sorted and blended like the game's translucent / additive materials)
    float listH = std::min(fs * 16, ImGui::GetContentRegionAvail().y * 0.42f);
    ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(fs * 10, ImGui::GetContentRegionAvail().y - listH));
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::vector<ParticleSprite> sprites;
    sim.Sprites(sprites);
    std::vector<ParticleLook> looks = ParticleLooks(ps, d.psTextures);
    for (size_t e = 0; e < looks.size(); e++)
        looks[e].visible = (e >= d.psVisible.size() || d.psVisible[e]) && !(d.psSel >= 0 && d.psSolo && (int)e != d.psSel);
    int drawn = 0;
    d.draw.grid = d.psGrid;
    DrawViewportScene(*m_renderer, d.camera, d.draw, size, true, [&] {
        d.camera.target = d.psCentre;
        d.camera.distance = d.psDist;
    }, [&] {
        if (d.psGrid) {
            float cell = powf(10.f, floorf(log10f(std::max(10.f, d.psDist * 0.3f))));
            m_renderer->DrawGrid(cell / 10, 20);
        }
        drawn = DrawParticleSprites(*m_renderer, d.camera, sprites, looks, Mat4::Identity());
    });
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 6, p.y + 4), IM_COL32(200, 200, 200, 200),
                                        Fmt("%d particles | drag orbits, wheel zooms, F frames", drawn).c_str());
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
