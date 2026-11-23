// Blueprint decompiler (UE4.10 / object version 483).
// Layouts after an export's tagged properties and object guid:
//   UField     Next (index)
//   UProperty  ArrayDim (i32), PropertyFlags (u64), RepNotifyFunc (FName), then per type: class / struct / inner refs
//   UStruct    SuperStruct (index), Children (index), BytecodeBufferSize (i32), SerializedScriptSize (i32), script
//   UFunction  FunctionFlags (u32), [RepOffset (u16) when FUNC_Net], EventGraphFunction (index), EventGraphCallOffset (i32)
//   UEnum      Names (TArray<{FName, uint8}>), CppForm (u8); UserDefinedEnum keeps DisplayNames as a property
// Script: object references are 4 bytes on disk but 8 in memory, FNames 8 on disk and 12 in memory; jump offsets are
// in-memory offsets, so the reader keeps both positions.
//
// Compiled Blueprint flow (what the structuring relies on): a function starts with PushExecutionFlow(<final return>);
// every chain of nodes ends with PopExecutionFlow, which resumes the innermost pushed address. A Sequence node pushes
// its later outputs (last first) and runs the first one; loop macros jump back to their condition. The event graph
// (ubergraph) is one function entered at an offset by each event's stub, after the stub copied the event parameters
// into the persistent frame; latent actions (Delay, ...) resume it at the offset in their FLatentActionInfo.
#include "ue/Blueprint.h"
#include "ue/SotReflection.h"
#include <map>
#include <memory>
#include <set>
#include <functional>

namespace {

enum : u64 { CPF_ConstParm = 0x2, CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400, CPF_ReferenceParm = 0x8000000ull };
enum : u32 {
    FUNC_Final = 0x1, FUNC_Net = 0x40, FUNC_UbergraphFunction = 0x400, FUNC_Event = 0x800, FUNC_Static = 0x2000, FUNC_MulticastDelegate = 0x10000,
    FUNC_Public = 0x20000, FUNC_Private = 0x40000, FUNC_Protected = 0x80000, FUNC_Delegate = 0x100000, FUNC_BlueprintCallable = 0x4000000,
    FUNC_BlueprintEvent = 0x8000000, FUNC_BlueprintPure = 0x10000000, FUNC_Const = 0x40000000
};

struct PropInfo {
    std::string name, type;
    u64 flags = 0;
    i32 arrayDim = 1;
    std::string enumName, enumPath;   // byte properties with an enum
};

struct FuncInfo {
    i32 index = 0;   // export, 1-based
    std::string name;
    u32 flags = 0;
    Bytes script;
    i32 eventGraph = 0, eventGraphOffset = 0;
    std::vector<PropInfo> params, locals;
};

bool IsUbergraph(const FuncInfo& f) { return (f.flags & FUNC_UbergraphFunction) || IStartsWith(f.name, "ExecuteUbergraph"); }

// Blueprint names may hold spaces and other characters C++ identifiers can't
std::string SafeIdent(const std::string& n) {
    std::string o;
    for (char c : n) o += (isalnum((u8)c) || c == '_') ? c : '_';
    if (o.empty() || isdigit((u8)o[0])) o = "_" + o;
    return o;
}

bool IsIdentChar(char c) { return isalnum((u8)c) || c == '_'; }

// occurrences of identifier `id` in `s` (whole words)
size_t CountIdent(const std::string& s, const std::string& id) {
    size_t n = 0;
    for (size_t k = 0; (k = s.find(id, k)) != std::string::npos; k += id.size())
        if ((k == 0 || !IsIdentChar(s[k - 1])) && (k + id.size() >= s.size() || !IsIdentChar(s[k + id.size()]))) n++;
    return n;
}
bool ReplaceIdent(std::string& s, const std::string& id, const std::string& with, bool all = false) {
    bool any = false;
    for (size_t k = 0; (k = s.find(id, k)) != std::string::npos;) {
        if ((k == 0 || !IsIdentChar(s[k - 1])) && (k + id.size() >= s.size() || !IsIdentChar(s[k + id.size()]))) {
            s.replace(k, id.size(), with);
            k += with.size();
            any = true;
            if (!all) break;
        } else {
            k += id.size();
        }
    }
    return any;
}

// "(...)" whose outer parentheses enclose the whole string
bool FullyWrapped(const std::string& s) {
    if (s.size() < 2 || s[0] != '(' || s.back() != ')') return false;
    int d = 0;
    bool str = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\')) str = !str;
        if (str) continue;
        if (c == '(') d++;
        else if (c == ')' && --d == 0 && i + 1 < s.size()) return false;
    }
    return true;
}
std::string StripParens(std::string s) {
    for (;;) {
        if (FullyWrapped(s)) { s = s.substr(1, s.size() - 2); continue; }
        // an object / interface tested as a condition: "(bool)(x)" is just "x"
        if (IStartsWith(s, "(bool)") && FullyWrapped(s.substr(6))) { s = s.substr(6); continue; }
        if (IStartsWith(s, "!(bool)") && FullyWrapped(s.substr(7))) { s = "!" + s.substr(7); continue; }
        return s;
    }
}
bool SimpleExpr(const std::string& s) {   // identifier, member access or call chain without operators
    int d = 0;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '<') d++;
        else if (c == ')' || c == ']' || c == '>') d--;
        else if (d == 0 && (c == ' ' || c == '!' || c == '?')) return false;
    }
    return !s.empty() && s[0] != '(';
}
std::string Wrap(const std::string& s) { return SimpleExpr(s) || FullyWrapped(s) ? s : "(" + s + ")"; }
std::string Neg(const std::string& c0) {
    std::string c = StripParens(c0);
    if (c.size() > 1 && c[0] == '!') {   // !x -> x
        std::string rest = c.substr(1);
        if (FullyWrapped(rest) || SimpleExpr(rest)) return StripParens(rest);
    }
    // a == b <-> a != b: exact for every type (ordering operators are not flipped: NaN)
    {
        int d = 0, ops = 0;
        size_t at = 0;
        bool str = false;
        for (size_t i = 0; i + 1 < c.size(); i++) {
            char ch = c[i];
            if (ch == '"' && (i == 0 || c[i - 1] != '\\')) str = !str;
            if (str) continue;
            if (ch == '(' || ch == '[') d++;
            else if (ch == ')' || ch == ']') d--;
            else if (d == 0 && (c.compare(i, 4, " == ") == 0 || c.compare(i, 4, " != ") == 0)) { ops++; at = i; }
            else if (d == 0 && (c.compare(i, 4, " && ") == 0 || c.compare(i, 4, " || ") == 0 || c.compare(i, 3, " < ") == 0 ||
                                c.compare(i, 3, " > ") == 0 || c.compare(i, 4, " <= ") == 0 || c.compare(i, 4, " >= ") == 0 || c[i] == '?')) ops += 2;
        }
        if (ops == 1) return "(" + c.substr(0, at) + (c[at + 1] == '=' ? " != " : " == ") + c.substr(at + 4) + ")";
    }
    if (SimpleExpr(c)) return "!" + c;
    return "!(" + c + ")";
}
struct StructFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Stmt {
    enum Kind { Normal, Return, Goto, IfNot, Push, Pop, PopIfNot, Computed } kind = Normal;
    u32 off = 0, next = 0, target = 0;
    std::string text, cond, lhs;   // lhs: the variable a Let assigns
    i64 latent = -1;               // resume offset of a latent action call
};

// byte constants keep their value in a marker until the statement knows whether they are an enum
const char kByteOpen = '\x02', kByteClose = '\x03';
std::string ByteMarker(u32 v) { return std::string(1, kByteOpen) + std::to_string(v) + kByteClose; }
bool IsByteMarker(const std::string& s, long long& v) {
    if (s.size() < 3 || s[0] != kByteOpen || s.back() != kByteClose) return false;
    v = atoll(s.substr(1, s.size() - 2).c_str());
    return true;
}
void ClearByteMarkers(std::string& s) {
    for (size_t k = 0; (k = s.find(kByteOpen, k)) != std::string::npos;) {
        size_t e = s.find(kByteClose, k);
        if (e == std::string::npos) break;
        s = s.substr(0, k) + s.substr(k + 1, e - k - 1) + s.substr(e + 1);
    }
}
std::string EnumOfType(const std::string& t) {
    if (IStartsWith(t, "TEnumAsByte<") && t.back() == '>') return t.substr(12, t.size() - 13);
    if (t.size() > 1 && t[0] == 'E' && isupper((u8)t[1])) return t;
    return "";
}

class Decompiler {
public:
    Decompiler(const Package& p, const BlueprintOptions& o) : pkg(p), opt(o) {}

    // ------------------------------------------------------------- package level helpers (any package)

    static bool BodyReader(const Package& p, size_t exportIdx, Reader& out) {
        try {
            size_t end = 0;
            std::string err;
            p.ReadProperties(exportIdx, &end, &err);
            Reader r = p.ExportReader(exportIdx);
            r.Seek(end);
            if (r.U32()) r.Skip(16);
            out = r;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    std::shared_ptr<Package> LoadOther(const std::string& packagePath) {
        if (!opt.cache || packagePath.empty() || IStartsWith(packagePath, "/Script/")) return nullptr;
        auto it = loaded.find(packagePath);
        if (it != loaded.end()) return it->second;
        std::shared_ptr<Package> p;
        try {
            p = opt.cache->LoadPath(packagePath);
        } catch (const std::exception&) {
        }
        loaded[packagePath] = p;
        return p;
    }

    // class prefix (A for actors, U for other objects) through the class's super chain
    char ClassPrefix(const Package& p, i32 idx, int depth = 0) {
        if (!idx) return 'U';
        std::string name = p.ObjectName(idx);
        if (auto* cpp = SotReflection::ClassCppName(name)) return (*cpp)[0];
        auto memo = prefixMemo.find(name);
        if (memo != prefixMemo.end()) return memo->second;
        char res = 'U';
        if (depth < 24) {
            if (idx > 0) {
                Reader r;
                if (BodyReader(p, (size_t)idx - 1, r)) {
                    try {
                        r.I32();   // Next
                        res = ClassPrefix(p, r.I32(), depth + 1);
                    } catch (const std::exception&) {
                    }
                }
            } else if (auto other = LoadOther(p.ImportPackage(idx))) {
                if (i32 e = other->FindExport(name)) res = ClassPrefix(*other, e, depth + 1);
            }
        }
        prefixMemo[name] = res;
        return res;
    }
    std::string ClassCpp(const Package& p, i32 idx) {
        if (!idx) return "UObject";
        std::string name = p.ObjectName(idx);
        if (idx < 0 ? !IStartsWith(p.ImportPackage(idx), "/Script/") : &p == &pkg)   // Blueprint classes (the dumps list some of them too)
            classPackages.emplace(name, idx < 0 ? p.ImportPackage(idx) : pkg.PackageName());
        if (auto* cpp = SotReflection::ClassCppName(name)) return *cpp;
        return std::string(1, ClassPrefix(p, idx)) + SafeIdent(name);
    }
    std::string ClassCppByName(const std::string& name) {
        if (auto* cpp = SotReflection::ClassCppName(name)) return *cpp;
        auto memo = prefixMemo.find(name);
        return std::string(1, memo != prefixMemo.end() ? memo->second : 'U') + SafeIdent(name);
    }

    std::string PropertyType(const Package& p, size_t exportIdx, PropInfo* info = nullptr, int depth = 0) {
        std::string cls = p.ExportClass(exportIdx);
        if (depth > 4) return cls;
        Reader r;
        if (BodyReader(p, exportIdx, r)) {
            try {
                r.I32();   // Next
                r.I32();   // ArrayDim
                r.U64();   // PropertyFlags
                p.ReadName(r);   // RepNotifyFunc
                auto ref = [&] { return r.I32(); };
                if (cls == "ObjectProperty") return ClassCpp(p, ref()) + "*";
                if (cls == "WeakObjectProperty") return "TWeakObjectPtr<" + ClassCpp(p, ref()) + ">";
                if (cls == "LazyObjectProperty") return "TLazyObjectPtr<" + ClassCpp(p, ref()) + ">";
                if (cls == "AssetObjectProperty") return "TAssetPtr<" + ClassCpp(p, ref()) + ">";
                if (cls == "ClassProperty") { ref(); return "TSubclassOf<" + ClassCpp(p, ref()) + ">"; }
                if (cls == "AssetClassProperty") { ref(); return "TAssetSubclassOf<" + ClassCpp(p, ref()) + ">"; }
                if (cls == "InterfaceProperty") {
                    i32 c = ref();
                    ClassCpp(p, c);   // records a Blueprint interface's package
                    return "TScriptInterface<I" + SafeIdent(p.ObjectName(c)) + ">";
                }
                if (cls == "StructProperty") {
                    i32 s = ref();
                    if (s < 0 ? !IStartsWith(p.ImportPackage(s), "/Script/") : &p == &pkg)   // a Blueprint struct: members readable
                        structPackages.emplace("F" + SafeIdent(p.ObjectName(s)), std::make_pair(s < 0 ? p.ImportPackage(s) : pkg.PackageName(), p.ObjectName(s)));
                    return "F" + SafeIdent(p.ObjectName(s));
                }
                if (cls == "ByteProperty") {
                    i32 e = ref();
                    if (!e) return "uint8";
                    std::string en = p.ObjectName(e);
                    if (info) { info->enumName = en; info->enumPath = p.FullPath(e); }
                    enumPaths[en] = p.FullPath(e);
                    return "TEnumAsByte<" + SafeIdent(en) + ">";
                }
                if (cls == "ArrayProperty") {
                    i32 inner = ref();
                    return "TArray<" + (inner > 0 ? PropertyType(p, (size_t)inner - 1, nullptr, depth + 1) : p.ObjectName(inner)) + ">";
                }
                if (cls == "DelegateProperty") return "FScriptDelegate /* " + p.ObjectName(ref()) + " */";
                if (cls == "MulticastDelegateProperty") return "FMulticastScriptDelegate /* " + p.ObjectName(ref()) + " */";
            } catch (const std::exception&) {
            }
        }
        static const std::map<std::string, std::string> simple = {
            {"BoolProperty", "bool"}, {"IntProperty", "int32"}, {"Int8Property", "int8"}, {"Int16Property", "int16"}, {"Int64Property", "int64"},
            {"UInt16Property", "uint16"}, {"UInt32Property", "uint32"}, {"UInt64Property", "uint64"}, {"FloatProperty", "float"},
            {"DoubleProperty", "double"}, {"NameProperty", "FName"}, {"StrProperty", "FString"}, {"TextProperty", "FText"}};
        auto it = simple.find(cls);
        return it != simple.end() ? it->second : cls;
    }

    PropInfo ReadProp(const Package& p, size_t exportIdx) {
        PropInfo info;
        info.name = SafeIdent(p.Exports()[exportIdx].objectName);
        info.type = PropertyType(p, exportIdx, &info);
        Reader r;
        if (BodyReader(p, exportIdx, r)) {
            try {
                r.I32();
                info.arrayDim = r.I32();
                info.flags = r.U64();
            } catch (const std::exception&) {
            }
        }
        return info;
    }

    bool ReadFunction(const Package& p, size_t exportIdx, FuncInfo& f, std::string& err, bool withScript = true) {
        f.index = (i32)exportIdx + 1;
        f.name = p.Exports()[exportIdx].objectName;
        Reader r;
        i32 children = 0;
        if (!BodyReader(p, exportIdx, r)) { err = "unreadable function"; return false; }
        try {
            r.I32();   // Next
            r.I32();   // SuperStruct
            children = r.I32();
            r.I32();   // BytecodeBufferSize (in memory)
            i32 serialized = r.I32();
            if (serialized < 0 || (size_t)serialized > r.Remaining()) throw ParseError("bad script size");
            if (withScript) f.script = r.ReadBytes((size_t)serialized);
            else r.Skip((size_t)serialized);
            f.flags = r.U32();
            if (f.flags & FUNC_Net) r.U16();
            f.eventGraph = r.I32();
            f.eventGraphOffset = r.I32();
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        }
        // parameters and locals in the function's Children -> Next chain: the declaration order, which is the order
        // calls pass arguments in (export order differs); exports the chain misses are added after it
        std::vector<size_t> order;
        std::set<size_t> seen;
        for (i32 c = children; c > 0 && (size_t)c <= p.Exports().size() && order.size() < 4096;) {
            size_t i = (size_t)c - 1;
            if (p.Exports()[i].outerIndex != f.index || !IEndsWith(p.ExportClass(i), "Property") || !seen.insert(i).second) break;
            order.push_back(i);
            Reader pr;
            c = 0;
            if (BodyReader(p, i, pr)) {
                try { c = pr.I32(); } catch (const std::exception&) { c = 0; }
            }
        }
        for (size_t i = 0; i < p.Exports().size(); i++)
            if (!seen.count(i) && p.Exports()[i].outerIndex == f.index && IEndsWith(p.ExportClass(i), "Property")) order.push_back(i);
        for (size_t i : order) {
            PropInfo pi = ReadProp(p, i);
            if (pi.flags & CPF_Parm) f.params.push_back(pi);
            else f.locals.push_back(pi);
        }
        return true;
    }

    static ReflFunction ToRefl(const FuncInfo& f) {
        ReflFunction r;
        r.isStatic = (f.flags & FUNC_Static) != 0;
        r.isConst = (f.flags & FUNC_Const) != 0;
        for (auto& p : f.params) {
            if (p.flags & CPF_ReturnParm) { r.ret = p.type; continue; }
            bool out = (p.flags & CPF_OutParm) && !(p.flags & CPF_ConstParm);
            r.params.push_back({out ? 'o' : (p.flags & (CPF_ReferenceParm | CPF_ConstParm)) ? 'r' : 'v', p.type, p.name});
        }
        return r;
    }

    // a Blueprint function of class export `cls` in package `p`
    const ReflFunction* BpFunction(const Package& p, i32 cls, const std::string& name) {
        std::string key = p.PackageName() + "\t" + std::to_string(cls) + "\t" + name;
        auto it = extFuncs.find(key);
        if (it != extFuncs.end()) return it->second.get();
        std::unique_ptr<ReflFunction> res;
        for (size_t i = 0; i < p.Exports().size(); i++) {
            const auto& ex = p.Exports()[i];
            if (ex.outerIndex != cls || ex.objectName != name || p.ExportClass(i) != "Function") continue;
            FuncInfo f;
            std::string err;
            if (ReadFunction(p, i, f, err, false)) res = std::make_unique<ReflFunction>(ToRefl(f));
            break;
        }
        return (extFuncs[key] = std::move(res)).get();
    }

    // the function a FinalFunction reference points at: own class, a native class, or another Blueprint
    const ReflFunction* FunctionByRef(i32 func, std::string* owner = nullptr) {
        if (func > 0) {   // own function
            for (auto& f : funcs)
                if (f.index == func) {
                    if (owner) *owner = clsName;
                    return &ownRefl[f.index];
                }
            return nullptr;
        }
        if (func == 0) return nullptr;
        const auto& im = pkg.Imports()[-func - 1];
        std::string name = im.objectName, own = im.outerIndex ? pkg.ObjectName(im.outerIndex) : "";
        if (owner) *owner = own;
        if (auto* f = SotReflection::Function(own, name)) return f;
        if (auto other = LoadOther(pkg.ImportPackage(func))) {
            if (i32 c = other->FindExport(own)) {   // another Blueprint: its functions, then its parents'
                if (auto* f = BpFunction(*other, c, name)) return f;
                const ReflFunction* found = nullptr;
                ForEachSuperOf(*other, c, [&](const Package& p, i32 cls) { return (found = BpFunction(p, cls, name)) != nullptr; },
                               [&](const std::string& n) { found = NativeFunction(n, name, owner); return true; });
                return found;
            }
        }
        return NativeFunction(own, name, owner);
    }
    // a function of a class by name: a Blueprint class seen in a type (its functions, then its parents'), else native
    std::map<std::string, std::string> classPackages;   // Blueprint class name -> package path
    const ReflFunction* ClassFunction(const std::string& cls, const std::string& name) {
        auto cp = classPackages.find(cls);
        if (cp == classPackages.end()) return NativeFunction(cls, name);
        if (cls == clsName) return SelfFunction(name);
        auto other = LoadOther(cp->second);
        i32 c = other ? other->FindExport(cls) : 0;
        if (!c) return NativeFunction(cls, name);
        if (auto* f = BpFunction(*other, c, name)) return f;
        const ReflFunction* found = nullptr;
        ForEachSuperOf(*other, c, [&](const Package& p, i32 k) { return (found = BpFunction(p, k, name)) != nullptr; },
                       [&](const std::string& n) { found = NativeFunction(n, name); return true; });
        return found ? found : NativeFunction(cls, name);
    }
    // a native function from class `cls` up, else the one interface declaring it (class interfaces aren't in the table)
    static const ReflFunction* NativeFunction(const std::string& cls, const std::string& name, std::string* owner = nullptr) {
        if (auto* f = SotReflection::FunctionInHierarchy(cls, name, owner)) return f;
        if (auto* f = SotReflection::FunctionInHierarchy(cls, SafeIdent(name), owner)) return f;   // the dumps write "A B" as A_B
        return SotReflection::UniqueFunction(name, true, owner);
    }

    // a function called by name on this object: own functions, then up the super chain (Blueprints, then native)
    // walks this class's super chain: `bp(package, class export)` for each parent Blueprint class, then `native(name)`
    // for the first native class; stops when a callback returns true
    template <typename Bp, typename Native> void ForEachSuper(Bp&& bp, Native&& native) { ForEachSuperOf(pkg, clsIndex, bp, native); }
    template <typename Bp, typename Native> void ForEachSuperOf(const Package& start, i32 startCls, Bp&& bp, Native&& native) {
        std::shared_ptr<Package> holder;
        const Package* p = &start;
        i32 cls = startCls;
        for (int guard = 0; guard < 24 && cls; guard++) {
            i32 sup = 0;
            Reader r;
            if (cls > 0 && BodyReader(*p, (size_t)cls - 1, r)) {
                try { r.I32(); sup = r.I32(); } catch (const std::exception&) { sup = 0; }
            }
            if (!sup) return;
            std::string supName = p->ObjectName(sup);
            if (sup < 0) {
                if (IStartsWith(p->ImportPackage(sup), "/Script/")) { native(supName); return; }
                auto other = LoadOther(p->ImportPackage(sup));
                if (!other) return;
                holder = other;
                p = other.get();
                cls = p->FindExport(supName);
            } else {
                cls = sup;
            }
            if (cls > 0 && bp(*p, cls)) return;
        }
    }

    const ReflFunction* SelfFunction(const std::string& name) {
        for (auto& f : funcs)
            if (f.name == name) return &ownRefl[f.index];
        const ReflFunction* found = nullptr;
        ForEachSuper([&](const Package& p, i32 cls) { return (found = BpFunction(p, cls, name)) != nullptr; },
                     [&](const std::string& n) { found = NativeFunction(n, name); return true; });
        return found;
    }

    // C++ type of a member inherited from a parent Blueprint or native class ("" when unknown)
    std::map<std::string, std::string> inheritedTypes;
    std::string InheritedType(const std::string& name) {
        auto it = inheritedTypes.find(name);
        if (it != inheritedTypes.end()) return it->second;
        std::string type;
        ForEachSuper(
            [&](const Package& p, i32 cls) {
                for (size_t i = 0; i < p.Exports().size(); i++)
                    if (p.Exports()[i].outerIndex == cls && SafeIdent(p.Exports()[i].objectName) == name && IEndsWith(p.ExportClass(i), "Property")) {
                        type = ReadProp(p, i).type;
                        return true;
                    }
                return false;
            },
            [&](const std::string& n) {
                if (auto* t = SotReflection::FieldInHierarchy(n, name)) type = *t;
                return true;
            });
        return inheritedTypes[name] = type;
    }
    // type of a variable or member path ("Mesh", "this->Pawn", "Foo->Bar"): own variables, inherited members, native members
    std::string ExprType(const std::string& expr) {
        std::string e = expr;
        if (IStartsWith(e, "this->")) e = e.substr(6);
        size_t sep = std::string::npos, sepLen = 0;
        int depth = 0;
        for (size_t i = 0; i + 1 < e.size(); i++) {
            if (e[i] == '(' || e[i] == '[') depth++;
            else if (e[i] == ')' || e[i] == ']') depth--;
            else if (depth == 0 && e[i] == '-' && e[i + 1] == '>') { sep = i; sepLen = 2; }
            else if (depth == 0 && e[i] == '.' && !isdigit((u8)e[i + 1])) { sep = i; sepLen = 1; }
        }
        if (sep != std::string::npos) {
            std::string leftType = ExprType(e.substr(0, sep)), member = e.substr(sep + sepLen);
            if (!IsPlainIdent(member)) return "";
            if (sepLen == 1) return StructMemberType(leftType, member);
            std::string left = ClassOfType(leftType);
            return left.empty() ? "" : MemberTypeOf(left, member);
        }
        if (!IsPlainIdent(e)) return "";
        auto st = scopeTypes.find(e);   // parameters / locals of the function being decompiled shadow members
        if (st != scopeTypes.end() && !IStartsWith(expr, "this->")) return st->second;
        auto it = varTypes.find(e);
        if (it != varTypes.end()) return it->second;
        return InheritedType(e);
    }
    // a property export named `member` directly inside export `outer` of package `p`
    std::string PropTypeIn(const Package& p, i32 outer, const std::string& member) {
        for (size_t i = 0; i < p.Exports().size(); i++)
            if (p.Exports()[i].outerIndex == outer && SafeIdent(p.Exports()[i].objectName) == member && IEndsWith(p.ExportClass(i), "Property"))
                return ReadProp(p, i).type;
        return "";
    }
    // member type of a class: Blueprint classes from their package (and parents), native ones from the table
    std::map<std::string, std::string> memberMemo;
    std::string MemberTypeOf(const std::string& cls, const std::string& member) {
        std::string key = cls + "\t" + member;
        auto mm = memberMemo.find(key);
        if (mm != memberMemo.end()) return mm->second;
        std::string type;
        auto cp = classPackages.find(cls);
        if (cp == classPackages.end()) {
            if (auto* t = SotReflection::FieldInHierarchy(cls, member)) type = *t;
        } else if (auto other = LoadOther(cp->second)) {
            if (i32 c = other->FindExport(cls)) {
                type = PropTypeIn(*other, c, member);
                if (type.empty())
                    ForEachSuperOf(*other, c, [&](const Package& p, i32 k) { type = PropTypeIn(p, k, member); return !type.empty(); },
                                   [&](const std::string& n) {
                                       if (auto* t = SotReflection::FieldInHierarchy(n, member)) type = *t;
                                       return true;
                                   });
            }
        }
        return memberMemo[key] = type;
    }
    // member type of a Blueprint struct (FName -> package, struct object name)
    std::map<std::string, std::pair<std::string, std::string>> structPackages;
    std::string StructMemberType(const std::string& structType, const std::string& member) {
        auto sp = structPackages.find(structType);
        if (sp == structPackages.end()) {   // a native struct (the table keys them by their F name)
            auto* t = SotReflection::FieldInHierarchy(structType, member);
            return t ? *t : "";
        }
        std::string key = structType + "\t" + member;
        auto mm = memberMemo.find(key);
        if (mm != memberMemo.end()) return mm->second;
        std::string type;
        if (auto other = LoadOther(sp->second.first))
            if (i32 s = other->FindExport(sp->second.second)) type = PropTypeIn(*other, s, member);
        return memberMemo[key] = type;
    }
    static bool IsPlainIdent(const std::string& s) {
        if (s.empty() || isdigit((u8)s[0])) return false;
        for (char c : s)
            if (!IsIdentChar(c)) return false;
        return true;
    }
    static std::string ClassOfType(const std::string& t) {   // unprefixed class of an object / interface type
        if (t.size() > 2 && t.back() == '*' && (t[0] == 'A' || t[0] == 'U')) return t.substr(1, t.size() - 2);
        if (IStartsWith(t, "TScriptInterface<I") && t.back() == '>') return t.substr(18, t.size() - 19);
        return "";
    }

    // enumerator name of a value: native enums from the reflection table, Blueprint enums from their package
    std::string EnumLiteral(const std::string& en, long long v) {
        if (auto* e = SotReflection::Enum(en))
            if (auto* n = e->Name(v)) return SafeIdent(en) + "::" + *n;
        auto it = bpEnums.find(en);
        if (it == bpEnums.end()) {
            std::vector<std::pair<long long, std::string>> vals;
            auto pit = enumPaths.find(en);
            if (pit != enumPaths.end()) {
                std::string path = pit->second.substr(0, pit->second.find('.'));
                if (auto other = LoadOther(path))
                    if (i32 e = other->FindExport(en)) {
                        std::vector<std::string> display;
                        size_t end = 0;
                        std::string err;
                        PropertyList props = other->ReadProperties((size_t)e - 1, &end, &err);
                        if (const PropValue* dn = FindValue(props, "DisplayNames"))
                            for (auto& t : dn->items) display.push_back(t.str);
                        Reader r;
                        if (BodyReader(*other, (size_t)e - 1, r)) {
                            try {
                                r.I32();   // Next
                                i32 n = r.I32();
                                for (i32 i = 0; i < n && i < 4096; i++) {
                                    std::string name = other->ReadName(r);
                                    long long val = r.U8();
                                    size_t sep = name.rfind("::");
                                    if (sep != std::string::npos) name = name.substr(sep + 2);
                                    if (IEndsWith(name, "_MAX")) continue;
                                    if ((size_t)i < display.size() && !display[i].empty() && display[i] != name) name += " /*" + display[i] + "*/";
                                    vals.push_back({val, name});
                                }
                            } catch (const std::exception&) {
                            }
                        }
                    }
            }
            it = bpEnums.emplace(en, std::move(vals)).first;
        }
        for (auto& [k, n] : it->second)
            if (k == v) return SafeIdent(en) + "::" + n;
        return "";
    }
    std::string ApplyEnum(const std::string& en, const std::string& arg) {
        long long v;
        if (en.empty() || !IsByteMarker(arg, v)) return arg;
        std::string lit = EnumLiteral(en, v);
        return lit.empty() ? arg : lit;
    }
    std::string VarEnum(const std::string& expr) { return EnumOfType(ExprType(expr)); }
    std::string VarClass(const std::string& expr) { return ClassOfType(ExprType(expr)); }   // unprefixed class of an object expression

    // ------------------------------------------------------------- bytecode

    struct Script {
        const Bytes* data = nullptr;
        size_t pos = 0;
        u32 mem = 0;
        template <typename T> T Read() {
            if (pos + sizeof(T) > data->size()) throw ParseError("script truncated");
            T v;
            memcpy(&v, data->data() + pos, sizeof(T));
            pos += sizeof(T);
            mem += sizeof(T);
            return v;
        }
        i32 Obj() {
            i32 v = Read<i32>();
            mem += 4;   // pointer in memory
            return v;
        }
        std::pair<i32, i32> Name() {
            i32 a = Read<i32>(), b = Read<i32>();
            mem += 4;   // FScriptName in memory
            return {a, b};
        }
        std::string CStr() {
            std::string s;
            for (;;) {
                char c = Read<char>();
                if (!c) break;
                s += c;
            }
            return s;
        }
        std::string WStr() {
            std::wstring s;
            for (;;) {
                wchar_t c = (wchar_t)Read<u16>();
                if (!c) break;
                s += c;
            }
            return Narrow(s);
        }
    };

    std::set<u32> targets;                       // jump destinations (in-memory offsets)
    std::set<std::string> scopeNames;            // parameters / locals of the function being decompiled (members get this-> when shadowed)
    std::set<std::string> outArgs;               // variables passed to out parameters
    std::string currentFunc;
    std::map<u32, std::string> entryNames;       // ubergraph entry offset -> event name
    std::map<u32, std::string> pendingEntries;   // latent resume points found while decompiling (the ubergraph itself)
    i64 lastLatent = -1;
    bool inContext = false;                      // rendering the call part of obj->Call()
    std::string ctxClass;                        // unprefixed class of the context object, when known
    std::string ctxObj;                          // the context object expression (for the unresolved-call report)
    std::string lastCdo;                         // class of the last class-default-object constant
    int counting = 0;                            // stats are taken on the final pass over each function only

    static std::string Quote(const std::string& s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if (c == '\n') { o += "\\n"; continue; }
            o += c;
        }
        return o + "\"";
    }
    static std::string Float(float f) {
        std::string s = Fmt("%.6g", f);
        if (s.find_first_of(".en") == std::string::npos) s += ".0";
        return s + "f";
    }
    std::string NameStr(std::pair<i32, i32> n) const { return pkg.Name(n.first, n.second); }
    static std::string Label(u32 off) { return Fmt("Label_%u", off); }
    std::string RefName(i32 idx) const { return idx ? pkg.ObjectName(idx) : "None"; }
    std::string Var(i32 idx) const { return SafeIdent(RefName(idx)); }

    std::vector<std::string> ArgList(Script& s) {
        std::vector<std::string> out;
        for (;;) {
            if (s.pos >= s.data->size()) throw ParseError("unterminated call");
            u8 peek = (*s.data)[s.pos];
            if (peek == 0x16) { s.Read<u8>(); break; }   // EX_EndFunctionParms
            bool saved = inContext;
            inContext = false;
            out.push_back(Expr(s));
            inContext = saved;
        }
        return out;
    }
    std::string ExprList(Script& s, u8 end) {
        std::string out;
        for (;;) {
            if (s.pos >= s.data->size()) throw ParseError("unterminated list");
            if ((*s.data)[s.pos] == end) { s.Read<u8>(); break; }
            if (!out.empty()) out += ", ";
            out += Expr(s);
        }
        return out;
    }

    static bool IsLiteral(const std::string& a) {
        if (a == "nullptr" || a == "true" || a == "false") return true;
        long long v;
        if (IsByteMarker(a, v)) return true;
        if (a.empty()) return false;
        size_t i = a[0] == '-' ? 1 : 0;
        if (i >= a.size() || !isdigit((u8)a[i])) return false;
        for (; i < a.size(); i++)
            if (!isdigit((u8)a[i]) && a[i] != '.' && a[i] != 'f' && a[i] != 'e' && a[i] != '-' && a[i] != 'l' && a[i] != 'u') return false;
        return true;
    }

    // arguments with the callee's signature: enum values by name, literals tagged with the parameter name
    // out parameter positions of a signature the SDK dumps disagree on, from this call: the compiler passes each out
    // parameter its own temporary named CallFunc_<Function>_<Parameter>[N]; the other parameters keep their order
    bool OrderFromCall(const ReflFunction& sig, const std::string& fn, const std::vector<std::string>& args, ReflFunction& res) {
        if (sig.params.size() != args.size()) return false;
        std::vector<const ReflParam*> at(args.size(), nullptr);
        for (auto& p : sig.params) {
            if (p.mode != 'o') continue;
            std::string temp = SafeIdent("CallFunc_" + fn + "_" + p.name);
            int found = -1;
            for (size_t i = 0; i < args.size(); i++) {
                std::string a = IStartsWith(args[i], "this->") ? args[i].substr(6) : args[i];
                if (!IStartsWith(a, temp)) continue;
                bool digits = true;
                for (size_t k = temp.size(); k < a.size(); k++) digits = digits && isdigit((u8)a[k]);
                if (!digits || a.size() > temp.size() && a.size() - temp.size() > 3) continue;
                if (found >= 0 || at[i]) return false;
                found = (int)i;
            }
            if (found < 0) return false;
            at[(size_t)found] = &p;
        }
        size_t next = 0;
        for (auto& slot : at) {
            if (slot) continue;
            while (next < sig.params.size() && sig.params[next].mode == 'o') next++;
            if (next >= sig.params.size()) return false;
            slot = &sig.params[next++];
        }
        res = sig;
        res.orderUnknown = false;
        res.params.clear();
        for (auto* p : at) res.params.push_back(*p);
        return true;
    }

    std::string FormatArgs(std::vector<std::string> args, const ReflFunction* sig, const std::string& fn = "") {
        ReflFunction ordered;
        if (sig && sig->orderUnknown && !fn.empty() && OrderFromCall(*sig, fn, args, ordered)) sig = &ordered;
        if (sig && sig->orderUnknown && opt.stats && counting > 0) opt.stats->callsOrderUnknown++;
        bool match = sig && !sig->orderUnknown && sig->params.size() == args.size();
        std::string out;
        for (size_t i = 0; i < args.size(); i++) {
            std::string a = args[i];
            if (match) {
                const ReflParam& p = sig->params[i];
                if (p.mode == 'o' && SimpleExpr(a)) outArgs.insert(IStartsWith(a, "this->") ? a.substr(6) : a);
                std::string en = EnumOfType(p.type);
                std::string e = ApplyEnum(en, a);
                if (e != a) a = e;
                else if (IsLiteral(a)) a = "/*" + p.name + "*/ " + a;
            }
            out += (i ? ", " : "") + a;
        }
        return out;
    }

    // KismetMathLibrary operators written as C++ operators (same semantics; Divide / Percent guard zero, kept as calls)
    static std::string MathOperator(const std::string& name, const std::vector<std::string>& a) {
        static const std::pair<const char*, const char*> bin[] = {
            {"Add_", "+"}, {"Subtract_", "-"}, {"Multiply_", "*"}, {"LessEqual_", "<="}, {"GreaterEqual_", ">="}, {"Less_", "<"},
            {"Greater_", ">"}, {"EqualEqual_", "=="}, {"NotEqual_", "!="}, {"BooleanAND", "&&"}, {"BooleanOR", "||"}, {"BooleanXOR", "!="},
            {"And_IntInt", "&"}, {"Or_IntInt", "|"}, {"Xor_IntInt", "^"}};
        if (a.size() == 2)
            for (auto& [pre, op] : bin)
                if (IStartsWith(name, pre)) {
                    // only plain operands: Vector / Rotator / Transform comparisons take an error tolerance (3 args, not matched here)
                    return "(" + a[0] + " " + op + " " + a[1] + ")";
                }
        if (a.size() == 1 && (name == "Not_PreBool")) return Neg(a[0]);
        return "";
    }

    void CountCall(const ReflFunction* sig, const std::string& owner, const std::string& name) {
        if (!opt.stats || counting == 0) return;
        opt.stats->calls++;
        if (sig) opt.stats->callsWithSignature++;
        else opt.stats->unresolved[owner + "::" + name]++;
    }

    std::string Call(i32 funcRef, const std::string& name, std::vector<std::string> args, bool callMath) {
        std::string owner;
        const ReflFunction* sig = FunctionByRef(funcRef, &owner);
        std::string fname = SafeIdent(name);
        CountCall(sig, owner, name);
        if (IStartsWith(name, "ExecuteUbergraph") && !args.empty() && isdigit((u8)args[0][0])) entryNames[(u32)strtoul(args[0].c_str(), nullptr, 10)] = currentFunc;
        bool isStatic = callMath || (sig && sig->isStatic) || (funcRef < 0 && !inContext && owner != clsName && IStartsWith(owner, "Kismet"));
        if (owner == "KismetMathLibrary") {
            std::vector<std::string> plain = args;
            for (auto& a : plain) ClearByteMarkers(a);
            std::string op = MathOperator(name, plain);
            if (!op.empty()) {
                // enum comparisons: name the byte constant after the other side's enum
                if (args.size() == 2 && (IStartsWith(name, "EqualEqual_Byte") || IStartsWith(name, "NotEqual_Byte"))) {
                    std::string en = VarEnum(args[0]).empty() ? VarEnum(args[1]) : VarEnum(args[0]);
                    std::string a0 = ApplyEnum(en, args[0]), a1 = ApplyEnum(en, args[1]);
                    ClearByteMarkers(a0);
                    ClearByteMarkers(a1);
                    return "(" + a0 + (IStartsWith(name, "Equal") ? " == " : " != ") + a1 + ")";
                }
                return op;
            }
        }
        if (owner == "KismetArrayLibrary" && name == "Array_Length" && args.size() == 1) return args[0] + ".Num()";
        std::string call = fname + "(" + FormatArgs(args, sig, name) + ")";
        // a non-virtual call of a parent's function this class overrides: the "Parent: X" node
        if (!inContext && !isStatic && funcRef < 0 && owner != clsName)
            for (auto& f : funcs)
                if (f.name == name) return "Super::" + call;
        if (isStatic && !inContext && !owner.empty() && owner != clsName) return ClassCppByName(owner) + "::" + call;
        return call;
    }

    std::string Expr(Script& s) {
        u8 op = s.Read<u8>();
        switch (op) {
        case 0x00: case 0x02: case 0x48: {   // Local / Default / LocalOut variable
            std::string n = Var(s.Obj());
            auto rn = renames.find(n);
            return rn != renames.end() ? rn->second : n;
        }
        case 0x01: {   // InstanceVariable
            std::string n = Var(s.Obj());
            return scopeNames.count(n) ? "this->" + n : n;
        }
        case 0x04: {   // Return
            std::string v = Expr(s);
            return v.empty() ? "return" : "return " + v;
        }
        case 0x06: { u32 t = s.Read<u32>(); targets.insert(t); return "goto " + Label(t); }
        case 0x07: {   // JumpIfNot
            u32 t = s.Read<u32>();
            targets.insert(t);
            return "if (" + Neg(Expr(s)) + ") goto " + Label(t);
        }
        case 0x09: { s.Read<u16>(); s.Read<u8>(); return "check(" + Expr(s) + ")"; }
        case 0x0B: return "";
        case 0x0C: s.Read<i32>(); return "";
        case 0x0F: case 0x14: case 0x5F: case 0x60: case 0x44: case 0x43: {   // Let / LetBool / LetObj / LetWeakObjPtr / LetDelegate / LetMulticastDelegate
            i32 prop = op == 0x0F ? s.Obj() : 0;
            std::string var = Expr(s), val = Expr(s);
            val = ApplyEnum(VarEnum(var), val);
            if ((prop && pkg.ClassName(prop) == "BoolProperty") || ExprType(var) == "bool") {   // a bool written from an integer constant
                std::string v = val;
                ClearByteMarkers(v);
                if (v == "0") val = "false";
                else if (v == "1") val = "true";
            }
            return var + " = " + val;
        }
        case 0x11: { s.Obj(); return std::to_string(s.Read<u8>()); }
        case 0x12: case 0x19: case 0x1A: {   // ClassContext / Context / Context_FailSilent
            lastCdo.clear();
            std::string obj = Expr(s);
            std::string cdo = lastCdo;
            s.Read<u32>();   // skip offset
            s.Obj();         // r-value property
            bool savedIn = inContext;
            std::string savedCls = ctxClass;
            inContext = true;
            ctxClass = !cdo.empty() ? cdo : VarClass(obj);
            std::string savedObj = ctxObj;
            ctxObj = obj;
            std::string ctx = Expr(s);
            inContext = savedIn;
            ctxClass = savedCls;
            ctxObj = savedObj;
            if (!cdo.empty()) {   // static call through the class default object (operators and .Num() stay as they are)
                size_t id = 0;
                while (id < ctx.size() && IsIdentChar(ctx[id])) id++;
                return id > 0 && id < ctx.size() && ctx[id] == '(' ? ClassCppByName(cdo) + "::" + ctx : ctx;
            }
            if (op == 0x12) return obj + "::" + ctx;
            return (FullyWrapped(obj) || SimpleExpr(obj) ? obj : "(" + obj + ")") + "->" + ctx;
        }
        case 0x13: { i32 c = s.Obj(); return "TSubclassOf<" + ClassCpp(pkg, c) + ">(" + Expr(s) + ")"; }   // MetaCast
        case 0x15: case 0x16: return "";
        case 0x17: return "this";
        case 0x18: { s.Read<u32>(); return Expr(s); }
        case 0x1B: case 0x45: {   // VirtualFunction / LocalVirtualFunction
            std::string name = NameStr(s.Name());
            bool in = inContext;
            std::string cc = ctxClass;
            auto args = ArgList(s);
            if (IStartsWith(name, "ExecuteUbergraph") && !args.empty() && isdigit((u8)args[0][0])) entryNames[(u32)strtoul(args[0].c_str(), nullptr, 10)] = currentFunc;
            // a context of unknown type: the only native class declaring the name
            const ReflFunction* sig = in ? (cc.empty() ? SotReflection::UniqueFunction(name, false) : ClassFunction(cc, name)) : SelfFunction(name);
            if (!IStartsWith(name, "ExecuteUbergraph")) CountCall(sig, in ? (cc.empty() ? "?[" + ctxObj + "]" : cc) : clsName, name);
            return SafeIdent(name) + "(" + FormatArgs(args, sig, name) + ")";
        }
        case 0x1C: case 0x46: case 0x68: {   // FinalFunction / LocalFinalFunction / CallMath
            i32 f = s.Obj();
            bool in = inContext;
            auto args = ArgList(s);
            bool saved = inContext;
            inContext = in;
            std::string r = Call(f, RefName(f), args, op == 0x68);
            inContext = saved;
            return r;
        }
        case 0x1D: return std::to_string(s.Read<i32>());
        case 0x1E: return Float(s.Read<float>());
        case 0x1F: return Quote(s.CStr());
        case 0x20: {   // ObjectConst
            i32 o = s.Obj();
            if (!o) return "nullptr";
            std::string name = pkg.ObjectName(o), cls = pkg.ClassName(o);
            if (IStartsWith(name, "Default__")) {
                lastCdo = name.substr(9);
                if (o < 0 ? !IStartsWith(pkg.ImportPackage(o), "/Script/") : true)   // a Blueprint's defaults: its class is in the same package
                    classPackages.emplace(lastCdo, o < 0 ? pkg.ImportPackage(o) : pkg.PackageName());
                return "GetDefault<" + ClassCppByName(lastCdo) + ">()";
            }
            if (cls == "Class" || cls == "BlueprintGeneratedClass") return ClassCpp(pkg, o) + "::StaticClass()";
            if (cls == "ScriptStruct" || cls == "UserDefinedStruct") return "F" + SafeIdent(name) + "::StaticStruct()";
            if (cls == "Enum" || cls == "UserDefinedEnum") return "StaticEnum<" + SafeIdent(name) + ">()";
            return cls + "'" + pkg.FullPath(o) + "'";   // an asset, as UE writes object references
        }
        case 0x21: return "FName(" + Quote(NameStr(s.Name())) + ")";
        case 0x22: { float p = s.Read<float>(), y = s.Read<float>(), r = s.Read<float>(); return "FRotator(" + Float(p) + ", " + Float(y) + ", " + Float(r) + ")"; }
        case 0x23: { float x = s.Read<float>(), y = s.Read<float>(), z = s.Read<float>(); return "FVector(" + Float(x) + ", " + Float(y) + ", " + Float(z) + ")"; }
        case 0x24: case 0x2C: return ByteMarker(s.Read<u8>());
        case 0x25: return "0";
        case 0x26: return "1";
        case 0x27: return "true";
        case 0x28: return "false";
        case 0x29: {   // TextConst (4.10: source, key, namespace)
            std::string src = Expr(s), key = Expr(s), ns = Expr(s);
            return "NSLOCTEXT(" + ns + ", " + key + ", " + src + ")";
        }
        case 0x2A: return "nullptr";
        case 0x2B: {
            float v[10];
            for (float& f : v) f = s.Read<float>();
            return Fmt("FTransform(FQuat(%s, %s, %s, %s), FVector(%s, %s, %s), FVector(%s, %s, %s))", Float(v[0]).c_str(), Float(v[1]).c_str(), Float(v[2]).c_str(),
                       Float(v[3]).c_str(), Float(v[4]).c_str(), Float(v[5]).c_str(), Float(v[6]).c_str(), Float(v[7]).c_str(), Float(v[8]).c_str(),
                       Float(v[9]).c_str());
        }
        case 0x2D: return "nullptr";   // NoInterface
        case 0x2E: { i32 c = s.Obj(); return "Cast<" + ClassCpp(pkg, c) + ">(" + Expr(s) + ")"; }
        case 0x2F: {   // StructConst
            i32 st = s.Obj();
            s.Read<i32>();
            std::string name = RefName(st), list = ExprList(s, 0x30);
            if (name == "LatentActionInfo" && !list.empty() && isdigit((u8)list[0])) {   // latent actions resume the ubergraph here
                u32 at = (u32)strtoul(list.c_str(), nullptr, 10);
                targets.insert(at);
                lastLatent = at;
                pendingEntries[at] = "resume after a latent action in " + currentFunc;
            }
            if (name == "Guid") {   // FGuid's A..D are uint32: written as hex, as UE prints GUIDs
                std::vector<std::string> parts;
                size_t b = 0;
                for (size_t e; (e = list.find(", ", b)) != std::string::npos; b = e + 2) parts.push_back(list.substr(b, e - b));
                parts.push_back(list.substr(b));
                bool ok = parts.size() == 4;
                for (auto& p : parts) ok = ok && !p.empty() && (isdigit((u8)p[0]) || p[0] == '-');
                if (ok && parts[0] == "0" && parts[1] == "0" && parts[2] == "0" && parts[3] == "0") return "FGuid()";
                if (ok) {
                    std::string g;
                    for (auto& p : parts) g += (g.empty() ? "" : ", ") + Fmt("0x%08X", (u32)strtoll(p.c_str(), nullptr, 10));
                    return "FGuid(" + g + ")";
                }
            }
            return "F" + SafeIdent(name) + "(" + list + ")";
        }
        case 0x31: {   // SetArray
            std::string arr = Expr(s);
            return arr + " = { " + ExprList(s, 0x32) + " }";
        }
        case 0x33: return Var(s.Obj());
        case 0x34: return "TEXT(" + Quote(s.WStr()) + ")";
        case 0x35: return std::to_string(s.Read<i64>()) + "ll";
        case 0x36: return std::to_string(s.Read<u64>()) + "ull";
        case 0x38: {   // PrimitiveCast
            u8 t = s.Read<u8>();
            std::string e = Expr(s);
            if (t == 0x47 || t == 0x49) return "(bool)" + (FullyWrapped(e) ? e : "(" + e + ")");   // object / interface to bool: valid
            if (t == 0x46) return "TScriptInterface<IInterface>(" + e + ")";
            return Fmt("Cast_%02X(", t) + e + ")";
        }
        case 0x42: {   // StructMemberContext
            i32 m = s.Obj();
            std::string base = Expr(s);
            return (SimpleExpr(base) || FullyWrapped(base) ? base : "(" + base + ")") + "." + Var(m);
        }
        case 0x4B: return "&" + NameStr(s.Name());   // InstanceDelegate
        case 0x4C: { u32 t = s.Read<u32>(); targets.insert(t); return "ExecutionFlow.Push(" + Label(t) + ")"; }
        case 0x4D: return "goto ExecutionFlow.Pop()";
        case 0x4E: return "goto (" + Expr(s) + ")";
        case 0x4F: return "if (" + Neg(Expr(s)) + ") goto ExecutionFlow.Pop()";
        case 0x50: case 0x5A: case 0x5E: return "";
        case 0x51: return Expr(s);   // InterfaceContext
        case 0x52: { i32 c = s.Obj(); return "TScriptInterface<I" + SafeIdent(RefName(c)) + ">(" + Expr(s) + ")"; }   // ObjToInterfaceCast
        case 0x54: { i32 c = s.Obj(); return "TScriptInterface<I" + SafeIdent(RefName(c)) + ">(" + Expr(s) + ".GetObject())"; }   // CrossInterfaceCast
        case 0x55: { i32 c = s.Obj(); return "Cast<" + ClassCpp(pkg, c) + ">(" + Expr(s) + ".GetObject())"; }   // InterfaceToObjCast
        case 0x53: return "";
        case 0x5B: return std::to_string(s.Read<u32>());
        case 0x5C: { std::string d = Expr(s); return d + ".Add(" + Expr(s) + ")"; }
        case 0x5D: return Expr(s) + ".Clear()";
        case 0x61: {   // BindDelegate
            std::string fn = NameStr(s.Name()), d = Expr(s);
            return d + ".BindUFunction(" + Expr(s) + ", " + Quote(fn) + ")";
        }
        case 0x62: { std::string d = Expr(s); return d + ".Remove(" + Expr(s) + ")"; }
        case 0x63: {   // CallMulticastDelegate
            s.Obj();
            std::string d = Expr(s);
            auto args = ArgList(s);
            std::string a;
            for (size_t i = 0; i < args.size(); i++) a += (i ? ", " : "") + args[i];
            return d + ".Broadcast(" + a + ")";
        }
        case 0x64: { i32 p = s.Obj(); return "UberGraphFrame->" + Var(p) + " = " + Expr(s); }   // LetValueOnPersistentFrame
        case 0x65: {
            s.Obj();
            s.Read<i32>();
            return "{ " + ExprList(s, 0x66) + " }";
        }
        case 0x67: return "FStringAssetReference(" + Expr(s) + ")";   // AssetConst
        case 0x69: {   // SwitchValue
            u16 n = s.Read<u16>();
            s.Read<u32>();
            std::string idx = Expr(s), out = "switch (" + idx + ") {";
            for (u16 i = 0; i < n; i++) {
                std::string c = Expr(s);
                s.Read<u32>();
                out += " case " + c + ": " + Expr(s) + ";";
            }
            return out + " default: " + Expr(s) + "; }";
        }
        case 0x6A: { u8 t = s.Read<u8>(); if (t == 4) s.Name(); return ""; }
        case 0x6B: { std::string a = Expr(s); return a + "[" + Expr(s) + "]"; }
        default: throw ParseError(Fmt("unknown opcode 0x%02X at %zu", op, s.pos - 1));
        }
    }

    // ------------------------------------------------------------- statements

    std::map<std::string, std::string> renames;   // ubergraph frame variable -> event parameter

    std::vector<Stmt> Parse(const FuncInfo& f, std::string& error) {
        std::vector<Stmt> st;
        Script s;
        s.data = &f.script;
        targets.clear();
        currentFunc = f.name;
        try {
            while (s.pos < f.script.size()) {
                Stmt t;
                t.off = s.mem;
                u8 op = f.script[s.pos];
                if (op == 0x53) break;   // EX_EndOfScript
                lastLatent = -1;
                if (op == 0x06) { s.Read<u8>(); t.kind = Stmt::Goto; t.target = s.Read<u32>(); targets.insert(t.target); }
                else if (op == 0x07) { s.Read<u8>(); t.kind = Stmt::IfNot; t.target = s.Read<u32>(); targets.insert(t.target); t.cond = Expr(s); }
                else if (op == 0x4C) { s.Read<u8>(); t.kind = Stmt::Push; t.target = s.Read<u32>(); targets.insert(t.target); }
                else if (op == 0x4D) { s.Read<u8>(); t.kind = Stmt::Pop; }
                else if (op == 0x4F) { s.Read<u8>(); t.kind = Stmt::PopIfNot; t.cond = Expr(s); }
                else if (op == 0x4E) { s.Read<u8>(); t.kind = Stmt::Computed; t.text = Expr(s); }
                else if (op == 0x04) {
                    s.Read<u8>();
                    t.kind = Stmt::Return;
                    std::string v = Expr(s);
                    t.text = v.empty() ? "return" : "return " + v;
                }
                else {
                    bool let = op == 0x0F || op == 0x14 || op == 0x5F || op == 0x60;
                    t.text = Expr(s);
                    if (let) {
                        size_t eq = t.text.find(" = ");
                        if (eq != std::string::npos) t.lhs = t.text.substr(0, eq);
                    }
                    t.latent = lastLatent;
                }
                t.next = s.mem;
                st.push_back(std::move(t));
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        for (auto& t : st) {
            ClearByteMarkers(t.text);
            ClearByteMarkers(t.cond);
        }
        return st;
    }

    // a temporary assigned once and read once by the very next statement is folded into it
    void Inline(std::vector<Stmt>& st, const std::set<std::string>& temps, const std::set<u32>& entries) {
        std::map<std::string, int> defs, uses;
        auto countIn = [&](const std::string& s, int d) {
            for (size_t k = 0; k < s.size();) {
                if (IsIdentChar(s[k]) && (k == 0 || !IsIdentChar(s[k - 1]))) {
                    size_t e = k;
                    while (e < s.size() && IsIdentChar(s[e])) e++;
                    std::string id = s.substr(k, e - k);
                    if (temps.count(id)) uses[id] += d;
                    k = e;
                } else {
                    k++;
                }
            }
        };
        for (auto& t : st) {
            countIn(t.text, 1);
            countIn(t.cond, 1);
            if (!t.lhs.empty() && temps.count(t.lhs)) { defs[t.lhs]++; uses[t.lhs]--; }
        }
        // a call whose result nothing reads is just the call
        for (auto& a : st)
            if (a.kind == Stmt::Normal && !a.lhs.empty() && temps.count(a.lhs) && IStartsWith(a.lhs, "CallFunc_") && defs[a.lhs] == 1 &&
                uses[a.lhs] == 0 && !outArgs.count(a.lhs) && a.text.size() > a.lhs.size() + 3 && a.text.back() == ')') {
                a.text = a.text.substr(a.lhs.size() + 3);
                a.lhs.clear();
            }
        for (size_t i = 0; i + 1 < st.size(); i++) {
            Stmt& a = st[i];
            Stmt& b = st[i + 1];
            if (a.kind != Stmt::Normal || a.lhs.empty() || a.latent >= 0 || !temps.count(a.lhs) || outArgs.count(a.lhs)) continue;
            if (defs[a.lhs] != 1 || uses[a.lhs] != 1 || targets.count(b.off) || entries.count(b.off)) continue;
            if (b.kind != Stmt::Normal && b.kind != Stmt::IfNot && b.kind != Stmt::PopIfNot && b.kind != Stmt::Return && b.kind != Stmt::Computed) continue;
            std::string& into = (b.kind == Stmt::IfNot || b.kind == Stmt::PopIfNot) ? b.cond : b.text;
            if (CountIdent(into, a.lhs) != 1) continue;
            std::string val = a.text.substr(a.lhs.size() + 3);
            if (val.find("switch (") == 0) continue;
            if (val[0] == '(' && !FullyWrapped(val)) val = "(" + val + ")";
            // a negated / compared temporary keeps its own parentheses
            ReplaceIdent(into, a.lhs, val);
            // tidy "!(x)" left by conditions on inlined calls
            if ((b.kind == Stmt::IfNot || b.kind == Stmt::PopIfNot) && into == val) into = val;
            a.text.clear();
            a.lhs.clear();
        }
    }

    // ------------------------------------------------------------- structured emission

    struct LoopState {
        u32 head = 0;
        i64 exit = -1;
        size_t depth = 0;   // flow stack size at the loop
        bool chainBreak = false;
    };
    struct Ctx {
        std::vector<u32> flow;
        std::vector<std::shared_ptr<LoopState>> loops;
        std::set<u32> stops, path;
    };

    const std::vector<Stmt>* cur = nullptr;
    std::map<u32, size_t> at;   // offset -> statement index
    std::set<u32> loopHeads;
    std::set<u32> pendingResume, inlinedResume;
    bool separateLatent = false;   // latent resumes as their own sections (retry when following them inline does not structure)
    size_t budget = 0;

    void Prepare(const std::vector<Stmt>& st) {
        cur = &st;
        at.clear();
        loopHeads.clear();
        for (size_t i = 0; i < st.size(); i++) at[st[i].off] = i;
        for (auto& t : st) {
            if ((t.kind == Stmt::Goto || t.kind == Stmt::IfNot || t.kind == Stmt::Push) && t.target <= t.off) loopHeads.insert(t.target);   // WhileLoop pushes its head
            if (t.latent >= 0 && !separateLatent) loopHeads.insert((u32)t.latent);   // loops may wait on a latent action (checked in EmitLoop)
        }
    }
    const Stmt* StmtAt(u32 off) const {
        auto it = at.find(off);
        return it == at.end() ? nullptr : &(*cur)[it->second];
    }
    // first statement with content from `off` (inlining leaves empty ones)
    const Stmt* Effective(u32 off) const {
        for (int guard = 0; guard < 100000; guard++) {
            const Stmt* s = StmtAt(off);
            if (!s) return nullptr;
            if (s->kind != Stmt::Normal || !s->text.empty()) return s;
            off = s->next;
        }
        return nullptr;
    }
    bool IsReturnLabel(u32 off) const {
        const Stmt* s = Effective(off);
        return !s || s->kind == Stmt::Return;
    }
    std::string ReturnText(u32 off) const {
        const Stmt* s = Effective(off);
        return s && !s->text.empty() && s->text != "return" ? s->text + ";" : "return;";
    }

    std::vector<u32> Succ(const Stmt& s) const {
        switch (s.kind) {
        case Stmt::Normal: {
            // a latent call that ends its chain continues at its resume point (shown inline)
            if (s.latent >= 0 && !separateLatent) {
                const Stmt* n = Effective(s.next);
                if (!n || n->kind == Stmt::Pop || n->kind == Stmt::Return) return {(u32)s.latent};
            }
            return {s.next};
        }
        case Stmt::Goto: return {s.target};
        case Stmt::IfNot: return {s.next, s.target};
        case Stmt::Push: return {s.next, s.target};
        case Stmt::PopIfNot: return {s.next};
        default: return {};
        }
    }
    // distances of the offsets reachable from `from` (terminal offsets are reached but not passed)
    std::map<u32, int> Reach(u32 from, const std::set<u32>& terminal) const {
        std::map<u32, int> d;
        std::vector<u32> q{from};
        d[from] = 0;
        for (size_t i = 0; i < q.size(); i++) {
            u32 o = q[i];
            if (terminal.count(o)) continue;
            const Stmt* s = StmtAt(o);
            if (!s) continue;
            for (u32 n : Succ(*s))
                if (!d.count(n)) { d[n] = d[o] + 1; q.push_back(n); }
        }
        return d;
    }

    void Line(std::vector<std::string>& out, int ind, const std::string& s) {
        if (++budget > 200000) throw StructFail("output too large");
        out.push_back(std::string((size_t)ind * 4, ' ') + s);
    }

    void EmitBlock(std::vector<std::string>& out, int ind, const std::vector<std::string>& body) {
        for (auto& l : body) out.push_back(std::string((size_t)ind * 4, ' ') + l);
    }

    // emits the flow from `off` until it ends, reaches a stop (join) or loops back
    void Emit(u32 off, Ctx ctx, std::vector<std::string>& out, int ind, bool enteringLoop = false) {
        for (;;) {
            if (ctx.stops.count(off)) return;
            for (auto it = ctx.loops.rbegin(); it != ctx.loops.rend(); ++it) {
                bool inner = it == ctx.loops.rbegin();
                if (!enteringLoop && off == (*it)->head) {
                    if (!inner) throw StructFail("continue of an outer loop");
                    Line(out, ind, "continue;");
                    return;
                }
                if ((*it)->exit >= 0 && off == (u32)(*it)->exit) {
                    if (!inner) throw StructFail("break out of an outer loop");
                    Line(out, ind, "break;");
                    return;
                }
            }
            const Stmt* sp = StmtAt(off);
            if (!sp) return;   // end of script
            const Stmt& s = *sp;
            if (loopHeads.count(off) && !enteringLoop) { EmitLoop(off, ctx, out, ind); return; }
            enteringLoop = false;
            if (ctx.path.count(off)) throw StructFail(Fmt("unstructured jump to %u", off));
            ctx.path.insert(off);
            switch (s.kind) {
            case Stmt::Normal: {
                if (s.text.empty()) { off = s.next; continue; }
                bool block = s.text.rfind("switch", 0) == 0;
                if (s.latent >= 0) {
                    const Stmt* n = Effective(s.next);
                    bool ends = !n || n->kind == Stmt::Pop || n->kind == Stmt::Return;
                    bool lastInChain = !separateLatent && ends && (ctx.flow.empty() || IsReturnLabel(ctx.flow.back()));
                    if (lastInChain) {
                        Line(out, ind, s.text + ";   // latent: what follows runs when it completes");
                        inlinedResume.insert((u32)s.latent);
                        off = (u32)s.latent;
                        continue;
                    }
                    Line(out, ind, s.text + ";   // latent: resumes at " + Label((u32)s.latent));
                    pendingResume.insert((u32)s.latent);
                } else {
                    Line(out, ind, s.text + (block ? "" : ";"));
                }
                off = s.next;
                continue;
            }
            case Stmt::Return:
                Line(out, ind, s.text.empty() || s.text == "return" ? "return;" : s.text + ";");
                return;
            case Stmt::Goto:
                off = s.target;
                continue;
            case Stmt::Computed:
                Line(out, ind, s.text + ";");
                return;
            case Stmt::Push: {
                Ctx inner = ctx;
                inner.flow.push_back(s.target);
                Emit(s.next, inner, out, ind);
                off = s.target;
                continue;
            }
            case Stmt::Pop: {
                EndChain(ctx, out, ind);
                return;
            }
            case Stmt::PopIfNot: {
                std::string c = StripParens(s.cond);
                auto loop = LoopAtDepth(ctx);
                if (loop) {
                    loop->chainBreak = true;
                    Line(out, ind, "if (" + StripParens(Neg(s.cond)) + ") break;");
                    off = s.next;
                    continue;
                }
                if (ctx.flow.empty() || IsReturnLabel(ctx.flow.back())) {
                    Line(out, ind, "if (" + StripParens(Neg(s.cond)) + ") " + (ctx.flow.empty() ? std::string("return;") : ReturnText(ctx.flow.back())));
                    off = s.next;
                    continue;
                }
                std::vector<std::string> body;
                Emit(s.next, ctx, body, 0);
                if (!body.empty()) {
                    Line(out, ind, "if (" + c + ")");
                    Line(out, ind, "{");
                    EmitBlock(out, ind + 1, body);
                    Line(out, ind, "}");
                }
                return;
            }
            case Stmt::IfNot: {
                std::set<u32> term = ctx.stops;
                for (auto& l : ctx.loops) {
                    term.insert(l->head);
                    if (l->exit >= 0) term.insert((u32)l->exit);
                }
                auto ra = Reach(s.next, term), rb = Reach(s.target, term);
                i64 join = -1;
                int best = 1 << 30;
                // the function's return is a join only where the chain would end there anyway (not inside a loop or a pushed chain)
                bool returnJoins = ctx.loops.empty() && (ctx.flow.empty() || IsReturnLabel(ctx.flow.back()));
                for (auto& [o, da] : ra) {
                    if (!returnJoins && IsReturnLabel(o)) continue;
                    auto it = rb.find(o);
                    if (it != rb.end() && da + it->second < best) { best = da + it->second; join = o; }
                }
                Ctx bc = ctx;
                if (join >= 0) bc.stops.insert((u32)join);
                std::vector<std::string> thenL, elseL;
                if (join != (i64)s.next) Emit(s.next, bc, thenL, 0);
                if (join != (i64)s.target) Emit(s.target, bc, elseL, 0);
                std::string c = StripParens(s.cond);
                auto terminal = [](const std::vector<std::string>& l) {
                    return l.size() == 1 && (l[0] == "return;" || IStartsWith(l[0], "return ") || l[0] == "break;" || l[0] == "continue;");
                };
                if (terminal(thenL) && !elseL.empty()) {   // if (c) return; then the other branch at this level
                    Line(out, ind, "if (" + c + ") " + thenL[0]);
                    EmitBlock(out, ind, elseL);
                } else if (terminal(elseL) && !thenL.empty()) {
                    Line(out, ind, "if (" + StripParens(Neg(s.cond)) + ") " + elseL[0]);
                    EmitBlock(out, ind, thenL);
                } else if (thenL.empty() && !elseL.empty()) {
                    Line(out, ind, "if (" + StripParens(Neg(s.cond)) + ")");
                    Line(out, ind, "{");
                    EmitBlock(out, ind + 1, elseL);
                    Line(out, ind, "}");
                } else if (!thenL.empty()) {
                    Line(out, ind, "if (" + c + ")");
                    Line(out, ind, "{");
                    EmitBlock(out, ind + 1, thenL);
                    Line(out, ind, "}");
                    if (!elseL.empty()) {
                        // else if: the else branch is a single if chain
                        bool chain = IStartsWith(elseL[0], "if (") && elseL.back() == "}";
                        if (chain) {
                            int depth = 0;
                            for (size_t k = 1; k + 1 < elseL.size() && chain; k++) {
                                const std::string& l = elseL[k];
                                if (l == "{") depth++;
                                else if (l == "}") depth--;
                                else if (depth == 0 && l != "else" && !IStartsWith(l, "else if (")) chain = false;
                            }
                        }
                        if (chain) {
                            out.push_back(std::string((size_t)ind * 4, ' ') + "else " + elseL[0]);
                            for (size_t k = 1; k < elseL.size(); k++) out.push_back(std::string((size_t)ind * 4, ' ') + elseL[k]);
                        } else if (thenL.back() == "return;" || IStartsWith(thenL.back(), "return ") || thenL.back() == "break;" || thenL.back() == "continue;") {
                            EmitBlock(out, ind, elseL);   // the then branch leaves: no else needed
                        } else {
                            Line(out, ind, "else");
                            Line(out, ind, "{");
                            EmitBlock(out, ind + 1, elseL);
                            Line(out, ind, "}");
                        }
                    }
                }
                if (join < 0) return;
                off = (u32)join;
                continue;
            }
            }
        }
    }

    std::shared_ptr<LoopState> LoopAtDepth(const Ctx& ctx) const {
        if (ctx.loops.empty()) return nullptr;
        auto& l = ctx.loops.back();
        return l->depth == ctx.flow.size() && !(ctx.flow.empty() || IsReturnLabel(ctx.flow.back())) ? l : nullptr;
    }

    // PopExecutionFlow: the chain ends; resumes the innermost pushed address (the caller's continuation) or returns
    void EndChain(const Ctx& ctx, std::vector<std::string>& out, int ind) {
        if (auto loop = LoopAtDepth(ctx)) {
            loop->chainBreak = true;
            Line(out, ind, "break;");
            return;
        }
        if (ctx.flow.empty()) { Line(out, ind, "return;"); return; }
        if (IsReturnLabel(ctx.flow.back())) Line(out, ind, ReturnText(ctx.flow.back()));
    }

    void EmitLoop(u32 head, const Ctx& ctx, std::vector<std::string>& out, int ind) {
        // natural loop: statements reachable from the head that reach a jump back to it
        std::set<u32> term = ctx.stops;
        for (auto& l : ctx.loops) {
            term.insert(l->head);
            if (l->exit >= 0) term.insert((u32)l->exit);
        }
        auto fwd = Reach(head, term);
        std::map<u32, std::vector<u32>> pred;
        for (auto& [o, d] : fwd) {
            const Stmt* s = StmtAt(o);
            if (!s || (term.count(o) && o != head)) continue;
            for (u32 n : Succ(*s)) pred[n].push_back(o);
        }
        std::set<u32> body{head};
        std::vector<u32> q;
        for (u32 p : pred[head]) if (fwd.count(p)) q.push_back(p);
        if (q.empty()) {   // a jump back to shared code, not a loop
            Emit(head, ctx, out, ind, true);
            return;
        }
        while (!q.empty()) {
            u32 o = q.back();
            q.pop_back();
            if (!body.insert(o).second) continue;
            for (u32 p : pred[o]) if (!body.count(p)) q.push_back(p);
        }
        // a chain pushed inside the loop (a Sequence output, the loop body of ForEach) returns into it when it ends
        // (joins of the code around the loop don't bound it: shared code reached there pops back into the loop too)
        std::set<u32> loopTerm{head};
        for (auto& l : ctx.loops) {
            loopTerm.insert(l->head);
            if (l->exit >= 0) loopTerm.insert((u32)l->exit);
        }
        for (bool grew = true; grew;) {
            grew = false;
            const std::set<u32>& t2 = loopTerm;
            for (u32 o : std::vector<u32>(body.begin(), body.end())) {
                const Stmt* s = StmtAt(o);
                if (!s || s->kind != Stmt::Push) continue;
                for (auto& [n, d] : Reach(s->next, t2))
                    if (!loopTerm.count(n) && body.insert(n).second) grew = true;
            }
        }
        std::set<u32> exits;
        for (u32 o : body) {
            const Stmt* s = StmtAt(o);
            if (!s) continue;
            for (u32 n : Succ(*s))
                if (!body.count(n) && !IsReturnLabel(n)) exits.insert(n);   // a jump to the function's return is a return inside the loop
        }
        // exits that only lead to the end of the chain can be written inside the loop (followed by break / return)
        auto inlinable = [&](const std::set<u32>& ex) {
            for (u32 e : ex)
                for (auto& [n, d] : Reach(e, {}))
                    if (ctx.stops.count(n) || n == head) return false;
                    else for (auto& l : ctx.loops) if (n == l->head || (l->exit >= 0 && n == (u32)l->exit)) return false;
            return true;
        };
        bool inlineExits = false;
        if (exits.size() > 1) {
            if (!inlinable(exits)) throw StructFail(Fmt("loop at %u with several exits", head));
            inlineExits = true;
        }
        std::shared_ptr<LoopState> loop;
        std::vector<std::string> bodyL;
        for (int attempt = 0;; attempt++) {
            loop = std::make_shared<LoopState>();
            loop->head = head;
            loop->exit = exits.empty() || inlineExits ? -1 : (i64)*exits.begin();
            loop->depth = ctx.flow.size();
            Ctx inner = ctx;
            inner.stops.clear();   // joins outside the loop are left through its exit
            inner.loops.push_back(loop);
            bodyL.clear();
            Emit(head, inner, bodyL, 0, true);
            if (loop->chainBreak && loop->exit >= 0) {
                if (attempt == 0 && inlinable(exits)) { inlineExits = true; continue; }
                throw StructFail(Fmt("loop at %u leaving both by jump and by chain end", head));
            }
            break;
        }
        while (!bodyL.empty() && bodyL.back() == "continue;") bodyL.pop_back();
        std::string cond = "true";
        bool returnsAfter = false;
        if (!bodyL.empty() && IStartsWith(bodyL[0], "if (") && IEndsWith(bodyL[0], ") break;")) {
            cond = StripParens(Neg(Wrap(bodyL[0].substr(4, bodyL[0].size() - 4 - 8))));
            bodyL.erase(bodyL.begin());
        } else if (!bodyL.empty() && IStartsWith(bodyL[0], "if (") && loop->exit < 0 && !loop->chainBreak && !ctx.flow.empty() &&
                   IsReturnLabel(ctx.flow.back()) && IEndsWith(bodyL[0], ") " + ReturnText(ctx.flow.back()))) {
            // the loop only ends by returning when its condition fails: while (condition), then the return
            std::string tail = ") " + ReturnText(ctx.flow.back());
            cond = StripParens(Neg(Wrap(bodyL[0].substr(4, bodyL[0].size() - 4 - tail.size()))));
            bodyL.erase(bodyL.begin());
            returnsAfter = true;
        }
        Line(out, ind, "while (" + cond + ")");
        Line(out, ind, "{");
        EmitBlock(out, ind + 1, bodyL);
        Line(out, ind, "}");
        if (returnsAfter) { Line(out, ind, ReturnText(ctx.flow.back())); return; }
        if (loop->chainBreak) { EndChainAfterLoop(ctx, out, ind); return; }
        if (loop->exit >= 0) Emit((u32)loop->exit, ctx, out, ind);
    }
    void EndChainAfterLoop(const Ctx& ctx, std::vector<std::string>& out, int ind) {
        if (auto loop = LoopAtDepth(ctx)) {
            loop->chainBreak = true;
            Line(out, ind, "break;");
        }
    }

    static void TrimTrailingReturn(std::vector<std::string>& lines) {
        while (!lines.empty() && (lines.back() == "return;")) lines.pop_back();
    }

    // ------------------------------------------------------------- flat output (raw, and the fallback)

    std::string Flat(const FuncInfo& f, const std::vector<Stmt>& st, const std::string& error, const std::string& indent) {
        std::string out;
        std::set<u32> tg = targets;
        for (auto& [o, n] : entryNames) if (IsUbergraph(f)) tg.insert(o);
        for (auto& [o, n] : pendingEntries) if (IsUbergraph(f)) tg.insert(o);
        for (size_t i = 0; i < st.size(); i++) {
            const Stmt& s = st[i];
            if (tg.count(s.off)) {
                out += indent.substr(0, indent.size() >= 4 ? indent.size() - 4 : 0) + Label(s.off) + ":";
                auto en = entryNames.find(s.off);
                auto pe = pendingEntries.find(s.off);
                if (en != entryNames.end() && IsUbergraph(f)) out += "   // entry of " + en->second;
                else if (pe != pendingEntries.end() && IsUbergraph(f)) out += "   // " + pe->second;
                out += "\n";
            }
            std::string line;
            switch (s.kind) {
            case Stmt::Normal: line = s.text; break;
            case Stmt::Return: line = s.text.empty() ? "return" : s.text; break;
            case Stmt::Goto: line = "goto " + Label(s.target); break;
            case Stmt::IfNot: line = "if (" + StripParens(Neg(s.cond)) + ") goto " + Label(s.target); break;
            case Stmt::Push: line = "ExecutionFlow.Push(" + Label(s.target) + ")"; break;
            case Stmt::Pop: line = "goto ExecutionFlow.Pop()"; break;
            case Stmt::PopIfNot: line = "if (" + StripParens(Neg(s.cond)) + ") goto ExecutionFlow.Pop()"; break;
            case Stmt::Computed: line = "goto (" + s.text + ")"; break;
            }
            if (line.empty()) continue;
            if (line == "return" && i + 1 == st.size()) continue;
            bool block = line.rfind("switch", 0) == 0;
            out += indent + line + (block ? "\n" : ";\n");
        }
        if (out.size() >= 2 && out.compare(out.size() - 2, 2, ":\n") == 0) out += indent + "return;\n";
        if (!error.empty()) out += indent + "// decompilation stopped: " + error + "\n";
        return out;
    }

    // ------------------------------------------------------------- class output

    std::string Signature(const FuncInfo& f, bool withClass) {
        std::string ret = "void";
        std::vector<std::string> args;
        for (auto& p : f.params) {
            if (p.flags & CPF_ReturnParm) { ret = p.type; continue; }
            bool out = (p.flags & CPF_OutParm) && !(p.flags & CPF_ConstParm);
            bool ref = (p.flags & CPF_ReferenceParm) != 0;
            args.push_back(std::string(p.flags & CPF_ConstParm ? "const " : "") + p.type + (out || ref ? "& " : " ") + p.name);
        }
        std::string s = ret + " " + (withClass ? clsCpp + "::" : "") + SafeIdent(f.name) + "(";
        for (size_t i = 0; i < args.size(); i++) s += (i ? ", " : "") + args[i];
        s += ")";
        if (f.flags & FUNC_Const) s += " const";
        return s;
    }

    static std::string FlagNames(u32 f) {
        static const std::pair<u32, const char*> names[] = {{FUNC_Final, "Final"}, {FUNC_Net, "Net"}, {FUNC_UbergraphFunction, "Ubergraph"},
                                                            {FUNC_Event, "Event"}, {FUNC_Static, "Static"}, {FUNC_MulticastDelegate, "MulticastDelegate"},
                                                            {FUNC_Public, "Public"}, {FUNC_Private, "Private"}, {FUNC_Protected, "Protected"},
                                                            {FUNC_Delegate, "Delegate"}, {FUNC_BlueprintCallable, "BlueprintCallable"},
                                                            {FUNC_BlueprintEvent, "BlueprintEvent"}, {FUNC_BlueprintPure, "BlueprintPure"}, {FUNC_Const, "Const"}};
        std::string s;
        for (auto& [bit, n] : names) if (f & bit) s += (s.empty() ? "" : " | ") + std::string(n);
        return s;
    }

    static std::string Join(const std::vector<std::string>& lines, const std::string& indent) {
        std::string s;
        for (auto& l : lines) s += (l.empty() ? "" : indent) + l + "\n";
        return s;
    }

    // locals referenced by a body, declared at its top
    static std::string Declarations(const std::vector<PropInfo>& vars, const std::string& body, const std::string& indent) {
        std::string s;
        for (auto& v : vars)
            if (CountIdent(body, v.name)) s += indent + v.type + " " + v.name + (v.arrayDim > 1 ? Fmt("[%d]", v.arrayDim) : "") + ";\n";
        return s.empty() ? s : s + "\n";
    }

    struct Body {
        std::string code;      // statements (indented)
        std::string locals;    // declarations
    };

    // a regular function: structured, or flat when the flow does not structure
    Body FunctionBody(const FuncInfo& f) {
        Body b;
        std::string error;
        scopeNames.clear();
        for (auto& p : f.params) scopeNames.insert(p.name);
        for (auto& p : f.locals) scopeNames.insert(p.name);
        SetScopeTypes(f);
        renames.clear();
        outArgs.clear();
        counting++;
        auto st = Parse(f, error);
        counting--;
        if (opt.raw || st.empty()) {
            b.code = Flat(f, st, error, "    ");
        } else {
            std::set<std::string> temps;
            for (auto& l : f.locals) temps.insert(l.name);
            Inline(st, temps, {});
            Prepare(st);
            budget = 0;
            pendingResume.clear();
            try {
                std::vector<std::string> lines;
                Ctx ctx;
                Emit(st[0].off, ctx, lines, 0);
                TrimTrailingReturn(lines);
                b.code = Join(lines, "    ");
                if (!error.empty()) b.code += "    // decompilation stopped: " + error + "\n";
            } catch (const StructFail& e) {
                b.code = "    // control flow kept as jumps (" + std::string(e.what()) + ")\n" + Flat(f, st, error, "    ");
            }
        }
        b.locals = Declarations(f.locals, b.code, "    ");
        return b;
    }

    bool Run(std::string& out, std::string& err) {
        for (size_t i = 0; i < pkg.Exports().size(); i++)
            if (pkg.ExportClass(i) == "BlueprintGeneratedClass") { clsIndex = (i32)i + 1; break; }
        if (!clsIndex) { err = "no BlueprintGeneratedClass in this package"; return false; }
        clsName = pkg.Exports()[clsIndex - 1].objectName;
        clsCpp = ClassCpp(pkg, clsIndex);
        std::string super = "UObject";
        {
            Reader r;
            if (BodyReader(pkg, (size_t)clsIndex - 1, r)) {
                try {
                    r.I32();
                    if (i32 sup = r.I32()) super = ClassCpp(pkg, sup);
                } catch (const std::exception&) {
                }
            }
        }
        std::vector<PropInfo> vars;
        for (size_t i = 0; i < pkg.Exports().size(); i++) {
            if (pkg.Exports()[i].outerIndex != clsIndex) continue;
            std::string c = pkg.ExportClass(i);
            if (c == "Function") {
                FuncInfo f;
                std::string e;
                if (ReadFunction(pkg, i, f, e)) funcs.push_back(std::move(f));
                else out += "// " + pkg.Exports()[i].objectName + ": " + e + "\n";
            } else if (IEndsWith(c, "Property")) {
                vars.push_back(ReadProp(pkg, i));
            }
        }
        for (auto& f : funcs) ownRefl[f.index] = ToRefl(f);
        for (auto& v : vars) varTypes[v.name] = v.type;
        // ubergraph entry points come from the event stubs
        const FuncInfo* uber = nullptr;
        for (auto& f : funcs) {
            if (f.eventGraph) entryNames[(u32)f.eventGraphOffset] = f.name;
            if (IsUbergraph(f)) uber = &f;
        }
        if (uber)
            for (auto& f : funcs) {
                i64 e = StubEntry(f, *uber);
                if (e >= 0) { stubEntries[f.index] = e; entryNames[(u32)e] = f.name; }
            }

        // ---- function bodies
        std::map<i32, Body> bodies;
        std::vector<PropInfo> frameVars;      // ubergraph state kept between events
        bool uberExtracted = false;
        std::string uberRest;                 // ubergraph code no event covers (latent resumes, ...)
        for (auto& f : funcs) {
            if ((f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) || IsUbergraph(f) || (stubEntries.count(f.index) && !opt.raw)) continue;
            bodies[f.index] = FunctionBody(f);
        }
        if (uber) {
            if (!opt.raw && ExtractEvents(*uber, bodies, frameVars, uberRest)) {
                uberExtracted = true;
            } else {
                // flat ubergraph, events as stubs
                bodies[uber->index] = FunctionBodyFlat(*uber);
                if (!splitFail.empty()) bodies[uber->index].code = "    // event graph kept whole (" + splitFail + ")\n" + bodies[uber->index].code;
                for (auto& f : funcs)
                    if (stubEntries.count(f.index)) bodies[f.index] = FunctionBodyFlat(f);
            }
        }

        out += "// Decompiled by SOTModel from " + pkg.GamePath() + "\n";
        if (opt.raw) out += "// Raw flow: Label_N is the script offset a jump or event lands on.\n\n";
        else
            out += "// Control flow is rebuilt from the bytecode jumps; events show the event graph code they run.\n"
                   "// Native calls use the game's reflected signatures (/*Name*/ marks the parameter a literal goes to).\n\n";
        for (auto& f : funcs)
            if (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) {
                std::string n = f.name;
                size_t p = n.find("__DelegateSignature");
                if (p != std::string::npos) n.resize(p);
                out += "// " + std::string(f.flags & FUNC_MulticastDelegate ? "multicast " : "") + "delegate " + n + "\n";
                out += "DECLARE_DYNAMIC_DELEGATE(" + Signature(f, false) + ");\n\n";
            }
        out += "class " + clsCpp + " : public " + super + "\n{\npublic:\n";
        for (auto& v : vars) {
            if (v.name == "UberGraphFrame") continue;
            out += "    " + v.type + " " + v.name + (v.arrayDim > 1 ? Fmt("[%d]", v.arrayDim) : "") + ";\n";
        }
        if (!frameVars.empty()) {
            out += "\n    // event graph state kept between events (the ubergraph's persistent frame)\n";
            for (auto& v : frameVars) out += "    " + v.type + " " + v.name + ";\n";
        }
        out += "\n";
        for (auto& f : funcs) {
            if (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) continue;
            if (uberExtracted && IsUbergraph(f) && uberRest.empty()) continue;
            out += "    " + std::string(f.flags & FUNC_Static ? "static " : f.flags & FUNC_BlueprintEvent ? "virtual " : "") + Signature(f, false) + ";\n";
        }
        out += "};\n";
        for (auto& f : funcs) {
            if (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) continue;
            if (uberExtracted && IsUbergraph(f) && uberRest.empty()) continue;
            std::string fl = FlagNames(f.flags);
            if (IsUbergraph(f) && !(f.flags & FUNC_UbergraphFunction)) fl = fl.empty() ? "Ubergraph" : "Ubergraph | " + fl;
            out += "\n// " + fl + "\n";
            if (f.eventGraph && !uberExtracted) out += Fmt("// event: runs %s at Label_%d\n", RefName(f.eventGraph).c_str(), f.eventGraphOffset);
            out += Signature(f, true) + "\n{\n";
            const Body& b = bodies[f.index];
            if (uberExtracted && IsUbergraph(f)) out += uberRest;
            else out += b.locals + b.code;
            out += "}\n";
        }
        return true;
    }

    Body FunctionBodyFlat(const FuncInfo& f) {
        Body b;
        std::string error;
        scopeNames.clear();
        for (auto& p : f.params) scopeNames.insert(p.name);
        for (auto& p : f.locals) scopeNames.insert(p.name);
        SetScopeTypes(f);
        renames.clear();
        counting++;
        auto st = Parse(f, error);
        counting--;
        b.code = Flat(f, st, error, "    ");
        b.locals = Declarations(f.locals, b.code, "    ");
        return b;
    }

    // an event stub: copies its parameters into the ubergraph frame and calls ExecuteUbergraph_X(entry); -1 otherwise
    i64 StubEntry(const FuncInfo& f, const FuncInfo& uber, std::map<std::string, std::string>* params = nullptr) {
        if (&f == &uber || (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate))) return -1;
        std::string e;
        scopeNames.clear();
        for (auto& p : f.params) scopeNames.insert(p.name);
        SetScopeTypes(f);
        renames.clear();
        auto st = Parse(f, e);
        if (!e.empty()) return -1;
        i64 entry = -1;
        for (auto& s : st) {
            if (s.kind == Stmt::Return || (s.kind == Stmt::Normal && s.text.empty())) continue;
            if (s.kind != Stmt::Normal) return -1;
            if (IStartsWith(s.text, "UberGraphFrame->")) {
                size_t eq = s.text.find(" = ");
                if (eq == std::string::npos) return -1;
                if (params) (*params)[s.text.substr(16, eq - 16)] = s.text.substr(eq + 3);
                continue;
            }
            std::string call = SafeIdent(uber.name) + "(";
            if (!IStartsWith(s.text, call) || entry >= 0) return -1;
            size_t d = s.text.find_first_of("0123456789", call.size());
            if (d == std::string::npos) return -1;
            entry = atoll(s.text.c_str() + d);
        }
        return entry;
    }

    std::map<i32, i64> stubEntries;   // event function -> ubergraph entry
    std::string splitFail;            // why the event graph could not be split into events

    // each event's body = the ubergraph code from its entry, with the event parameters in place of their frame copies
    bool ExtractEvents(const FuncInfo& uber, std::map<i32, Body>& bodies, std::vector<PropInfo>& frameVars, std::string& rest) {
        std::string error;
        outArgs.clear();
        separateLatent = false;
        struct Ev {
            const FuncInfo* f;
            i64 entry;
            std::map<std::string, std::string> params;   // frame variable -> parameter
        };
        std::vector<Ev> events;
        for (auto& f : funcs) {
            auto it = stubEntries.find(f.index);
            if (it == stubEntries.end()) continue;
            Ev ev{&f, it->second, {}};
            StubEntry(f, uber, &ev.params);
            events.push_back(std::move(ev));
        }
        renames.clear();
        scopeNames.clear();
        for (auto& p : uber.locals) scopeNames.insert(p.name);
        SetScopeTypes(uber);
        counting++;
        auto st = Parse(uber, error);
        counting--;
        if (!error.empty() || st.empty()) return false;
        std::set<std::string> temps;
        for (auto& l : uber.locals) temps.insert(l.name);
        std::set<u32> entries;
        for (auto& [o, n] : entryNames) entries.insert(o);
        for (auto& [o, n] : pendingEntries) entries.insert(o);
        Inline(st, temps, entries);
        Prepare(st);
        std::vector<u32> sentinel;
        if (st[0].kind == Stmt::Push && IsReturnLabel(st[0].target)) sentinel.push_back(st[0].target);
        std::set<u32> rendered;   // entries shown somewhere
        std::map<std::string, int> useCount;   // frame variable -> events using it
        std::map<i32, std::string> evCode;
        try {
            for (auto& ev : events) {
                renames = {};
                budget = 0;
                pendingResume.clear();
                std::vector<std::string> lines;
                Ctx ctx;
                ctx.flow = sentinel;
                if (!StmtAt((u32)ev.entry)) throw StructFail("event entry outside the ubergraph");
                try {
                    Emit((u32)ev.entry, ctx, lines, 0);
                } catch (const StructFail&) {
                    if (separateLatent) throw;
                    separateLatent = true;   // a loop through latent actions: show the resumes as sections instead
                    Prepare(st);
                    lines.clear();
                    budget = 0;
                    pendingResume.clear();
                    Emit((u32)ev.entry, ctx, lines, 0);
                }
                TrimTrailingReturn(lines);
                rendered.insert((u32)ev.entry);
                for (u32 r : inlinedResume) rendered.insert(r);
                // latent resumes that could not follow their call inline
                std::set<u32> done;
                while (true) {
                    u32 r = 0;
                    bool any = false;
                    for (u32 p : pendingResume) if (!done.count(p)) { r = p; any = true; break; }
                    if (!any) break;
                    done.insert(r);
                    rendered.insert(r);
                    if (!lines.empty() && lines.back() != "return;") lines.push_back("return;");
                    lines.push_back("");
                    lines.push_back(Label(r) + ":   // resumed when the latent action completes");
                    Ctx c2;
                    c2.flow = sentinel;
                    std::vector<std::string> more;
                    Emit(r, c2, more, 0);
                    TrimTrailingReturn(more);
                    for (auto& l : more) lines.push_back(l);
                }
                std::string code = Join(lines, "    ");
                for (auto& [frameVar, param] : ev.params) ReplaceIdent(code, frameVar, param, true);
                evCode[ev.f->index] = code;
                for (auto& l : uber.locals)
                    if (CountIdent(code, l.name)) useCount[l.name]++;
            }
            // entries no event covers (resumes of latent actions started elsewhere, ...)
            std::vector<std::string> restLines;
            for (u32 e : entries) {
                if (rendered.count(e) || !StmtAt(e)) continue;
                budget = 0;
                pendingResume.clear();
                restLines.push_back(Label(e) + ":   // " + (pendingEntries.count(e) ? pendingEntries[e] : "entry of " + entryNames[e]));
                Ctx c;
                c.flow = sentinel;
                std::vector<std::string> more;
                Emit(e, c, more, 0);
                TrimTrailingReturn(more);
                for (auto& l : more) restLines.push_back("    " + l);
                restLines.push_back("    return;");
            }
            if (!restLines.empty()) {
                std::string r;
                for (auto& l : restLines) r += l + "\n";
                rest = "    switch (EntryPoint) { /* entries below */ }\n" + r;
                for (auto& l : uber.locals)
                    if (CountIdent(rest, l.name)) useCount[l.name] += 2;
            }
        } catch (const StructFail& e) {
            renames.clear();
            splitFail = e.what();
            separateLatent = false;
            return false;
        }
        separateLatent = false;
        // locals of one event stay in it; state shared between events (or macro state such as DoOnce / loop counters) lives in the frame
        for (auto& ev : events) {
            Body b;
            b.code = evCode[ev.f->index];
            std::vector<PropInfo> mine;
            for (auto& l : uber.locals)
                if (useCount[l.name] == 1 && !IStartsWith(l.name, "Temp_") && CountIdent(b.code, l.name)) mine.push_back(l);
            b.locals = Declarations(mine, b.code, "    ");
            bodies[ev.f->index] = std::move(b);
        }
        for (auto& l : uber.locals)
            if (useCount[l.name] > 1 || (useCount[l.name] == 1 && IStartsWith(l.name, "Temp_"))) frameVars.push_back(l);
        return true;
    }

    const Package& pkg;
    BlueprintOptions opt;
    i32 clsIndex = 0;
    std::string clsName, clsCpp;
    std::vector<FuncInfo> funcs;
    std::map<i32, ReflFunction> ownRefl;
    std::map<std::string, std::string> varTypes;      // class variable -> C++ type
    std::map<std::string, std::string> scopeTypes;    // parameter / local of the function being decompiled -> C++ type
    void SetScopeTypes(const FuncInfo& f) {
        scopeTypes.clear();
        for (auto& p : f.params) scopeTypes[p.name] = p.type;
        for (auto& p : f.locals) scopeTypes[p.name] = p.type;
    }
    std::map<std::string, std::string> enumPaths;     // enum name -> object path
    std::map<std::string, std::vector<std::pair<long long, std::string>>> bpEnums;
    std::map<std::string, std::shared_ptr<Package>> loaded;
    std::map<std::string, char> prefixMemo;
    std::map<std::string, std::unique_ptr<ReflFunction>> extFuncs;
};

}   // namespace

bool IsBlueprintPackage(const Package& pkg) {
    for (size_t i = 0; i < pkg.Exports().size(); i++)
        if (pkg.ExportClass(i) == "BlueprintGeneratedClass") return true;
    return false;
}

bool DecompileBlueprint(const Package& pkg, std::string& out, std::string* error, const BlueprintOptions& options) {
    Decompiler d(pkg, options);
    std::string err;
    try {
        if (d.Run(out, err)) return true;
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (error) *error = err;
    return false;
}
