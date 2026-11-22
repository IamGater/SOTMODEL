// References tab, graph view: referencers on the left, the package in the middle, what it imports on the right.
// Click a node to re-centre on it, double-click to open it, right-click for the file menu.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "app/RefIndex.h"
#include "imgui.h"
#include <cmath>
#include <map>

namespace {

// a hue per asset family; unknown classes get a stable hue from their name
ImU32 ClassColor(const std::string& c) {
    float h;
    if (c.empty()) return IM_COL32(110, 112, 125, 255);
    if (c.find("Texture") != std::string::npos) h = 0.58f;
    else if (c.find("MaterialFunction") != std::string::npos) h = 0.11f;
    else if (c.find("Material") != std::string::npos) h = 0.07f;
    else if (c == "StaticMesh") h = 0.33f;
    else if (c == "SkeletalMesh" || c == "Skeleton" || c == "PhysicsAsset") h = 0.45f;
    else if (c.find("Anim") != std::string::npos || c == "BlendSpace" || c == "BlendSpace1D") h = 0.5f;
    else if (c.find("Blueprint") != std::string::npos || c == "Class") h = 0.75f;
    else if (c == "World" || c == "Level") h = 0.0f;
    else if (c.find("Particle") != std::string::npos) h = 0.9f;
    else if (c.find("DataTable") != std::string::npos || c.find("Curve") != std::string::npos) h = 0.18f;
    else h = (float)(std::hash<std::string>{}(c) % 1000) / 1000.f;
    return ImColor::HSV(h, 0.55f, 0.85f);
}

std::string Stem(const std::string& path) {
    size_t s = path.rfind('/') + 1, d = path.rfind('.');
    return path.substr(s, d == std::string::npos || d < s ? std::string::npos : d - s);
}

}   // namespace

void App::NavigateReferences(u32 entry) {
    if (entry == m_refTarget) return;
    if (m_refTarget != UINT32_MAX) m_refHistory.push_back(m_refTarget);
    if (m_refHistory.size() > 64) m_refHistory.erase(m_refHistory.begin());
    m_refTarget = entry;
    m_selected = entry;
}

void App::DrawDependencyGraph() {
    const Vfs& vfs = m_api->vfs;
    float fs = ImGui::GetFontSize();
    // toolbar
    ImGui::SameLine(0, fs * 1.5f);
    ImGui::BeginDisabled(m_refHistory.empty());
    if (ImGui::Button("< Back")) {
        m_refTarget = m_refHistory.back();
        m_refHistory.pop_back();
        m_selected = m_refTarget;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 3.5f);
    const char* depths[] = {"1", "2", "3"};
    int di = std::clamp(m_refDepth, 1, 3) - 1;
    if (ImGui::Combo("Depth", &di, depths, 3)) m_refDepth = di + 1;

    // (re)build in the background when the target or depth changed
    u64 key = ((u64)m_refDepth << 32) | m_refTarget;
    if (m_depGraphKey != key && m_depGraphPendingKey != key) {
        m_depGraphPendingKey = key;
        auto idx = m_api->references;
        u32 target = m_refTarget;
        int depth = m_refDepth;
        m_jobs->Push([this, idx, target, depth, key] {
            auto g = std::make_shared<DepGraph>(BuildDepGraph(m_api->vfs, *m_api->cache, idx.get(), target, depth, true, true, 40, 400));
            Post([this, g, key] {
                if (m_depGraphPendingKey != key) return;   // superseded
                m_depGraph = g;
                m_depGraphKey = key;
                m_depGraphPendingKey = 0;
            });
        });
    }
    std::shared_ptr<DepGraph> g = m_depGraph;
    if (!g || g->nodes.empty() || m_depGraphKey != key) {
        ImGui::TextDisabled("Reading package imports...");
        m_busyFrames = 3;
        if (!g || g->nodes.empty()) return;
    }
    const DepNode& root = g->nodes[0];
    int nIn = 0, nOut = 0;
    for (auto& n : g->nodes) {
        if (n.side < 0) nIn++;
        if (n.side > 0) nOut++;
    }
    ImGui::TextDisabled("%d referencer(s)%s  |  %d reference(s)%s   -  click a node to centre on it, double-click to open",
                        nIn, root.hiddenReferencers ? Fmt(" (+%d not shown)", root.hiddenReferencers).c_str() : "", nOut,
                        root.hiddenReferences ? Fmt(" (+%d not shown)", root.hiddenReferences).c_str() : "");

    // layout: one column per (side, depth), nodes stacked in their insertion order, columns centred vertically
    int maxDepth = 0;
    for (auto& n : g->nodes) maxDepth = std::max(maxDepth, n.depth);
    std::map<int, std::vector<int>> cols;
    for (int i = 0; i < (int)g->nodes.size(); i++) cols[g->nodes[i].side * g->nodes[i].depth].push_back(i);
    size_t rows = 1;
    for (auto& [c, v] : cols) rows = std::max(rows, v.size());
    float nodeW = fs * 12.5f, nodeH = fs * 2.6f, gapX = fs * 4, gapY = fs * 0.6f, pad = fs;
    int minCol = cols.begin()->first, maxCol = cols.rbegin()->first;
    ImVec2 total(pad * 2 + (maxCol - minCol + 1) * nodeW + (maxCol - minCol) * gapX, pad * 2 + rows * nodeH + (rows - 1) * gapY);
    std::vector<ImVec2> pos(g->nodes.size());
    for (auto& [c, v] : cols) {
        float x = pad + (c - minCol) * (nodeW + gapX);
        float y0 = pad + (total.y - pad * 2 - (v.size() * nodeH + (v.size() - 1) * gapY)) * 0.5f;
        for (size_t k = 0; k < v.size(); k++) pos[v[k]] = ImVec2(x, y0 + k * (nodeH + gapY));
    }

    ImGui::BeginChild("##depgraph", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    // centre on the root the first frame a graph is shown
    static u64 centred = 0;
    if (centred != key && m_depGraphKey == key) {
        centred = key;
        ImGui::SetScrollX(std::max(0.f, pos[0].x + nodeW * 0.5f - ImGui::GetWindowWidth() * 0.5f));
        ImGui::SetScrollY(std::max(0.f, pos[0].y + nodeH * 0.5f - ImGui::GetWindowHeight() * 0.5f));
    }
    // pan with the right / middle mouse button
    if (ImGui::IsWindowHovered() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - d.y);
    }

    // hovered node (from last frame's item rects) highlights its edges
    static int hoveredNode = -1;
    int hoverNow = -1;
    for (auto& [from, to] : g->edges) {
        ImVec2 a(o.x + pos[from].x + nodeW, o.y + pos[from].y + nodeH * 0.5f), b(o.x + pos[to].x, o.y + pos[to].y + nodeH * 0.5f);
        if (pos[from].x > pos[to].x) {   // an edge going back (cycles): from the importer's left to the imported's right
            a.x -= nodeW;
            b.x += nodeW;
        }
        bool hot = hoveredNode == from || hoveredNode == to;
        float dx = std::max(fs * 2, std::fabs(b.x - a.x) * 0.5f) * (a.x <= b.x ? 1.f : -1.f);
        dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, hot ? IM_COL32(120, 170, 255, 255) : IM_COL32(110, 112, 130, 140),
                           hot ? 2.2f : 1.2f);
        dl->AddTriangleFilled(ImVec2(b.x, b.y), ImVec2(b.x - fs * 0.45f * (a.x <= b.x ? 1 : -1), b.y - fs * 0.25f),
                              ImVec2(b.x - fs * 0.45f * (a.x <= b.x ? 1 : -1), b.y + fs * 0.25f), hot ? IM_COL32(120, 170, 255, 255) : IM_COL32(110, 112, 130, 200));
    }
    for (int i = 0; i < (int)g->nodes.size(); i++) {
        const DepNode& n = g->nodes[i];
        ImVec2 a(o.x + pos[i].x, o.y + pos[i].y), b(a.x + nodeW, a.y + nodeH);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        ImGui::InvisibleButton("##node", ImVec2(nodeW, nodeH));
        bool hov = ImGui::IsItemHovered();
        if (hov) hoverNow = i;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                SelectFolder(FolderByPath(n.path.substr(0, n.path.rfind('/'))));
                m_selected = n.entry;
                OpenFile(n.entry);
            } else if (i != 0) {
                NavigateReferences(n.entry);
            }
        }
        if (ImGui::BeginPopupContextItem("##nctx")) {
            DrawFileMenu(n.entry);
            ImGui::EndPopup();
        }
        if (hov) Tip("%s\n%s%s", n.path.c_str(), n.cls.empty() ? "?" : n.cls.c_str(),
                                   i == 0 ? "" : n.side < 0 ? "\nimports the package on its right" : "\nimported by the package on its left");
        ImGui::PopID();
        ImU32 cc = ClassColor(n.cls);
        dl->AddRectFilled(a, b, i == 0 ? IM_COL32(52, 62, 86, 255) : hov ? IM_COL32(62, 64, 80, 255) : IM_COL32(46, 47, 58, 255), fs * 0.3f);
        dl->AddRectFilled(a, ImVec2(a.x + fs * 0.35f, b.y), cc, fs * 0.3f, ImDrawFlags_RoundCornersLeft);
        dl->AddRect(a, b, i == 0 ? IM_COL32(32, 107, 212, 255) : (m_selected == (i64)n.entry ? IM_COL32(120, 170, 255, 200) : IM_COL32(31, 32, 41, 255)),
                    fs * 0.3f, 0, i == 0 ? 2.f : 1.f);
        ImVec4 clip(a.x + fs * 0.6f, a.y, b.x - fs * 0.3f, b.y);
        std::string name = Stem(n.path);
        dl->AddText(nullptr, 0, ImVec2(clip.x, a.y + fs * 0.25f), IM_COL32(240, 240, 240, 255), name.c_str(), nullptr, 0, &clip);
        std::string sub = n.cls.empty() ? std::string("?") : n.cls;
        int more = n.side < 0 ? n.hiddenReferencers : n.side > 0 ? n.hiddenReferences : 0;
        if (more) sub += Fmt("  (+%d)", more);
        dl->AddText(nullptr, 0, ImVec2(clip.x, a.y + fs * 1.3f), cc, sub.c_str(), nullptr, 0, &clip);
    }
    hoveredNode = hoverNow;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(total);
    ImGui::EndChild();
}
