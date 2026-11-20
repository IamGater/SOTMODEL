#pragma once
#include "core/Common.h"

// Small streaming JSON writer (pretty printed, 2-space indent like FModel).
class JsonWriter {
public:
    void BeginObject() { Value(); m_out += '{'; Push(false); }
    void EndObject() { Pop('}'); }
    void BeginArray() { Value(); m_out += '['; Push(true); }
    void EndArray() { Pop(']'); }
    void Key(std::string_view k) {
        Comma();
        Newline();
        String(k);
        m_out += ": ";
        m_afterKey = true;
    }
    void Str(std::string_view s) { Value(); String(s); }
    void Num(double d);
    void Int(i64 v) { Value(); m_out += std::to_string(v); }
    void UInt(u64 v) { Value(); m_out += std::to_string(v); }
    void Bool(bool b) { Value(); m_out += b ? "true" : "false"; }
    void Null() { Value(); m_out += "null"; }

    template <typename F> void Field(std::string_view k, F&& f) { Key(k); f(); }
    void KV(std::string_view k, std::string_view v) { Key(k); Str(v); }
    void KV(std::string_view k, const char* v) { Key(k); Str(v); }
    void KV(std::string_view k, double v) { Key(k); Num(v); }
    void KV(std::string_view k, i64 v) { Key(k); Int(v); }
    void KV(std::string_view k, int v) { Key(k); Int(v); }
    void KV(std::string_view k, bool v) { Key(k); Bool(v); }

    const std::string& Result() const { return m_out; }
    std::string Take() { return std::move(m_out); }

private:
    struct Level { bool array; int count; };
    void Push(bool array) { m_stack.push_back({array, 0}); }
    void Pop(char c) {
        bool had = !m_stack.empty() && m_stack.back().count > 0;
        m_stack.pop_back();
        if (had) Newline();
        m_out += c;
    }
    void Comma() {
        if (!m_stack.empty()) {
            if (m_stack.back().count++) m_out += ',';
        }
    }
    void Newline() {
        m_out += '\n';
        m_out.append(m_stack.size() * 2, ' ');
    }
    void Value() {
        if (m_afterKey) { m_afterKey = false; return; }
        if (!m_stack.empty()) { Comma(); Newline(); }
    }
    void String(std::string_view s);

    std::string m_out;
    std::vector<Level> m_stack;
    bool m_afterKey = false;
};
