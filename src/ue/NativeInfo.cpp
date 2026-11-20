#include "ue/NativeInfo.h"
#include "ue/Texture.h"
#include "core/Json.h"

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
    if (propsEnd && end > propsEnd) w.KV("NativeDataBytes", (i64)(end - propsEnd));
}
