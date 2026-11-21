// Main window laid out like FModel (MainWindow.xaml): menu with the explorer switch, the Archives / Folders / Packages
// panel, the assets explorer or the document tabs (JSON editor + preview column), the collapsible log and the status bar.
// Sizes and colours follow FModel's XAML; every icon is drawn here (no FModel artwork). The 3D preview is our own viewer.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "render/ShaderTools.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <cmath>
#include <set>

namespace {

// AdonisUI dark layers as FModel shows them
constexpr ImU32 kLayer0 = IM_COL32(42, 43, 52, 255);     // window, menu, splitters
constexpr ImU32 kBorder = IM_COL32(31, 32, 41, 255);     // group box borders, preview backgrounds
constexpr ImU32 kPanel = IM_COL32(38, 38, 48, 255);      // panels, editor, log
constexpr ImU32 kTabSel = IM_COL32(59, 61, 74, 255);     // selected tab, tab highlight strip
constexpr ImU32 kHover = IM_COL32(54, 56, 70, 255);
constexpr ImU32 kField = IM_COL32(50, 51, 65, 255);      // text boxes, combo boxes, buttons
constexpr ImU32 kFieldBorder = IM_COL32(65, 66, 85, 255);
constexpr ImU32 kSeparator = IM_COL32(74, 74, 94, 255);
constexpr ImU32 kAccent = IM_COL32(32, 107, 212, 255);   // #206BD4
constexpr ImU32 kAlert = IM_COL32(212, 146, 32, 255);    // #D49220
constexpr ImU32 kError = IM_COL32(194, 43, 43, 255);     // #C22B2B
constexpr ImU32 kText = IM_COL32(240, 240, 240, 255);
constexpr ImU32 kTextDim = IM_COL32(150, 151, 165, 255);
constexpr ImU32 kCardFolder = IM_COL32(50, 50, 63, 255);
constexpr ImU32 kCardFile = IM_COL32(50, 51, 65, 255);
constexpr ImU32 kBadge = IM_COL32(112, 85, 66, 255);

float U() { return ImGui::GetFontSize() / 16.f; }
float Dip(float v) { return v * U(); }   // FModel's XAML units (our UI scale matches FModel's fonts at the same DPI)
float FontDip(float dip) { return ImGui::GetStyle().FontSizeBase * dip / 12.f; }   // FModel base font is 12
float FieldH() { return Dip(24); }

ImU32 Alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 0xFF) * a) << 24); }
ImU32 Shade(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.f, v.x * k), std::min(1.f, v.y * k), std::min(1.f, v.z * k), v.w));
}

// Hover / press transitions: per item progress 0..1, eased (smoothstep) over ~120 ms. While any item is still moving
// the app keeps rendering frames (see App::Frame).
std::unordered_map<ImGuiID, float> g_anim;
bool g_animating = false;
float Anim(ImGuiID id, bool on, float seconds = 0.12f) {
    auto it = g_anim.find(id);
    if (it == g_anim.end()) {
        if (!on) return 0.f;
        it = g_anim.emplace(id, 0.f).first;
    }
    float& v = it->second;
    float step = std::min(ImGui::GetIO().DeltaTime, 0.05f) / seconds;
    v = on ? std::min(1.f, v + step) : std::max(0.f, v - step);
    float r = v * v * (3 - 2 * v);
    if (on ? v < 1.f : v > 0.f) g_animating = true;
    if (!on && v <= 0.f) g_anim.erase(it);
    return r;
}
float HoverAnim() { return Anim(ImGui::GetItemID(), ImGui::IsItemHovered()); }
ImU32 Mix(ImU32 a, ImU32 b, float t) {
    return ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(a), ImGui::ColorConvertU32ToFloat4(b), t));
}

void Text(ImDrawList* dl, ImVec2 p, ImU32 c, const char* s, const char* e = nullptr) { dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), p, c, s, e); }
void TextEllipsis(ImDrawList* dl, ImVec2 p, float maxX, ImU32 c, const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::RenderTextEllipsis(dl, p, ImVec2(maxX, p.y + ImGui::GetFontSize()), maxX, s, nullptr, nullptr);
    ImGui::PopStyleColor();
}
void TextCentered(ImDrawList* dl, float x0, float x1, float y, ImU32 c, const std::string& s) {
    float w = ImGui::CalcTextSize(s.c_str()).x;
    if (w <= x1 - x0) Text(dl, ImVec2(x0 + (x1 - x0 - w) * 0.5f, y), c, s.c_str());
    else TextEllipsis(dl, ImVec2(x0, y), x1, c, s.c_str());
}

// ------------------------------------------------------------------ icons (own drawings)

void IconSearch(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float r = s * 0.28f, t = std::max(1.f, s * 0.1f);
    ImVec2 o(c.x - s * 0.08f, c.y - s * 0.08f);
    dl->AddCircle(o, r, col, 0, t);
    dl->AddLine(ImVec2(o.x + r * 0.7f, o.y + r * 0.7f), ImVec2(c.x + s * 0.38f, c.y + s * 0.38f), col, t * 1.2f);
}
void IconBackspace(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {   // key shape pointing left with an x
    float w = s * 0.8f, h = s * 0.56f, x0 = c.x - w * 0.5f, t = std::max(1.f, s * 0.08f);
    ImVec2 pts[5] = {{x0, c.y}, {x0 + w * 0.3f, c.y - h * 0.5f}, {x0 + w, c.y - h * 0.5f}, {x0 + w, c.y + h * 0.5f}, {x0 + w * 0.3f, c.y + h * 0.5f}};
    dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, t);
    float k = h * 0.2f, cx = x0 + w * 0.64f;
    dl->AddLine(ImVec2(cx - k, c.y - k), ImVec2(cx + k, c.y + k), col, t);
    dl->AddLine(ImVec2(cx - k, c.y + k), ImVec2(cx + k, c.y - k), col, t);
}
void IconClose(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float k = s * 0.28f, t = std::max(1.f, s * 0.1f);
    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t);
    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, t);
}
void IconPlus(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float k = s * 0.32f, t = std::max(1.f, s * 0.11f);
    dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), col, t);
    dl->AddLine(ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k), col, t);
}
// chevron pointing along angle (radians, 0 = right, y down)
void IconChevron(ImDrawList* dl, ImVec2 c, float s, float angle, ImU32 col) {
    ImVec2 d(cosf(angle), sinf(angle)), n(-d.y, d.x);
    float a = s * 0.18f;
    ImVec2 tip(c.x + d.x * a, c.y + d.y * a), b(c.x - d.x * a, c.y - d.y * a);
    ImVec2 pts[3] = {{b.x + n.x * a * 1.6f, b.y + n.y * a * 1.6f}, tip, {b.x - n.x * a * 1.6f, b.y - n.y * a * 1.6f}};
    dl->AddPolyline(pts, 3, col, 0, std::max(1.2f, s * 0.11f));
}
void IconGrid(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float g = s * 0.1f, q = (s * 0.8f - g) * 0.5f, x0 = c.x - s * 0.4f, y0 = c.y - s * 0.4f;
    for (int i = 0; i < 4; i++) {
        float x = x0 + (i % 2) * (q + g), y = y0 + (i / 2) * (q + g);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + q, y + q), col, s * 0.05f);
    }
}
void IconList(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    for (int i = 0; i < 3; i++) {
        float y = c.y - s * 0.3f + i * s * 0.3f;
        dl->AddRectFilled(ImVec2(c.x - s * 0.4f, y - s * 0.06f), ImVec2(c.x - s * 0.24f, y + s * 0.06f), col);
        dl->AddRectFilled(ImVec2(c.x - s * 0.14f, y - s * 0.06f), ImVec2(c.x + s * 0.4f, y + s * 0.06f), col);
    }
}
void IconLocate(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float r = s * 0.24f, t = std::max(1.f, s * 0.09f);
    dl->AddCircle(c, r, col, 0, t);
    dl->AddCircleFilled(c, s * 0.08f, col);
    for (int k = 0; k < 4; k++) {
        ImVec2 d(k == 0 ? 1.f : k == 1 ? -1.f : 0.f, k == 2 ? 1.f : k == 3 ? -1.f : 0.f);
        dl->AddLine(ImVec2(c.x + d.x * r, c.y + d.y * r), ImVec2(c.x + d.x * s * 0.42f, c.y + d.y * s * 0.42f), col, t);
    }
}
void IconFold(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {   // two chevrons pointing at each other
    float a = s * 0.18f;
    IconChevron(dl, ImVec2(c.x, c.y - a * 1.4f), s, kPi * 0.5f, col);
    IconChevron(dl, ImVec2(c.x, c.y + a * 1.4f), s, -kPi * 0.5f, col);
}
void IconOpenExternal(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.f, s * 0.08f), h = s * 0.36f;
    ImVec2 pts[5] = {{c.x, c.y - h}, {c.x - h, c.y - h}, {c.x - h, c.y + h}, {c.x + h, c.y + h}, {c.x + h, c.y}};
    dl->AddPolyline(pts, 5, col, 0, t);
    dl->AddLine(ImVec2(c.x - h * 0.1f, c.y + h * 0.1f), ImVec2(c.x + h * 1.05f, c.y - h * 1.05f), col, t);
    dl->AddLine(ImVec2(c.x + h * 0.35f, c.y - h * 1.05f), ImVec2(c.x + h * 1.05f, c.y - h * 1.05f), col, t);
    dl->AddLine(ImVec2(c.x + h * 1.05f, c.y - h * 1.05f), ImVec2(c.x + h * 1.05f, c.y - h * 0.35f), col, t);
}
void IconTrash(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float w = s * 0.42f, h = s * 0.5f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.62f, c.y - h * 0.62f), ImVec2(c.x + w * 0.62f, c.y - h * 0.48f), col, 1);
    dl->AddRectFilled(ImVec2(c.x - w * 0.2f, c.y - h * 0.76f), ImVec2(c.x + w * 0.2f, c.y - h * 0.6f), col, 1);
    dl->AddRectFilled(ImVec2(c.x - w * 0.48f, c.y - h * 0.38f), ImVec2(c.x + w * 0.48f, c.y + h * 0.6f), col, s * 0.06f);
    for (int k = -1; k <= 1; k++) dl->AddLine(ImVec2(c.x + k * w * 0.2f, c.y - h * 0.2f), ImVec2(c.x + k * w * 0.2f, c.y + h * 0.42f), kPanel, std::max(1.f, s * 0.06f));
}
void IconExpandLog(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool expanded) {   // up + down arrows
    float k = s * 0.16f, t = std::max(1.f, s * 0.08f);
    ImVec2 a(c.x - k * 1.2f, c.y), b(c.x + k * 1.2f, c.y);
    dl->AddLine(ImVec2(a.x, a.y - k * 2.2f), ImVec2(a.x, a.y + k * 2.2f), col, t);
    dl->AddLine(ImVec2(b.x, b.y - k * 2.2f), ImVec2(b.x, b.y + k * 2.2f), col, t);
    float da = expanded ? 1.f : -1.f;   // which way the heads point flips with the state
    dl->AddTriangleFilled(ImVec2(a.x, a.y - da * k * 2.6f), ImVec2(a.x - k, a.y - da * k * 1.4f), ImVec2(a.x + k, a.y - da * k * 1.4f), col);
    dl->AddTriangleFilled(ImVec2(b.x, b.y + da * k * 2.6f), ImVec2(b.x - k, b.y + da * k * 1.4f), ImVec2(b.x + k, b.y + da * k * 1.4f), col);
}
void IconRefresh(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float r = s * 0.3f, t = std::max(1.f, s * 0.09f);
    dl->PathArcTo(c, r, kPi * 0.15f, kPi * 1.75f, 16);
    dl->PathStroke(col, 0, t);
    ImVec2 e(c.x + r * cosf(kPi * 0.15f), c.y + r * sinf(kPi * 0.15f));
    dl->AddTriangleFilled(ImVec2(e.x + s * 0.12f, e.y - s * 0.02f), ImVec2(e.x - s * 0.1f, e.y - s * 0.04f), ImVec2(e.x + s * 0.02f, e.y + s * 0.16f), col);
}
void IconInfo(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddCircle(c, s * 0.38f, col, 0, std::max(1.f, s * 0.08f));
    dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.16f), s * 0.05f, col);
    dl->AddRectFilled(ImVec2(c.x - s * 0.04f, c.y - s * 0.04f), ImVec2(c.x + s * 0.04f, c.y + s * 0.2f), col);
}
void IconKey(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.f, s * 0.09f);
    dl->AddCircle(ImVec2(c.x - s * 0.18f, c.y), s * 0.17f, col, 0, t);
    dl->AddLine(ImVec2(c.x - s * 0.01f, c.y), ImVec2(c.x + s * 0.38f, c.y), col, t);
    dl->AddLine(ImVec2(c.x + s * 0.26f, c.y), ImVec2(c.x + s * 0.26f, c.y + s * 0.14f), col, t);
    dl->AddLine(ImVec2(c.x + s * 0.36f, c.y), ImVec2(c.x + s * 0.36f, c.y + s * 0.1f), col, t);
}
void IconCube(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float r = s * 0.38f, t = std::max(1.f, s * 0.08f);
    ImVec2 top(c.x, c.y - r), ul(c.x - r * 0.87f, c.y - r * 0.5f), ur(c.x + r * 0.87f, c.y - r * 0.5f);
    ImVec2 bl(c.x - r * 0.87f, c.y + r * 0.5f), br(c.x + r * 0.87f, c.y + r * 0.5f), bot(c.x, c.y + r);
    ImVec2 hex[6] = {top, ur, br, bot, bl, ul};
    dl->AddPolyline(hex, 6, col, ImDrawFlags_Closed, t);
    dl->AddLine(ul, c, col, t);
    dl->AddLine(ur, c, col, t);
    dl->AddLine(c, bot, col, t);
}
void IconClock(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.f, s * 0.08f);
    dl->AddCircle(c, s * 0.36f, col, 0, t);
    dl->AddLine(c, ImVec2(c.x, c.y - s * 0.22f), col, t);
    dl->AddLine(c, ImVec2(c.x + s * 0.16f, c.y + s * 0.08f), col, t);
}
void IconNote(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.f, s * 0.09f);
    dl->AddCircleFilled(ImVec2(c.x - s * 0.16f, c.y + s * 0.22f), s * 0.12f, col);
    dl->AddLine(ImVec2(c.x - s * 0.05f, c.y + s * 0.22f), ImVec2(c.x - s * 0.05f, c.y - s * 0.32f), col, t);
    dl->AddLine(ImVec2(c.x - s * 0.05f, c.y - s * 0.32f), ImVec2(c.x + s * 0.25f, c.y - s * 0.2f), col, t);
}
void IconImages(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.f, s * 0.08f), w = s * 0.34f;
    dl->AddRect(ImVec2(c.x - w, c.y - w * 0.8f), ImVec2(c.x + w, c.y + w * 0.8f), col, 1, 0, t);
    dl->AddTriangleFilled(ImVec2(c.x - w * 0.7f, c.y + w * 0.6f), ImVec2(c.x - w * 0.1f, c.y - w * 0.1f), ImVec2(c.x + w * 0.4f, c.y + w * 0.6f), col);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.45f, c.y - w * 0.35f), s * 0.06f, col);
}
void IconExportBox(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.f, s * 0.08f), w = s * 0.3f;
    dl->AddRect(ImVec2(c.x - w, c.y - w * 0.4f), ImVec2(c.x + w * 0.5f, c.y + w), col, 1, 0, t);
    dl->AddLine(ImVec2(c.x - w * 0.2f, c.y + w * 0.2f), ImVec2(c.x + w * 1.2f, c.y - w * 1.2f), col, t);
    dl->AddLine(ImVec2(c.x + w * 0.5f, c.y - w * 1.2f), ImVec2(c.x + w * 1.2f, c.y - w * 1.2f), col, t);
    dl->AddLine(ImVec2(c.x + w * 1.2f, c.y - w * 1.2f), ImVec2(c.x + w * 1.2f, c.y - w * 0.5f), col, t);
}
void IconTag(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {   // label: pointed tag with a hole
    float w = s * 0.82f, h = s * 0.5f, x0 = c.x - w * 0.5f;
    ImVec2 pts[5] = {{x0, c.y - h * 0.5f}, {x0 + w * 0.7f, c.y - h * 0.5f}, {x0 + w, c.y}, {x0 + w * 0.7f, c.y + h * 0.5f}, {x0, c.y + h * 0.5f}};
    dl->AddConvexPolyFilled(pts, 5, col);
    dl->AddCircleFilled(ImVec2(x0 + w * 0.68f, c.y), s * 0.06f, kPanel);
}
void IconArchive(ImDrawList* dl, ImVec2 c, float s, bool enabled) {   // pak: green sheet with a zip strip
    ImU32 body = enabled ? IM_COL32(96, 186, 112, 255) : IM_COL32(120, 120, 130, 255);
    float w = s * 0.7f, h = s * 0.88f, fold = w * 0.3f;
    ImVec2 p(c.x - w * 0.5f, c.y - h * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + w - fold, p.y + h), body, s * 0.06f);
    dl->AddRectFilled(ImVec2(p.x + w - fold - 1, p.y + fold), ImVec2(p.x + w, p.y + h), body, s * 0.06f);
    dl->AddTriangleFilled(ImVec2(p.x + w - fold, p.y), ImVec2(p.x + w, p.y + fold), ImVec2(p.x + w - fold, p.y + fold), Shade(body, 0.7f));
    for (int i = 0; i < 4; i++) {
        float y = p.y + h * 0.18f + i * h * 0.17f;
        dl->AddRectFilled(ImVec2(c.x - w * 0.12f + (i % 2) * w * 0.12f, y), ImVec2(c.x + (i % 2) * w * 0.12f, y + h * 0.09f), IM_COL32(235, 248, 236, 255));
    }
}

// Folder (closed / open) in FModel's warm two-tone palette; size = width of the icon box
void IconFolder(ImDrawList* dl, ImVec2 mn, float size, bool open) {
    float w = size, h = size * 0.82f, r = size * 0.08f;
    ImVec2 p(mn.x, mn.y + (size - h) * 0.5f);
    const ImU32 tabA = IM_COL32(232, 196, 128, 255), tabB = IM_COL32(217, 170, 99, 255);
    const ImU32 bodyA = IM_COL32(243, 217, 164, 255), bodyB = IM_COL32(229, 189, 119, 255), line = IM_COL32(172, 120, 63, 255);
    float t = std::max(1.f, size / 18.f);
    // back panel with its tab
    dl->AddRectFilled(p, ImVec2(p.x + w * 0.45f, p.y + h * 0.3f), tabA, r);
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.14f), ImVec2(p.x + w, p.y + h), tabA, tabA, tabB, tabB);
    dl->AddRect(ImVec2(p.x, p.y + h * 0.14f), ImVec2(p.x + w, p.y + h), line, 0, 0, t);
    if (!open) {
        dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.36f), ImVec2(p.x + w, p.y + h), bodyA, bodyA, bodyB, bodyB);
        dl->AddRect(ImVec2(p.x, p.y + h * 0.36f), ImVec2(p.x + w, p.y + h), line, 0, 0, t);
    } else {   // front panel tilted forward
        ImVec2 q[4] = {{p.x + w * 0.2f, p.y + h * 0.42f}, {p.x + w * 1.06f, p.y + h * 0.42f}, {p.x + w * 0.86f, p.y + h}, {p.x, p.y + h}};
        dl->AddConvexPolyFilled(q, 4, bodyA);
        dl->AddPolyline(q, 4, line, ImDrawFlags_Closed, t);
    }
}

ImU32 FileTint(std::string_view name) {
    if (IEndsWith(name, ".umap")) return IM_COL32(255, 160, 40, 255);
    if (IEndsWith(name, ".uasset")) return IM_COL32(40, 150, 255, 255);
    if (IEndsWith(name, ".ubulk") || IEndsWith(name, ".uexp")) return IM_COL32(160, 120, 235, 255);
    return IM_COL32(245, 215, 40, 255);
}

// Per type glyphs: folded map (.umap), cube (.uasset), sheet with lines (anything else)
void FileGlyph(ImDrawList* dl, ImVec2 c, float s, std::string_view name, ImU32 bg) {
    ImU32 col = FileTint(name);
    if (IEndsWith(name, ".umap")) {
        float w = s * 0.9f, h = s * 0.7f, x0 = c.x - w * 0.5f, y0 = c.y - h * 0.5f, pw = w / 3, k = h * 0.1f;
        for (int i = 0; i < 3; i++) {
            float xa = x0 + pw * i, xb = xa + pw, ya = (i % 2) ? k : 0, yb = (i % 2) ? 0 : k;
            dl->AddQuadFilled(ImVec2(xa, y0 + ya), ImVec2(xb, y0 + yb), ImVec2(xb, y0 + h + yb - k), ImVec2(xa, y0 + h + ya - k), (i % 2) ? Shade(col, 0.8f) : col);
        }
        dl->AddCircleFilled(ImVec2(x0 + pw * 1.5f, y0 + h * 0.42f), s * 0.08f, bg);
        return;
    }
    if (IEndsWith(name, ".uasset")) {
        float r = s * 0.45f;
        ImVec2 top(c.x, c.y - r), ul(c.x - r * 0.87f, c.y - r * 0.5f), ur(c.x + r * 0.87f, c.y - r * 0.5f);
        ImVec2 bl(c.x - r * 0.87f, c.y + r * 0.5f), br(c.x + r * 0.87f, c.y + r * 0.5f), bot(c.x, c.y + r);
        dl->AddQuadFilled(top, ur, c, ul, Shade(col, 1.15f));
        dl->AddQuadFilled(ul, c, bot, bl, col);
        dl->AddQuadFilled(c, ur, br, bot, Shade(col, 0.72f));
        return;
    }
    float w = s * 0.62f, h = s * 0.8f, fold = w * 0.3f;
    ImVec2 p(c.x - w * 0.5f, c.y - h * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + w - fold, p.y + h), col, s * 0.04f);
    dl->AddRectFilled(ImVec2(p.x + w - fold - 1, p.y + fold), ImVec2(p.x + w, p.y + h), col, s * 0.04f);
    dl->AddTriangleFilled(ImVec2(p.x + w - fold, p.y), ImVec2(p.x + w, p.y + fold), ImVec2(p.x + w - fold, p.y + fold), Shade(col, 0.7f));
    for (int i = 0; i < 3; i++) {
        float ly = p.y + h * (0.45f + i * 0.15f);
        dl->AddRectFilled(ImVec2(p.x + w * 0.18f, ly), ImVec2(p.x + w * (i == 2 ? 0.55f : 0.82f), ly + s * 0.05f), bg);
    }
}

using IconFn = void (*)(ImDrawList*, ImVec2, float, ImU32);

// ------------------------------------------------------------------ widgets

// Toolbar button (AdonisUI ToolbarButton): flat, highlighted on hover
bool ToolbarButton(const char* id, ImVec2 size, IconFn icon, const char* tip, bool enabled = true) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float ht = enabled ? HoverAnim() : 0.f;
    if (ht > 0) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::IsItemActive() ? kAccent : Alpha(kTabSel, ht), Dip(2));
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    icon(dl, ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), Dip(16), enabled ? kText : kTextDim);
    return r && enabled;
}

// FModel SearchTextBox: field with a magnifier on the left and a "clear" toolbar button on the right
bool SearchBox(const char* id, char* buf, size_t size, const char* hint, float width, bool focus) {
    float h = FieldH(), fs = ImGui::GetFontSize();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), kField);
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dip(25), (h - fs) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::SetNextItemWidth(width - h);
    if (focus) ImGui::SetKeyboardFocusHere();
    bool changed = ImGui::InputTextWithHint("##t", hint, buf, size);
    bool active = ImGui::IsItemActive();
    float ht = HoverAnim(), at = Anim(ImGui::GetItemID() ^ 0x5eed, active);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    IconSearch(dl, ImVec2(p.x + Dip(5) + Dip(8), p.y + h * 0.5f), Dip(16), kText);
    ImGui::SameLine(0, 0);
    if (ToolbarButton("##clear", ImVec2(h, h), IconBackspace, "Clear Search Filter")) {
        buf[0] = 0;
        changed = true;
    }
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), Mix(Mix(kFieldBorder, Shade(kFieldBorder, 1.35f), ht), kAccent, at));
    ImGui::PopID();
    return changed;
}

void PushFieldStyle() {
    float h = FieldH();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dip(6), (h - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kField);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kField);
    ImGui::PushStyleColor(ImGuiCol_Border, kFieldBorder);
    ImGui::PushStyleColor(ImGuiCol_Button, kField);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTabSel);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
}
void PopFieldStyle() {
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(2);
}
bool Combo(const char* id, int* v, const char* const items[], int n, float width) {
    PushFieldStyle();
    ImGui::SetNextItemWidth(width);
    bool r = ImGui::Combo(id, v, items, n);
    PopFieldStyle();
    return r;
}
bool Button(const char* label, ImVec2 size, bool enabled = true) {
    PushFieldStyle();
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, ImVec2(size.x, size.y > 0 ? size.y : FieldH()));
    ImGui::EndDisabled();
    PopFieldStyle();
    return r;
}

// FModel CustomSeparator: line, optional bold caption in the middle, 8 dip margins
void SeparatorTag(const char* tag, float margin = -1) {
    if (margin < 0) margin = Dip(8);
    ImGui::Dummy(ImVec2(0, margin - ImGui::GetStyle().ItemSpacing.y));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = tag && *tag ? FontDip(11) : 1;
    float y = std::round(p.y + h * 0.5f);
    if (tag && *tag) {
        ImGui::PushFont(nullptr, FontDip(11));
        ImVec2 ts = ImGui::CalcTextSize(tag);
        float x0 = p.x + (w - ts.x) * 0.5f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(x0 - Dip(10), y), kSeparator);
        dl->AddLine(ImVec2(x0 + ts.x + Dip(10), y), ImVec2(p.x + w, y), kSeparator);
        dl->AddText(nullptr, 0, ImVec2(x0, p.y), kText, tag);   // pushed font
        ImGui::PopFont();
    } else {
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + w, y), kSeparator);
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Dummy(ImVec2(0, margin - ImGui::GetStyle().ItemSpacing.y));
}

// INFORMATION block: value on the left (trimmed), caption on the right
void InfoRows(const std::vector<std::pair<std::string, const char*>>& rows) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x, lh = ImGui::GetTextLineHeight() * 1.25f;
    for (auto& [value, caption] : rows) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float cw = ImGui::CalcTextSize(caption).x;
        Text(dl, ImVec2(p.x + w - cw, p.y), kText, caption);
        TextEllipsis(dl, p, p.x + w - cw - Dip(10), kText, value.c_str());
        ImGui::Dummy(ImVec2(w, lh - ImGui::GetStyle().ItemSpacing.y));
    }
}

// list row background; returns pressed (on press, like WPF list boxes select)
struct RowState { bool pressed, hovered, doubleClicked, rightClicked; };
RowState ListRow(const char* id, ImVec2 size, bool selected) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    RowState s{ImGui::IsItemClicked(ImGuiMouseButton_Left), ImGui::IsItemHovered(), false, ImGui::IsItemClicked(ImGuiMouseButton_Right)};
    s.doubleClicked = s.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    float ht = HoverAnim(), st = Anim(ImGui::GetItemID() ^ 0x5e1, selected, 0.08f);
    if (st > 0 || ht > 0) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Mix(Alpha(kHover, ht), kAccent, st));
    return s;
}

// menu entry with a 16 dip icon in the margin
bool MenuItemIcon(const char* label, const char* shortcut, IconFn icon, bool enabled = true) {
    std::string padded = std::string("      ") + label;
    bool r = ImGui::MenuItem(padded.c_str(), shortcut, false, enabled);
    if (icon) {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        icon(ImGui::GetWindowDrawList(), ImVec2(mn.x + Dip(8), (mn.y + mx.y) * 0.5f), Dip(14), enabled ? kText : kTextDim);
    }
    return r;
}

std::string FolderPath(const std::vector<VfsFolder>& folders, u32 f) {
    std::string out;
    for (; f != UINT32_MAX && f != 0; f = folders[f].parent) out = folders[f].name + (out.empty() ? "" : "/") + out;
    return out;
}

// JSON colouring (FModel's Json.xshd palette)
constexpr ImU32 kJsonDefault = IM_COL32(0xDA, 0xE5, 0xF2, 255);
constexpr ImU32 kJsonKey = IM_COL32(0xFF, 0xCB, 0x6B, 255);
constexpr ImU32 kJsonString = IM_COL32(0xC3, 0xE8, 0x8D, 255);
constexpr ImU32 kJsonNumber = IM_COL32(0xF7, 0x8C, 0x6C, 255);
constexpr ImU32 kJsonBool = IM_COL32(0x61, 0xAF, 0xEF, 255);
constexpr ImU32 kJsonNull = IM_COL32(0x7F, 0x84, 0x8E, 255);
constexpr ImU32 kJsonPunct = IM_COL32(0x89, 0xDD, 0xFF, 255);

}   // namespace

// ================================================================== frame

void App::Frame() {
    {
        std::vector<std::function<void()>> posted;
        {
            std::lock_guard lk(m_postMutex);
            posted.swap(m_posted);
        }
        for (auto& f : posted) f();
        if (!posted.empty()) m_busyFrames = 3;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() ||
        io.InputQueueCharacters.Size)
        m_busyFrames = 3;
    else if (m_busyFrames > 0)
        m_busyFrames--;
    if (m_docs.empty()) AddTab();   // FModel always keeps at least the "New Tab"
    if (m_player.Finished()) {   // end of the clip: loop or stop
        if (m_playLoop && m_player.Clip()) {
            for (auto& dp : m_docs)
                if (dp->id == m_playDoc) {
                    auto pcm = dp->audio;
                    if (dp->isBank && dp->bankSel >= 0 && dp->bankSel < (int)dp->bank.wems.size()) pcm = dp->bankPcm[dp->bank.wems[dp->bankSel].id];
                    PlayAudio(*dp, pcm, 0);
                }
        } else {
            m_player.Stop();
        }
    }
    if (m_playDoc && std::none_of(m_docs.begin(), m_docs.end(), [&](auto& dp) { return dp->id == m_playDoc; })) {   // its tab was closed
        m_player.Stop();
        m_playDoc = 0;
    }
    g_animating = false;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kLayer0);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    DrawMenu();
    HandleHotkeys();

    // rows: menu | 8 dip | panels | status bar (28 dip)
    float statusH = Dip(27), gap = Dip(8);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float bodyY = origin.y + gap, bodyH = avail.y - gap - statusH - Dip(4);
    float splitW = Dip(4), minLeft = Dip(400) * 0.8f;
    float leftW = m_settings.leftPanelWidth > 0 ? m_settings.leftPanelWidth : std::max(minLeft, avail.x * 0.37f);
    leftW = std::clamp(leftW, std::min(minLeft, avail.x * 0.5f), std::max(minLeft, avail.x - Dip(300)));

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bodyY));
    DrawLeftPanel(leftW, bodyH);

    // column splitter (double click: automatic width)
    ImGui::SetCursorScreenPos(ImVec2(origin.x + leftW, bodyY));
    ImGui::InvisibleButton("##colsplit", ImVec2(splitW, bodyH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive() && io.MouseDelta.x != 0) m_settings.leftPanelWidth = leftW + io.MouseDelta.x;
    if (ImGui::IsItemDeactivated()) m_settings.Save();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { m_settings.leftPanelWidth = 0; m_settings.Save(); }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + leftW + splitW, bodyY));
    DrawRightPanel(avail.x - leftW - splitW, bodyH);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y - statusH));
    DrawStatusBar();
    ImGui::End();

    if (m_showSettings) DrawSettings();
    if (m_showSearch) DrawSearchWindow();
    if (m_showBackups) DrawBackupWindow();
    DrawExportProgress();
    if (g_animating) m_busyFrames = std::max(m_busyFrames, 2);   // transitions still running: keep drawing
}

void App::HandleHotkeys() {
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput;
    bool ready = m_mountState == MountState::Ready;
    if (ready && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F)) { m_showSearch = true; m_searchTab = 0; m_focusGlobalSearch = true; }
    if (ready && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R)) FindReferences(m_selected >= 0 ? (u32)m_selected : m_refTarget);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_T)) AddTab();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W) && m_activeDoc) CloseDocument(m_activeDoc);
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false) && !m_explorerVisible) {
        if (Document* d = ActiveDoc()) { d->searchOpen = true; d->focusSearch = true; }
    }
    // single-letter shortcuts (FModel defaults) only when nothing is being typed or dragged (WASD flies the 3D view)
    if (typing || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Right) || io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_A, false) && m_leftTab > 0) m_leftTab--;
    if (ImGui::IsKeyPressed(ImGuiKey_D, false) && m_leftTab < 2) m_leftTab++;
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && ready) m_explorerVisible = !m_explorerVisible;
    auto step = [&](int dir) {
        for (size_t i = 0; i < m_docs.size(); i++)
            if (m_docs[i]->id == m_activeDoc) { m_activeDoc = m_docs[(i + m_docs.size() + dir) % m_docs.size()]->id; break; }
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false) && !m_docs.empty()) step(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && !m_docs.empty()) step(1);
}

// ================================================================== menu

void App::DrawMenu() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dip(6), Dip(5)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dip(6), Dip(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(6), Dip(6)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kTabSel);
    ImGui::PushStyleColor(ImGuiCol_Header, kTabSel);
    if (!ImGui::BeginMenuBar()) {
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
        return;
    }
    bool ready = m_mountState == MountState::Ready;
    if (ImGui::BeginMenu("Directory")) {
        if (MenuItemIcon("Selector", nullptr, [](ImDrawList* dl, ImVec2 c, float s, ImU32 col) { IconFolder(dl, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), s, false); (void)col; }))
            m_showSettings = true;
        if (MenuItemIcon("AES", nullptr, IconKey, ready)) m_showSettings = true;
        if (MenuItemIcon("Backup", nullptr, IconClock, ready)) m_showBackups = true;
        if (MenuItemIcon("Archives Info", nullptr, IconInfo, ready)) ShowArchivesInfo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Packages")) {
        if (MenuItemIcon("Search", "Ctrl+Shift+F", IconSearch, ready)) { m_showSearch = true; m_searchTab = 0; m_focusGlobalSearch = true; }
        if (MenuItemIcon("References", "Ctrl+Shift+R", IconSearch, ready)) FindReferences(m_selected >= 0 ? (u32)m_selected : m_refTarget);
        {
            ImVec2 rp = ImGui::GetCursorScreenPos();
            bool open = ImGui::BeginMenu("      Favorite Directories", ready);
            IconFolder(ImGui::GetWindowDrawList(), ImVec2(rp.x + Dip(1), ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - Dip(14)) * 0.5f), Dip(14), true);
            if (open) {
                if (m_settings.favoriteDirs.empty()) ImGui::MenuItem("Right-click a folder > Favorite Directory", nullptr, false, false);
                for (auto& f : m_settings.favoriteDirs)
                    if (ImGui::MenuItem(f.c_str())) { m_leftTab = 1; SelectFolder(FolderByPath(f)); }
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Views")) {
        Document* d = ActiveDoc();
        bool has3d = d && !d->loading && (d->hasMesh || d->anim || d->isWorld);
        if (MenuItemIcon("3D Viewer", nullptr, IconCube, has3d)) m_explorerVisible = false;
        MenuItemIcon("Export Session", nullptr, IconExportBox, false);
        if (MenuItemIcon("Audio Player", nullptr, IconNote, d && (d->audio || d->isBank))) m_explorerVisible = false;
        MenuItemIcon("Image Merger", nullptr, IconImages, false);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Settings")) m_showSettings = true;
    if (ImGui::BeginMenu("Help")) {
        MenuItemIcon("Automation: SOTModel.exe --mcp / --cli tools", nullptr, IconInfo, false);
        MenuItemIcon("3D view: LMB orbit, RMB + WASD/QE fly, MMB pan, F frame", nullptr, IconCube, false);
        MenuItemIcon("About SOTModel", nullptr, IconInfo, false);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

// ================================================================== left panel

void App::DrawLeftPanel(float width, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), kPanel);
    dl->AddRect(p, ImVec2(p.x + width, p.y + height), kBorder);
    const VfsFolder* cur = m_mountState == MountState::Ready ? &m_api->vfs.Folders()[m_folder] : nullptr;
    // tab row: three tabs filling the width, then the highlight strip
    float tabH = Dip(25.6f), strip = Dip(4);
    std::string labels[3] = {"Archives", "Folders", Fmt("%zu Packages", cur ? cur->files.size() : (size_t)0)};
    float tw = (width - 2) / 3.f;
    for (int i = 0; i < 3; i++) {
        ImVec2 a(p.x + 1 + tw * i, p.y + 1), b(a.x + tw, a.y + tabH);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##ltab", ImVec2(tw, tabH))) m_leftTab = i;
        ImGui::PopID();
        bool sel = m_leftTab == i;
        float ht = HoverAnim(), st = Anim(ImGui::GetItemID() ^ 0x7ab, sel);
        dl->AddRectFilled(a, b, Mix(Mix(kPanel, kHover, ht), kTabSel, st));
        if (i > 0) dl->AddRectFilled(ImVec2(a.x - 1, a.y), ImVec2(a.x + 1, b.y), kBorder);
        ImVec2 ts = ImGui::CalcTextSize(labels[i].c_str());
        Text(dl, ImVec2(a.x + (tw - ts.x) * 0.5f, a.y + (tabH - ts.y) * 0.5f), kText, labels[i].c_str());
    }
    dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1 + tabH), ImVec2(p.x + width - 1, p.y + 1 + tabH + strip), kTabSel);

    float top = 1 + tabH + strip;
    ImGui::SetCursorScreenPos(ImVec2(p.x + 1, p.y + top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(8), Dip(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dip(5), Dip(5)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::BeginChild("##leftbody", ImVec2(width - 2, height - top - 1), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (m_mountState != MountState::Ready && m_leftTab != 0) {
        if (m_mountState == MountState::Mounting) ImGui::TextDisabled("Loading archives...");
        else {
            ImGui::TextWrapped("Archives are not loaded. Open Directory > Selector to choose the game folder and the AES key.");
            if (!m_mountError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError), "%s", m_mountError.c_str());
        }
    } else if (m_leftTab == 0) {
        DrawArchivesTab();
    } else if (m_leftTab == 1) {
        DrawFoldersTab();
    } else {
        DrawPackagesTab();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void App::DrawArchivesTab() {
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, (FieldH() - ImGui::GetFontSize()) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Loading Mode");
    ImGui::PopStyleVar();
    ImGui::SameLine(0, Dip(10));
    const char* modes[] = {"All"};
    int mode = 0;
    ImGui::BeginDisabled(m_mountState == MountState::Mounting);
    Combo("##loadmode", &mode, modes, 1, ImGui::GetContentRegionAvail().x);
    if (Button("Load", ImVec2(w, 0), m_mountState != MountState::Mounting)) ReloadArchives();
    ImGui::EndDisabled();
    SeparatorTag("GAME ARCHIVES");

    float infoH = (ImGui::GetTextLineHeight() * 1.25f) * 4 + FontDip(11) + Dip(16) + Dip(6);
    ImGui::BeginChild("##paks", ImVec2(0, ImGui::GetContentRegionAvail().y - infoH));
    if (m_mountState != MountState::Ready) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError), "%s", m_mountState == MountState::Mounting ? "Loading archives..." : "No archives found in the specified directory");
    } else {
        auto& paks = m_api->vfs.Paks();
        if (m_pakSizes.size() != paks.size()) m_pakSizes.assign(paks.size(), UINT64_MAX);
        float rowH = Dip(22.4f), rw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip;
        clip.Begin((int)paks.size(), rowH);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                if (m_pakSizes[i] == UINT64_MAX) {
                    WIN32_FILE_ATTRIBUTE_DATA fad = {};
                    m_pakSizes[i] = GetFileAttributesExW(paks[i]->path.c_str(), GetFileExInfoStandard, &fad) ? ((u64)fad.nFileSizeHigh << 32 | fad.nFileSizeLow) : 0;
                }
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                RowState rs = ListRow("##pak", ImVec2(rw, rowH), m_selPak == i);
                if (rs.pressed) m_selPak = i;
                float cy = p.y + rowH * 0.5f, ty = cy - ImGui::GetFontSize() * 0.5f;
                IconArchive(dl, ImVec2(p.x + Dip(5) + Dip(8), cy), Dip(16), true);
                std::string files = Fmt("%u files", paks[i]->fileCount), size = HumanSize(m_pakSizes[i]);
                float sizeX = p.x + rw - Dip(5) - ImGui::CalcTextSize(size.c_str()).x;
                float filesRight = p.x + rw - Dip(5) - Dip(75) - Dip(10);
                ImGui::PushFont(nullptr, FontDip(11));
                float fw = ImGui::CalcTextSize(files.c_str()).x;
                bool showFiles = rw > Dip(330);
                if (showFiles) Text(dl, ImVec2(filesRight - fw, cy - ImGui::GetFontSize() * 0.5f + 1), rs.hovered || m_selPak == i ? kText : kTextDim, files.c_str());
                ImGui::PopFont();
                float nameMax = (showFiles ? filesRight - Dip(110) : sizeX) - Dip(10);
                TextEllipsis(dl, ImVec2(p.x + Dip(5) + Dip(20), ty), std::max(nameMax, p.x + Dip(60)), kText, paks[i]->name.c_str());
                Text(dl, ImVec2(sizeX, ty), kText, size.c_str());
                ImGui::PopID();
            }
    }
    ImGui::EndChild();
    SeparatorTag("INFORMATION");
    if (m_mountState == MountState::Ready && m_selPak >= 0 && m_selPak < (int)m_api->vfs.Paks().size()) {
        auto& pk = *m_api->vfs.Paks()[m_selPak];
        InfoRows({{pk.mountPoint.empty() ? "/" : pk.mountPoint, "Mount Point"},
                  {Fmt("%u Files", pk.fileCount), "File Count"},
                  {pk.keyIndex >= 0 ? "True" : "False", "Is Encrypted"},
                  {"00000000000000000000000000000000", "Global Unique Identifier"}});
    } else {
        InfoRows({{"/", "Mount Point"}, {"0 Files", "File Count"}, {"False", "Is Encrypted"}, {"00000000000000000000000000000000", "Global Unique Identifier"}});
    }
}

void App::SelectFolder(u32 folder) {
    if (m_mountState != MountState::Ready) return;
    auto& folders = m_api->vfs.Folders();
    m_folder = folder;
    for (u32 p = folders[folder].parent; p != UINT32_MAX && p != 0; p = folders[p].parent) m_expanded.insert(p);
    m_scrollTreeTo = folder;
    m_explorerVisible = true;
}

void App::DrawFolderMenu(u32 f) {
    const auto& folders = m_api->vfs.Folders();
    if (ImGui::BeginMenu("Export Folder")) {
        if (ImGui::MenuItem("Raw Data (.uasset)")) ExportFiles(FolderFilesRecursive(f), 1);
        ImGui::MenuItem("Embedded Files", nullptr, false, false);
        if (ImGui::MenuItem("Properties (.json)")) ExportFiles(FolderFilesRecursive(f), 2);
        if (ImGui::MenuItem("Textures")) ExportFiles(FolderFilesRecursive(f), 0, 1);
        if (ImGui::MenuItem("Decompiled Blueprints")) ExportFiles(FolderFilesRecursive(f), 0, 6);
        if (ImGui::MenuItem("Models")) ExportFiles(FolderFilesRecursive(f), 0, 2);
        if (ImGui::MenuItem("Worlds")) ExportFiles(FolderFilesRecursive(f), 0, 3);
        if (ImGui::MenuItem("Animations")) ExportFiles(FolderFilesRecursive(f), 0, 4);
        if (ImGui::MenuItem("Audio")) ExportFiles(FolderFilesRecursive(f), 0, 5);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Favorite Directory", nullptr, IsFavorite(f))) ToggleFavorite(f);
    if (ImGui::MenuItem("Copy Directory Path")) CopyToClipboard(FolderPath(folders, f));
}

void App::DrawFoldersTab() {
    auto& vfs = m_api->vfs;
    auto& folders = vfs.Folders();
    const VfsFolder& cur = folders[m_folder];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // header: label, summary, locate / collapse
    {
        float bs = Dip(26), w = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();
        IconTag(dl, ImVec2(p.x + Dip(8), p.y + bs * 0.5f), Dip(16), IM_COL32(94, 188, 144, 255));
        std::string s = Fmt("'%s' has %zu folders and %zu packages", m_folder ? cur.name.c_str() : "None", cur.folders.size(), cur.files.size());
        TextEllipsis(dl, ImVec2(p.x + Dip(19.5f), p.y + (bs - ImGui::GetFontSize()) * 0.5f), p.x + w - bs * 2 - Dip(10), kText, s.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bs * 2, p.y));
        if (ToolbarButton("##locate", ImVec2(bs, bs), IconLocate, "Bring Selected Folder To View")) SelectFolder(m_folder);
        ImGui::SameLine(0, 0);
        if (ToolbarButton("##fold", ImVec2(bs, bs), IconFold, "Collapse All")) m_expanded.clear();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bs));
    }
    SeparatorTag(nullptr, Dip(2));

    // visible rows of the tree (depth-first over expanded folders)
    struct Row { u32 f; int depth; };
    std::vector<Row> rows;
    rows.reserve(256);
    std::vector<Row> stack;
    for (auto it = folders[0].folders.rbegin(); it != folders[0].folders.rend(); ++it) stack.push_back({*it, 0});
    while (!stack.empty()) {
        Row r = stack.back();
        stack.pop_back();
        rows.push_back(r);
        if (m_expanded.count(r.f))
            for (auto it = folders[r.f].folders.rbegin(); it != folders[r.f].folders.rend(); ++it) stack.push_back({*it, r.depth + 1});
    }

    float infoH = (ImGui::GetTextLineHeight() * 1.25f) * 5 + FontDip(11) + Dip(16) + Dip(6);
    ImGui::BeginChild("##tree", ImVec2(0, ImGui::GetContentRegionAvail().y - infoH), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    dl = ImGui::GetWindowDrawList();
    float rowH = Dip(22), rw = ImGui::GetContentRegionAvail().x;
    if (m_scrollTreeTo != UINT32_MAX) {
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].f == m_scrollTreeTo) {
                float y = i * rowH, sy = ImGui::GetScrollY(), wh = ImGui::GetWindowHeight();
                if (y < sy || y + rowH > sy + wh) ImGui::SetScrollY(std::max(0.f, y - wh * 0.4f));
                break;
            }
        m_scrollTreeTo = UINT32_MAX;
    }
    bool focused = ImGui::IsWindowFocused();
    int selIndex = -1;
    ImGuiListClipper clip;
    clip.Begin((int)rows.size(), rowH);
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const Row& r = rows[i];
            const VfsFolder& f = folders[r.f];
            ImGui::PushID((int)r.f);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float x = p.x + r.depth * Dip(16);
            bool hasKids = !f.folders.empty(), open = m_expanded.count(r.f) > 0, sel = m_folder == r.f;
            // the expander arrow is its own hit box (toggles on press, like FModel)
            bool arrowHover = hasKids && ImGui::IsMouseHoveringRect(ImVec2(x, p.y), ImVec2(x + Dip(20), p.y + rowH));
            RowState rs = ListRow("##row", ImVec2(std::max(rw, x - p.x + Dip(250)), rowH), sel);
            if (rs.pressed) {
                if (arrowHover) {
                    if (open) m_expanded.erase(r.f);
                    else m_expanded.insert(r.f);
                } else {
                    m_folder = r.f;
                    m_explorerVisible = true;
                }
            }
            if (rs.doubleClicked && !arrowHover) {
                if (hasKids) {
                    if (open) m_expanded.erase(r.f);
                    else m_expanded.insert(r.f);
                } else if (!f.files.empty()) {
                    m_leftTab = 2;
                }
            }
            if (rs.hovered) ImGui::SetTooltip("%s has %zu folders and %zu packages", f.name.c_str(), f.folders.size(), f.files.size());
            if (ImGui::BeginPopupContextItem("##fctx")) {
                DrawFolderMenu(r.f);
                ImGui::EndPopup();
            }
            float cy = p.y + rowH * 0.5f;
            if (hasKids) IconChevron(dl, ImVec2(x + Dip(10), cy), Dip(16), open ? kPi * 0.25f : 0.f, kText);
            float ix = x + Dip(20);
            IconFolder(dl, ImVec2(ix, cy - Dip(8)), Dip(16), open || sel);
            float tx = ix + Dip(16) + Dip(3.5f);
            if (!f.files.empty()) {   // package count badge over the folder's corner
                ImGui::PushFont(nullptr, FontDip(7));
                std::string n = std::to_string(f.files.size());
                float bw = std::max(Dip(10), ImGui::CalcTextSize(n.c_str()).x + Dip(5)), bh = Dip(10);
                ImVec2 b0(tx - Dip(10.5f), cy + Dip(8) - bh);
                dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), kBadge, bh * 0.5f);
                Text(dl, ImVec2(b0.x + (bw - ImGui::CalcTextSize(n.c_str()).x) * 0.5f, b0.y + (bh - ImGui::GetFontSize()) * 0.5f), kText, n.c_str());
                ImGui::PopFont();
                tx = b0.x + bw + Dip(4.5f);
            }
            Text(dl, ImVec2(tx, cy - ImGui::GetFontSize() * 0.5f), kText, f.name.c_str());
            if (sel) selIndex = i;
            ImGui::PopID();
        }
    if (selIndex < 0)
        for (size_t i = 0; i < rows.size(); i++) if (rows[i].f == m_folder) { selIndex = (int)i; break; }
    // keyboard (FolderTree.OnPreviewKeyDown)
    if (focused && !rows.empty() && !ImGui::GetIO().WantTextInput) {
        int next = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next = std::max(0, selIndex - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next = std::min((int)rows.size() - 1, selIndex + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) next = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End)) next = (int)rows.size() - 1;
        if (selIndex >= 0) {
            u32 f = rows[selIndex].f;
            bool open = m_expanded.count(f) > 0;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                if (open) m_expanded.erase(f);
                else if (folders[f].parent != 0 && folders[f].parent != UINT32_MAX) SelectFolder(folders[f].parent);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                if (!open && !folders[f].folders.empty()) m_expanded.insert(f);
                else if (!folders[f].folders.empty()) SelectFolder(folders[f].folders[0]);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                if ((open || folders[f].folders.empty()) && !folders[f].files.empty()) m_leftTab = 2;
                else {
                    while (folders[f].folders.size() == 1 && folders[f].files.empty()) { m_expanded.insert(f); f = folders[f].folders[0]; }
                    m_expanded.insert(f);
                    SelectFolder(f);
                }
            }
        }
        if (next >= 0 && next != selIndex) SelectFolder(rows[next].f);
    }
    ImGui::EndChild();

    SeparatorTag("INFORMATION");
    std::string archive = "None", mount = "/", version = "VER_UE4_LATEST";
    if (!cur.files.empty()) {
        const VfsEntry& e = vfs.Entry(cur.files[0]);
        auto& pk = *vfs.Paks()[e.pak];
        archive = pk.name;
        mount = pk.mountPoint.empty() ? "/" : pk.mountPoint;
        version = Fmt("PAK v%u", pk.version);
    }
    InfoRows({{Fmt("%zu", cur.files.size()), "Packages Count"},
              {Fmt("%zu", cur.folders.size()), "Folders Count"},
              {archive, "Included In Archive"},
              {mount, "Archive Mount Point"},
              {version, "Archive Version"}});
}

std::string& App::FolderFilter() {
    if (m_filterFolder != m_folder) {
        if (m_filterFolder != UINT32_MAX) m_folderFilters[m_filterFolder] = m_filterBuf;
        m_filterFolder = m_folder;
        auto it = m_folderFilters.find(m_folder);
        snprintf(m_filterBuf, sizeof(m_filterBuf), "%s", it == m_folderFilters.end() ? "" : it->second.c_str());
    }
    static std::string s;
    s = m_filterBuf;
    return s;
}

// FModel Breadcrumb: path segments with hover boxes, chevrons between them; the last segment is the current folder
void App::Breadcrumb(u32 folder) {
    auto& folders = m_api->vfs.Folders();
    std::vector<u32> chain;
    for (u32 f = folder; f != UINT32_MAX && f != 0; f = folders[f].parent) chain.push_back(f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pad(Dip(6), Dip(3));
    float h = ImGui::GetFontSize() + pad.y * 2;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (chain.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        Text(dl, ImVec2(p.x + pad.x, p.y + pad.y), kTextDim, "No/Directory/Detected/In/Folder");
        ImGui::Dummy(ImVec2(1, h));
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        bool last = it + 1 == chain.rend();
        const std::string& name = folders[*it].name;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 sz(ImGui::CalcTextSize(name.c_str()).x + pad.x * 2, h);
        ImGui::PushID((int)*it);
        bool clicked = ImGui::InvisibleButton("##seg", sz);
        ImGui::PopID();
        float bt = last ? 0.f : HoverAnim();
        if (bt > 0) {
            dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), Alpha(IM_COL32(72, 73, 92, 255), bt));
            dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), Alpha(IM_COL32(127, 127, 144, 255), bt));
        }
        if (!last && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        Text(dl, ImVec2(p.x + pad.x, p.y + pad.y), kText, name.c_str());
        if (clicked && !last) SelectFolder(*it);
        if (!last) {
            ImGui::SameLine(0, 0);
            ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(Dip(16), h));
            IconChevron(dl, ImVec2(c.x + Dip(8), c.y + h * 0.5f), Dip(16), 0, Alpha(kText, 0.6f));
            ImGui::SameLine(0, 0);
        }
    }
    ImGui::PopStyleVar();
}

void App::DrawPackagesTab() {
    auto& vfs = m_api->vfs;
    auto& folders = vfs.Folders();
    FolderFilter();
    SearchBox("##pkgfilter", m_filterBuf, sizeof(m_filterBuf), "Filter assets in current folder...", 0, false);
    ImGui::Dummy(ImVec2(0, Dip(5) - ImGui::GetStyle().ItemSpacing.y));
    Breadcrumb(m_folder);
    ImGui::Dummy(ImVec2(0, Dip(5) - ImGui::GetStyle().ItemSpacing.y));

    std::string filter = ToLower(m_filterBuf);
    std::vector<u32> files;
    for (u32 e : folders[m_folder].files)
        if (filter.empty() || ToLower(vfs.FileName(e)).find(filter) != std::string::npos) files.push_back(e);

    float infoH = (ImGui::GetTextLineHeight() * 1.25f) * 5 + FontDip(11) + Dip(16) + Dip(6);
    ImGui::BeginChild("##pkglist", ImVec2(0, ImGui::GetContentRegionAvail().y - infoH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (files.empty()) {
        float w = ImGui::GetContentRegionAvail().x;
        const char* msg = "No packages found in folder";
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize(msg).x) * 0.5f);
        ImGui::PushFont(boldFont, 0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError), "%s", msg);
        ImGui::PopFont();
    }
    float rowH = Dip(22), rw = ImGui::GetContentRegionAvail().x;
    bool focused = ImGui::IsWindowFocused();
    int selIndex = -1;
    ImGuiListClipper clip;
    clip.Begin((int)files.size(), rowH);
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            u32 e = files[i];
            ImGui::PushID((int)e);
            ImVec2 p = ImGui::GetCursorScreenPos();
            RowState rs = ListRow("##f", ImVec2(rw, rowH), m_selected == (i64)e);
            if (rs.pressed || rs.rightClicked) m_selected = e;
            if (rs.doubleClicked) { m_explorerVisible = false; OpenFile(e); }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                DrawFileMenu(e);
                ImGui::EndPopup();
            }
            float cy = p.y + rowH * 0.5f;
            std::string_view name = vfs.FileName(e);
            FileGlyph(dl, ImVec2(p.x + Dip(5) + Dip(8), cy), Dip(16), name, m_selected == (i64)e ? kAccent : kPanel);
            Text(dl, ImVec2(p.x + Dip(5) + Dip(19), cy - ImGui::GetFontSize() * 0.5f), kText, std::string(name).c_str());
            ImGui::PopID();
        }
    for (size_t i = 0; i < files.size(); i++) if ((i64)files[i] == m_selected) { selIndex = (int)i; break; }
    if (focused && !files.empty() && !ImGui::GetIO().WantTextInput) {
        int next = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next = std::max(0, selIndex - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next = std::min((int)files.size() - 1, selIndex + 1);
        if (next >= 0) {
            m_selected = files[next];
            float y = next * rowH, sy = ImGui::GetScrollY(), wh = ImGui::GetWindowHeight();
            if (y < sy) ImGui::SetScrollY(y);
            if (y + rowH > sy + wh) ImGui::SetScrollY(y + rowH - wh);
        }
        if (selIndex >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter)) { m_explorerVisible = false; OpenFile(files[selIndex]); }
    }
    ImGui::EndChild();

    SeparatorTag("INFORMATION");
    if (m_selected >= 0 && (size_t)m_selected < vfs.FileCount()) {
        const VfsEntry& e = vfs.Entry((u32)m_selected);
        InfoRows({{Fmt("0x%llX", (unsigned long long)e.offset), "Offset"},
                  {HumanSize(e.usize), "Size"},
                  {CompressionName(e.comp), "Compression Method"},
                  {e.encrypted ? "True" : "False", "Is Encrypted"},
                  {vfs.Paks()[e.pak]->name, "Included In Archive"}});
    } else {
        InfoRows({{"0x0", "Offset"}, {"0 B", "Size"}, {"Unknown", "Compression Method"}, {"False", "Is Encrypted"}, {"None", "Included In Archive"}});
    }
}

// ================================================================== right panel

void App::DrawRightPanel(float width, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float logBlock = Dip(26) + Dip(10) + (m_settings.loggerExpanded ? Dip(175) : 0);   // expander header + margins + log
    float contentH = height - logBlock;
    bool explorer = m_explorerVisible && m_mountState == MountState::Ready;

    ImGui::SetCursorScreenPos(p);
    if (explorer) {
        dl->AddRectFilled(p, ImVec2(p.x + width - Dip(3), p.y + contentH), kPanel);
        dl->AddRect(p, ImVec2(p.x + width - Dip(3), p.y + contentH), kBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(10), Dip(10)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dip(5), Dip(5)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("##explorerbox", ImVec2(width - Dip(3), contentH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        DrawExplorer();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    } else {
        DrawDocTabs(contentH);
    }

    // floating switch, bottom right of the content area (FModel's Assets Explorer toggle)
    if (m_mountState == MountState::Ready) {
        float b = Dip(32), pad = Dip(4);
        ImVec2 mx(p.x + width - Dip(3) - Dip(20), p.y + contentH - Dip(12));
        ImVec2 mn(mx.x - b * 2 - Dip(2) - pad * 2, mx.y - b - pad * 2);
        float op = 0.5f + 0.5f * Anim(ImGui::GetID("##floatpanel"), ImGui::IsMouseHoveringRect(mn, mx), 0.2f);   // fades in over 0.2 s
        ImDrawList* fg = ImGui::GetWindowDrawList();
        fg->AddRectFilled(mn, mx, Alpha(IM_COL32(50, 50, 63, 255), op), Dip(8));
        ImGui::SetCursorScreenPos(ImVec2(mn.x + pad, mn.y + pad));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##explorertoggle", ImVec2(b, b))) m_explorerVisible = !m_explorerVisible;
        bool bh = ImGui::IsItemHovered();
        float bt = HoverAnim();
        ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
        ImU32 bg = Mix(m_explorerVisible ? kAccent : IM_COL32(38, 38, 48, 255), kTabSel, bt);
        fg->AddRectFilled(a, z, Alpha(bg, op), Dip(8));
        fg->AddRect(a, z, Alpha(kPanel, op), Dip(8));
        ImVec2 c((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f);
        if (m_explorerVisible) IconFolder(fg, ImVec2(c.x - Dip(9), c.y - Dip(9)), Dip(18), false);
        else FileGlyph(fg, c, Dip(18), "x.uasset", bg);
        if (bh) ImGui::SetTooltip(m_explorerVisible ? "Assets Explorer (Z)" : "File Properties (Z)");
        // texture thumbnails on the explorer cards
        ImGui::SetCursorScreenPos(ImVec2(mn.x + pad + b + Dip(2), mn.y + pad));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##previewtex", ImVec2(b, b))) { m_settings.previewTextures = !m_settings.previewTextures; m_settings.Save(); }
        bool ph = ImGui::IsItemHovered();
        float pt = HoverAnim();
        a = ImGui::GetItemRectMin();
        z = ImGui::GetItemRectMax();
        bg = Mix(m_settings.previewTextures ? IM_COL32(147, 112, 219, 255) : IM_COL32(38, 38, 48, 255), kTabSel, pt);
        fg->AddRectFilled(a, z, Alpha(bg, op), Dip(8));
        fg->AddRect(a, z, Alpha(kPanel, op), Dip(8));
        IconImages(fg, ImVec2((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f), Dip(18), Alpha(kText, std::max(op, 0.8f)));
        if (ph) ImGui::SetTooltip(m_settings.previewTextures ? "Preview Textures (ON)" : "Preview Textures (OFF)");
    }

    // log expander
    float ly = p.y + contentH + Dip(5);
    ImGui::SetCursorScreenPos(ImVec2(p.x, ly));
    DrawLog(width - Dip(3));
}

void App::DrawExplorer() {
    auto& vfs = m_api->vfs;
    auto& folders = vfs.Folders();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    FolderFilter();
    // search | categories | grid / list
    {
        float avail = ImGui::GetContentRegionAvail().x, catW = Dip(150), tb = Dip(32);
        float searchW = std::max(Dip(120), avail - Dip(5) - catW - Dip(5) - tb * 2);
        ImVec2 p = ImGui::GetCursorScreenPos();
        SearchBox("##xfilter", m_filterBuf, sizeof(m_filterBuf), "Filter assets in current folder...", searchW, false);
        ImGui::SetCursorScreenPos(ImVec2(p.x + searchW + Dip(5), p.y));
        const char* cats[] = {"All", "Packages", "Maps", "Other files"};
        Combo("##cat", &m_explorerFilter, cats, 4, catW);
        float bh = FieldH();
        for (int i = 0; i < 2; i++) {
            ImVec2 bp(p.x + avail - tb * (2 - i), p.y);
            ImGui::SetCursorScreenPos(bp);
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("##view", ImVec2(tb, bh));
            ImGui::PopID();
            bool on = m_settings.explorerViewMode == i, hov = ImGui::IsItemHovered();
            float ht = HoverAnim(), ot = Anim(ImGui::GetItemID() ^ 0x0e, on);
            dl->AddRectFilled(bp, ImVec2(bp.x + tb, bp.y + bh), Mix(Mix(kField, kTabSel, ht), kAccent, ot));
            dl->AddRect(bp, ImVec2(bp.x + tb, bp.y + bh), on ? Shade(kAccent, 1.3f) : kBorder);
            (i == 0 ? IconGrid : IconList)(dl, ImVec2(bp.x + tb * 0.5f, bp.y + bh * 0.5f), Dip(14), kText);
            if (hov) ImGui::SetTooltip(i == 0 ? "Grid View" : "List View");
            if (clicked && !on) { m_settings.explorerViewMode = i; m_settings.Save(); }
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bh + Dip(5)));
    }
    Breadcrumb(m_folder);
    ImGui::Dummy(ImVec2(0, Dip(2)));

    // folders then files of the current folder, filtered
    std::string filter = ToLower(m_filterBuf);
    std::vector<u32> subfolders, files;
    if (m_explorerFilter == 0)
        for (u32 f : folders[m_folder].folders)
            if (filter.empty() || ToLower(folders[f].name).find(filter) != std::string::npos) subfolders.push_back(f);
    for (u32 e : folders[m_folder].files) {
        std::string_view n = vfs.FileName(e);
        bool pkg = IsPackageFile(n), map = IEndsWith(n, ".umap");
        if (m_explorerFilter == 1 && !pkg) continue;
        if (m_explorerFilter == 2 && !map) continue;
        if (m_explorerFilter == 3 && pkg) continue;
        if (!filter.empty() && ToLower(n).find(filter) == std::string::npos) continue;
        files.push_back(e);
    }
    int total = (int)(subfolders.size() + files.size());
    ImGui::BeginChild("##tiles", ImVec2(0, 0), ImGuiChildFlags_None);
    dl = ImGui::GetWindowDrawList();
    std::string viewKey = Fmt("#%u|%s|%d|%d", m_folder, filter.c_str(), m_explorerFilter, m_settings.explorerViewMode);
    if (viewKey != m_explorerView) {
        m_explorerView = viewKey;
        ImGui::SetScrollY(0);
    }
    if (total == 0) {
        const char* msg = "No folders or packages found";
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) * 0.5f);
        ImGui::PushFont(boldFont, 0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError), "%s", msg);
        ImGui::PopFont();
        ImGui::EndChild();
        return;
    }
    auto openFolder = [&](u32 f) { SelectFolder(f); };
    auto folderMenu = [&](u32 f) {
        if (ImGui::BeginPopupContextItem("##fctx")) {
            DrawFolderMenu(f);
            ImGui::EndPopup();
        }
    };
    auto fileMenu = [&](u32 e) {
        if (ImGui::BeginPopupContextItem("##ctx")) {
            DrawFileMenu(e);
            ImGui::EndPopup();
        }
    };
    const ImU32 accentBorder = kAccent;

    if (m_settings.explorerViewMode == 1) {
        // list: name / type / details columns (3 : 1 : 1.2), 32 dip rows
        float rowH = Dip(32), rw = ImGui::GetContentRegionAvail().x, mx = Dip(8);
        float inner = rw - mx * 2, c1 = inner * 3 / 5.2f, c2 = inner * 1 / 5.2f;
        ImGuiListClipper clip;
        clip.Begin(total, rowH);
        while (clip.Step())
            for (int idx = clip.DisplayStart; idx < clip.DisplayEnd; idx++) {
                bool isFolder = idx < (int)subfolders.size();
                u32 id = isFolder ? subfolders[idx] : files[idx - subfolders.size()];
                ImGui::PushID(idx);
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool sel = isFolder ? m_selFolder == id : m_selected == (i64)id;
                RowState rs = ListRow("##lrow", ImVec2(rw, rowH), sel);
                float cy = p.y + rowH * 0.5f, x = p.x + mx;
                if (isFolder) {
                    if (rs.pressed) m_selFolder = id;
                    if (rs.doubleClicked) openFolder(id);
                    folderMenu(id);
                    IconFolder(dl, ImVec2(x, cy - Dip(10)), Dip(20), false);
                    ImGui::PushFont(boldFont, FontDip(12));
                    TextEllipsis(dl, ImVec2(x + Dip(28), cy - ImGui::GetFontSize() * 0.5f), x + c1 - Dip(8), kText, folders[id].name.c_str());
                    ImGui::PopFont();
                    ImGui::PushFont(nullptr, FontDip(12));
                    Text(dl, ImVec2(x + c1, cy - ImGui::GetFontSize() * 0.5f), kTextDim, "Folder");
                    std::string det = Fmt("%zu folders, %zu assets", folders[id].folders.size(), folders[id].files.size());
                    Text(dl, ImVec2(x + inner - ImGui::CalcTextSize(det.c_str()).x, cy - ImGui::GetFontSize() * 0.5f), kTextDim, det.c_str());
                    ImGui::PopFont();
                } else {
                    if (rs.pressed || rs.rightClicked) m_selected = id;
                    if (rs.doubleClicked) { m_explorerVisible = false; OpenFile(id); }
                    fileMenu(id);
                    std::string_view fn = vfs.FileName(id);
                    size_t dot = fn.rfind('.');
                    std::string base(dot == std::string_view::npos ? fn : fn.substr(0, dot)), ext(dot == std::string_view::npos ? "" : fn.substr(dot + 1));
                    std::shared_ptr<GpuTexture> thumb = m_settings.previewTextures && IEndsWith(fn, ".uasset") ? Thumbnail(id) : nullptr;
                    if (thumb) {
                        float s = std::min(Dip(20) / thumb->width, Dip(20) / thumb->height);
                        ImVec2 isz(thumb->width * s, thumb->height * s), ia(x + (Dip(20) - isz.x) * 0.5f, cy - isz.y * 0.5f);
                        dl->AddImage((ImTextureID)(intptr_t)thumb->srvRaw.Get(), ia, ImVec2(ia.x + isz.x, ia.y + isz.y));
                    } else {
                        FileGlyph(dl, ImVec2(x + Dip(10), cy), Dip(20), fn, sel ? kAccent : kPanel);
                    }
                    ImGui::PushFont(boldFont, FontDip(13));
                    TextEllipsis(dl, ImVec2(x + Dip(28), cy - ImGui::GetFontSize() * 0.5f), x + c1 - Dip(8), kText, base.c_str());
                    ImGui::PopFont();
                    ImGui::PushFont(nullptr, FontDip(12));
                    TextEllipsis(dl, ImVec2(x + c1, cy - ImGui::GetFontSize() * 0.5f), x + c1 + c2 - Dip(8), kTextDim, ext.c_str());
                    std::string sz = HumanSize(vfs.Entry(id).usize);
                    Text(dl, ImVec2(x + inner - ImGui::CalcTextSize(sz.c_str()).x, cy - ImGui::GetFontSize() * 0.5f), kTextDim, sz.c_str());
                    ImGui::PopFont();
                    if (rs.hovered) ImGui::SetTooltip("%s", std::string(vfs.Path(id)).c_str());
                }
                ImGui::PopID();
            }
        ImGui::EndChild();
        return;
    }

    // grid: 128 x 192 dip cards, uniform spacing (VirtualizingWrapPanel SpacingMode=Uniform)
    float tw = Dip(128), th = Dip(192), margin = Dip(5);
    float availW = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)(availW / (tw + margin * 2)));
    float gx = std::max(margin * 2, (availW - cols * tw) / (cols + 1)), gy = margin * 2;
    int rows = (total + cols - 1) / cols;
    float startX = ImGui::GetCursorPosX() + gx, y0 = ImGui::GetCursorPosY() + margin;
    ImGuiListClipper clip;
    clip.Begin(rows, th + gy);
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++)
            for (int ci = 0; ci < cols; ci++) {
                int idx = row * cols + ci;
                if (idx >= total) break;
                ImGui::SetCursorPos(ImVec2(startX + ci * (tw + gx), y0 + row * (th + gy)));
                ImVec2 mn = ImGui::GetCursorScreenPos(), mxp(mn.x + tw, mn.y + th);
                ImGui::PushID(idx);
                bool isFolder = idx < (int)subfolders.size();
                u32 id = isFolder ? subfolders[idx] : files[idx - subfolders.size()];
                ImGui::InvisibleButton("##tile", ImVec2(tw, th), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                bool hov = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked(0) || ImGui::IsItemClicked(1);
                float ht = HoverAnim();
                bool dbl = hov && ImGui::IsMouseDoubleClicked(0);
                float pad = Dip(5);
                if (isFolder) {
                    const VfsFolder& fo = folders[id];
                    bool sel = m_selFolder == id;
                    dl->AddRectFilled(mn, mxp, Mix(kCardFolder, Shade(kCardFolder, 1.2f), ht));
                    // icon (96 dip) + 10 + name + 6 + two count lines, centred vertically
                    float icon = Dip(84), nameH = FontDip(13), subH = FontDip(11);
                    float block = icon + Dip(10) + nameH + Dip(6) + subH * 2;
                    float y = mn.y + (th - block) * 0.5f;
                    IconFolder(dl, ImVec2(mn.x + (tw - icon) * 0.5f, y), icon, false);
                    y += icon + Dip(10);
                    ImGui::PushFont(boldFont, FontDip(13));
                    TextCentered(dl, mn.x + pad, mxp.x - pad, y, kText, fo.name);
                    ImGui::PopFont();
                    y += nameH + Dip(6);
                    ImGui::PushFont(nullptr, FontDip(11));
                    TextCentered(dl, mn.x + pad, mxp.x - pad, y, Alpha(kText, 0.8f), Fmt("Subfolders: %zu", fo.folders.size()));
                    TextCentered(dl, mn.x + pad, mxp.x - pad, y + subH, Alpha(kText, 0.8f), Fmt("Assets: %zu", fo.files.size()));
                    ImGui::PopFont();
                    if (sel) dl->AddRect(mn, mxp, accentBorder, 0, 0, Dip(1.5f));
                    if (clicked) m_selFolder = id;
                    if (dbl) openFolder(id);
                    folderMenu(id);
                } else {
                    bool sel = m_selected == (i64)id;
                    std::string_view fn = vfs.FileName(id);
                    size_t dot = fn.rfind('.');
                    std::string base(dot == std::string_view::npos ? fn : fn.substr(0, dot)), ext(dot == std::string_view::npos ? "" : fn.substr(dot + 1));
                    float topH = Dip(128);
                    ImU32 tint = FileTint(fn);
                    dl->AddRectFilled(mn, mxp, Mix(kCardFile, Shade(kCardFile, 1.2f), ht));
                    dl->AddRectFilled(mn, ImVec2(mxp.x, mn.y + topH), kBorder);
                    std::shared_ptr<GpuTexture> thumb = m_settings.previewTextures && IEndsWith(fn, ".uasset") ? Thumbnail(id) : nullptr;
                    if (thumb) {   // fitted into the preview square
                        float s = std::min(topH / thumb->width, topH / thumb->height);
                        ImVec2 isz(thumb->width * s, thumb->height * s), ia(mn.x + (tw - isz.x) * 0.5f, mn.y + (topH - isz.y) * 0.5f);
                        dl->AddImage((ImTextureID)(intptr_t)thumb->srvRaw.Get(), ia, ImVec2(ia.x + isz.x, ia.y + isz.y));
                    } else {
                        FileGlyph(dl, ImVec2(mn.x + tw * 0.5f, mn.y + topH * 0.5f), Dip(64), fn, kBorder);
                    }
                    dl->AddRectFilled(ImVec2(mn.x, mn.y + topH), ImVec2(mxp.x, mn.y + topH + Dip(2)), tint);
                    // name: up to two lines, then type (left) and size (right) at the bottom
                    ImGui::PushFont(boldFont, FontDip(13));
                    float nx = mn.x + pad, nw = tw - pad * 2, ny = mn.y + topH + Dip(2) + pad;
                    size_t n = 0;
                    while (n < base.size() && ImGui::CalcTextSize(base.c_str(), base.c_str() + n + 1).x <= nw) n++;
                    Text(dl, ImVec2(nx, ny), kText, base.c_str(), base.c_str() + n);
                    if (n < base.size()) TextEllipsis(dl, ImVec2(nx, ny + ImGui::GetFontSize() * 1.1f), nx + nw, kText, base.c_str() + n);
                    ImGui::PopFont();
                    ImGui::PushFont(nullptr, FontDip(9));
                    std::string sz = HumanSize(vfs.Entry(id).usize);
                    float by = mxp.y - pad - ImGui::GetFontSize();
                    TextEllipsis(dl, ImVec2(nx, by), mxp.x - pad - ImGui::CalcTextSize(sz.c_str()).x - Dip(5), kTextDim, ext.c_str());
                    Text(dl, ImVec2(mxp.x - pad - ImGui::CalcTextSize(sz.c_str()).x, by), kTextDim, sz.c_str());
                    ImGui::PopFont();
                    if (sel) dl->AddRect(mn, mxp, accentBorder, 0, 0, Dip(1.5f));
                    if (clicked) m_selected = id;
                    if (dbl) { m_explorerVisible = false; OpenFile(id); }
                    if (hov) ImGui::SetTooltip("%s", std::string(vfs.Path(id)).c_str());
                    fileMenu(id);
                }
                ImGui::PopID();
            }
    ImGui::EndChild();
}

// ================================================================== document tabs

Document* App::ActiveDoc() {
    for (auto& d : m_docs) if (d->id == m_activeDoc) return d.get();
    return m_docs.empty() ? nullptr : m_docs.front().get();
}

void App::AddTab() {
    if (Document* cur = ActiveDoc(); cur && cur->newTab) return;   // FModel reuses an empty "New Tab"
    if (m_docs.size() >= 25) return;
    auto d = std::make_shared<Document>();
    d->id = m_nextDocId++;
    d->newTab = true;
    d->loading = false;
    d->name = "New Tab";
    d->entry = UINT32_MAX;
    m_docs.push_back(d);
    m_activeDoc = d->id;
}

void App::OpenFile(u32 entry, bool newTab) {
    if (m_mountState != MountState::Ready) return;
    auto d = std::make_shared<Document>();
    d->entry = entry;
    d->gamePath = std::string(m_api->vfs.Path(entry));
    d->name = std::string(m_api->vfs.FileName(entry));
    // FModel: extracts into the selected tab; a new tab only when asked (or there is none / it is full)
    PlaceDocument(d, newTab);
    if (IEndsWith(d->gamePath, ".wem")) {   // Wwise audio: info + player
        LoadAudioDocument(d);
        return;
    }
    if (!IsPackageFile(d->gamePath)) {   // loose files: show their text
        d->loading = true;
        m_jobs->Push([this, d] {
            Bytes data;
            std::string err;
            if (m_api->vfs.Read(d->entry, data, &err)) {
                bool text = true;
                for (size_t i = 0; i < std::min<size_t>(data.size(), 4096); i++)
                    if (data[i] == 0) { text = false; break; }
                d->json = text ? std::string((const char*)data.data(), data.size()) : Fmt("// binary file, %s", HumanSize(data.size()).c_str());
            } else {
                d->json = "// " + err;
            }
            d->lines.clear();
            d->lines.push_back(0);
            for (u32 i = 0; i < (u32)d->json.size(); i++) if (d->json[i] == '\n') d->lines.push_back(i + 1);
            d->loading = false;
            Post([] {});
        });
        return;
    }
    LoadDocument(d);
}

void App::CloseDocument(u64 id) {
    for (auto& dp : m_docs)
        if (!id || dp->id == id) dp->cancel = true;
    if (id) {   // select the previous tab first (FModel RemoveTab)
        for (size_t i = 0; i < m_docs.size(); i++)
            if (m_docs[i]->id == id && m_activeDoc == id) m_activeDoc = m_docs[i > 0 ? i - 1 : (m_docs.size() > 1 ? 1 : 0)]->id;
    }
    m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(), [&](auto& d) { return !id || d->id == id; }), m_docs.end());
    if (m_docs.empty()) m_activeDoc = 0;
    if (m_jobs->Pending() == 0 && m_assets) m_assets->Trim();
}

void App::CloseOtherDocuments(u64 keep) {
    for (auto& dp : m_docs) if (dp->id != keep) dp->cancel = true;
    m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(), [&](auto& d) { return d->id != keep; }), m_docs.end());
    m_activeDoc = keep;
    if (m_jobs->Pending() == 0 && m_assets) m_assets->Trim();
}

void App::DrawDocTabs(float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x - Dip(3);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // header: wrap panel of tabs (6 per row), "+" on the right, highlight strip below
    float tabH = Dip(23.2f), plusW = Dip(24);
    float textW = std::max(Dip(40), (width - plusW) / 6 - Dip(8));
    float tabW = Dip(6) + textW + Dip(10) + Dip(11) + Dip(4);
    int perRow = std::max(1, (int)((width - plusW) / tabW));
    int rows = std::max(1, ((int)m_docs.size() + perRow - 1) / perRow);
    float headerH = rows * tabH;
    u64 close = 0, closeOthers = 0;
    bool closeAll = false;
    for (size_t i = 0; i < m_docs.size(); i++) {
        Document& d = *m_docs[i];
        int r = (int)i / perRow, c = (int)i % perRow;
        ImVec2 a(p.x + c * tabW, p.y + r * tabH), b(a.x + tabW, a.y + tabH);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID((int)d.id);
        ImGui::InvisibleButton("##tab", ImVec2(tabW, tabH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered(), sel = d.id == m_activeDoc;
        float ht = HoverAnim(), st = Anim(ImGui::GetItemID() ^ 0x7ab, sel);
        if (ImGui::IsItemClicked(0)) m_activeDoc = d.id;
        if (ImGui::IsItemClicked(2)) close = d.id;
        if (hov) ImGui::SetTooltip("%s", d.newTab ? d.name.c_str() : d.gamePath.c_str());
        if (ImGui::BeginPopupContextItem("##tabctx")) {
            if (MenuItemIcon("Close", nullptr, IconClose)) close = d.id;
            if (MenuItemIcon("Close All Tabs", nullptr, IconClose)) closeAll = true;
            if (MenuItemIcon("Close Other Tabs", nullptr, IconClose)) closeOthers = d.id;
            bool pkg = !d.newTab && IsPackageFile(d.gamePath);
            ImGui::Separator();
            if (MenuItemIcon("Show Metadata", nullptr, IconInfo, pkg)) { u32 e = d.entry; Post([this, e] { ShowMetadata(e, true); }); }
            if (MenuItemIcon("Find References", nullptr, IconSearch, pkg)) FindReferences(d.entry);
            if (MenuItemIcon("Decompile Blueprint", nullptr, IconCube, pkg)) DecompileBlueprintTab(d.entry);
            if (MenuItemIcon("Decompile Shaders", nullptr, IconCube, pkg || (!d.newTab && IsShaderCacheFile(d.gamePath)))) DecompileShadersTab(d.entry);
            ImGui::Separator();
            if (ImGui::BeginMenu("      Export", !d.newTab)) {
                if (ImGui::MenuItem("Raw Data (.uasset)")) ExportFiles({d.entry}, 1);
                if (ImGui::MenuItem("Properties (.json)", nullptr, false, pkg)) ExportFiles({d.entry}, 2);
                if (ImGui::MenuItem("Texture", nullptr, false, d.texture != nullptr)) ExportFiles({d.entry}, 0, 1);
                if (ImGui::MenuItem("Model", nullptr, false, d.hasMesh)) ExportFiles({d.entry}, 0, 2);
                if (ImGui::MenuItem("World", nullptr, false, d.isWorld)) ExportFiles({d.entry}, 0, 3);
                if (ImGui::MenuItem("Animation", nullptr, false, d.anim != nullptr)) ExportFiles({d.entry}, 0, 4);
                if (ImGui::MenuItem("Audio", nullptr, false, d.audio || d.isBank)) ExportFiles({d.entry}, 0, 5);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("      Jump to Package Folder", nullptr, false, !d.newTab)) {
                std::string_view gp = d.gamePath;
                auto& folders = m_api->vfs.Folders();
                u32 cur = 0;
                size_t pos = 0;
                for (;;) {
                    size_t sl = gp.find('/', pos);
                    if (sl == std::string_view::npos) break;
                    u32 next = UINT32_MAX;
                    for (u32 f : folders[cur].folders) if (IEquals(folders[f].name, gp.substr(pos, sl - pos))) { next = f; break; }
                    if (next == UINT32_MAX) break;
                    cur = next;
                    pos = sl + 1;
                }
                m_leftTab = 1;
                SelectFolder(cur);
                m_selected = d.entry;
            }
            if (ImGui::BeginMenu("      Copy", !d.newTab)) {
                std::string path = d.gamePath, dir = path.substr(0, path.rfind('/')), name = d.name;
                auto noExt = [](std::string s) { size_t dot = s.rfind('.'); return dot == std::string::npos ? s : s.substr(0, dot); };
                if (ImGui::MenuItem("Package Path")) CopyToClipboard(path);
                if (ImGui::MenuItem("Package Name")) CopyToClipboard(name);
                if (ImGui::MenuItem("Directory Path")) CopyToClipboard(dir);
                if (ImGui::MenuItem("Package Path w/o Extension")) CopyToClipboard(noExt(path));
                if (ImGui::MenuItem("Package Name w/o Extension")) CopyToClipboard(noExt(name));
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        dl->AddRectFilled(a, ImVec2(b.x - 1, b.y), Mix(Mix(kLayer0, kHover, ht), kTabSel, st));
        if (!sel) dl->AddRect(a, ImVec2(b.x - 1, b.y + 1), kBorder);
        TextEllipsis(dl, ImVec2(a.x + Dip(6), a.y + (tabH - ImGui::GetFontSize()) * 0.5f), a.x + Dip(6) + textW, kText, d.name.c_str());
        // close: small round button
        ImVec2 cc(b.x - Dip(4) - Dip(5.5f), a.y + tabH * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(cc.x - Dip(7), cc.y - Dip(7)));
        if (ImGui::InvisibleButton("##close", ImVec2(Dip(14), Dip(14)))) close = d.id;
        bool ch = ImGui::IsItemHovered();
        float ct = HoverAnim();
        dl->AddCircleFilled(cc, Dip(5.5f) + Dip(1.5f) * ct, Mix(kBorder, kError, ct));
        if (ct > 0) IconClose(dl, cc, Dip(9), Alpha(kText, ct));
        if (ch) {
            ImGui::SetTooltip("Close Tab");
        }
        ImGui::PopID();
    }
    // "+" (Add New Tab)
    {
        ImVec2 a(p.x + width - plusW, p.y + headerH - tabH);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##addtab", ImVec2(plusW, tabH))) AddTab();
        bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(a, ImVec2(a.x + plusW, a.y + tabH), Mix(kPanel, kTabSel, HoverAnim()));
        IconPlus(dl, ImVec2(a.x + plusW * 0.5f, a.y + tabH * 0.5f), Dip(16), kText);
        if (hov) ImGui::SetTooltip("Add New Tab (Ctrl+T)");
    }
    float stripY = p.y + headerH;
    dl->AddRectFilled(ImVec2(p.x, stripY), ImVec2(p.x + width, stripY + Dip(4)), kTabSel);

    if (close) CloseDocument(close);
    if (closeOthers) CloseOtherDocuments(closeOthers);
    if (closeAll) CloseDocument(0);
    if (m_docs.empty()) AddTab();

    float top = headerH + Dip(4);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::BeginChild("##doccontent", ImVec2(width, height - top), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (Document* d = ActiveDoc()) DrawDocContent(*d);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::DrawDocContent(Document& d) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    bool has3d = !d.loading && (d.hasMesh || d.anim || d.isWorld);
    bool hasImage = !d.loading && (d.texture || (d.hasMaterial && !d.materialTextures.empty()));
    bool hasAudio = !d.loading && (d.audio || d.isBank || !d.audioError.empty());
    bool hasTable = !d.loading && d.table != nullptr;
    bool hasShaders = d.shaders != nullptr;
    bool side = has3d || hasImage || hasAudio || hasTable || hasShaders;
    float splitW = Dip(4);
    if (side && d.sideW <= 0) d.sideW = hasShaders ? std::min(avail.x * 0.4f, Dip(520)) : has3d || hasTable ? avail.x * 0.6f : std::min(avail.x * 0.45f, Dip(400) + avail.x * 0.1f);
    float sideW = side ? std::clamp(d.sideW, Dip(160), std::max(Dip(160), avail.x - Dip(200))) : 0;
    float editW = side ? avail.x - sideW - splitW : avail.x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    DrawJsonEditor(d, ImVec2(editW, avail.y));
    if (!side) return;
    ImGui::SetCursorScreenPos(ImVec2(p.x + editW, p.y));
    ImGui::InvisibleButton("##sidesplit", ImVec2(splitW, avail.y));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) d.sideW = sideW - ImGui::GetIO().MouseDelta.x;
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kLayer0);
    ImGui::SetCursorScreenPos(ImVec2(p.x + editW + splitW, p.y));
    if (hasShaders) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(6), Dip(6)));
        ImGui::BeginChild("##shaders", ImVec2(sideW, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        DrawShaderPanel(d);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }
    if (hasTable) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(6), Dip(6)));
        ImGui::BeginChild("##table", ImVec2(sideW, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        DrawTablePanel(d);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }
    if (hasAudio) {
        ImGui::BeginChild("##audio", ImVec2(sideW, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        DrawAudioPanel(d);
        ImGui::EndChild();
        return;
    }
    if (hasImage && !has3d) {
        ImGui::BeginChild("##image", ImVec2(sideW, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        DrawImagePanel(d);
        ImGui::EndChild();
        return;
    }
    // our own 3D viewer (not FModel's), with its toolbar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(6), Dip(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dip(5), Dip(5)));
    ImGui::BeginChild("##viewer", ImVec2(sideW, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (d.isWorld) DrawWorld(d);
    else if (d.anim) DrawAnimation(d);
    else DrawModel(d);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// texture / material images, with FModel's header (name, format / size / dimensions / mips) and paging
void App::DrawImagePanel(Document& d) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    std::shared_ptr<GpuTexture> tex;
    std::string name, info;
    int pages = 1;
    if (d.texture) {
        tex = d.texture;
        name = d.name.substr(0, d.name.rfind('.'));
    } else {
        pages = (int)d.materialTextures.size();
        d.imagePage = std::clamp(d.imagePage, 0, pages - 1);
        name = d.materialTextures[d.imagePage].first;
        tex = d.materialTextures[d.imagePage].second;
    }
    float padL = Dip(10), padT = Dip(6), padR = Dip(8), padB = Dip(8);
    ImGui::PushFont(boldFont, FontDip(12));
    float nameH = ImGui::GetFontSize();
    TextEllipsis(dl, ImVec2(p.x + padL, p.y + padT), p.x + w - padR, kText, name.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, FontDip(11));
    float y2 = p.y + padT + nameH + Dip(2);
    float x = p.x + padL;
    if (d.texture) {
        Text(dl, ImVec2(x, y2), kAlert, d.tex.pixelFormat.c_str());
        x += ImGui::CalcTextSize(d.tex.pixelFormat.c_str()).x;
        std::string rest = Fmt(" / %s / ", HumanSize(m_api->vfs.Entry(d.entry).usize).c_str());
        Text(dl, ImVec2(x, y2), kTextDim, rest.c_str());
        x += ImGui::CalcTextSize(rest.c_str()).x;
        info = Fmt("%d \xC3\x97 %dpx / %zu mips", d.tex.sizeX, d.tex.sizeY, d.tex.mips.size());
    } else if (tex) {
        info = Fmt("%d \xC3\x97 %dpx", tex->width, tex->height);
    }
    Text(dl, ImVec2(x, y2), kTextDim, info.c_str());
    if (pages > 1) {   // < page / count >
        std::string pg = Fmt("%d / %d", d.imagePage + 1, pages);
        float bw = Dip(18), pw = ImGui::CalcTextSize(pg.c_str()).x + Dip(10);
        float bx = p.x + w - padR - bw * 2 - pw - Dip(2), by = y2 - Dip(2);
        auto pager = [&](const char* id, float px, bool next) {
            ImGui::SetCursorScreenPos(ImVec2(px, by));
            bool r = ImGui::InvisibleButton(id, ImVec2(bw, bw));
            dl->AddRectFilled(ImVec2(px, by), ImVec2(px + bw, by + bw), Mix(IM_COL32(50, 50, 63, 255), kTabSel, HoverAnim()), Dip(3));
            IconChevron(dl, ImVec2(px + bw * 0.5f, by + bw * 0.5f), Dip(14), next ? 0.f : kPi, kText);
            return r;
        };
        if (pager("##prev", bx, false)) d.imagePage = (d.imagePage + pages - 1) % pages;
        dl->AddRectFilled(ImVec2(bx + bw + Dip(1), by), ImVec2(bx + bw + Dip(1) + pw, by + bw), kPanel, Dip(3));
        Text(dl, ImVec2(bx + bw + Dip(1) + Dip(5), by + (bw - ImGui::GetFontSize()) * 0.5f), kText, pg.c_str());
        if (pager("##next", bx + bw + Dip(2) + pw, true)) d.imagePage = (d.imagePage + 1) % pages;
    }
    float headH = padT + nameH + Dip(2) + ImGui::GetFontSize() + padB;
    ImGui::PopFont();
    dl->AddLine(ImVec2(p.x, p.y + headH), ImVec2(p.x + w, p.y + headH), kSeparator);
    // image: fitted, centred, 1px border, 8 dip margin
    float m = Dip(8);
    ImVec2 area(w - m * 2, h - headH - m * 2);
    if (tex && area.x > 4 && area.y > 4) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 a0(p.x + m, p.y + headH + m);
        float fit = std::min({area.x / tex->width, area.y / tex->height, 1.f});   // fitted, never upscaled
        float s = d.texZoom > 0 ? d.texZoom : fit;
        ImGui::SetCursorScreenPos(a0);
        ImGui::InvisibleButton("##img", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        bool hov = ImGui::IsItemHovered();
        ImVec2 center(a0.x + area.x * 0.5f + d.texPanX, a0.y + area.y * 0.5f + d.texPanY);
        if (hov && io.MouseWheel != 0) {   // zoom around the cursor
            float ns = std::clamp(s * powf(1.2f, io.MouseWheel), fit * 0.25f, 32.f);
            d.texPanX += (io.MousePos.x - center.x) * (1 - ns / s);
            d.texPanY += (io.MousePos.y - center.y) * (1 - ns / s);
            d.texZoom = ns;
            s = ns;
        }
        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(0, 1) || ImGui::IsMouseDragging(2, 1))) { d.texPanX += io.MouseDelta.x; d.texPanY += io.MouseDelta.y; }
        if (hov && ImGui::IsMouseDoubleClicked(0)) { d.texZoom = 0; d.texPanX = d.texPanY = 0; s = fit; }
        center = ImVec2(a0.x + area.x * 0.5f + d.texPanX, a0.y + area.y * 0.5f + d.texPanY);
        ImVec2 sz(tex->width * s, tex->height * s), a(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f);
        dl->PushClipRect(a0, ImVec2(a0.x + area.x, a0.y + area.y), true);
        dl->AddImage((ImTextureID)(intptr_t)tex->srvRaw.Get(), a, ImVec2(a.x + sz.x, a.y + sz.y));
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + sz.x + 1, a.y + sz.y + 1), kSeparator);
        dl->PopClipRect();
        if (hov && d.texture) {
            float u = (io.MousePos.x - a.x) / sz.x, v = (io.MousePos.y - a.y) / sz.y;
            if (u >= 0 && u < 1 && v >= 0 && v < 1)
                ImGui::SetTooltip("%d, %d  (%.0f%%)\nWheel: zoom, drag: pan, double click: fit", (int)(u * d.tex.sizeX), (int)(v * d.tex.sizeY),
                                  s * tex->width / std::max(1, d.tex.sizeX) * 100);
        }
        if (ImGui::BeginPopupContextItem("##imgctx")) {
            if (ImGui::MenuItem("Copy Image", nullptr, false, d.texture && d.pkg)) {
                for (auto& sp : m_docs) if (sp.get() == &d) CopyTextureImage(sp);
            }
            if (ImGui::MenuItem("Export Image")) ExportFiles({d.entry}, 0, d.texture ? 1 : 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Fit")) { d.texZoom = 0; d.texPanX = d.texPanY = 0; }
            if (ImGui::MenuItem("Actual Size")) { d.texZoom = (float)std::max(1, d.tex.sizeX) / tex->width; d.texPanX = d.texPanY = 0; }
            ImGui::EndPopup();
        }
    } else if (!tex) {
        Text(dl, ImVec2(p.x + m, p.y + headH + m), kTextDim, "(texture not loaded)");
    }
}

// ================================================================== DataTable / CurveTable grid

// compiled shaders of a material / the global shader cache: pick one, see it as HLSL / assembly / reflection
void App::DrawShaderPanel(Document& d) {
    auto& list = *d.shaders;
    std::shared_ptr<Document> self;
    for (auto& x : m_docs)
        if (x.get() == &d) self = x;
    if (!self) return;
    int counts[6] = {};
    for (auto& sh : list) if (sh.frequency >= 0 && sh.frequency < 6) counts[sh.frequency]++;
    std::string summary = Fmt("%zu shaders", list.size());
    for (int f = 0; f < 6; f++) if (counts[f]) summary += Fmt(" / %d %s", counts[f], ShaderStagePrefix(f));
    ImGui::PushFont(boldFont, FontDip(12));
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, FontDip(11));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", summary.c_str());
    ImGui::PopFont();
    // view mode
    static const char* modes[] = {"HLSL", "HLSL (exact)", "Assembly", "Reflection"};
    float avail = ImGui::GetContentRegionAvail().x, gap = Dip(4), bw = (avail - gap * 3) / 4;
    for (int m = 0; m < 4; m++) {
        if (m) ImGui::SameLine(0, gap);
        ImVec2 bp = ImGui::GetCursorScreenPos();
        if (Button(modes[m], ImVec2(bw, 0)) && m != d.shaderMode) ShowShader(self, d.shaderSel, m);
        if (m == d.shaderMode) {
            ImVec2 bq = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bp.x, bq.y - Dip(2)), bq, kAccent);
        }
    }
    // filter + save
    float sw = Dip(70);
    SearchBox("##sf", d.shaderFilter, sizeof(d.shaderFilter), "Filter by type, stage, vertex factory...", ImGui::GetContentRegionAvail().x - (sw + Dip(6)) * 2, false);
    std::string base = m_settings.exportKeepDirectories ? d.gamePath : d.gamePath.substr(d.gamePath.rfind('/') + 1);
    base = base.substr(0, base.rfind('.')) + "_shaders";
    std::wstring outDir = m_settings.outputDir;
    auto save = [base, outDir](int i, int frequency, int mode, const std::string& text) {
        const char* ext = mode == 2 ? ".asm" : mode == 3 ? ".txt" : ".hlsl";
        std::wstring out = outDir + L"\\" + Widen(base + "\\" + Fmt("%03d_%s", i, ShaderStagePrefix(frequency)) + ext);
        for (auto& c : out) if (c == L'/') c = L'\\';
        CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
        return WriteWholeFile(out, text.data(), text.size()) ? Narrow(out) : std::string();
    };
    ImGui::SameLine(0, Dip(6));
    if (Button("Save", ImVec2(sw, 0), d.shaderSel >= 0)) {
        std::string w = save(d.shaderSel, list[d.shaderSel].frequency, d.shaderMode, d.json);
        if (!w.empty()) LOGI("shader -> %s", w.c_str());
        else LOGW("could not write the shader");
    }
    ImGui::SameLine(0, Dip(6));
    if (Button("Save All", ImVec2(sw, 0))) {
        int mode = d.shaderMode;
        auto lst = d.shaders;
        m_jobs->Push([lst, mode, save] {
            int n = 0;
            for (int i = 0; i < (int)lst->size(); i++) {
                std::string text, err;
                const Bytes& code = (*lst)[i].dxbc;
                bool ok = false;
                if (mode == 2) ok = DisassembleShader(code, text, &err);
                else if (mode == 3) {
                    ShaderReflectionInfo info;
                    if ((ok = ReflectShader(code, info, &err))) text = ReflectionToText(info);
                } else ok = DecompileShader(code, text, mode == 1, &err);
                if (ok && !save(i, (*lst)[i].frequency, mode, text).empty()) n++;
            }
            LOGI("saved %d of %zu shaders", n, lst->size());
        });
    }
    // list
    std::string filter = ToLower(d.shaderFilter);
    std::vector<int> rows;
    for (int i = 0; i < (int)list.size(); i++) {
        auto& sh = list[i];
        if (!filter.empty() && ToLower(sh.type + " " + ShaderFrequencyName(sh.frequency) + " " + ShaderStagePrefix(sh.frequency) + " " + sh.vertexFactory).find(filter) == std::string::npos) continue;
        rows.push_back(i);
    }
    ImGui::BeginChild("##shaderlist", ImVec2(0, 0), ImGuiChildFlags_None);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float lh = ImGui::GetFontSize(), rowH = lh * 2 + Dip(10), rw = ImGui::GetContentRegionAvail().x;
    ImGuiListClipper clip;
    clip.Begin((int)rows.size(), rowH);
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            int i = rows[r];
            const ShaderEntry& sh = list[i];
            ImGui::PushID(i);
            ImVec2 rp = ImGui::GetCursorScreenPos();
            RowState rs = ListRow("##sh", ImVec2(rw, rowH), d.shaderSel == i);
            if (rs.pressed && d.shaderSel != i) ShowShader(self, i, d.shaderMode);
            std::string tag = Fmt("%d  %s", i, ShaderStagePrefix(sh.frequency));
            Text(dl, ImVec2(rp.x + Dip(6), rp.y + Dip(3)), d.shaderSel == i ? kText : kAccent, tag.c_str());
            float tx = rp.x + Dip(6) + ImGui::CalcTextSize("0000  xx ").x;
            std::string instr = Fmt("%u instr", sh.numInstructions);
            float iw = ImGui::CalcTextSize(instr.c_str()).x;
            TextEllipsis(dl, ImVec2(tx, rp.y + Dip(3)), rp.x + rw - iw - Dip(14), kText, sh.type.empty() ? "(unknown type)" : sh.type.c_str());
            Text(dl, ImVec2(rp.x + rw - iw - Dip(6), rp.y + Dip(3)), kTextDim, instr.c_str());
            std::string sub = sh.vertexFactory.empty() ? std::string(ShaderFrequencyName(sh.frequency)) : sh.vertexFactory;
            if (sh.numTextureSamplers) sub += Fmt("   %u samplers", sh.numTextureSamplers);
            TextEllipsis(dl, ImVec2(tx, rp.y + Dip(5) + lh), rp.x + rw - Dip(6), kTextDim, sub.c_str());
            if (rs.hovered && !sh.usage.empty()) ImGui::SetTooltip("%s", sh.usage.c_str());
            ImGui::PopID();
        }
    ImGui::EndChild();
}

void App::DrawTablePanel(Document& d) {
    TableData& t = *d.table;
    ImGui::PushFont(boldFont, FontDip(12));
    ImGui::TextUnformatted(d.name.substr(0, d.name.rfind('.')).c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, FontDip(11));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s%s / %zu rows / %zu columns", t.curves ? "CurveTable" : "DataTable",
                       t.rowStruct.empty() ? "" : (" of " + t.rowStruct).c_str(), t.rows.size(), t.columns.size());
    ImGui::PopFont();
    float bw = Dip(110);
    SearchBox("##tf", d.tableFilter, sizeof(d.tableFilter), "Filter rows...", ImGui::GetContentRegionAvail().x - bw - Dip(6), false);
    ImGui::SameLine(0, Dip(6));
    if (Button("Export CSV", ImVec2(bw, 0))) {
        std::string csv = TableToCsv(t);
        std::string rel = m_settings.exportKeepDirectories ? d.gamePath : d.gamePath.substr(d.gamePath.rfind('/') + 1);
        std::wstring out = m_settings.outputDir + L"\\" + Widen(rel.substr(0, rel.rfind('.')) + ".csv");
        for (auto& c : out) if (c == L'/') c = L'\\';
        CreateDirectories(out.substr(0, out.find_last_of(L'\\')));
        if (WriteWholeFile(out, csv.data(), csv.size())) LOGI("table -> %s", Narrow(out).c_str());
        else LOGW("could not write %s", Narrow(out).c_str());
    }
    // rows filtered by any cell, sorted by the clicked column
    std::string filter = ToLower(d.tableFilter);
    int cols = (int)t.columns.size() + 1;
    std::vector<std::vector<std::string>> cells;
    cells.reserve(t.cells.size());
    for (auto& c : t.cells) {
        if (!filter.empty()) {
            bool hit = false;
            for (auto& s : c) if (ToLower(s).find(filter) != std::string::npos) { hit = true; break; }
            if (!hit) continue;
        }
        cells.push_back(std::move(c));
    }
    int tcols = std::min(cols, 64);
    ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                         ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, kField);
    if (ImGui::BeginTable("##grid", tcols, fl)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, Dip(140));
        for (int c = 1; c < tcols; c++) ImGui::TableSetupColumn(t.columns[c - 1].c_str(), ImGuiTableColumnFlags_WidthFixed, Dip(130));
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
            int sc = ss->Specs[0].ColumnIndex;
            bool asc = ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(cells.begin(), cells.end(), [&](auto& a, auto& b) {
                const std::string &x = a[sc], &y = b[sc];
                char* ex = nullptr;
                char* ey = nullptr;
                double nx = strtod(x.c_str(), &ex), ny = strtod(y.c_str(), &ey);
                bool numeric = !x.empty() && !y.empty() && ex && !*ex && ey && !*ey;
                int r = numeric ? (nx < ny ? -1 : nx > ny ? 1 : 0) : x.compare(y);
                return asc ? r < 0 : r > 0;
            });
        }
        ImGuiListClipper clip;
        clip.Begin((int)cells.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                ImGui::TableNextRow();
                for (int c = 0; c < tcols; c++) {
                    ImGui::TableNextColumn();
                    const std::string& s = cells[i][c];
                    ImGui::PushID(i * 97 + c);
                    if (c == 0) ImGui::PushFont(boldFont, 0);
                    ImGui::TextUnformatted(s.c_str());
                    if (c == 0) ImGui::PopFont();
                    if (ImGui::IsItemHovered() && s.size() > 24) ImGui::SetTooltip("%s", s.c_str());
                    if (ImGui::BeginPopupContextItem("##cell")) {
                        if (ImGui::MenuItem("Copy Value")) CopyToClipboard(s);
                        if (ImGui::MenuItem("Copy Row Name")) CopyToClipboard(cells[i][0]);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
}

// ================================================================== audio player (own design)

void App::DrawAudioPanel(Document& d) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    float padL = Dip(10), padT = Dip(6), padR = Dip(8);
    // current clip: the .wem itself or the selected bank media (decoded on first use)
    std::shared_ptr<AudioPcm> pcm = d.audio;
    std::string clipName = d.name.substr(0, d.name.rfind('.')), clipError = d.audioError;
    WemInfo info = d.wem;
    if (d.isBank) {
        if (d.bankSel >= (int)d.bank.wems.size()) d.bankSel = -1;
        if (d.bankSel >= 0 && d.pkg) {
            const BankWem& m = d.bank.wems[d.bankSel];
            const u8* src = d.pkg->Data().data() + m.offset;
            ReadWemInfo(src, m.size, info);
            auto& slot = d.bankPcm[m.id];
            if (!slot) {
                auto np = std::make_shared<AudioPcm>();
                std::string err;
                if (DecodeWem(src, m.size, *np, &err)) slot = np;
                else clipError = err;
            }
            pcm = slot;
            clipName = std::to_string(m.id);
        } else {
            pcm = nullptr;
            clipName = Fmt("%s - %zu embedded sounds", d.name.substr(0, d.name.rfind('.')).c_str(), d.bank.wems.size());
        }
    }
    if (m_playRequest == d.id && pcm) {
        PlayAudio(d, pcm, 0);
        m_playRequest = 0;
    }
    bool mine = m_playDoc == d.id && m_player.Active() && m_player.Clip() == pcm.get();
    // header: name, codec / rate / channels / length
    ImGui::PushFont(boldFont, FontDip(12));
    float nameH = ImGui::GetFontSize();
    TextEllipsis(dl, ImVec2(p.x + padL, p.y + padT), p.x + w - padR, kText, clipName.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, FontDip(11));
    float y2 = p.y + padT + nameH + Dip(2);
    if (pcm || !info.codecName.empty()) {
        float x = p.x + padL;
        Text(dl, ImVec2(x, y2), kAlert, info.codecName.c_str());
        x += ImGui::CalcTextSize(info.codecName.c_str()).x;
        std::string rest = pcm ? Fmt(" / %d Hz / %s / %d:%05.2f", pcm->sampleRate, pcm->channels == 1 ? "mono" : pcm->channels == 2 ? "stereo" : Fmt("%d ch", pcm->channels).c_str(),
                                     (int)(pcm->Seconds() / 60), fmod(pcm->Seconds(), 60.0))
                               : std::string();
        Text(dl, ImVec2(x, y2), kTextDim, rest.c_str());
    } else if (d.isBank) {
        Text(dl, ImVec2(p.x + padL, y2), kTextDim, Fmt("Bank %u / %u objects", d.bank.bankId, d.bank.hircObjects).c_str());
    }
    float headH = padT + nameH + Dip(2) + ImGui::GetFontSize() + Dip(8);
    ImGui::PopFont();
    dl->AddLine(ImVec2(p.x, p.y + headH), ImVec2(p.x + w, p.y + headH), kSeparator);
    float y = p.y + headH + Dip(8), m = Dip(8);

    // bank media list
    if (d.isBank) {
        float listH = std::max(Dip(120), (h - headH) * 0.45f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + m, y));
        ImGui::BeginChild("##bankmedia", ImVec2(w - m * 2, listH), ImGuiChildFlags_None);
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        float rowH = Dip(22), rw = ImGui::GetContentRegionAvail().x;
        if (d.bank.wems.empty()) ImGui::TextDisabled("This bank has no embedded media (its sounds are streamed .wem files)");
        ImGuiListClipper clip;
        clip.Begin((int)d.bank.wems.size(), rowH);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const BankWem& bw = d.bank.wems[i];
                ImGui::PushID(i);
                ImVec2 rp = ImGui::GetCursorScreenPos();
                RowState rs = ListRow("##bw", ImVec2(rw, rowH), d.bankSel == i);
                if (rs.pressed) d.bankSel = i;
                if (rs.doubleClicked) {   // select + play (it is decoded at the top of the next frame)
                    d.bankSel = i;
                    m_playRequest = d.id;
                    m_busyFrames = 3;
                }
                float cy = rp.y + rowH * 0.5f, ty = cy - ImGui::GetFontSize() * 0.5f;
                IconNote(ldl, ImVec2(rp.x + Dip(10), cy), Dip(14), d.bankSel == i ? kText : kTextDim);
                Text(ldl, ImVec2(rp.x + Dip(22), ty), kText, Fmt("%d   %u", i + 1, bw.id).c_str());
                std::string sz = HumanSize(bw.size);
                Text(ldl, ImVec2(rp.x + rw - Dip(6) - ImGui::CalcTextSize(sz.c_str()).x, ty), kTextDim, sz.c_str());
                ImGui::PopID();
            }
        ImGui::EndChild();
        y += listH + Dip(8);
    }

    if (!pcm) {
        const char* msg = !clipError.empty() ? clipError.c_str() : d.isBank ? "Select a sound (double click plays it)" : "No audio";
        Text(dl, ImVec2(p.x + m, y), clipError.empty() ? kTextDim : kError, msg);
        return;
    }

    // waveform (min / max per pixel column, channels mixed) with the playhead; click to seek
    float waveH = std::max(Dip(60), std::min(Dip(160), p.y + h - y - Dip(60)));
    ImVec2 wa(p.x + m, y), wb(p.x + w - m, y + waveH);
    int cols = std::max(1, (int)(wb.x - wa.x));
    if (d.peaksFor != pcm.get() || d.peaksWidth != cols) {
        d.peaks.assign(cols, {0.f, 0.f});
        size_t frames = pcm->Frames();
        for (int c = 0; c < cols; c++) {
            size_t f0 = frames * c / cols, f1 = std::max(f0 + 1, frames * (c + 1) / cols);
            float lo = 0, hi = 0;
            for (size_t f = f0; f < f1 && f < frames; f++) {
                float s = 0;
                for (int ch = 0; ch < pcm->channels; ch++) s += pcm->samples[f * pcm->channels + ch];
                s /= 32768.f * pcm->channels;
                lo = std::min(lo, s);
                hi = std::max(hi, s);
            }
            d.peaks[c] = {lo, hi};
        }
        d.peaksFor = pcm.get();
        d.peaksWidth = cols;
    }
    dl->AddRectFilled(wa, wb, kBorder, Dip(3));
    float mid = (wa.y + wb.y) * 0.5f, amp = waveH * 0.45f;
    double pos = mine ? m_player.Position() : 0, len = std::max(1e-6, pcm->Seconds());
    float playX = wa.x + (float)(pos / len) * (wb.x - wa.x);
    for (int c = 0; c < cols; c++) {
        float x = wa.x + c + 0.5f;
        ImU32 col = mine && x <= playX ? kAccent : IM_COL32(120, 160, 230, 255);
        dl->AddLine(ImVec2(x, mid - d.peaks[c].second * amp), ImVec2(x, mid - d.peaks[c].first * amp + 1), col);
    }
    dl->AddLine(ImVec2(wa.x, mid), ImVec2(wb.x, mid), Alpha(kText, 0.15f));
    if (mine) dl->AddLine(ImVec2(playX, wa.y), ImVec2(playX, wb.y), kText, Dip(1.5f));
    ImGui::SetCursorScreenPos(wa);
    ImGui::InvisibleButton("##wave", ImVec2(wb.x - wa.x, waveH));
    if (ImGui::IsItemHovered()) {
        float t = (ImGui::GetIO().MousePos.x - wa.x) / (wb.x - wa.x);
        dl->AddLine(ImVec2(ImGui::GetIO().MousePos.x, wa.y), ImVec2(ImGui::GetIO().MousePos.x, wb.y), Alpha(kText, 0.35f));
        ImGui::SetTooltip("%.2f s", t * len);
        if (ImGui::IsItemClicked()) {
            if (mine) m_player.Seek(t * len);
            else PlayAudio(d, pcm, t * len);
        }
    }

    // transport: play / pause, stop, time, loop, volume, export
    float by = wb.y + Dip(10), bs = Dip(30);
    auto roundButton = [&](const char* id, float x, bool accent, auto&& icon, const char* tip) {
        ImGui::SetCursorScreenPos(ImVec2(x, by));
        bool r = ImGui::InvisibleButton(id, ImVec2(bs, bs));
        float ht = HoverAnim();
        ImVec2 c(x + bs * 0.5f, by + bs * 0.5f);
        dl->AddCircleFilled(c, bs * 0.5f, Mix(accent ? kAccent : kField, accent ? Shade(kAccent, 1.2f) : kTabSel, ht));
        icon(c);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return r;
    };
    bool playing = mine && !m_player.Paused();
    float x = p.x + m;
    if (roundButton("##play", x, true, [&](ImVec2 c) {
            float k = bs * 0.2f;
            if (playing) {
                dl->AddRectFilled(ImVec2(c.x - k, c.y - k), ImVec2(c.x - k * 0.3f, c.y + k), kText);
                dl->AddRectFilled(ImVec2(c.x + k * 0.3f, c.y - k), ImVec2(c.x + k, c.y + k), kText);
            } else {
                dl->AddTriangleFilled(ImVec2(c.x - k * 0.7f, c.y - k), ImVec2(c.x + k, c.y), ImVec2(c.x - k * 0.7f, c.y + k), kText);
            }
        }, playing ? "Pause" : "Play")) {
        if (playing) m_player.Pause();
        else if (mine) m_player.Resume();
        else PlayAudio(d, pcm, 0);
    }
    x += bs + Dip(6);
    if (roundButton("##stop", x, false, [&](ImVec2 c) {
            float k = bs * 0.16f;
            dl->AddRectFilled(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kText, Dip(1));
        }, "Stop") && mine)
        m_player.Stop();
    x += bs + Dip(10);
    std::string time = Fmt("%d:%05.2f / %d:%05.2f", (int)(pos / 60), fmod(pos, 60.0), (int)(len / 60), fmod(len, 60.0));
    Text(dl, ImVec2(x, by + (bs - ImGui::GetFontSize()) * 0.5f), kText, time.c_str());
    x += ImGui::CalcTextSize("00:00.00 / 00:00.00").x + Dip(12);
    ImGui::SetCursorScreenPos(ImVec2(x, by + (bs - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::Checkbox("Loop", &m_playLoop);
    // second row: volume, export
    ImGui::SetCursorScreenPos(ImVec2(p.x + m, by + bs + Dip(10)));
    float vol = m_player.Volume();
    ImGui::SetNextItemWidth(std::max(Dip(80), std::min(Dip(200), w - m * 2 - Dip(120))));
    if (ImGui::SliderFloat("##vol", &vol, 0, 1, "Volume %.2f")) m_player.SetVolume(vol);
    ImGui::SameLine(0, Dip(8));
    if (Button("Export WAV", ImVec2(0, 0))) ExportFiles({d.entry}, 0, 5);
}

// ================================================================== JSON editor (AvalonEdit look)

void App::DrawJsonEditor(Document& d, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##json", size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PushFont(monoFont, monoFont->LegacySize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // CalcTextSize rounds up: measure a long run for the real (fractional) advance of the monospaced font
    float lh = ImGui::GetFontSize() * 1.17f, cw = ImGui::CalcTextSize("0000000000000000000000000000000000000000").x / 40.f;
    const std::string& text = d.json;
    if (d.lines.empty()) d.lines.push_back(0);
    int nLines = (int)d.lines.size();
    if (d.maxCols == 0)
        for (int i = 0; i < nLines; i++) {
            u32 s = d.lines[i], e = i + 1 < nLines ? d.lines[i + 1] - 1 : (u32)text.size();
            d.maxCols = std::max(d.maxCols, e - s);
        }
    int digits = std::max(2, (int)std::to_string(nLines).size());
    float gutter = Dip(8) + digits * cw + Dip(8), textX = gutter + Dip(6);
    auto lineEnd = [&](int i) { return i + 1 < nLines ? d.lines[i + 1] - 1 : (u32)text.size(); };
    auto lineOf = [&](u32 off) { return (int)(std::upper_bound(d.lines.begin(), d.lines.end(), off) - d.lines.begin()) - 1; };

    // content extent drives the scroll bars; one invisible item takes the mouse
    ImVec2 contentSz(textX + d.maxCols * cw + Dip(20), nLines * lh + lh);
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##text", ImVec2(std::max(contentSz.x, size.x), std::max(contentSz.y, size.y)));
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    auto posToOffset = [&](ImVec2 m) {
        int line = std::clamp((int)((m.y - c0.y) / lh), 0, nLines - 1);
        int col = std::max(0, (int)std::lround((m.x - c0.x - textX) / cw));
        u32 s = d.lines[line], e = lineEnd(line);
        return std::min(s + (u32)col, e);
    };
    if (!d.loading && ImGui::IsItemClicked(0) && !io.KeyCtrl) {
        u32 off = posToOffset(io.MousePos);
        if (ImGui::IsMouseDoubleClicked(0)) {   // word
            auto isWord = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
            u32 a = off, b = off;
            while (a > 0 && isWord(text[a - 1])) a--;
            while (b < text.size() && isWord(text[b])) b++;
            d.selA = a;
            d.selB = b;
        } else {
            if (!io.KeyShift) d.selA = off;
            d.selB = off;
        }
    } else if (active && ImGui::IsMouseDragging(0, 1)) {
        d.selB = posToOffset(io.MousePos);
        // autoscroll while dragging past the edges
        float wy = ImGui::GetWindowPos().y, wh = ImGui::GetWindowHeight();
        if (io.MousePos.y < wy) ImGui::SetScrollY(ImGui::GetScrollY() - lh);
        if (io.MousePos.y > wy + wh) ImGui::SetScrollY(ImGui::GetScrollY() + lh);
    }
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (focused && !io.WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) { d.selA = 0; d.selB = (u32)text.size(); }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) && d.selA != d.selB) {
            u32 a = std::min(d.selA, d.selB), b = std::max(d.selA, d.selB);
            CopyToClipboard(text.substr(a, b - a));
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) { d.searchOpen = true; d.focusSearch = true; }
    }
    if (d.scrollToLine >= 0) {
        ImGui::SetScrollY(std::max(0.f, d.scrollToLine * lh - size.y * 0.4f));
        d.scrollToLine = -1;
    }

    // visible lines
    float scrollY = ImGui::GetScrollY(), scrollX = ImGui::GetScrollX();
    int first = std::max(0, (int)(scrollY / lh)), last = std::min(nLines - 1, (int)((scrollY + size.y) / lh) + 1);
    u32 sa = std::min(d.selA, d.selB), sb = std::max(d.selA, d.selB);
    ImVec2 wp = ImGui::GetWindowPos();
    float clipL = wp.x + gutter;
    dl->PushClipRect(ImVec2(clipL, wp.y), ImVec2(wp.x + size.x, wp.y + size.y), true);
    for (int i = first; i <= last && !d.loading; i++) {
        u32 s = d.lines[i], e = lineEnd(i);
        float y = c0.y + i * lh, x0 = c0.x + textX;
        if (sb > sa && sa <= e && sb >= s) {   // selection on this line
            u32 a = std::max(sa, s), b = std::min(sb, e);
            float xa = x0 + (a - s) * cw, xb = x0 + (b - s) * cw + (sb > e ? cw * 0.5f : 0);
            dl->AddRectFilled(ImVec2(xa, y), ImVec2(xb, y + lh), IM_COL32(38, 79, 120, 255));
        }
        // tokens
        std::string_view line(text.data() + s, e - s);
        size_t k = 0;
        if (d.syntax == 1) {
            static const std::set<std::string_view> keywords = {"class", "public", "private", "protected", "virtual", "static", "const", "void", "bool",
                                                                 "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float",
                                                                 "double", "if", "else", "switch", "case", "default", "struct", "enum", "nullptr"};
            static const std::set<std::string_view> jumps = {"goto", "return", "break", "continue"};
            while (k < line.size()) {
                char ch = line[k];
                size_t j = k + 1;
                ImU32 col = kJsonDefault;
                if (ch == '/' && k + 1 < line.size() && (line[k + 1] == '/' || line[k + 1] == '*')) {
                    if (line[k + 1] == '/') j = line.size();
                    else { size_t c = line.find("*/", k + 2); j = c == std::string_view::npos ? line.size() : c + 2; }
                    col = IM_COL32(0x84, 0xc3, 0x6b, 255);
                } else if (ch == '"') {
                    while (j < line.size() && line[j] != '"') j += line[j] == '\\' ? 2 : 1;
                    j = std::min(j + 1, line.size());
                    col = IM_COL32(0xEC, 0xC4, 0x8D, 255);
                } else if (isdigit((u8)ch)) {
                    while (j < line.size() && (isalnum((u8)line[j]) || line[j] == '.')) j++;
                    col = kJsonNumber;
                } else if (isalpha((u8)ch) || ch == '_') {
                    while (j < line.size() && (isalnum((u8)line[j]) || line[j] == '_')) j++;
                    std::string_view w = line.substr(k, j - k);
                    size_t q = j;
                    while (q < line.size() && line[q] == ' ') q++;
                    if (keywords.count(w)) col = IM_COL32(0x82, 0xAA, 0xFF, 255);
                    else if (jumps.count(w)) col = IM_COL32(0xdd, 0xa0, 0xdd, 255);
                    else if (w == "this" || w == "true" || w == "false") col = IM_COL32(0x56, 0x9c, 0xd6, 255);
                    else if (IStartsWith(w, "Label_") && q < line.size() && line[q] == ':') col = IM_COL32(0x80, 0x80, 0x80, 255);
                    else if (IStartsWith(w, "Label_")) col = IM_COL32(0x80, 0x80, 0x80, 255);
                    else if (q < line.size() && line[q] == '(') col = IM_COL32(0xC3, 0xE8, 0x8D, 255);   // call
                    else if (w.size() > 1 && (w[0] == 'F' || w[0] == 'U' || w[0] == 'T' || w[0] == 'A') && isupper((u8)w[1])) col = IM_COL32(0x4c, 0xc9, 0xb0, 255);
                } else if (strchr("{}[]()", ch)) {
                    col = kJsonPunct;
                } else if (ch == '*' || ch == '&' || (ch == '-' && k + 1 < line.size() && line[k + 1] == '>')) {
                    if (ch == '-') j = k + 2;
                    col = IM_COL32(0xff, 0x88, 0x88, 255);
                } else if (ch == ' ') {
                    while (j < line.size() && line[j] == ' ') j++;
                    k = j;
                    continue;
                }
                dl->AddText(ImVec2(x0 + k * cw, y), col, line.data() + k, line.data() + j);
                k = j;
            }
            continue;
        }
        while (k < line.size()) {
            char ch = line[k];
            size_t j = k + 1;
            ImU32 col = kJsonDefault;
            if (ch == '"') {
                while (j < line.size() && line[j] != '"') j += line[j] == '\\' ? 2 : 1;
                j = std::min(j + 1, line.size());
                size_t q = j;
                while (q < line.size() && line[q] == ' ') q++;
                col = q < line.size() && line[q] == ':' ? kJsonKey : kJsonString;
            } else if (ch == '-' || isdigit((unsigned char)ch)) {
                while (j < line.size() && (isalnum((unsigned char)line[j]) || line[j] == '.' || line[j] == '-' || line[j] == '+')) j++;
                col = kJsonNumber;
            } else if (isalpha((unsigned char)ch)) {
                while (j < line.size() && isalpha((unsigned char)line[j])) j++;
                std::string_view w = line.substr(k, j - k);
                col = (w == "true" || w == "false") ? kJsonBool : w == "null" ? kJsonNull : kJsonDefault;
            } else if (strchr("{}[],:", ch)) {
                col = kJsonPunct;
            } else if (ch == ' ') {
                while (j < line.size() && line[j] == ' ') j++;
                k = j;
                continue;
            }
            dl->AddText(ImVec2(x0 + k * cw, y), col, line.data() + k, line.data() + j);
            if (col == kJsonString && j - k > 8 && hovered) {   // game / package path value: Ctrl+click opens it
                ImVec2 ta(x0 + k * cw, y), tb(x0 + j * cw, y + lh);
                if (ImGui::IsMouseHoveringRect(ta, tb)) {
                    std::string val(line.substr(k + 1, j - k - 2));
                    bool candidate = IStartsWith(val, "/Game/") || (IStartsWith(val, "Athena/") && (IEndsWith(val, ".uasset") || IEndsWith(val, ".umap")));
                    size_t colon = val.find(':');
                    if (colon != std::string::npos) val.resize(colon);   // "/Game/X.Y:Inner" -> package object
                    i64 target = candidate ? m_api->ResolveFile(val) : -1;
                    if (target >= 0) {
                        dl->AddLine(ImVec2(ta.x + cw, tb.y - 1), ImVec2(tb.x - cw, tb.y - 1), col);
                        ImGui::SetTooltip("Ctrl+Click to open %s", std::string(m_api->vfs.Path((u32)target)).c_str());
                        if (io.KeyCtrl) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (ImGui::IsMouseClicked(0)) { u32 e = (u32)target; Post([this, e] { OpenFile(e, true); }); }
                        }
                    }
                }
            }
            k = j;
        }
    }
    dl->PopClipRect();
    // gutter (does not scroll horizontally): line numbers + dotted separator
    dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + gutter, wp.y + size.y), kPanel);
    for (int i = first; i <= last && !d.loading; i++) {
        std::string n = std::to_string(i + 1);
        float y = c0.y + i * lh;
        dl->AddText(ImVec2(wp.x + gutter - Dip(8) - n.size() * cw, y), lineOf(d.selB) == i && focused ? kJsonDefault : IM_COL32(110, 112, 128, 255), n.c_str());
    }
    for (float yy = wp.y; yy < wp.y + size.y; yy += 2) dl->AddLine(ImVec2(wp.x + gutter, yy), ImVec2(wp.x + gutter, yy + 1), IM_COL32(55, 55, 63, 255));
    (void)scrollX;
    ImGui::PopFont();

    // search bar (AvalonEditor: 500 dip, top right, accent border)
    if (d.searchOpen) {
        float bw = std::min(Dip(500), size.x - Dip(10)), bh = FieldH(), tb = Dip(26);
        ImVec2 bp(wp.x + size.x - bw - Dip(3) - (ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0), wp.y);
        ImGui::SetCursorScreenPos(ImVec2(bp.x + ImGui::GetScrollX(), bp.y));   // window coords include scroll
        ImGui::SetCursorScreenPos(bp);
        float fieldW = bw - tb * 5 - bh - Dip(2);
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        fdl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), kField);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dip(25), (bh - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::SetNextItemWidth(fieldW);
        if (d.focusSearch) { ImGui::SetKeyboardFocusHere(); d.focusSearch = false; }
        bool enter = ImGui::InputTextWithHint("##find", "Write your pattern and press enter...", d.find, sizeof(d.find), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) d.searchOpen = false;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        fdl->AddRect(bp, ImVec2(bp.x + fieldW + tb * 5, bp.y + bh), kAccent);
        IconSearch(fdl, ImVec2(bp.x + Dip(13), bp.y + bh * 0.5f), Dip(16), kText);
        auto toggle = [&](const char* id, bool* v, const char* tip, const char* glyph) {
            ImGui::SameLine(0, 0);
            ImVec2 tp = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(id, ImVec2(tb, bh))) *v = !*v;
            bool hv = ImGui::IsItemHovered();
            if (*v || hv) fdl->AddRectFilled(tp, ImVec2(tp.x + tb, tp.y + bh), *v ? kAccent : kTabSel);
            ImVec2 gs = ImGui::CalcTextSize(glyph);
            fdl->AddText(ImVec2(tp.x + (tb - gs.x) * 0.5f, tp.y + (bh - gs.y) * 0.5f), kText, glyph);
            if (hv) ImGui::SetTooltip("%s", tip);
        };
        bool regex = false;
        toggle("##up", &d.searchUp, "Search Up", "\xE2\x86\x91");
        toggle("##ww", &d.wholeWord, "Match Whole Word", "ab");
        toggle("##mc", &d.matchCase, "Match Case", "Aa");
        ImGui::BeginDisabled();
        toggle("##re", &regex, "Regex (not supported)", ".*");
        ImGui::EndDisabled();
        ImGui::SameLine(0, 0);
        if (ToolbarButton("##fclr", ImVec2(tb, bh), IconBackspace, "Clear Search Filter")) d.find[0] = 0;
        ImGui::SameLine(0, Dip(2));
        ImVec2 xp = ImGui::GetCursorScreenPos();
        fdl->AddRectFilled(xp, ImVec2(xp.x + bh, xp.y + bh), kPanel);
        fdl->AddRect(xp, ImVec2(xp.x + bh, xp.y + bh), kAccent);
        if (ToolbarButton("##fclose", ImVec2(bh, bh), IconClose, "Close")) d.searchOpen = false;
        if (enter && d.find[0] && !text.empty()) {
            std::string needle = d.find, hay;
            if (!d.matchCase) needle = ToLower(needle);
            auto matchAt = [&](size_t pos) {
                if (pos + needle.size() > text.size()) return false;
                for (size_t q = 0; q < needle.size(); q++) {
                    char c = text[pos + q];
                    if (!d.matchCase) c = (char)tolower((unsigned char)c);
                    if (c != needle[q]) return false;
                }
                if (d.wholeWord) {
                    auto w = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
                    if (pos > 0 && w(text[pos - 1])) return false;
                    if (pos + needle.size() < text.size() && w(text[pos + needle.size()])) return false;
                }
                return true;
            };
            size_t start = d.searchUp ? std::min(d.selA, d.selB) : std::max(d.selA, d.selB), n = text.size(), found = std::string::npos;
            for (size_t k = 0; k < n; k++) {
                size_t pos = d.searchUp ? (start + n * 2 - 1 - k) % n : (start + k) % n;
                if (matchAt(pos)) { found = pos; break; }
            }
            if (found != std::string::npos) {
                d.selA = (u32)found;
                d.selB = (u32)(found + needle.size());
                d.scrollToLine = lineOf(d.selA);
            } else {
                LOGW("'%s' not found", d.find);
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    if (d.loading && !d.newTab) {
        // the status bar shows "Loading"; keep the editor empty like FModel
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    (void)origin;
}

// ================================================================== log + status bar

void App::DrawLog(float width) {
    LogSnapshot(m_log, m_logSeen);
    if (m_log.size() > 5000) m_log.erase(m_log.begin(), m_log.begin() + (m_log.size() - 4000));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    // expander header: a strip with the expand / collapse glyph
    float hh = Dip(26);
    ImGui::InvisibleButton("##logexp", ImVec2(width, hh));
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { m_settings.loggerExpanded = !m_settings.loggerExpanded; m_settings.Save(); }
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + hh), Mix(kLayer0, kHover, HoverAnim()));
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + width, p.y), kBorder);
    dl->AddLine(ImVec2(p.x, p.y + hh - 1), ImVec2(p.x + width, p.y + hh - 1), kBorder);
    IconExpandLog(dl, ImVec2(p.x + width * 0.5f, p.y + hh * 0.5f), Dip(16), kText, m_settings.loggerExpanded);
    if (!m_settings.loggerExpanded) return;

    float h = Dip(175);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dip(5), Dip(5)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##log", ImVec2(width, h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(monoFont, FontDip(11) * 0.9f);
    float wrap = ImGui::GetContentRegionAvail().x - Dip(28);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    for (auto& l : m_log) {
        ImVec4 tag = l.level == LogLevel::Error ? ImGui::ColorConvertU32ToFloat4(kError)
                     : l.level == LogLevel::Warn ? ImGui::ColorConvertU32ToFloat4(kAlert)
                                                  : ImVec4(0.42f, 0.58f, 0.86f, 1);
        ImGui::TextColored(tag, "[%s]", l.level == LogLevel::Error ? "ERR" : l.level == LogLevel::Warn ? "WRN" : "INF");
        ImGui::SameLine(0, ImGui::CalcTextSize(" ").x);
        ImGui::TextWrapped("%s", l.text.c_str());
    }
    ImGui::PopTextWrapPos();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    // bottom-right buttons: open the output folder, clear the log
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        float bs = Dip(22);
        float x = win->InnerRect.Max.x - Dip(5) - bs, y = win->InnerRect.Max.y - Dip(5) - bs * 2;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ToolbarButton("##openout", ImVec2(bs, bs), IconOpenExternal, "Open Output Folder")) {
            CreateDirectories(m_settings.outputDir);
            ShellExecuteW(nullptr, L"open", m_settings.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y + bs));
        if (ToolbarButton("##clearlog", ImVec2(bs, bs), IconTrash, "Clear Logs")) m_log.clear();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x, p.y + hh - 1), ImVec2(p.x + width, p.y + hh + h), kBorder);
}

void App::DrawStatusBar() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = Dip(27), w = ImGui::GetContentRegionAvail().x;
    bool docLoading = false;
    for (auto& d : m_docs) docLoading |= d->loading || d->worldLoading;
    bool busy = m_mountState == MountState::Mounting || m_exporting || docLoading || m_refBuilding;
    ImU32 col = m_mountState == MountState::Failed ? kError : busy ? kAlert : kAccent;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col);
    dl->AddLine(p, ImVec2(p.x + w, p.y), kBorder);
    std::string label = m_mountState == MountState::Mounting ? "Loading archives \xE2\x80\xA6"
                        : m_mountState == MountState::Failed  ? "Failed: " + m_mountError
                        : m_exporting                          ? Fmt("Exporting \xE2\x80\xA6    %d / %d", m_exportDone.load(), m_exportTotal.load())
                        : docLoading                           ? std::string("Loading \xE2\x80\xA6")
                        : m_refBuilding                        ? Fmt("Indexing references \xE2\x80\xA6    %d / %d", m_refDone.load(), m_refTotal.load())
                        : m_mountState == MountState::Ready    ? std::string("Completed")
                                                               : std::string("Ready");
    float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
    // right: last refresh
    std::string right;
    if (m_mountState == MountState::Ready) {
        double mins = (ImGui::GetTime() - m_readyAt) / 60.0;
        right = mins < 1 ? "Last Refresh: Just now" : mins < 60 ? Fmt("Last Refresh: %d minute%s ago", (int)mins, (int)mins == 1 ? "" : "s")
                                                                : Fmt("Last Refresh: %d hour%s ago", (int)(mins / 60), (int)(mins / 60) == 1 ? "" : "s");
    }
    float rw = right.empty() ? 0 : ImGui::CalcTextSize(right.c_str()).x + Dip(16) + Dip(10);
    dl->PushClipRect(p, ImVec2(p.x + w - rw - Dip(10), p.y + h), true);
    Text(dl, ImVec2(p.x + Dip(5) + Dip(4), ty), IM_COL32_WHITE, label.c_str());
    dl->PopClipRect();
    if (!right.empty()) {
        float rx = p.x + w - Dip(5) - ImGui::CalcTextSize(right.c_str()).x;
        Text(dl, ImVec2(rx, ty), IM_COL32_WHITE, right.c_str());
        IconRefresh(dl, ImVec2(rx - Dip(10) - Dip(8), p.y + h * 0.5f), Dip(16), IM_COL32_WHITE);
    }
    if (m_mountState == MountState::Failed && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + h))) ImGui::SetTooltip("%s", m_mountError.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

// ================================================================== Packages > Search window

void App::DrawSearchWindow() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.55f, vp->WorkSize.y * 0.6f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kLayer0);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kLayer0);
    if (!ImGui::Begin("Search For Packages", &m_showSearch, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        ImGui::PopStyleColor(3);
        return;
    }
    // Search / References (FModel SearchView tabs)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* names[3] = {"Search", "References", "Localization"};
        float tw = Dip(120), th = Dip(24);
        ImVec2 p = ImGui::GetCursorScreenPos();
        for (int i = 0; i < 3; i++) {
            ImGui::SetCursorScreenPos(ImVec2(p.x + tw * i, p.y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##stab", ImVec2(tw, th))) m_searchTab = i;
            float ht = HoverAnim(), st = Anim(ImGui::GetItemID() ^ 0x7ab, m_searchTab == i);
            ImGui::PopID();
            ImVec2 a(p.x + tw * i, p.y);
            dl->AddRectFilled(a, ImVec2(a.x + tw - 1, a.y + th), Mix(Mix(kLayer0, kHover, ht), kTabSel, st));
            TextCentered(dl, a.x, a.x + tw, a.y + (th - ImGui::GetFontSize()) * 0.5f, kText, names[i]);
        }
        float w = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(ImVec2(p.x, p.y + th), ImVec2(p.x + w, p.y + th + Dip(4)), kTabSel);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + th + Dip(10)));
    }
    if (m_searchTab == 1 || m_searchTab == 2) {
        if (m_searchTab == 1) DrawReferencesTab();
        else DrawLocalizationTab();
        ImGui::End();
        ImGui::PopStyleColor(3);
        return;
    }
    SearchBox("##gsearch", m_search, sizeof(m_search), m_searchRegex ? "Write your regex pattern..." : "Write your pattern (terms or * ? wildcards)...", 0,
              m_focusGlobalSearch);
    m_focusGlobalSearch = false;
    ImGui::Checkbox("Match Case", &m_searchMatchCase);
    ImGui::SameLine();
    ImGui::Checkbox("Regex", &m_searchRegex);
    UpdateSearch();
    if (!m_searchError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError), "%s", m_searchError.c_str());
    else ImGui::TextDisabled("%zu result(s)%s", m_searchResults.size(), m_searchTruncated ? " (truncated)" : "");
    auto& vfs = m_api->vfs;
    if (ImGui::BeginTable("##sres", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Dip(70));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)m_searchResults.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                u32 e = m_searchResults[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)e);
                std::string path(vfs.Path(e));
                if (ImGui::Selectable(path.c_str(), m_selected == (i64)e, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_selected = e;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        // FModel: opens the package and selects it in its folder
                        auto& folders = vfs.Folders();
                        u32 cur = 0;
                        size_t pos = 0;
                        for (;;) {
                            size_t sl = path.find('/', pos);
                            if (sl == std::string::npos) break;
                            u32 next = UINT32_MAX;
                            for (u32 f : folders[cur].folders) if (IEquals(folders[f].name, std::string_view(path).substr(pos, sl - pos))) { next = f; break; }
                            if (next == UINT32_MAX) break;
                            cur = next;
                            pos = sl + 1;
                        }
                        SelectFolder(cur);
                        m_selected = e;
                        OpenFile(e);
                    }
                }
                if (ImGui::BeginPopupContextItem("##sctx")) {
                    DrawFileMenu(e);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", HumanSize(vfs.Entry(e).usize).c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
}

// Localization tab: translated strings by text / key in the chosen language
void App::DrawLocalizationTab() {
    auto loc = m_api->localization;
    if (!loc) {
        ImGui::TextDisabled("No localisation loaded");
        return;
    }
    float cw = Dip(120);
    SearchBox("##locq", m_locQuery, sizeof(m_locQuery), "Search translated text (all words must match)...", ImGui::GetContentRegionAvail().x - cw - Dip(6), false);
    ImGui::SameLine(0, Dip(6));
    ImGui::SetNextItemWidth(cw);
    PushFieldStyle();
    if (ImGui::BeginCombo("##locc", loc->Culture().c_str())) {
        static std::vector<std::string> cultures;
        if (cultures.empty()) cultures = Localization::Cultures(m_api->vfs);
        for (auto& c : cultures)
            if (ImGui::Selectable(c.c_str(), c == loc->Culture())) {
                std::string err;
                if (m_api->SetLanguage(c, &err)) { m_settings.language = c; m_settings.Save(); m_locLast.clear(); }
                else LOGW("language %s: %s", c.c_str(), err.c_str());
            }
        ImGui::EndCombo();
    }
    PopFieldStyle();
    ImGui::Checkbox("Search keys too", &m_locInKeys);
    std::string key = Fmt("%s|%s|%d", m_locQuery, loc->Culture().c_str(), (int)m_locInKeys);
    if (key != m_locLast) {
        m_locLast = key;
        m_locResults = loc->Search(m_locQuery, 5000, m_locInKeys);
    }
    ImGui::TextDisabled("%zu result(s) of %zu strings", m_locResults.size(), loc->Size());
    if (ImGui::BeginTable("##loc", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, Dip(260));
        ImGui::TableSetupColumn("Namespace", ImGuiTableColumnFlags_WidthFixed, Dip(110));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)m_locResults.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const LocEntry* e = m_locResults[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::Selectable(e->text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::IsItemHovered() && e->text.size() > 60) ImGui::SetTooltip("%s", e->text.c_str());
                if (ImGui::BeginPopupContextItem("##lctx")) {
                    if (ImGui::MenuItem("Copy Text")) CopyToClipboard(e->text);
                    if (ImGui::MenuItem("Copy Key")) CopyToClipboard(e->key);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", e->key.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", e->ns.empty() ? "(none)" : e->ns.c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
}

// ================================================================== file context menu (FModel FileContextMenu)

void App::DrawFileMenu(u32 e) {
    const Vfs& vfs = m_api->vfs;
    std::string path(vfs.Path(e)), name(vfs.FileName(e));
    bool pkg = IsPackageFile(path);
    m_selected = e;
    if (MenuItemIcon("Extract in New Tab", nullptr, IconExportBox)) OpenFile(e, true);
    if (MenuItemIcon("Show Metadata", nullptr, IconInfo, pkg)) ShowMetadata(e, true);
    if (MenuItemIcon("Find References", nullptr, IconSearch, pkg)) FindReferences(e);
    if (MenuItemIcon("Decompile Blueprint", nullptr, IconCube, pkg)) DecompileBlueprintTab(e);
    if (MenuItemIcon("Decompile Shaders", nullptr, IconCube, pkg || IsShaderCacheFile(path))) DecompileShadersTab(e);
    ImGui::Separator();
    if (ImGui::BeginMenu("      Export")) {
        if (ImGui::MenuItem("Raw Data (.uasset)")) ExportFiles({e}, 1);
        if (ImGui::MenuItem("Properties (.json)", nullptr, false, pkg)) ExportFiles({e}, 2);
        if (ImGui::MenuItem("Texture", nullptr, false, pkg)) ExportFiles({e}, 0, 1);
        if (ImGui::MenuItem("Model", nullptr, false, pkg)) ExportFiles({e}, 0, 2);
        if (ImGui::MenuItem("World", nullptr, false, IEndsWith(path, ".umap"))) ExportFiles({e}, 0, 3);
        if (ImGui::MenuItem("Animation", nullptr, false, pkg)) ExportFiles({e}, 0, 4);
        if (ImGui::MenuItem("Audio", nullptr, false, IEndsWith(path, ".wem") || pkg)) ExportFiles({e}, 0, 5);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("      Copy")) {
        std::string dir = path.substr(0, path.rfind('/'));
        auto noExt = [](std::string s) { size_t dot = s.rfind('.'); return dot == std::string::npos ? s : s.substr(0, dot); };
        if (ImGui::MenuItem("Package Path")) CopyToClipboard(path);
        if (ImGui::MenuItem("Package Name")) CopyToClipboard(name);
        if (ImGui::MenuItem("Directory Path")) CopyToClipboard(dir);
        if (ImGui::MenuItem("Package Path w/o Extension")) CopyToClipboard(noExt(path));
        if (ImGui::MenuItem("Package Name w/o Extension")) CopyToClipboard(noExt(name));
        if (ImGui::MenuItem("/Game Path")) CopyToClipboard(GamePathToPackage(path));
        ImGui::EndMenu();
    }
}
