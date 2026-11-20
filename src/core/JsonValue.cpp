#include "core/JsonValue.h"
#include <cmath>

const JsonValue* JsonValue::Get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (auto& [k, v] : obj) if (k == key) return &v;
    return nullptr;
}
std::string JsonValue::GetStr(std::string_view key, std::string_view def) const {
    auto v = Get(key);
    if (!v) return std::string(def);
    if (v->type == Type::String) return v->str;
    if (v->type == Type::Number) return Fmt("%g", v->num);
    if (v->type == Type::Bool) return v->b ? "true" : "false";
    return std::string(def);
}
double JsonValue::GetNum(std::string_view key, double def) const {
    auto v = Get(key);
    if (!v) return def;
    if (v->type == Type::Number) return v->num;
    if (v->type == Type::String && !v->str.empty()) return atof(v->str.c_str());
    return def;
}
bool JsonValue::GetBool(std::string_view key, bool def) const {
    auto v = Get(key);
    if (!v) return def;
    if (v->type == Type::Bool) return v->b;
    if (v->type == Type::String) return v->str == "true" || v->str == "1" || v->str == "yes";
    if (v->type == Type::Number) return v->num != 0;
    return def;
}

namespace {
struct Parser {
    std::string_view s;
    size_t p = 0;
    std::string err;

    void Ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++; }
    bool Fail(const char* m) { if (err.empty()) err = Fmt("%s at %zu", m, p); return false; }

    static void AppendUtf8(std::string& o, u32 cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    bool Hex4(u32& v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool String(std::string& o) {
        if (s[p] != '"') return Fail("expected string");
        p++;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { o += c; continue; }
            if (p >= s.size()) return Fail("bad escape");
            char e = s[p++];
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                u32 cp;
                if (!Hex4(cp)) return Fail("bad \\u");
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                    p += 2;
                    u32 lo;
                    if (!Hex4(lo)) return Fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(o, cp);
                break;
            }
            default: o += e;
            }
        }
        if (p >= s.size()) return Fail("unterminated string");
        p++;
        return true;
    }
    bool Value(JsonValue& v, int depth) {
        if (depth > 200) return Fail("too deep");
        Ws();
        if (p >= s.size()) return Fail("unexpected end");
        char c = s[p];
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            p++;
            Ws();
            if (p < s.size() && s[p] == '}') { p++; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (p >= s.size() || !String(k)) return Fail("expected key");
                Ws();
                if (p >= s.size() || s[p] != ':') return Fail("expected ':'");
                p++;
                JsonValue child;
                if (!Value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                Ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == '}') { p++; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = JsonValue::Type::Array;
            p++;
            Ws();
            if (p < s.size() && s[p] == ']') { p++; return true; }
            for (;;) {
                JsonValue child;
                if (!Value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == ']') { p++; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = JsonValue::Type::String; return String(v.str); }
        if (s.substr(p, 4) == "true") { v.type = JsonValue::Type::Bool; v.b = true; p += 4; return true; }
        if (s.substr(p, 5) == "false") { v.type = JsonValue::Type::Bool; v.b = false; p += 5; return true; }
        if (s.substr(p, 4) == "null") { v.type = JsonValue::Type::Null; p += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = p;
            p++;
            while (p < s.size() && (isdigit((u8)s[p]) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '+' || s[p] == '-')) p++;
            v.type = JsonValue::Type::Number;
            v.num = atof(std::string(s.substr(start, p - start)).c_str());
            return true;
        }
        return Fail("unexpected character");
    }
};

void DumpString(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) o += Fmt("\\u%04x", c);
        else o += (char)c;
    }
    o += '"';
}
void DumpValue(std::string& o, const JsonValue& v) {
    switch (v.type) {
    case JsonValue::Type::Null: o += "null"; break;
    case JsonValue::Type::Bool: o += v.b ? "true" : "false"; break;
    case JsonValue::Type::Number:
        if (v.num == std::floor(v.num) && std::fabs(v.num) < 1e15) o += std::to_string((i64)v.num);
        else o += Fmt("%.17g", v.num);
        break;
    case JsonValue::Type::String: DumpString(o, v.str); break;
    case JsonValue::Type::Array:
        o += '[';
        for (size_t i = 0; i < v.arr.size(); i++) { if (i) o += ','; DumpValue(o, v.arr[i]); }
        o += ']';
        break;
    case JsonValue::Type::Object:
        o += '{';
        for (size_t i = 0; i < v.obj.size(); i++) {
            if (i) o += ',';
            DumpString(o, v.obj[i].first);
            o += ':';
            DumpValue(o, v.obj[i].second);
        }
        o += '}';
        break;
    }
}
}

bool JsonValue::Parse(std::string_view text, JsonValue& out, std::string* error) {
    Parser ps{text};
    out = JsonValue();
    bool ok = ps.Value(out, 0);
    if (ok) {
        ps.Ws();
        if (ps.p != text.size()) ok = ps.Fail("trailing data");
    }
    if (!ok && error) *error = ps.err;
    return ok;
}

std::string JsonValue::Dump() const {
    std::string o;
    DumpValue(o, *this);
    return o;
}
