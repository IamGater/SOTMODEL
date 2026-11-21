#include "app/WwiseEvents.h"
#include "ue/Properties.h"
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
    g_names.files = api.vfs.FileCount();
}

}   // namespace

std::string WwiseEventName(Api& api, u32 id) {
    std::lock_guard lk(g_names.mutex);
    EnsureNames(api);
    auto it = g_names.byId.find(id);
    return it == g_names.byId.end() ? std::string() : it->second.first;
}

bool ResolveEventInBank(Api& api, std::shared_ptr<Package> bank, const BankInfo& info, u32 eventId, WwiseEventInfo& out, std::string* error) {
    out.id = eventId;
    out.bank = bank;
    out.bankInfo = info;
    out.bankPath = std::string(bank->GamePath());
    if (out.name.empty()) out.name = WwiseEventName(api, eventId);
    std::vector<u32> ids;
    if (!ResolveWwiseEvent(bank->Data().data(), info, eventId, ids, error)) return false;
    out.media.clear();
    for (u32 id : ids) {
        WwiseEventMedia m;
        m.id = id;
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
