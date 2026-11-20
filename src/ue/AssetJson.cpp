#include "ue/AssetJson.h"
#include "ue/NativeInfo.h"
#include "core/Json.h"

std::string PackageToJson(const Package& pkg) {
    JsonWriter w;
    SetJsonPackage(&pkg);
    w.BeginArray();
    const auto& exports = pkg.Exports();
    for (size_t i = 0; i < exports.size(); i++) {
        const auto& ex = exports[i];
        w.BeginObject();
        w.KV("Type", pkg.ExportClass(i));
        w.KV("Name", ex.objectName);
        if (ex.outerIndex) {
            w.Key("Outer");
            w.Str(pkg.ObjectName(ex.outerIndex));
        }
        if (ex.classIndex < 0) w.KV("Class", pkg.FullPath(ex.classIndex));
        if (ex.superIndex) w.KV("Super", pkg.FullPath(ex.superIndex));
        std::string flags;
        if (ex.objectFlags & RF_Public) flags += "RF_Public | ";
        if (ex.objectFlags & RF_Standalone) flags += "RF_Standalone | ";
        if (ex.objectFlags & RF_ClassDefaultObject) flags += "RF_ClassDefaultObject | ";
        if (ex.objectFlags & RF_ArchetypeObject) flags += "RF_ArchetypeObject | ";
        if (ex.objectFlags & RF_Transactional) flags += "RF_Transactional | ";
        if (!flags.empty()) w.KV("Flags", flags.substr(0, flags.size() - 3));
        std::string err;
        size_t end = 0;
        PropertyList props = pkg.ReadProperties(i, &end, &err);
        w.Key("Properties");
        WriteProperties(w, props);
        if (!err.empty()) w.KV("PropertyError", err);
        WriteNativeInfo(w, pkg, i, end);
        w.EndObject();
    }
    w.EndArray();
    SetJsonPackage(nullptr);
    return w.Take();
}
