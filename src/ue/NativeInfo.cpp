#include "ue/NativeInfo.h"
#include "core/Json.h"

void WriteNativeInfo(JsonWriter& w, const Package& pkg, size_t exportIdx, size_t propsEnd) {
    const auto& ex = pkg.Exports()[exportIdx];
    size_t end = (size_t)ex.serialOffset + ex.serialSize;
    if (propsEnd && end > propsEnd) w.KV("NativeDataBytes", (i64)(end - propsEnd));
}
