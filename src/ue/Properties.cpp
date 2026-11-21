#include "ue/Properties.h"
#include "ue/Package.h"
#include "core/Json.h"
#include "ue/Localization.h"
#include <cmath>

// ---------------------------------------------------------------- accessors
double PropValue::AsFloat(double def) const {
    switch (type) {
    case PropType::Float: case PropType::Double: return f;
    case PropType::Int: case PropType::Int8: case PropType::Int16: case PropType::Int64: case PropType::Byte: return (double)i;
    case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: return (double)u;
    default: return def;
    }
}
i64 PropValue::AsInt(i64 def) const {
    switch (type) {
    case PropType::Int: case PropType::Int8: case PropType::Int16: case PropType::Int64: case PropType::Byte: case PropType::Object: return i;
    case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: return (i64)u;
    case PropType::Float: case PropType::Double: return (i64)f;
    case PropType::Bool: return b;
    default: return def;
    }
}
bool PropValue::AsBool(bool def) const {
    if (type == PropType::Bool) return b;
    if (type == PropType::Byte || type == PropType::Int) return i != 0;
    return def;
}
const PropValue* PropValue::Field(std::string_view name) const { return fields ? FindValue(*fields, name) : nullptr; }

const Property* FindProp(const PropertyList& props, std::string_view name, i32 arrayIndex) {
    for (auto& p : props) if (p.arrayIndex == arrayIndex && IEquals(p.name, name)) return &p;
    return nullptr;
}
const PropValue* FindValue(const PropertyList& props, std::string_view name, i32 arrayIndex) {
    auto p = FindProp(props, name, arrayIndex);
    return p ? &p->value : nullptr;
}
double PropFloat(const PropertyList& props, std::string_view name, double def) { auto v = FindValue(props, name); return v ? v->AsFloat(def) : def; }
i64 PropInt(const PropertyList& props, std::string_view name, i64 def) { auto v = FindValue(props, name); return v ? v->AsInt(def) : def; }
bool PropBool(const PropertyList& props, std::string_view name, bool def) { auto v = FindValue(props, name); return v ? v->AsBool(def) : def; }
std::string PropStr(const PropertyList& props, std::string_view name) { auto v = FindValue(props, name); return v ? v->str : std::string(); }
i32 PropObj(const PropertyList& props, std::string_view name, i32 arrayIndex) { auto v = FindValue(props, name, arrayIndex); return v ? v->ObjIndex() : 0; }

// ---------------------------------------------------------------- reading
namespace {
PropValue MakeFloat(double d) { PropValue v; v.type = PropType::Float; v.f = d; return v; }
PropValue MakeInt(i64 x, PropType t = PropType::Int) { PropValue v; v.type = t; v.i = x; return v; }

void AddField(PropertyList& l, const char* name, PropValue v) {
    Property p;
    p.name = name;
    p.value = std::move(v);
    l.push_back(std::move(p));
}

PropValue Floats(Reader& r, const char* const* names, int n) {
    PropValue v;
    v.type = PropType::Struct;
    v.fields = std::make_shared<PropertyList>();
    for (int i = 0; i < n; i++) AddField(*v.fields, names[i], MakeFloat(r.F32()));
    return v;
}

PropValue ObjectRef(const Package& pkg, i32 idx) {
    PropValue v;
    v.type = PropType::Object;
    v.i = idx;
    return v;
}

PropValue ReadText(Reader& r) {
    PropValue v;
    v.type = PropType::Text;
    r.U32();   // flags
    i8 history = r.Read<i8>();
    if (history == 0) {   // base: namespace, key, source string
        std::string ns = r.FString(), key = r.FString(), src = r.FString();
        v.str = src;
        v.items.resize(2);
        v.items[0].type = PropType::Str; v.items[0].str = ns;
        v.items[1].type = PropType::Str; v.items[1].str = key;
    } else if (history != -1) {
        throw ParseError(Fmt("unsupported FText history %d", history));
    }
    return v;
}

const char* const XYZ[] = {"X", "Y", "Z", "W"};
const char* const PYR[] = {"Pitch", "Yaw", "Roll"};
const char* const RGBA[] = {"R", "G", "B", "A"};

bool IsValidName(const Package& pkg, Reader& r) {
    if (r.Remaining() < 8) return false;
    i32 i = r.Peek<i32>();
    return i >= 0 && i < (i32)pkg.Names().size();
}

struct TagInfo {
    std::string name, type, structName, enumName, innerType;
    i32 size = 0, arrayIndex = 0;
    u8 boolVal = 0;
};

bool ReadTag(const Package& pkg, Reader& r, TagInfo& t) {
    t.name = pkg.ReadName(r);
    if (t.name == "None") return false;
    t.type = pkg.ReadName(r);
    t.size = r.I32();
    t.arrayIndex = r.I32();
    if (t.type == "StructProperty") { t.structName = pkg.ReadName(r); r.Skip(16); }
    else if (t.type == "BoolProperty") t.boolVal = r.U8();
    else if (t.type == "ByteProperty") t.enumName = pkg.ReadName(r);
    else if (t.type == "ArrayProperty") t.innerType = pkg.ReadName(r);
    if (t.size < 0) throw ParseError(Fmt("negative property size for %s", t.name.c_str()));
    return true;
}

PropValue ReadStructValue(const Package& pkg, Reader& r, std::string_view structName, size_t end);

// One value of a known simple type. elemSize is the per element size when known (arrays), 0 otherwise.
PropValue ReadSimple(const Package& pkg, Reader& r, std::string_view type, size_t elemSize, std::string_view enumName) {
    PropValue v;
    if (type == "IntProperty") return MakeInt(r.I32());
    if (type == "FloatProperty") return MakeFloat(r.F32());
    if (type == "BoolProperty") { v.type = PropType::Bool; v.b = r.U8() != 0; return v; }
    if (type == "ObjectProperty" || type == "ClassProperty" || type == "WeakObjectProperty" || type == "InterfaceProperty" || type == "ComponentProperty")
        return ObjectRef(pkg, r.I32());
    if (type == "NameProperty") { v.type = PropType::Name; v.str = pkg.ReadName(r); return v; }
    if (type == "StrProperty") { v.type = PropType::Str; v.str = r.FString(); return v; }
    if (type == "TextProperty") return ReadText(r);
    if (type == "AssetObjectProperty" || type == "AssetClassProperty" || type == "SoftObjectProperty" || type == "SoftClassProperty") {
        v.type = PropType::SoftObject;
        v.str = r.FString();
        return v;
    }
    if (type == "ByteProperty") {
        if (elemSize == 8 || (!enumName.empty() && enumName != "None" && elemSize == 0)) { v.type = PropType::Enum; v.str = pkg.ReadName(r); return v; }
        return MakeInt(r.U8(), PropType::Byte);
    }
    if (type == "Int8Property") return MakeInt(r.Read<i8>(), PropType::Int8);
    if (type == "Int16Property") return MakeInt(r.Read<i16>(), PropType::Int16);
    if (type == "Int64Property") return MakeInt(r.I64(), PropType::Int64);
    if (type == "UInt16Property") { v.type = PropType::UInt16; v.u = r.U16(); return v; }
    if (type == "UInt32Property") { v.type = PropType::UInt32; v.u = r.U32(); return v; }
    if (type == "UInt64Property") { v.type = PropType::UInt64; v.u = r.U64(); return v; }
    if (type == "DoubleProperty") { v.type = PropType::Double; v.f = r.Read<double>(); return v; }
    if (type == "LazyObjectProperty") { v.type = PropType::Raw; v.raw = r.ReadBytes(16); return v; }
    if (type == "DelegateProperty") {
        v.type = PropType::Delegate;
        v.i = r.I32();
        v.str = pkg.ReadName(r);
        return v;
    }
    throw ParseError("unknown property type " + std::string(type));
}

PropValue ReadArray(const Package& pkg, Reader& r, const TagInfo& tag, size_t end) {
    PropValue v;
    v.type = PropType::Array;
    v.str = tag.innerType;
    i32 n = r.I32();
    if (n < 0 || (size_t)n > end - r.Pos()) throw ParseError(Fmt("bad array count %d", n));
    size_t payload = end - r.Pos();
    size_t elemSize = (n > 0 && payload % n == 0) ? payload / n : 0;
    v.items.reserve(n);
    const std::string& inner = tag.innerType;
    if (inner == "StructProperty") {
        std::string structName;
        // newer layout: an inner tag describes the element struct
        if (n > 0 && IsValidName(pkg, r)) {
            size_t save = r.Pos();
            try {
                TagInfo it;
                if (ReadTag(pkg, r, it) && it.type == "StructProperty") structName = it.structName;
                else r.Seek(save);
            } catch (...) { r.Seek(save); }
        }
        if (!structName.empty()) {
            v.str = structName;
            for (i32 i = 0; i < n; i++) v.items.push_back(ReadStructValue(pkg, r, structName, end));
            return v;
        }
        // 4.10: the element struct is unnamed - tagged bags, or a native layout guessed from the size
        size_t save = r.Pos();
        try {
            for (i32 i = 0; i < n; i++) {
                PropValue e;
                e.type = PropType::Struct;
                e.fields = std::make_shared<PropertyList>();
                ReadTaggedProperties(pkg, r, end, *e.fields);
                v.items.push_back(std::move(e));
            }
            if (r.Pos() == end) return v;
        } catch (...) {}
        v.items.clear();
        r.Seek(save);
        const char* guess = elemSize == 12 ? "Vector" : elemSize == 16 ? "Vector4" : elemSize == 8 ? "Vector2D" : elemSize == 4 ? "Color" : elemSize == 64 ? "Matrix" : nullptr;
        if (guess) {
            for (i32 i = 0; i < n; i++) v.items.push_back(ReadStructValue(pkg, r, guess, end));
            v.str = std::string("StructProperty(") + guess + "?)";
            return v;
        }
        throw ParseError("cannot decode struct array");
    }
    if (inner == "ArrayProperty" || inner == "MapProperty") throw ParseError("nested container");
    for (i32 i = 0; i < n; i++) v.items.push_back(ReadSimple(pkg, r, inner, elemSize, {}));
    return v;
}

PropValue ReadStructValue(const Package& pkg, Reader& r, std::string_view structName, size_t end) {
    PropValue v;
    if (ReadNativeStruct(pkg, r, structName, end - r.Pos(), v)) return v;
    v.type = PropType::Struct;
    v.str = std::string(structName);
    v.fields = std::make_shared<PropertyList>();
    ReadTaggedProperties(pkg, r, end, *v.fields);
    return v;
}

PropValue ReadValue(const Package& pkg, Reader& r, const TagInfo& tag, size_t end) {
    if (tag.type == "BoolProperty") { PropValue v; v.type = PropType::Bool; v.b = tag.boolVal != 0; return v; }
    if (tag.type == "StructProperty") return ReadStructValue(pkg, r, tag.structName, end);
    if (tag.type == "ArrayProperty") return ReadArray(pkg, r, tag, end);
    if (tag.type == "MapProperty") throw ParseError("map");
    if (tag.type == "ByteProperty") return ReadSimple(pkg, r, tag.type, (size_t)tag.size, tag.enumName);
    if (tag.type == "MulticastDelegateProperty") {
        PropValue v;
        v.type = PropType::Array;
        v.str = "Delegate";
        i32 n = r.Count(12);
        for (i32 i = 0; i < n; i++) v.items.push_back(ReadSimple(pkg, r, "DelegateProperty", 12, {}));
        return v;
    }
    return ReadSimple(pkg, r, tag.type, (size_t)tag.size, tag.enumName);
}
}

bool ReadNativeStruct(const Package& pkg, Reader& r, std::string_view s, size_t size, PropValue& v) {
    auto fields = [&]() { v.type = PropType::Struct; v.str = std::string(s); v.fields = std::make_shared<PropertyList>(); return v.fields.get(); };
    if (s == "Vector" || s == "IntVector") {
        if (s == "Vector") { v = Floats(r, XYZ, 3); v.str = "Vector"; return true; }
        auto f = fields();
        for (int i = 0; i < 3; i++) AddField(*f, XYZ[i], MakeInt(r.I32()));
        return true;
    }
    if (s == "Vector4" || s == "Quat" || s == "Plane") { v = Floats(r, XYZ, 4); v.str = std::string(s); return true; }
    if (s == "Vector2D") { v = Floats(r, XYZ, 2); v.str = "Vector2D"; return true; }
    if (s == "Rotator") { v = Floats(r, PYR, 3); v.str = "Rotator"; return true; }
    if (s == "LinearColor") { v = Floats(r, RGBA, 4); v.str = "LinearColor"; return true; }
    if (s == "Color") {
        auto f = fields();
        u8 b = r.U8(), g = r.U8(), rr = r.U8(), a = r.U8();
        AddField(*f, "B", MakeInt(b, PropType::Byte));
        AddField(*f, "G", MakeInt(g, PropType::Byte));
        AddField(*f, "R", MakeInt(rr, PropType::Byte));
        AddField(*f, "A", MakeInt(a, PropType::Byte));
        return true;
    }
    if (s == "IntPoint") {
        auto f = fields();
        AddField(*f, "X", MakeInt(r.I32()));
        AddField(*f, "Y", MakeInt(r.I32()));
        return true;
    }
    if (s == "Guid") {
        v.type = PropType::Str;
        u32 a = r.U32(), b = r.U32(), c = r.U32(), d = r.U32();
        v.str = Fmt("%08X%08X%08X%08X", a, b, c, d);
        return true;
    }
    if (s == "Box" || s == "Box2D") {
        auto f = fields();
        int n = s == "Box" ? 3 : 2;
        PropValue mn = Floats(r, XYZ, n), mx = Floats(r, XYZ, n);
        AddField(*f, "Min", std::move(mn));
        AddField(*f, "Max", std::move(mx));
        PropValue valid;
        valid.type = PropType::Bool;
        valid.b = r.U8() != 0;
        AddField(*f, "IsValid", std::move(valid));
        return true;
    }
    if (s == "TwoVectors") {
        auto f = fields();
        AddField(*f, "v1", Floats(r, XYZ, 3));
        AddField(*f, "v2", Floats(r, XYZ, 3));
        return true;
    }
    if (s == "Matrix") {
        auto f = fields();
        const char* rows[] = {"XPlane", "YPlane", "ZPlane", "WPlane"};
        for (auto row : rows) AddField(*f, row, Floats(r, XYZ, 4));
        return true;
    }
    if (s == "OrientedBox") {
        auto f = fields();
        const char* n3[] = {"Center", "AxisX", "AxisY", "AxisZ"};
        for (auto x : n3) AddField(*f, x, Floats(r, XYZ, 3));
        AddField(*f, "ExtentX", MakeFloat(r.F32()));
        AddField(*f, "ExtentY", MakeFloat(r.F32()));
        AddField(*f, "ExtentZ", MakeFloat(r.F32()));
        return true;
    }
    if (s == "PackedNormal") { v = MakeInt(r.U32(), PropType::UInt32); v.type = PropType::UInt32; v.u = (u32)v.i; return true; }
    if (s == "DateTime" || s == "Timespan") { v = MakeInt(r.I64(), PropType::Int64); return true; }
    if (s == "StringAssetReference" || s == "StringClassReference" || s == "SoftObjectPath" || s == "SoftClassPath") {
        v.type = PropType::SoftObject;
        v.str = r.FString();
        return true;
    }
    if (s == "RichCurveKey") {
        auto f = fields();
        AddField(*f, "InterpMode", MakeInt(r.U8(), PropType::Byte));
        AddField(*f, "TangentMode", MakeInt(r.U8(), PropType::Byte));
        AddField(*f, "TangentWeightMode", MakeInt(r.U8(), PropType::Byte));
        const char* fl[] = {"Time", "Value", "ArriveTangent", "ArriveTangentWeight", "LeaveTangent", "LeaveTangentWeight"};
        for (auto x : fl) AddField(*f, x, MakeFloat(r.F32()));
        return true;
    }
    if (s == "NameCurveKey") {
        auto f = fields();
        AddField(*f, "Time", MakeFloat(r.F32()));
        PropValue n;
        n.type = PropType::Name;
        n.str = pkg.ReadName(r);
        AddField(*f, "Value", std::move(n));
        return true;
    }
    if (s == "FontCharacter") {
        auto f = fields();
        const char* fl[] = {"StartU", "StartV", "USize", "VSize"};
        for (auto x : fl) AddField(*f, x, MakeInt(r.I32()));
        AddField(*f, "TextureIndex", MakeInt(r.U8(), PropType::Byte));
        AddField(*f, "VerticalOffset", MakeInt(r.I32()));
        return true;
    }
    if (s == "GameplayTagContainer") {
        v.type = PropType::Array;
        v.str = "GameplayTag";
        i32 n = r.Count(8);
        for (i32 i = 0; i < n; i++) {
            PropValue t;
            t.type = PropType::Name;
            t.str = pkg.ReadName(r);
            v.items.push_back(std::move(t));
        }
        return true;
    }
    if (s == "KeyHandleMap") { fields(); return true; }   // transacting only: empty on disk
    (void)size;
    return false;
}

void ReadTaggedProperties(const Package& pkg, Reader& r, size_t limit, PropertyList& out) {
    while (r.Pos() + 8 <= limit) {
        TagInfo tag;
        if (!ReadTag(pkg, r, tag)) return;
        size_t start = r.Pos();
        size_t end = start + (size_t)tag.size;
        if (end > limit) throw ParseError(Fmt("property %s overruns its container (%zu > %zu)", tag.name.c_str(), end, limit));
        Property p;
        p.name = tag.name;
        p.typeName = tag.type;
        p.arrayIndex = tag.arrayIndex;
        try {
            Reader sub(r.Data(), end, start);
            p.value = ReadValue(pkg, sub, tag, end);
        } catch (const std::exception&) {
            p.value = PropValue();
            p.value.type = PropType::Raw;
            p.value.str = tag.type == "StructProperty" ? tag.structName : tag.type;
            p.value.raw.assign(r.Data() + start, r.Data() + end);
        }
        out.push_back(std::move(p));
        r.Seek(end);
    }
}

// ---------------------------------------------------------------- JSON
static void WriteObjectRef(JsonWriter& w, const PropValue& v, const Package* pkg);
static thread_local const Package* t_jsonPackage = nullptr;
static const Localization* g_jsonLoc = nullptr;
void SetJsonLocalization(const Localization* loc) { g_jsonLoc = loc; }

void WriteValue(JsonWriter& w, const PropValue& v) {
    switch (v.type) {
    case PropType::None: w.Null(); break;
    case PropType::Bool: w.Bool(v.b); break;
    case PropType::Int: case PropType::Int8: case PropType::Int16: case PropType::Int64: case PropType::Byte: w.Int(v.i); break;
    case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: w.UInt(v.u); break;
    case PropType::Float: case PropType::Double: w.Num(v.f); break;
    case PropType::Enum: case PropType::Name: case PropType::Str: case PropType::SoftObject: w.Str(v.str); break;
    case PropType::Text:
        w.BeginObject();
        if (v.items.size() == 2) { w.KV("Namespace", v.items[0].str); w.KV("Key", v.items[1].str); }
        w.KV("SourceString", v.str);
        if (g_jsonLoc && v.items.size() == 2)   // the text in the chosen game language
            if (const std::string* loc = g_jsonLoc->Find(v.items[0].str, v.items[1].str)) w.KV("LocalizedString", *loc);
        w.EndObject();
        break;
    case PropType::Object: WriteObjectRef(w, v, t_jsonPackage); break;
    case PropType::Delegate:
        w.BeginObject();
        w.Key("Object");
        WriteObjectRef(w, v, t_jsonPackage);
        w.KV("FunctionName", v.str);
        w.EndObject();
        break;
    case PropType::Struct:
        if (v.fields) WriteProperties(w, *v.fields);
        else { w.BeginObject(); w.EndObject(); }
        break;
    case PropType::Array:
    case PropType::Map:
        w.BeginArray();
        for (auto& e : v.items) WriteValue(w, e);
        w.EndArray();
        break;
    case PropType::Raw: {
        std::string hex;
        static const char* d = "0123456789ABCDEF";
        size_t n = std::min<size_t>(v.raw.size(), 4096);
        hex.reserve(n * 2 + 32);
        for (size_t i = 0; i < n; i++) { hex += d[v.raw[i] >> 4]; hex += d[v.raw[i] & 15]; }
        if (n < v.raw.size()) hex += Fmt("...(%zu bytes)", v.raw.size());
        w.BeginObject();
        w.KV("UndecodedType", v.str);
        w.KV("Bytes", hex);
        w.EndObject();
        break;
    }
    }
}

void WriteProperties(JsonWriter& w, const PropertyList& props) {
    w.BeginObject();
    for (auto& p : props) {
        w.Key(p.arrayIndex ? Fmt("%s[%d]", p.name.c_str(), p.arrayIndex) : p.name);
        WriteValue(w, p.value);
    }
    w.EndObject();
}

static void WriteObjectRef(JsonWriter& w, const PropValue& v, const Package* pkg) {
    i32 idx = (i32)v.i;
    if (idx == 0 || !pkg) { w.Null(); return; }
    w.BeginObject();
    w.KV("ObjectName", pkg->ClassName(idx) + "'" + pkg->ObjectName(idx) + "'");
    if (idx > 0) w.KV("ObjectPath", pkg->GamePath().substr(0, pkg->GamePath().rfind('.')) + "." + std::to_string(idx - 1));
    else w.KV("ObjectPath", pkg->FullPath(idx));
    w.EndObject();
}

// Writers that render object references need the owning package; set it for the current thread.
void SetJsonPackage(const Package* pkg) { t_jsonPackage = pkg; }
