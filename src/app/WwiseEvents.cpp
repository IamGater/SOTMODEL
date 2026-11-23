#include "app/WwiseEvents.h"
#include "ue/Properties.h"
#include "ue/GameConfig.h"
#include "ue/SotReflection.h"
#include <mutex>
#include <unordered_map>

namespace {

std::string_view StemOfPath(std::string_view p) {
    size_t s = p.find_last_of('/');
    if (s != std::string_view::npos) p = p.substr(s + 1);
    size_t d = p.find('.');
    return d == std::string_view::npos ? p : p.substr(0, d);
}

bool IsEventAsset(std::string_view p) {
    return IEndsWith(p, ".uasset") && ToLower(p).find("/wwiseaudio/uassets/events/") != std::string::npos;
}

struct NameIndex {
    std::mutex mutex;
    size_t files = SIZE_MAX;   // vfs file count the index was built for
    std::unordered_map<u32, std::pair<std::string, u32>> byId;   // id -> (name, vfs entry)
    std::unordered_map<u32, std::string> words;   // guessed switch / state names
};
NameIndex g_names;

void EnsureNames(Api& api) {   // caller holds g_names.mutex
    if (g_names.files == api.vfs.FileCount()) return;
    g_names.byId.clear();
    for (u32 i = 0; i < (u32)api.vfs.FileCount(); i++) {
        std::string_view p = api.vfs.Path(i);
        if (!IsEventAsset(p)) continue;
        std::string n(StemOfPath(p));
        g_names.byId[WwiseShortId(n)] = {n, i};
    }
    // switch / state names: every run of up to 4 words of the event and bank names ("play_sfx_footstep_wood_wet" -> "wood_wet")
    g_names.words.clear();
    auto addRuns = [&](std::string_view name) {
        std::vector<std::string_view> w;
        for (size_t s = 0; s <= name.size();) {
            size_t e = name.find('_', s);
            if (e == std::string_view::npos) e = name.size();
            if (e > s) w.push_back(name.substr(s, e - s));
            s = e + 1;
        }
        for (size_t a = 0; a < w.size(); a++)
            for (size_t b = a; b < w.size() && b < a + 4; b++) {
                std::string run(w[a].data(), w[b].data() + w[b].size() - w[a].data());
                g_names.words.try_emplace(WwiseShortId(run), run);
            }
    };
    for (auto& [id, v] : g_names.byId) addRuns(v.first);
    for (u32 i = 0; i < (u32)api.vfs.FileCount(); i++) {
        std::string_view p = api.vfs.Path(i);
        if (IEndsWith(p, ".uasset") && ToLower(p).find("/wwiseaudio/uassets/banks/") != std::string::npos) addRuns(StemOfPath(p));
    }
    for (const char* w : {"none", "default", "on", "off", "true", "false", "yes", "no", "enabled", "disabled", "inside", "outside", "indoor",
                          "outdoor", "land", "sea", "ship", "island", "underwater", "abovewater", "first_person", "third_person", "local",
                          "remote", "player", "npc", "surface", "material", "surface_type", "footstep_surface", "physical_material",
                          "is_attacking", "is_not_attacking", "small", "medium", "large", "light", "heavy", "male", "female"})
        g_names.words.try_emplace(WwiseShortId(w), w);
    g_names.files = api.vfs.FileCount();
}

}   // namespace

std::string WwiseEventName(Api& api, u32 id) {
    std::lock_guard lk(g_names.mutex);
    EnsureNames(api);
    auto it = g_names.byId.find(id);
    return it == g_names.byId.end() ? std::string() : it->second.first;
}

// More names, read once on the first guess: the name tables of the audio packages (audio Blueprints and DataTables set switches
// by FName: SurfaceSwitch "WoodDeck", ...).
std::once_flag g_audioNamesOnce;
std::unordered_map<u32, std::string> g_audioNames;

void LoadAudioNames(Api& api) {
    // physical surface names (the "surface" switch values) and every native identifier of the game (enum values such as
    // EFootwearType::Pegleg, members, parameters: what native code passes to SetSwitch / SetRTPCValue is often one of them)
    for (auto& [type, name] : GameConfig::PhysicalSurfaces()) g_audioNames.try_emplace(WwiseShortId(name), name);
    SotReflection::ForEachName([](std::string_view n) {
        if (!n.empty() && n.size() < 64) g_audioNames.try_emplace(WwiseShortId(n), std::string(n));
    });
    std::vector<u32> entries;
    for (u32 i = 0; i < (u32)api.vfs.FileCount() && entries.size() < 600; i++) {
        std::string_view p = api.vfs.Path(i);
        if (!IEndsWith(p, ".uasset")) continue;
        std::string l = ToLower(p);
        if (l.find("/wwiseaudio/") != std::string::npos) continue;
        if (l.find("/audio/") != std::string::npos || (l.find("/datatables/") != std::string::npos && l.find("audio") != std::string::npos))
            entries.push_back(i);
    }
    for (u32 e : entries) {
        auto pkg = api.cache->Load(e);
        if (!pkg) continue;
        for (auto& n : pkg->Names())
            if (!n.empty() && n.size() < 64 && n.find('/') == std::string::npos) g_audioNames.try_emplace(WwiseShortId(n), n);
    }
}

std::string WwiseGuessName(Api& api, u32 id) {
    std::call_once(g_audioNamesOnce, [&] { LoadAudioNames(api); });   // these keep the case ("WoodDeck")
    auto a = g_audioNames.find(id);
    if (a != g_audioNames.end()) return a->second;
    std::lock_guard lk(g_names.mutex);
    EnsureNames(api);
    auto it = g_names.words.find(id);
    return it == g_names.words.end() ? std::string() : it->second;
}

std::string WwiseNameOrId(Api& api, u32 id) {
    std::string n = WwiseGuessName(api, id);
    return n.empty() ? Fmt("0x%08X", id) : n;
}

bool WwiseEventInfo::Plays(size_t i, const std::unordered_map<u32, u32>& choice) const {
    if (i >= media.size()) return false;
    for (auto& c : media[i].conditions) {
        auto it = choice.find(c.group);
        if (it != choice.end() && std::find(c.values.begin(), c.values.end(), it->second) == c.values.end()) return false;
    }
    return true;
}

bool ResolveEventInBank(Api& api, std::shared_ptr<Package> bank, const BankInfo& info, u32 eventId, WwiseEventInfo& out, std::string* error) {
    out.id = eventId;
    out.bank = bank;
    out.bankInfo = info;
    out.bankPath = std::string(bank->GamePath());
    if (out.name.empty()) out.name = WwiseEventName(api, eventId);
    WwiseEventTree tree;
    if (!ResolveWwiseEvent(bank->Data().data(), info, eventId, tree, error)) return false;
    out.media.clear();
    out.switches = tree.switches;
    for (size_t mi = 0; mi < tree.media.size(); mi++) {
        u32 id = tree.media[mi];
        WwiseEventMedia m;
        m.id = id;
        m.conditions = tree.conditions[mi];
        for (auto& w : info.wems)
            if (w.id == id) {
                m.where = WwiseEventMedia::Bank;
                m.offset = w.offset;
                m.size = w.size;
                break;
            }
        if (m.where == WwiseEventMedia::Missing) {
            i64 e = api.vfs.Find("Athena/Content/WwiseAudio/Streams/" + std::to_string(id) + ".wem");
            if (e >= 0) {
                m.where = WwiseEventMedia::Stream;
                m.entry = (u32)e;
            }
        }
        out.media.push_back(m);
    }
    return true;
}

bool LoadWwiseEvent(Api& api, const std::string& pathOrName, WwiseEventInfo& out, std::string* error) {
    out = WwiseEventInfo{};
    i64 entry = api.ResolveFile(pathOrName);
    if (entry < 0) {   // an event name: find its asset
        std::lock_guard lk(g_names.mutex);
        EnsureNames(api);
        auto it = g_names.byId.find(WwiseShortId(StemOfPath(pathOrName)));
        if (it != g_names.byId.end()) entry = it->second.second;
    }
    if (entry < 0) {
        if (error) *error = "no WwiseEvent asset named " + pathOrName;
        return false;
    }
    std::string err;
    auto pkg = api.cache->Load((u32)entry, &err);
    i32 main = pkg ? pkg->MainExport() : 0;
    if (!main || pkg->ExportClass(main - 1) != "WwiseEvent") {
        if (error) *error = pkg ? std::string(api.vfs.Path((u32)entry)) + " is not a WwiseEvent" : err;
        return false;
    }
    PropertyList props = pkg->ReadProperties(main - 1);
    out.name = pkg->ObjectName(main);
    const PropValue* idv = FindValue(props, "WwiseId");
    u32 id = idv ? (u32)idv->AsInt() : WwiseShortId(out.name);
    const PropValue* bv = FindValue(props, "RequiredBank");
    std::string bankPath = bv && bv->ObjIndex() < 0 ? pkg->ImportPackage(bv->ObjIndex()) : std::string();
    if (bankPath.empty()) {
        if (error) *error = out.name + " has no RequiredBank";
        return false;
    }
    auto bank = api.cache->LoadPath(bankPath, &err);
    BankInfo info;
    if (!bank || !ParseBank(bank->Data().data(), bank->Data().size(), info, nullptr, &err)) {
        if (error) *error = "bank " + bankPath + ": " + err;
        return false;
    }
    return ResolveEventInBank(api, bank, info, id, out, error);
}

bool ReadEventMedia(Api& api, const WwiseEventInfo& ev, const WwiseEventMedia& m, Bytes& out, std::string* error) {
    if (m.where == WwiseEventMedia::Bank && ev.bank) {
        const Bytes& d = ev.bank->Data();
        if ((size_t)m.offset + m.size > d.size()) return false;
        out.assign(d.begin() + m.offset, d.begin() + m.offset + m.size);
        return true;
    }
    if (m.where == WwiseEventMedia::Stream) return api.vfs.Read(m.entry, out, error);
    if (error) *error = Fmt("media %u is in another bank", m.id);
    return false;
}
