#include "ue/GameConfig.h"
#include "core/Vfs.h"
#include <mutex>

namespace {
std::mutex g_mutex;
std::map<std::string, std::string> g_surfaces;

// +PhysicalSurfaces=(Type=SurfaceType10,Name="Foliage")
void ParseSurfaces(const std::string& ini) {
    size_t pos = 0;
    while ((pos = ini.find("PhysicalSurfaces=(", pos)) != std::string::npos) {
        size_t end = ini.find(')', pos);
        if (end == std::string::npos) break;
        std::string body = ini.substr(pos + 18, end - pos - 18);
        pos = end;
        auto field = [&](const char* key) {
            size_t k = body.find(key);
            if (k == std::string::npos) return std::string();
            k += strlen(key);
            if (k < body.size() && body[k] == '"') {
                size_t q = body.find('"', k + 1);
                return body.substr(k + 1, q == std::string::npos ? std::string::npos : q - k - 1);
            }
            size_t e = body.find(',', k);
            return body.substr(k, e == std::string::npos ? std::string::npos : e - k);
        };
        std::string type = field("Type="), name = field("Name=");
        if (!type.empty() && !name.empty()) g_surfaces[type] = name;
    }
}
}   // namespace

namespace GameConfig {

void Load(const Vfs& vfs) {
    std::lock_guard lk(g_mutex);
    g_surfaces.clear();
    for (const char* path : {"Engine/Content/Config/BaseEngine.ini", "Athena/Config/DefaultEngine.ini"}) {
        i64 e = vfs.Find(path);
        Bytes data;
        if (e < 0 || !vfs.Read((u32)e, data)) continue;
        ParseSurfaces(std::string((const char*)data.data(), data.size()));
    }
}

void Clear() {
    std::lock_guard lk(g_mutex);
    g_surfaces.clear();
}

const std::map<std::string, std::string>& PhysicalSurfaces() { return g_surfaces; }

std::string PhysicalSurfaceName(const std::string& type) {
    std::lock_guard lk(g_mutex);
    if (type.empty() || type == "SurfaceType_Default") return "Default";
    auto it = g_surfaces.find(type);
    return it != g_surfaces.end() ? it->second : "";
}

}   // namespace GameConfig
