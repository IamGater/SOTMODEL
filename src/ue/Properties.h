#pragma once
#include "core/Common.h"
#include "core/Reader.h"

class Package;
struct Property;

enum class PropType : u8 {
    None, Bool, Int8, Int16, Int, Int64, UInt16, UInt32, UInt64, Float, Double,
    Byte,      // numeric byte
    Enum,      // enum value name (str)
    Name, Str, Text,
    Object,    // object reference (num = package index, str = resolved path)
    SoftObject,// string asset reference (str)
    Struct,    // tagged struct (fields) or native struct (fields synthesized)
    Array,
    Map,
    Delegate,
    Raw,       // undecoded bytes
};

using PropertyList = std::vector<Property>;

struct PropValue {
    PropType type = PropType::None;
    union {
        bool b;
        i64 i;
        u64 u;
        double f;
    };
    std::string str;              // name / string / enum / object path / struct type
    std::vector<PropValue> items; // array elements, map key/value pairs (interleaved)
    std::shared_ptr<PropertyList> fields;   // struct members
    Bytes raw;

    PropValue() : i(0) {}

    // helpers (return defaults on type mismatch)
    double AsFloat(double def = 0) const;
    i64 AsInt(i64 def = 0) const;
    bool AsBool(bool def = false) const;
    i32 ObjIndex() const { return type == PropType::Object ? (i32)i : 0; }
    const PropValue* Field(std::string_view name) const;
    const PropertyList* Fields() const { return fields.get(); }
};

struct Property {
    std::string name;
    std::string typeName;   // tag type ("FloatProperty", ...)
    i32 arrayIndex = 0;
    PropValue value;
};

const Property* FindProp(const PropertyList& props, std::string_view name, i32 arrayIndex = 0);
const PropValue* FindValue(const PropertyList& props, std::string_view name, i32 arrayIndex = 0);
double PropFloat(const PropertyList& props, std::string_view name, double def = 0);
i64 PropInt(const PropertyList& props, std::string_view name, i64 def = 0);
bool PropBool(const PropertyList& props, std::string_view name, bool def = false);
std::string PropStr(const PropertyList& props, std::string_view name);
i32 PropObj(const PropertyList& props, std::string_view name, i32 arrayIndex = 0);

// Reads tagged properties until "None" or `limit`.
void ReadTaggedProperties(const Package& pkg, Reader& r, size_t limit, PropertyList& out);

// Native (binary) struct layouts known for UE4.10; returns false when `structName` is not native.
bool ReadNativeStruct(const Package& pkg, Reader& r, std::string_view structName, size_t size, PropValue& out);

// JSON (FModel-like) rendering
class JsonWriter;
void SetJsonPackage(const Package* pkg);   // package used to render object references on this thread
void WriteProperties(JsonWriter& w, const PropertyList& props);
void WriteValue(JsonWriter& w, const PropValue& v);
