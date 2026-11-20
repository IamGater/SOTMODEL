#include "core/Json.h"
#include <cmath>

void JsonWriter::Num(double d) {
    Value();
    if (!std::isfinite(d)) { m_out += std::isnan(d) ? "\"NaN\"" : (d > 0 ? "\"Infinity\"" : "\"-Infinity\""); return; }
    if (d == std::floor(d) && std::fabs(d) < 1e15) { m_out += std::to_string((i64)d); return; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9g", d);
    m_out += buf;
}

void JsonWriter::String(std::string_view s) {
    m_out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': m_out += "\\\""; break;
        case '\\': m_out += "\\\\"; break;
        case '\n': m_out += "\\n"; break;
        case '\r': m_out += "\\r"; break;
        case '\t': m_out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); m_out += b; }
            else m_out += (char)c;
        }
    }
    m_out += '"';
}
