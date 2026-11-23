// Behavior tree side panel: the blackboard keys and the node tree (composites, tasks, their decorators and services).
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "imgui.h"
#include "ue/BehaviorTree.h"
#include <functional>

namespace {
const ImU32 kComposite = IM_COL32(150, 155, 170, 255), kTask = IM_COL32(170, 120, 230, 255), kDecorator = IM_COL32(90, 160, 240, 255),
            kService = IM_COL32(90, 200, 130, 255), kSubtree = IM_COL32(240, 190, 80, 255);

void Tag(ImU32 col, const char* tag) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(tag);
    ImGui::PopStyleColor();
    ImGui::SameLine();
}
}   // namespace

void App::DrawBehaviorTreePanel(Document& d) {
    const BehaviorTreeInfo& bt = *d.behaviorTree;
    if (!bt.blackboard.empty()) {
        ImGui::TextDisabled("Blackboard");
        ImGui::SameLine();
        std::string bb = bt.blackboard.substr(bt.blackboard.rfind('/') + 1);
        bb = bb.substr(0, bb.find('.'));
        if (ImGui::SmallButton(bb.c_str())) {
            i64 e = m_api->ResolveFile(bt.blackboard);
            if (e >= 0) OpenFile((u32)e, true);
        }
        if (ImGui::IsItemHovered()) Tip("%s\nclick to open", bt.blackboard.c_str());
    }
    if (!bt.keys.empty() && ImGui::CollapsingHeader(Fmt("Keys (%zu)###bbkeys", bt.keys.size()).c_str())) {
        for (auto& k : bt.keys) {
            ImGui::TextUnformatted(k.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s%s", k.type.c_str(), k.detail.empty() ? "" : ("<" + k.detail + ">").c_str(),
                                k.from.empty() ? "" : ("  from " + k.from).c_str());
        }
    }
    ImGui::Separator();
    ImGui::BeginChild("##bttree", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    auto aux = [&](const BtAux& a, ImU32 col, const char* tag) {
        ImGui::Indent(ImGui::GetFontSize());
        Tag(col, tag);
        ImGui::TextUnformatted(a.name.c_str());
        if (ImGui::IsItemHovered()) Tip("%s%s%s", a.cls.c_str(), a.details.empty() ? "" : "\n", a.details.c_str());
        if (!a.details.empty() && a.details != a.name) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", a.details.c_str());
        }
        ImGui::Unindent(ImGui::GetFontSize());
    };
    std::function<void(int)> node = [&](int i) {
        const BtNode& n = bt.nodes[(size_t)i];
        ImGui::PushID(i);
        for (auto& dec : n.decorators) aux(dec, kDecorator, "?");
        if (!n.decoratorLogic.empty()) {
            ImGui::Indent(ImGui::GetFontSize());
            ImGui::TextDisabled("decorator logic %s", n.decoratorLogic.c_str());
            ImGui::Unindent(ImGui::GetFontSize());
        }
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (n.children.empty() && n.services.empty()) fl |= ImGuiTreeNodeFlags_Leaf;
        ImGui::PushStyleColor(ImGuiCol_Text, n.composite ? kComposite : n.subtree.empty() ? kTask : kSubtree);
        std::string label = n.composite ? (n.name == n.cls ? n.cls : n.cls + ": " + n.name) : n.name;
        bool open = ImGui::TreeNodeEx("##n", fl, "%s", label.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            Tip("%s%s%s%s", n.cls.c_str(), n.details.empty() ? "" : "\n", n.details.c_str(), n.subtree.empty() ? "" : "\nclick to open the subtree");
        if (!n.subtree.empty() && ImGui::IsItemClicked()) {
            i64 e = m_api->ResolveFile(n.subtree);
            if (e >= 0) OpenFile((u32)e, true);
        }
        if (!n.details.empty() || !n.subtree.empty() || (!n.composite && n.name != n.cls)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s%s", !n.composite && n.name != n.cls ? ("(" + n.cls + ")  ").c_str() : "", n.details.c_str(),
                                n.subtree.empty() ? "" : ("  -> " + n.subtree.substr(n.subtree.rfind('/') + 1)).c_str());
        }
        if (open) {
            for (auto& s : n.services) aux(s, kService, "*");
            for (int c : n.children) node(c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    if (!bt.nodes.empty()) node(0);
    ImGui::EndChild();
}
