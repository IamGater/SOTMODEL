// Loads the embedded reflection table (resource 103, see ui/Resources.rc). Line format, tab separated:
//   C  <name> <PrefixedName> <super name>
//   F  <owner> <function> <flags: s static, c const, u out-parameter positions unknown> <return type> <mode|type|name>...
//   P  <class> <member> <C++ type>          (object / interface / class / enum members only)
//   E  <enum> <value=Enumerator>...
#include "ue/SotReflection.h"
#include <windows.h>
#include <mutex>
#include <unordered_map>

namespace {

struct Db {
    struct Cls { std::string cpp, super; };
    std::unordered_map<std::string, Cls> classes;
    std::unordered_map<std::string, ReflFunction> functions;   // "owner\tname"
    std::unordered_map<std::string, ReflEnum> enums;
    std::unordered_map<std::string, std::string> fields;       // "class\tmember" -> type
    std::unordered_map<std::string, std::vector<std::string>> owners;   // function name -> classes declaring it
    bool loaded = false;
};

std::vector<std::string_view> SplitTabs(std::string_view s) {
    std::vector<std::string_view> out;
    size_t b = 0;
    for (;;) {
        size_t e = s.find('\t', b);
        out.push_back(s.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b));
        if (e == std::string_view::npos) break;
        b = e + 1;
    }
    return out;
}

const Db& Get() {
    static Db db;
    static std::once_flag once;
    std::call_once(once, [] {
        HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(103), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
        HGLOBAL rg = rs ? LoadResource(nullptr, rs) : nullptr;
        const char* data = rg ? (const char*)LockResource(rg) : nullptr;
        if (!data) return;
        std::string_view text(data, SizeofResource(nullptr, rs));
        size_t pos = 0;
        while (pos < text.size()) {
            size_t e = text.find('\n', pos);
            if (e == std::string_view::npos) e = text.size();
            std::string_view line = text.substr(pos, e - pos);
            pos = e + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.size() < 3 || line[1] != '\t') continue;
            auto f = SplitTabs(line);
            if (line[0] == 'C' && f.size() >= 4) {
                db.classes[std::string(f[1])] = {std::string(f[2]), std::string(f[3])};
            } else if (line[0] == 'F' && f.size() >= 5) {
                ReflFunction fn;
                fn.isStatic = f[3].find('s') != std::string_view::npos;
                fn.isConst = f[3].find('c') != std::string_view::npos;
                fn.orderUnknown = f[3].find('u') != std::string_view::npos;
                fn.ret = std::string(f[4]);
                for (size_t i = 5; i < f.size(); i++) {
                    std::string_view p = f[i];
                    size_t a = p.find('|'), b = p.rfind('|');
                    if (a == std::string_view::npos || a == b) continue;
                    fn.params.push_back({p[0], std::string(p.substr(a + 1, b - a - 1)), std::string(p.substr(b + 1))});
                }
                db.functions[std::string(f[1]) + "\t" + std::string(f[2])] = std::move(fn);
                db.owners[std::string(f[2])].push_back(std::string(f[1]));
            } else if (line[0] == 'P' && f.size() >= 4) {
                db.fields[std::string(f[1]) + "\t" + std::string(f[2])] = std::string(f[3]);
            } else if (line[0] == 'E' && f.size() >= 2) {
                ReflEnum en;
                for (size_t i = 2; i < f.size(); i++) {
                    size_t eq = f[i].find('=');
                    if (eq == std::string_view::npos) continue;
                    en.values.push_back({atoll(std::string(f[i].substr(0, eq)).c_str()), std::string(f[i].substr(eq + 1))});
                }
                db.enums[std::string(f[1])] = std::move(en);
            }
        }
        db.loaded = true;
    });
    return db;
}

}   // namespace

const std::string* ReflEnum::Name(long long v) const {
    for (auto& [k, n] : values)
        if (k == v) return &n;
    return nullptr;
}

namespace SotReflection {

const std::string* ClassCppName(std::string_view name) {
    auto& db = Get();
    auto it = db.classes.find(std::string(name));
    return it != db.classes.end() ? &it->second.cpp : nullptr;
}

std::string SuperClass(std::string_view name) {
    auto& db = Get();
    auto it = db.classes.find(std::string(name));
    return it != db.classes.end() ? it->second.super : std::string();
}

const ReflFunction* Function(std::string_view owner, std::string_view name) {
    auto& db = Get();
    auto it = db.functions.find(std::string(owner) + "\t" + std::string(name));
    return it != db.functions.end() ? &it->second : nullptr;
}

const ReflFunction* FunctionInHierarchy(std::string_view cls, std::string_view name, std::string* owner) {
    std::string c(cls);
    for (int guard = 0; guard < 64 && !c.empty(); guard++) {
        if (auto* f = Function(c, name)) {
            if (owner) *owner = c;
            return f;
        }
        c = SuperClass(c);
    }
    return nullptr;
}

const ReflFunction* UniqueFunction(std::string_view name, bool interfacesOnly, std::string* owner) {
    auto& db = Get();
    auto it = db.owners.find(std::string(name));
    if (it == db.owners.end()) return nullptr;
    const std::string* found = nullptr;
    for (auto& o : it->second) {
        if (interfacesOnly && (o.size() < 9 || o.compare(o.size() - 9, 9, "Interface") != 0)) continue;
        if (found) return nullptr;
        found = &o;
    }
    if (!found) return nullptr;
    if (owner) *owner = *found;
    return Function(*found, name);
}

const std::string* FieldInHierarchy(std::string_view cls, std::string_view member) {
    auto& db = Get();
    std::string c(cls);
    for (int guard = 0; guard < 64 && !c.empty(); guard++) {
        auto it = db.fields.find(c + "\t" + std::string(member));
        if (it != db.fields.end()) return &it->second;
        c = SuperClass(c);
    }
    return nullptr;
}

const ReflEnum* Enum(std::string_view name) {
    auto& db = Get();
    auto it = db.enums.find(std::string(name));
    return it != db.enums.end() ? &it->second : nullptr;
}

bool Loaded() { return Get().loaded; }

}   // namespace SotReflection
