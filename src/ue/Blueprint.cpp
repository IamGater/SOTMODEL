// Blueprint decompiler (UE4.10 / object version 483).
// Layouts after an export's tagged properties and object guid:
//   UField     Next (index)
//   UProperty  ArrayDim (i32), PropertyFlags (u64), RepNotifyFunc (FName), then per type: class / struct / inner refs
//   UStruct    SuperStruct (index), Children (index), BytecodeBufferSize (i32), SerializedScriptSize (i32), script
//   UFunction  FunctionFlags (u32), [RepOffset (u16) when FUNC_Net], EventGraphFunction (index), EventGraphCallOffset (i32)
// Script: object references are 4 bytes on disk but 8 in memory, FNames 8 on disk and 12 in memory; jump offsets are
// in-memory offsets, so the reader keeps both positions.
#include "ue/Blueprint.h"
#include <map>
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
};

struct FuncInfo;
bool IsUbergraph(const FuncInfo& f);
struct FuncInfo {
    i32 index = 0;   // export, 1-based
    std::string name;
    u32 flags = 0;
    Bytes script;
    i32 eventGraph = 0, eventGraphOffset = 0;
    std::vector<PropInfo> params, locals;
};

bool IsUbergraph(const FuncInfo& f) { return (f.flags & FUNC_UbergraphFunction) || IStartsWith(f.name, "ExecuteUbergraph"); }

// prefix-less object name -> C++ style type names
std::string StructType(const std::string& n) { return n.empty() ? "FStruct" : "F" + n; }

class Decompiler {
public:
    explicit Decompiler(const Package& p) : pkg(p) {}

    // reader positioned after the tagged properties and the object guid of an export
    Reader BodyReader(size_t exportIdx) {
        size_t end = 0;
        std::string err;
        pkg.ReadProperties(exportIdx, &end, &err);
        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32()) r.Skip(16);
        return r;
    }

    std::string RefName(i32 idx) const { return idx ? pkg.ObjectName(idx) : "None"; }

    std::string PropertyType(size_t exportIdx, int depth = 0) {
        std::string cls = pkg.ExportClass(exportIdx);
        if (depth > 4) return cls;
        try {
            Reader r = BodyReader(exportIdx);
            r.I32();   // Next
            r.I32();   // ArrayDim
            r.U64();   // PropertyFlags
            pkg.ReadName(r);   // RepNotifyFunc
            auto ref = [&] { return r.I32(); };
            if (cls == "ObjectProperty") return RefName(ref()) + "*";
            if (cls == "WeakObjectProperty") return "TWeakObjectPtr<" + RefName(ref()) + ">";
            if (cls == "LazyObjectProperty") return "TLazyObjectPtr<" + RefName(ref()) + ">";
            if (cls == "AssetObjectProperty") return "TAssetPtr<" + RefName(ref()) + ">";
            if (cls == "ClassProperty") { ref(); return "TSubclassOf<" + RefName(ref()) + ">"; }
            if (cls == "AssetClassProperty") { ref(); return "TAssetSubclassOf<" + RefName(ref()) + ">"; }
            if (cls == "InterfaceProperty") return "TScriptInterface<" + RefName(ref()) + ">";
            if (cls == "StructProperty") return StructType(RefName(ref()));
            if (cls == "ByteProperty") { i32 e = ref(); return e ? "TEnumAsByte<" + RefName(e) + ">" : "uint8"; }
            if (cls == "ArrayProperty") {
                i32 inner = ref();
                return "TArray<" + (inner > 0 ? PropertyType((size_t)inner - 1, depth + 1) : RefName(inner)) + ">";
            }
            if (cls == "DelegateProperty") return "FScriptDelegate /* " + RefName(ref()) + " */";
            if (cls == "MulticastDelegateProperty") return "FMulticastScriptDelegate /* " + RefName(ref()) + " */";
        } catch (const std::exception&) {
        }
        static const std::map<std::string, std::string> simple = {
            {"BoolProperty", "bool"}, {"IntProperty", "int32"}, {"Int8Property", "int8"}, {"Int16Property", "int16"}, {"Int64Property", "int64"},
            {"UInt16Property", "uint16"}, {"UInt32Property", "uint32"}, {"UInt64Property", "uint64"}, {"FloatProperty", "float"},
            {"DoubleProperty", "double"}, {"NameProperty", "FName"}, {"StrProperty", "FString"}, {"TextProperty", "FText"}};
        auto it = simple.find(cls);
        return it != simple.end() ? it->second : cls;
    }

    PropInfo ReadProp(size_t exportIdx) {
        PropInfo p;
        p.name = pkg.Exports()[exportIdx].objectName;
        p.type = PropertyType(exportIdx);
        try {
            Reader r = BodyReader(exportIdx);
            r.I32();
            p.arrayDim = r.I32();
            p.flags = r.U64();
        } catch (const std::exception&) {
        }
        return p;
    }

    bool ReadFunction(size_t exportIdx, FuncInfo& f, std::string& err) {
        f.index = (i32)exportIdx + 1;
        f.name = pkg.Exports()[exportIdx].objectName;
        try {
            Reader r = BodyReader(exportIdx);
            r.I32();   // Next
            r.I32();   // SuperStruct
            r.I32();   // Children
            r.I32();   // BytecodeBufferSize (in memory)
            i32 serialized = r.I32();
            if (serialized < 0 || (size_t)serialized > r.Remaining()) throw ParseError("bad script size");
            f.script = r.ReadBytes((size_t)serialized);
            f.flags = r.U32();
            if (f.flags & FUNC_Net) r.U16();
            f.eventGraph = r.I32();
            f.eventGraphOffset = r.I32();
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        }
        // parameters and locals: property exports inside the function, in export order
        for (size_t i = 0; i < pkg.Exports().size(); i++) {
            const auto& ex = pkg.Exports()[i];
            if (ex.outerIndex != f.index || !IEndsWith(pkg.ExportClass(i), "Property")) continue;
            PropInfo p = ReadProp(i);
            if (p.flags & CPF_Parm) f.params.push_back(p);
            else f.locals.push_back(p);
        }
        return true;
    }

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

    std::set<u32> targets;   // jump destinations (in-memory offsets)
    std::set<std::string> scopeNames;   // parameters / locals of the function being decompiled (members get this-> when shadowed)
    std::string currentFunc;
    std::map<u32, std::string> entryNames;   // ubergraph entry offset -> event name
    std::map<u32, std::string> pendingEntries;   // latent resume points found while decompiling (the ubergraph itself)

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
    std::string Label(u32 off) const { return Fmt("Label_%u", off); }

    std::string Args(Script& s) {
        std::string out;
        for (;;) {
            u8 peek = (*s.data)[s.pos];
            if (peek == 0x16) { s.Read<u8>(); break; }   // EX_EndFunctionParms
            if (!out.empty()) out += ", ";
            out += Expr(s);
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
    std::string FuncOwner(i32 func) const {
        if (func < 0) {
            const auto& im = pkg.Imports()[-func - 1];
            return im.outerIndex ? pkg.ObjectName(im.outerIndex) : "";
        }
        return "";
    }

    std::string Expr(Script& s) {
        u8 op = s.Read<u8>();
        switch (op) {
        case 0x00: case 0x02: case 0x48:   // Local / Default / LocalOut variable
            return RefName(s.Obj());
        case 0x01: {   // InstanceVariable
            std::string n = RefName(s.Obj());
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
            return "if (!(" + Expr(s) + ")) goto " + Label(t);
        }
        case 0x09: { s.Read<u16>(); s.Read<u8>(); return "check(" + Expr(s) + ")"; }
        case 0x0B: return "";
        case 0x0C: s.Read<i32>(); return "";
        case 0x0F: {   // Let
            s.Obj();
            std::string var = Expr(s);
            return var + " = " + Expr(s);
        }
        case 0x11: { s.Obj(); return std::to_string(s.Read<u8>()); }
        case 0x12: case 0x19: case 0x1A: {   // ClassContext / Context / Context_FailSilent
            std::string obj = Expr(s);
            s.Read<u32>();   // skip offset
            s.Obj();         // r-value property
            std::string ctx = Expr(s);
            if (op == 0x12) return obj + "::" + ctx;   // class default object
            size_t cdo = obj.find("Default__");
            if (cdo != std::string::npos && obj.find_first_of("(-", cdo) == std::string::npos)   // static library call through the CDO
                return "U" + obj.substr(cdo + 9) + "::" + ctx;
            return obj + "->" + ctx;
        }
        case 0x13: { i32 c = s.Obj(); return "Cast<UClass>(" + Expr(s) + ") /* " + RefName(c) + " */"; }
        case 0x14: case 0x5F: case 0x60: case 0x44: case 0x43: {   // LetBool / LetObj / LetWeakObjPtr / LetDelegate / LetMulticastDelegate
            std::string var = Expr(s);
            return var + " = " + Expr(s);
        }
        case 0x15: case 0x16: return "";
        case 0x17: return "this";
        case 0x18: { s.Read<u32>(); return Expr(s); }
        case 0x1B: case 0x45: {   // VirtualFunction / LocalVirtualFunction
            std::string name = NameStr(s.Name()), args = Args(s);
            if (IStartsWith(name, "ExecuteUbergraph") && !args.empty() && isdigit((u8)args[0])) entryNames[(u32)strtoul(args.c_str(), nullptr, 10)] = currentFunc;
            return name + "(" + args + ")";
        }
        case 0x1C: case 0x46: case 0x68: {   // FinalFunction / LocalFinalFunction / CallMath
            i32 f = s.Obj();
            std::string name = RefName(f), args = Args(s), owner = FuncOwner(f);
            // an event stub entering the ubergraph: remember where it lands
            if (IStartsWith(name, "ExecuteUbergraph") && !args.empty() && isdigit((u8)args[0])) entryNames[(u32)strtoul(args.c_str(), nullptr, 10)] = currentFunc;
            if (op == 0x68 && !owner.empty()) return "U" + owner + "::" + name + "(" + args + ")";
            return name + "(" + args + ")";
        }
        case 0x1D: return std::to_string(s.Read<i32>());
        case 0x1E: return Float(s.Read<float>());
        case 0x1F: return Quote(s.CStr());
        case 0x20: {   // ObjectConst: native classes / structs by name, assets by path
            i32 o = s.Obj();
            if (!o) return "nullptr";
            std::string p = pkg.FullPath(o);
            if (IStartsWith(p, "/Script/") && p.find("Default__") == std::string::npos) return RefName(o) + "::StaticClass()";
            return p;
        }
        case 0x21: return "FName(" + Quote(NameStr(s.Name())) + ")";
        case 0x22: { float p = s.Read<float>(), y = s.Read<float>(), r = s.Read<float>(); return "FRotator(" + Float(p) + ", " + Float(y) + ", " + Float(r) + ")"; }
        case 0x23: { float x = s.Read<float>(), y = s.Read<float>(), z = s.Read<float>(); return "FVector(" + Float(x) + ", " + Float(y) + ", " + Float(z) + ")"; }
        case 0x24: case 0x2C: return std::to_string(s.Read<u8>());
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
        case 0x2D: return "nullptr /* interface */";
        case 0x2E: { i32 c = s.Obj(); return "Cast<" + RefName(c) + ">(" + Expr(s) + ")"; }
        case 0x2F: {   // StructConst
            i32 st = s.Obj();
            s.Read<i32>();
            std::string name = RefName(st), list = ExprList(s, 0x30);
            if (name == "LatentActionInfo" && !list.empty() && isdigit((u8)list[0])) {   // latent actions resume the ubergraph here
                u32 at = (u32)strtoul(list.c_str(), nullptr, 10);
                targets.insert(at);
                pendingEntries[at] = "resume after a latent action in " + currentFunc;
            }
            return StructType(name) + "(" + list + ")";
        }
        case 0x31: {   // SetArray
            std::string arr = Expr(s);
            return arr + " = { " + ExprList(s, 0x32) + " }";
        }
        case 0x33: return RefName(s.Obj());
        case 0x34: return "TEXT(" + Quote(s.WStr()) + ")";
        case 0x35: return std::to_string(s.Read<i64>()) + "ll";
        case 0x36: return std::to_string(s.Read<u64>()) + "ull";
        case 0x38: {
            u8 t = s.Read<u8>();
            std::string e = Expr(s);
            return t == 0x47 || t == 0x01 ? "(bool)(" + e + ")" : t == 0x49 || t == 0x02 ? "(bool)(" + e + ")" : "Cast(" + e + ")";
        }
        case 0x42: {   // StructMemberContext
            i32 m = s.Obj();
            return Expr(s) + "." + RefName(m);
        }
        case 0x4B: return "&" + NameStr(s.Name());   // InstanceDelegate
        case 0x4C: { u32 t = s.Read<u32>(); targets.insert(t); return "ExecutionFlow.Push(" + Label(t) + ")"; }
        case 0x4D: return "goto ExecutionFlow.Pop()";
        case 0x4E: return "goto (" + Expr(s) + ")";
        case 0x4F: return "if (!(" + Expr(s) + ")) goto ExecutionFlow.Pop()";
        case 0x50: case 0x5A: case 0x5E: return "";
        case 0x51: return Expr(s);
        case 0x52: case 0x54: case 0x55: { i32 c = s.Obj(); return "Cast<" + RefName(c) + ">(" + Expr(s) + ")"; }
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
            return d + ".Broadcast(" + Args(s) + ")";
        }
        case 0x64: { i32 p = s.Obj(); return "UberGraphFrame->" + RefName(p) + " = " + Expr(s); }
        case 0x65: {
            s.Obj();
            s.Read<i32>();
            return "{ " + ExprList(s, 0x66) + " }";
        }
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

    // one function: signature + statements with labels where something jumps
    std::string Decompile(const FuncInfo& f, const std::string& indent) {
        std::vector<std::pair<u32, std::string>> stmts;
        std::string error;
        Script s;
        s.data = &f.script;
        targets.clear();
        currentFunc = f.name;
        scopeNames.clear();
        for (auto& p : f.params) scopeNames.insert(p.name);
        for (auto& p : f.locals) scopeNames.insert(p.name);
        try {
            while (s.pos < f.script.size()) {
                u32 off = s.mem;
                if (f.script[s.pos] == 0x53) break;   // EX_EndOfScript
                std::string st = Expr(s);
                stmts.push_back({off, st});
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        for (auto& [off, name] : entryNames)
            if (IsUbergraph(f)) targets.insert(off);
        std::string out;
        for (size_t i = 0; i < stmts.size(); i++) {
            auto& [off, st] = stmts[i];
            if (targets.count(off)) {
                out += indent.substr(0, indent.size() >= 4 ? indent.size() - 4 : 0) + Label(off) + ":";
                auto en = entryNames.find(off);
                auto pe = pendingEntries.find(off);
                if (en != entryNames.end() && IsUbergraph(f)) out += "   // entry of " + en->second;
                else if (pe != pendingEntries.end() && IsUbergraph(f)) out += "   // " + pe->second;
                out += "\n";
            }
            if (st.empty()) continue;
            // "return" at the very end of a function adds nothing
            if (st == "return" && i + 1 == stmts.size()) continue;
            bool block = st.rfind("switch", 0) == 0;
            out += indent + st + (block ? "\n" : ";\n");
        }
        // a label on the end of the script (jumped to, nothing after it) returns
        if (out.size() >= 2 && out.compare(out.size() - 2, 2, ":\n") == 0) out += indent + "return;\n";
        if (!error.empty()) out += indent + "// decompilation stopped: " + error + "\n";
        return out;
    }

    std::string Signature(const FuncInfo& f, bool withClass, const std::string& cls) {
        std::string ret = "void";
        std::vector<std::string> args;
        for (auto& p : f.params) {
            if (p.flags & CPF_ReturnParm) { ret = p.type; continue; }
            bool out = (p.flags & CPF_OutParm) && !(p.flags & CPF_ConstParm);
            bool ref = (p.flags & CPF_ReferenceParm) != 0;
            args.push_back(std::string(p.flags & CPF_ConstParm ? "const " : "") + p.type + (out || ref ? "& " : " ") + p.name);
        }
        std::string s = ret + " " + (withClass ? cls + "::" : "") + f.name + "(";
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

    bool Run(std::string& out, std::string& err) {
        // the generated class
        i32 cls = 0;
        for (size_t i = 0; i < pkg.Exports().size(); i++)
            if (pkg.ExportClass(i) == "BlueprintGeneratedClass") { cls = (i32)i + 1; break; }
        if (!cls) { err = "no BlueprintGeneratedClass in this package"; return false; }
        std::string clsName = pkg.Exports()[cls - 1].objectName, super = "UObject";
        try {
            Reader r = BodyReader((size_t)cls - 1);
            r.I32();
            i32 sup = r.I32();
            if (sup) super = pkg.ObjectName(sup);
        } catch (const std::exception&) {
        }
        std::vector<PropInfo> vars;
        std::vector<FuncInfo> funcs;
        for (size_t i = 0; i < pkg.Exports().size(); i++) {
            if (pkg.Exports()[i].outerIndex != cls) continue;
            std::string c = pkg.ExportClass(i);
            if (c == "Function") {
                FuncInfo f;
                std::string e;
                if (ReadFunction(i, f, e)) funcs.push_back(std::move(f));
                else out += "// " + pkg.Exports()[i].objectName + ": " + e + "\n";
            } else if (IEndsWith(c, "Property")) {
                vars.push_back(ReadProp(i));
            }
        }
        // ubergraph entry points come from the event stubs
        for (auto& f : funcs)
            if (f.eventGraph) entryNames[(u32)f.eventGraphOffset] = f.name;

        out += "// Decompiled by SOTModel from " + pkg.GamePath() + "\n";
        out += "// Flow is kept as in the bytecode: Label_N is the script offset a jump or event lands on.\n\n";
        // delegate signatures
        for (auto& f : funcs)
            if (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) {
                std::string n = f.name;
                size_t p = n.find("__DelegateSignature");
                if (p != std::string::npos) n.resize(p);
                out += "// " + std::string(f.flags & FUNC_MulticastDelegate ? "multicast " : "") + "delegate " + n + "\n";
                out += "DECLARE_DYNAMIC_DELEGATE(" + Signature(f, false, "") + ");\n\n";
            }
        out += "class " + clsName + " : public " + super + "\n{\npublic:\n";
        for (auto& v : vars) {
            if (v.name == "UberGraphFrame") continue;
            out += "    " + v.type + " " + v.name + (v.arrayDim > 1 ? Fmt("[%d]", v.arrayDim) : "") + ";\n";
        }
        if (!vars.empty()) out += "\n";
        for (auto& f : funcs) {
            if (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) continue;
            out += "    " + std::string(f.flags & FUNC_Static ? "static " : f.flags & FUNC_BlueprintEvent ? "virtual " : "") + Signature(f, false, clsName) + ";\n";
        }
        out += "};\n";
        std::map<i32, std::string> bodies;
        for (int pass = 0; pass < 2; pass++)
            for (auto& f : funcs) {
                if ((f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) || (pass == 0) == IsUbergraph(f)) continue;
                bodies[f.index] = Decompile(f, "    ");
            }
        for (auto& f : funcs) {
            if (f.flags & (FUNC_Delegate | FUNC_MulticastDelegate)) continue;
            {
                std::string fl = FlagNames(f.flags);
                if (IsUbergraph(f) && !(f.flags & FUNC_UbergraphFunction)) fl = fl.empty() ? "Ubergraph" : "Ubergraph | " + fl;
                out += "\n// " + fl + "\n";
            }
            if (f.eventGraph) out += Fmt("// event: runs %s at Label_%d\n", RefName(f.eventGraph).c_str(), f.eventGraphOffset);
            out += Signature(f, true, clsName) + "\n{\n";
            for (auto& l : f.locals) out += "    " + l.type + " " + l.name + ";\n";
            if (!f.locals.empty()) out += "\n";
            out += bodies[f.index];
            out += "}\n";
        }
        return true;
    }

    const Package& pkg;
};

}   // namespace

bool IsBlueprintPackage(const Package& pkg) {
    for (size_t i = 0; i < pkg.Exports().size(); i++)
        if (pkg.ExportClass(i) == "BlueprintGeneratedClass") return true;
    return false;
}

bool DecompileBlueprint(const Package& pkg, std::string& out, std::string* error) {
    Decompiler d(pkg);
    std::string err;
    try {
        if (d.Run(out, err)) return true;
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (error) *error = err;
    return false;
}
