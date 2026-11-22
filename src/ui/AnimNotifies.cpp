// AnimNotify playback in the animation view: Wwise event sounds (AnimNotify_WwiseSound) and particle effects
// (AnimNotify_PlayParticleEffect / AnimNotify_AthenaPlayParticleEffect) fired when the playing animation passes their time.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "app/WwiseEvents.h"
#include "ue/World.h"
#include "imgui.h"
#include <random>

// Socket (or bone) -> mesh space under the current pose; false when the mesh has neither.
static bool NotifySocketOnMesh(const Document& d, const std::string& name, const std::vector<Mat4>* boneWorld, Mat4& out) {
    if (name.empty() || !d.mesh) return false;
    for (auto& so : d.sockets) {
        if (!IEquals(so.name, name)) continue;
        out = so.transform;
        if (boneWorld && !so.bone.empty())
            for (size_t b = 0; b < d.mesh->skeleton.size() && b < boneWorld->size(); b++)
                if (IEquals(d.mesh->skeleton[b].name, so.bone)) {
                    out = Mat4::FromTRS(so.location, RotatorToQuat(so.rotation.x, so.rotation.y, so.rotation.z), so.scale) * (*boneWorld)[b];
                    break;
                }
        return true;
    }
    if (boneWorld)
        for (size_t b = 0; b < d.mesh->skeleton.size() && b < boneWorld->size(); b++)
            if (IEquals(d.mesh->skeleton[b].name, name)) {
                out = (*boneWorld)[b];
                return true;
            }
    return false;
}

// A bone the preview mesh lacks (face bones on a skeleton mesh) falls back to its nearest ancestor in the Skeleton asset.
static bool NotifySocket(const Document& d, std::string name, const std::vector<Mat4>* boneWorld, Mat4& out) {
    for (int depth = 0; depth < 64 && !name.empty(); depth++) {
        if (NotifySocketOnMesh(d, name, boneWorld, out)) return true;
        auto it = d.notifyParents.find(ToLower(name));
        if (it == d.notifyParents.end()) return false;
        name = it->second;
    }
    return false;
}

void App::LoadNotifies(Document& d) {
    d.notifiesLoaded = true;
    d.notifies.clear();
    if (d.montage) d.notifies = d.montage->notifies;
    else if (d.pkg && d.pkg->MainExport()) ReadAnimNotifies(*d.pkg, d.pkg->ReadProperties(d.pkg->MainExport() - 1), d.notifies);
    std::sort(d.notifies.begin(), d.notifies.end(), [](auto& a, auto& b) { return a.time < b.time; });
    bool sockets = false;
    for (auto& n : d.notifies) sockets |= !n.particle.empty() && !n.socket.empty();
    if (sockets && d.anim) {   // the skeleton hierarchy for sockets the preview mesh does not have
        std::vector<Bone> skel;
        auto sp = m_api->cache->LoadPath(d.anim->skeletonPath);
        if (sp && sp->MainExport() && ReadSkeleton(*sp, sp->MainExport() - 1, skel))
            for (auto& b : skel)
                if (b.parent >= 0 && b.parent < (int)skel.size()) d.notifyParents[ToLower(b.name)] = skel[b.parent].name;
    }
    // decode the sounds and load the effects up front so they fire on time
    std::lock_guard lk(m_notifyMutex);
    for (auto& n : d.notifies) {
        if (!n.sound.empty() && !m_notifySounds.count(n.sound)) {
            m_notifySounds[n.sound];
            m_jobs->Push([this, path = n.sound] {
                NotifySound s;
                WwiseEventInfo ev;
                std::string err;
                if (LoadWwiseEvent(*m_api, path, ev, &err))
                    for (auto& m : ev.media) {
                        if (m.where == WwiseEventMedia::Missing || s.clips.size() >= 8) continue;
                        Bytes wem;
                        auto pcm = std::make_shared<AudioPcm>();
                        if (ReadEventMedia(*m_api, ev, m, wem, &err) && DecodeWem(wem.data(), wem.size(), *pcm, &err)) s.clips.push_back(pcm);
                    }
                if (s.clips.empty()) LOGW("notify sound %s: %s", path.c_str(), err.empty() ? "no playable media" : err.c_str());
                s.state = s.clips.empty() ? 2 : 1;
                std::lock_guard lk2(m_notifyMutex);
                m_notifySounds[path] = std::move(s);
            });
        }
        if (!n.itemClass.empty() && !m_itemTemplates.count(n.itemClass)) {   // the item Blueprint assembled from its components
            m_itemTemplates[n.itemClass];
            m_jobs->Push([this, path = n.itemClass] {
                WorldLoader loader(*m_api->cache);
                WorldOptions opt;
                opt.landscape = opt.lights = false;
                WorldData w;
                std::string err;
                auto parts = std::make_shared<std::vector<Document::ItemPart>>();
                if (loader.AddBlueprint(path, Mat4::Identity(), opt, w, &err))
                    for (auto& g : w.groups) {
                        auto base = m_assets->MeshByPath(g.mesh, 0);
                        if (!base) continue;
                        auto mesh = base;
                        if (!g.materials.empty()) {   // per-component material overrides on a copy
                            mesh = std::make_shared<GpuMesh>(*base);
                            for (size_t k = 0; k < g.materials.size() && k < mesh->materials.size(); k++)
                                if (!g.materials[k].empty()) mesh->materials[k] = m_assets->Material(g.materials[k]);
                        }
                        for (auto& in : g.instances) parts->push_back({mesh, in.transform});
                    }
                if (parts->empty()) LOGW("notify item %s: %s", path.c_str(), err.empty() ? "no mesh components" : err.c_str());
                std::lock_guard lk2(m_notifyMutex);
                m_itemTemplates[path] = {parts->empty() ? 2 : 1, parts};
            });
        }
        if (!n.particle.empty() && !m_notifyFxTemplates.count(n.particle)) {
            m_notifyFxTemplates[n.particle];
            m_jobs->Push([this, path = n.particle] {
                std::string err;
                auto fx = std::make_shared<Document::Attachment>();
                fx->mesh = path;
                auto pkg = m_api->cache->LoadPath(path, &err);
                bool ok = pkg && pkg->MainExport() && LoadParticleEffect(*pkg, *fx, &err);
                if (!ok) LOGW("notify effect %s: %s", path.c_str(), err.c_str());
                std::lock_guard lk2(m_notifyMutex);
                m_notifyFxTemplates[path] = {ok ? 1 : 2, ok ? fx : nullptr};
            });
        }
    }
}

void App::FireNotifies(Document& d, float from, float to, bool itemsOnly) {
    if (!d.playNotifies) return;
    static std::mt19937 rng{std::random_device{}()};
    for (auto& n : d.notifies) {
        if (n.time <= from || n.time > to) continue;
        std::lock_guard lk(m_notifyMutex);
        auto held = [&](int hand) {
            for (size_t k = d.heldItems.size(); k-- > 0;)
                if (d.heldItems[k].hand == hand) return (int)k;
            return -1;
        };
        switch (n.itemAction) {   // cosmetic items in the hands
        case AnimNotifyInfo::SpawnItem: {
            auto it = m_itemTemplates.find(n.itemClass);
            if (it == m_itemTemplates.end() || it->second.state == 2) break;   // still loading: the parts are filled in later
            if (int k = held(n.itemHand); k >= 0) d.heldItems.erase(d.heldItems.begin() + k);
            Document::HeldItem h;
            h.cls = n.itemClass;
            h.parts = it->second.parts;
            h.hand = n.itemHand;
            h.visible = n.itemVisible;
            d.heldItems.push_back(std::move(h));
            break;
        }
        case AnimNotifyInfo::DestroyItem:
            if (int k = held(n.itemHand); k >= 0) d.heldItems.erase(d.heldItems.begin() + k);
            break;
        case AnimNotifyInfo::SwitchItem:
            if (int k = held(n.itemHand); k >= 0) d.heldItems[k].hand = n.itemHand2;
            break;
        case AnimNotifyInfo::ShowItem:
            if (int k = held(n.itemHand); k >= 0) d.heldItems[k].visible = n.itemVisible;
            break;
        default:
            break;
        }
        if (itemsOnly) continue;
        if (!n.sound.empty()) {   // a random one of the event's sounds on a free device
            auto it = m_notifySounds.find(n.sound);
            if (it != m_notifySounds.end() && it->second.state == 1) {
                auto& clips = it->second.clips;
                auto pcm = clips[std::uniform_int_distribution<size_t>(0, clips.size() - 1)(rng)];
                AudioPlayer* free = nullptr;
                for (auto& p : m_notifyPlayers)
                    if (!p.Active() || p.Finished()) { free = &p; break; }
                if (free) {
                    free->SetVolume(m_player.Volume());
                    free->Play(pcm);
                }
            }
        }
        if (!n.particle.empty() && d.notifyFx.size() < 24) {
            auto it = m_notifyFxTemplates.find(n.particle);
            if (it == m_notifyFxTemplates.end() || it->second.state != 1) continue;
            const auto& tpl = *it->second.fx;
            Document::Attachment fx;
            fx.mesh = n.particle;
            fx.socket = n.socket;
            fx.ps = tpl.ps;
            fx.looks = tpl.looks;
            fx.psLength = tpl.psLength;
            fx.sim = std::make_shared<ParticleSim>();
            fx.sim->Init(*tpl.ps);
            fx.follow = n.attached;
            fx.offset = Mat4::FromTRS(n.locationOffset, RotatorToQuat(n.rotationOffset.x, n.rotationOffset.y, n.rotationOffset.z), {1, 1, 1});
            fx.placed = false;
            d.notifyFx.push_back(std::move(fx));
        }
    }
}

void App::UpdateNotifyFx(Document& d, const std::vector<Mat4>* boneWorld) {
    float dt = ImGui::GetIO().DeltaTime * (d.animPlaying ? d.animSpeed : 1.f);
    for (size_t i = 0; i < d.notifyFx.size();) {
        auto& fx = d.notifyFx[i];
        fx.psTime += dt;
        fx.sim->AdvanceTo(fx.psTime);
        if ((fx.psTime > 0.25f && fx.sim->Count() == 0) || fx.psTime > std::max(fx.psLength * 2, 4.f)) {   // burnt out
            d.notifyFx.erase(d.notifyFx.begin() + i);
            continue;
        }
        if (fx.follow || !fx.placed) {   // detached effects stay where they were spawned
            Mat4 s = Mat4::Identity();
            NotifySocket(d, fx.socket, boneWorld, s);   // no such socket: the mesh origin
            fx.world = fx.offset * s;
            fx.placed = true;
        }
        i++;
    }
    if (!d.notifyFx.empty()) m_busyFrames = 3;
    for (auto& h : d.heldItems) {   // the hand prop sockets
        if (!h.parts) {
            std::lock_guard lk(m_notifyMutex);
            if (auto it = m_itemTemplates.find(h.cls); it != m_itemTemplates.end()) h.parts = it->second.parts;
        }
        Mat4 s = Mat4::Identity();
        h.placed = NotifySocket(d, h.hand ? "LeftHandProp" : "RightHandProp", boneWorld, s) ||
                   NotifySocket(d, h.hand ? "LeftHandSocket" : "RightHandSocket", boneWorld, s);
        h.world = s;
    }
}

void App::DrawNotifyToggle(Document& d) {
    size_t sounds = 0, effects = 0, items = 0;
    for (auto& n : d.notifies) {
        sounds += !n.sound.empty();
        effects += !n.particle.empty();
        items += n.itemAction != AnimNotifyInfo::NoItem;
    }
    if (!sounds && !effects && !items) return;
    ImGui::SameLine();
    ImGui::Checkbox(Fmt("Notifies (%zu)", sounds + effects + items).c_str(), &d.playNotifies);
    if (!d.playNotifies) {
        d.notifyFx.clear();
        d.heldItems.clear();
    }
    if (ImGui::IsItemHovered()) {
        std::string tip = Fmt("Plays the animation's %zu sound, %zu particle and %zu held item notifies at their frames:\n", sounds, effects, items);
        int shown = 0;
        for (auto& n : d.notifies) {
            if (n.sound.empty() && n.particle.empty() && n.itemAction == AnimNotifyInfo::NoItem) continue;
            if (++shown > 24) { tip += "...\n"; break; }
            static const char* actions[] = {"", "spawn item", "destroy item", "switch hands", "show / hide item"};
            std::string what = !n.sound.empty() ? n.sound : !n.particle.empty() ? n.particle : !n.itemClass.empty() ? n.itemClass : actions[n.itemAction];
            tip += Fmt("\n%6.2f s  %s", n.time, what.substr(what.rfind('.') + 1).c_str());
            if (!n.particle.empty() && !n.socket.empty()) tip += " @ " + n.socket + (n.attached ? "" : " (detached)");
        }
        Tip("%s", tip.c_str());
    }
}

// One row of markers lined up with the frame slider: sounds blue, effects orange, held items green, other events grey.
// Hover lists the notifies under the mouse, a click jumps there (paused).
void App::DrawNotifyStrip(Document& d, float x0, float x1) {
    if (d.notifies.empty() || !d.anim) return;
    const auto& a = *d.anim;
    float fps = a.FrameRate(), last = (float)std::max(1, a.numFrames - 1);
    // the slider's value range (imgui: 2 px padding, half a grab at each end)
    float grab = ImGui::GetStyle().GrabMinSize, u0 = x0 + 2 + grab * 0.5f, u1 = x1 - 2 - grab * 0.5f;
    auto xOf = [&](float t) { return u0 + std::clamp(t * fps / last, 0.f, 1.f) * (u1 - u0); };
    float h = ImGui::GetFontSize() * 0.7f;
    ImVec2 p(x0, ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##nstrip", ImVec2(std::max(1.f, x1 - x0), h));
    bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(x1, p.y + h), IM_COL32(255, 255, 255, 14), 2);
    float mx = ImGui::GetIO().MousePos.x, best = 1e9f;
    std::string tip;
    const AnimNotifyInfo* nearest = nullptr;
    for (auto& n : d.notifies) {
        float x = xOf(n.time);
        ImU32 col = !n.sound.empty()                            ? IM_COL32(90, 170, 255, 255)
                    : !n.particle.empty()                       ? IM_COL32(255, 160, 60, 255)
                    : n.itemAction != AnimNotifyInfo::NoItem    ? IM_COL32(110, 220, 110, 255)
                                                                : IM_COL32(170, 170, 170, 200);
        if (n.duration > 0) dl->AddRectFilled(ImVec2(x, p.y + h * 0.35f), ImVec2(std::max(x + 2, xOf(n.time + n.duration)), p.y + h * 0.65f), (col & 0x00ffffff) | 0x60000000);
        dl->AddRectFilled(ImVec2(x - 1, p.y), ImVec2(x + 1, p.y + h), col);
        if (hovered && fabsf(x - mx) <= 4) {
            const std::string& what = !n.sound.empty() ? n.sound : !n.particle.empty() ? n.particle : !n.itemClass.empty() ? n.itemClass : n.cls;
            tip += Fmt("%.2f s (frame %.0f)  %s", n.time, n.time * fps, n.name.c_str());
            if (!what.empty() && what != n.name) tip += "  " + what.substr(what.rfind('.') + 1);
            tip += "\n";
        }
        if (fabsf(x - mx) < best) { best = fabsf(x - mx); nearest = &n; }
    }
    float cx = xOf(d.animTime);   // playhead
    dl->AddTriangleFilled(ImVec2(cx - 4, p.y), ImVec2(cx + 4, p.y), ImVec2(cx, p.y + h * 0.6f), IM_COL32(255, 255, 255, 200));
    if (hovered) Tip("%s", tip.empty() ? "Notify markers: blue sound, orange effect, green held item, grey event. Click one to jump to it." : tip.c_str());
    if (clicked && nearest && best <= 6) {
        d.animTime = nearest->time;
        d.animPlaying = false;
    }
}