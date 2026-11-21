#include "ue/Package.h"
#include "core/Vfs.h"

static constexpr u32 PackageTag = 0x9E2A83C1;

bool Package::Load(Bytes data, std::string gamePath, std::string* error) {
    m_data = std::move(data);
    m_gamePath = std::move(gamePath);
    {
        std::string p = m_gamePath;
        size_t dot = p.rfind('.');
        if (dot != std::string::npos && p.find('/', dot) == std::string::npos) p.resize(dot);
        if (IStartsWith(p, "Athena/Content/")) m_packageName = "/Game/" + p.substr(15);
        else if (IStartsWith(p, "Engine/Content/")) m_packageName = "/Engine/" + p.substr(15);
        else m_packageName = "/" + p;
    }
    try {
        Reader r(m_data);
        auto& s = m_summary;
        if (r.U32() != PackageTag) throw ParseError("not a package (bad tag)");
        s.legacyVersion = r.I32();
        if (s.legacyVersion >= 0 || s.legacyVersion < -7) throw ParseError(Fmt("unsupported legacy version %d", s.legacyVersion));
        if (s.legacyVersion != -4) s.ue3Version = r.I32();
        s.ue4Version = r.I32();
        s.licenseeVersion = r.I32();
        if (s.legacyVersion <= -2) {
            i32 n = r.Count(20);
            r.Skip((i64)n * 20);   // custom versions (enum-less format: guid + version)
        }
        s.totalHeaderSize = r.I32();
        r.FString();   // folder name
        s.packageFlags = r.U32();
        s.nameCount = r.I32();
        s.nameOffset = r.I32();
        if (s.ue4Version >= 459) { r.I32(); r.I32(); }   // gatherable text data
        s.exportCount = r.I32();
        s.exportOffset = r.I32();
        s.importCount = r.I32();
        s.importOffset = r.I32();
        s.dependsOffset = r.I32();
        if (s.ue4Version >= 384) { r.I32(); r.I32(); }   // string asset references
        r.I32();                                          // thumbnail table
        s.guid = r.Read<FGuid>();
        i32 gens = r.Count(8);
        r.Skip((i64)gens * 8);
        if (s.ue4Version >= 336) { r.Skip(10); r.FString(); }   // saved-by engine version
        else r.I32();
        if (s.ue4Version >= 444) { r.Skip(10); r.FString(); }   // compatible engine version
        r.U32();                                                 // compression flags
        i32 chunks = r.Count(16);
        if (chunks) throw ParseError("compressed packages are not supported");
        r.U32();                                                 // package source
        i32 extra = r.Count(4);
        for (i32 i = 0; i < extra; i++) r.FString();             // additional packages to cook
        if (s.legacyVersion > -7) { i32 ta = r.Count(); if (ta) throw ParseError("texture allocations present"); }
        r.I32();                                                 // asset registry data offset
        if (s.licenseeVersion > 0) r.Skip(6);                    // Athena: 6 reserved licensee bytes
        s.bulkDataStartOffset = r.I64();
        if (s.ue4Version >= 224) s.worldTileInfoOffset = r.I32();

        if (s.nameCount < 0 || s.exportCount < 0 || s.importCount < 0) throw ParseError("negative table counts");
        Reader nr(m_data, s.nameOffset);
        m_names.resize(s.nameCount);
        for (auto& n : m_names) n = nr.FString();

        Reader ir(m_data, s.importOffset);
        m_imports.resize(s.importCount);
        for (auto& im : m_imports) {
            im.classPackage = ReadName(ir);
            im.className = ReadName(ir);
            im.outerIndex = ir.I32();
            im.objectName = ReadName(ir);
        }

        Reader er(m_data, s.exportOffset);
        m_exports.resize(s.exportCount);
        for (auto& ex : m_exports) {
            ex.classIndex = er.I32();
            ex.superIndex = er.I32();
            ex.outerIndex = er.I32();
            ex.objectName = ReadName(er);
            ex.objectFlags = er.U32();
            ex.serialSize = er.I32();
            ex.serialOffset = er.I32();
            er.Skip(12);   // forced export, not for client, not for server
            er.Skip(16);   // package guid
            er.U32();      // package flags
            if (s.ue4Version >= 365) er.U32();   // not for editor game
            if (ex.serialOffset < 0 || ex.serialSize < 0 || (size_t)ex.serialOffset + ex.serialSize > m_data.size()) {
                LOGW("%s: export %s has out-of-range serial data", m_gamePath.c_str(), ex.objectName.c_str());
                ex.serialSize = 0;
            }
        }
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

std::string Package::ShortName() const {
    size_t s = m_packageName.rfind('/');
    return s == std::string::npos ? m_packageName : m_packageName.substr(s + 1);
}

std::string Package::Name(i32 index, i32 number) const {
    if (index < 0 || index >= (i32)m_names.size()) return Fmt("<bad name %d>", index);
    if (number == 0) return m_names[index];
    return m_names[index] + "_" + std::to_string(number - 1);
}

std::string Package::ReadName(Reader& r) const {
    i32 i = r.I32();
    i32 n = r.I32();
    if (i < 0 || i >= (i32)m_names.size()) throw ParseError(Fmt("bad FName index %d at %zu", i, r.Pos() - 8));
    return Name(i, n);
}

std::string Package::ObjectName(i32 index) const {
    if (index > 0 && index <= (i32)m_exports.size()) return m_exports[index - 1].objectName;
    if (index < 0 && -index <= (i32)m_imports.size()) return m_imports[-index - 1].objectName;
    return "None";
}

std::string Package::ClassName(i32 index) const {
    if (index > 0 && index <= (i32)m_exports.size()) {
        i32 c = m_exports[index - 1].classIndex;
        if (c == 0) return "Class";
        return ObjectName(c);
    }
    if (index < 0 && -index <= (i32)m_imports.size()) return m_imports[-index - 1].className;
    return "None";
}

std::string Package::ImportPackage(i32 index) const {
    i32 cur = index;
    for (int guard = 0; guard < 64 && cur < 0 && -cur <= (i32)m_imports.size(); guard++) {
        const auto& im = m_imports[-cur - 1];
        if (im.outerIndex == 0) return im.objectName;
        cur = im.outerIndex;
    }
    return {};
}

std::string Package::FullPath(i32 index) const {
    if (index == 0) return "None";
    std::vector<std::string> chain;
    i32 cur = index;
    bool sawPackage = false;
    for (int guard = 0; guard < 64 && cur != 0; guard++) {
        if (cur > 0) {
            if (cur > (i32)m_exports.size()) break;
            chain.push_back(m_exports[cur - 1].objectName);
            cur = m_exports[cur - 1].outerIndex;
            if (cur == 0) { chain.push_back(m_packageName); sawPackage = true; }
        } else {
            if (-cur > (i32)m_imports.size()) break;
            const auto& im = m_imports[-cur - 1];
            chain.push_back(im.objectName);
            if (im.outerIndex == 0) sawPackage = true;
            cur = im.outerIndex;
        }
    }
    if (chain.empty()) return "None";
    std::string s = chain.back();
    for (size_t i = chain.size() - 1; i-- > 0;) {
        // first separator after the package is '.', deeper levels use ':'
        s += (i == chain.size() - 2 && sawPackage) ? "." : ":";
        s += chain[i];
    }
    return s;
}

i32 Package::FindExport(std::string_view name, std::string_view className) const {
    for (size_t i = 0; i < m_exports.size(); i++) {
        if (!IEquals(m_exports[i].objectName, name)) continue;
        if (!className.empty() && !IEquals(ExportClass(i), className)) continue;
        return (i32)i + 1;
    }
    return 0;
}

i32 Package::MainExport() const {
    std::string sn = ShortName();
    for (size_t i = 0; i < m_exports.size(); i++)
        if (m_exports[i].outerIndex == 0 && IEquals(m_exports[i].objectName, sn)) return (i32)i + 1;
    for (size_t i = 0; i < m_exports.size(); i++)
        if (m_exports[i].outerIndex == 0 && !(m_exports[i].objectFlags & RF_ClassDefaultObject)) return (i32)i + 1;
    return m_exports.empty() ? 0 : 1;
}

Reader Package::ExportReader(size_t exportIdx) const {
    const auto& ex = m_exports.at(exportIdx);
    return Reader(m_data.data(), (size_t)ex.serialOffset + ex.serialSize, ex.serialOffset);
}

PropertyList Package::ReadProperties(size_t exportIdx, size_t* end, std::string* error) const {
    Reader r = ExportReader(exportIdx);
    PropertyList props = ReadProperties(r, r.Size(), error);
    if (end) *end = r.Pos();
    return props;
}

PropertyList Package::ReadProperties(Reader& r, size_t limit, std::string* error) const {
    PropertyList out;
    try {
        ReadTaggedProperties(*this, r, limit, out);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
    }
    return out;
}

// ---------------------------------------------------------------- cache
std::shared_ptr<Package> PackageCache::Load(u32 entry, std::string* error) {
    {
        std::lock_guard lk(m_mutex);
        auto it = m_items.find(entry);
        if (it != m_items.end()) {
            it->second.tick = ++m_tick;
            return it->second.pkg;
        }
    }
    Bytes data;
    if (!m_vfs.Read(entry, data, error)) return nullptr;
    auto pkg = std::make_shared<Package>();
    size_t bytes = data.size();
    if (!pkg->Load(std::move(data), std::string(m_vfs.Path(entry)), error)) return nullptr;
    std::lock_guard lk(m_mutex);
    m_items[entry] = {pkg, ++m_tick, bytes};
    m_used += bytes;
    while (m_used > m_budget && m_items.size() > 1) {
        auto victim = m_items.end();
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
            if (it->first != entry && (victim == m_items.end() || it->second.tick < victim->second.tick)) victim = it;
        if (victim == m_items.end()) break;
        m_used -= victim->second.bytes;
        m_items.erase(victim);
    }
    return pkg;
}

std::shared_ptr<Package> PackageCache::LoadPath(std::string_view path, std::string* error) {
    i64 e = m_vfs.FindPackage(path);
    if (e < 0) {
        if (error) *error = "package not found: " + std::string(path);
        return nullptr;
    }
    return Load((u32)e, error);
}

void PackageCache::Clear() {
    std::lock_guard lk(m_mutex);
    m_items.clear();
    m_used = 0;
}

bool Package::ReadWorldTileInfo(WorldTileInfo& t) const {
    t = WorldTileInfo{};
    i32 off = m_summary.worldTileInfoOffset;
    if (off <= 0 || (size_t)off >= m_data.size()) return false;
    try {
        Reader r(m_data, off);
        t.x = r.I32();
        t.y = r.I32();
        for (float& f : t.boundsMin) f = r.F32();
        for (float& f : t.boundsMax) f = r.F32();
        t.boundsValid = r.U8() != 0;
        t.layer = r.FString();
        r.I32();            // Reserved0
        r.Skip(8);          // Reserved1
        t.streamingDistance = r.I32();
        t.distanceStreaming = r.Bool32();
        t.hidden = r.Bool32();
        t.parent = r.FString();
        i32 n = r.Count(20);
        for (i32 i = 0; i < n; i++) { t.lodDistances.push_back((float)r.I32()); r.Skip(16); }
        t.zOrder = r.I32();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
