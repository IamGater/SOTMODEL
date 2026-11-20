#pragma once
#include "core/Common.h"
#include <map>

// Minimal JSON DOM for parsing requests (MCP / CLI arguments).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    bool IsNull() const { return type == Type::Null; }
    bool IsObject() const { return type == Type::Object; }
    const JsonValue* Get(std::string_view key) const;
    std::string GetStr(std::string_view key, std::string_view def = {}) const;
    double GetNum(std::string_view key, double def = 0) const;
    bool GetBool(std::string_view key, bool def = false) const;

    static bool Parse(std::string_view text, JsonValue& out, std::string* error = nullptr);
    std::string Dump() const;   // compact
};
