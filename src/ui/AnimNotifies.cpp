// AnimNotify playback in the animation view: Wwise event sounds (AnimNotify_WwiseSound) and particle effects
// (AnimNotify_PlayParticleEffect / AnimNotify_AthenaPlayParticleEffect) fired when the playing animation passes their time.
#include "ui/App.h"
#include "ui/UiCommon.h"
#include "app/WwiseEvents.h"
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

void App::FireNotifies(Document& d, float from, float to) {
    if (!d.playNotifies) return;
    static std::mt19937 rng{std::random_device{}()};
    for (auto& n : d.notifies) {
        if (n.time <= from || n.time > to) continue;
        std::lock_guard lk(m_notifyMutex);
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
}

void App::DrawNotifyToggle(Document& d) {
    size_t sounds = 0, effects = 0;
    for (auto& n : d.notifies) {
        sounds += !n.sound.empty();
        effects += !n.particle.empty();
    }
    if (!sounds && !effects) return;
    ImGui::SameLine();
    ImGui::Checkbox(Fmt("Notifies (%zu)", sounds + effects).c_str(), &d.playNotifies);
    if (!d.playNotifies) d.notifyFx.clear();
    if (ImGui::IsItemHovered()) {
        std::string tip = Fmt("Plays the animation's %zu sound and %zu particle notifies at their frames:\n", sounds, effects);
        int shown = 0;
        for (auto& n : d.notifies) {
            if (n.sound.empty() && n.particle.empty()) continue;
            if (++shown > 24) { tip += "...\n"; break; }
            const std::string& what = n.sound.empty() ? n.particle : n.sound;
            tip += Fmt("\n%6.2f s  %s", n.time, what.substr(what.rfind('.') + 1).c_str());
            if (!n.particle.empty() && !n.socket.empty()) tip += " @ " + n.socket + (n.attached ? "" : " (detached)");
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
}
