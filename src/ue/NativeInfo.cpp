#include "ue/NativeInfo.h"
#include "ue/Texture.h"
#include "ue/DataTable.h"
#include "core/Json.h"
#include "ue/GameConfig.h"
#include "ue/Properties.h"

void WriteNativeInfo(JsonWriter& w, const Package& pkg, size_t exportIdx, size_t propsEnd) {
    const auto& ex = pkg.Exports()[exportIdx];
    size_t end = (size_t)ex.serialOffset + ex.serialSize;
    std::string cls = pkg.ExportClass(exportIdx);
    if (IsTextureClass(cls)) {
        TextureInfo t;
        std::string err;
        w.Key("Texture");
        w.BeginObject();
        if (ReadTextureInfo(pkg, exportIdx, t, &err)) {
            w.KV("PixelFormat", t.pixelFormat);
            w.KV("SizeX", t.sizeX);
            w.KV("SizeY", t.sizeY);
            w.KV("NumSlices", t.numSlices);
            w.KV("SRGB", t.srgb);
            w.Key("Mips");
            w.BeginArray();
            for (auto& m : t.mips) {
                w.BeginObject();
                w.KV("SizeX", m.sizeX);
                w.KV("SizeY", m.sizeY);
                w.KV("Bytes", (i64)(m.bulk.Unused() ? 0 : m.bulk.sizeOnDisk));
                w.EndObject();
            }
            w.EndArray();
        } else w.KV("Error", err);
        w.EndObject();
        return;
    }
    if (IsTableClass(cls)) {   // DataTable / CurveTable rows, like FModel
        TableData t;
        std::string err;
        if (ReadTable(pkg, exportIdx, t, &err)) {
            WriteTableRows(w, t);
            return;
        }
        w.KV("RowsError", err);
    }
    if (cls == "PhysicalMaterial") {   // the surface's name from the game's config, effective physics values (UE 4.10 defaults when not set)
        size_t pe = 0;
        std::string err;
        PropertyList props = pkg.ReadProperties(exportIdx, &pe, &err);
        std::string type = PropStr(props, "SurfaceType");
        w.Key("Surface");
        w.BeginObject();
        w.KV("Type", type.empty() ? std::string("SurfaceType_Default") : type);
        w.KV("Name", GameConfig::PhysicalSurfaceName(type));
        w.KV("Friction", PropFloat(props, "Friction", 0.7));
        w.KV("Restitution", PropFloat(props, "Restitution", 0.3));
        w.KV("Density", PropFloat(props, "Density", 1.0));
        w.EndObject();
    }
    if (propsEnd && end > propsEnd) w.KV("NativeDataBytes", (i64)(end - propsEnd));
}
