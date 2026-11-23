#pragma once
#include "core/Common.h"
#include <string>
#include <string_view>
#include <vector>

// Reflection of the game's native code (classes, UFunction signatures, enums) for the Blueprint decompiler, from the
// embedded third_party/sot_reflection/sot_reflection.txt (built from SDK dumps of the game, see its README).
struct ReflParam {
    char mode = 'v';    // 'v' by value, 'r' const reference, 'o' out (non-const reference)
    std::string type;   // C++ type ("AActor*", "FVector", "TArray<int>")
    std::string name;
};
struct ReflFunction {
    bool isStatic = false, isConst = false;
    bool orderUnknown = false;   // out parameters listed last by the source SDK, real argument positions unknown
    std::string ret = "void";
    std::vector<ReflParam> params;
};
struct ReflEnum {
    std::vector<std::pair<long long, std::string>> values;
    const std::string* Name(long long v) const;
};

namespace SotReflection {
// "Actor" -> "AActor"; nullptr when the class is unknown
const std::string* ClassCppName(std::string_view name);
// unprefixed super class name ("" when unknown or a root)
std::string SuperClass(std::string_view name);
// a native function by its owner class (unprefixed) and name
const ReflFunction* Function(std::string_view owner, std::string_view name);
// searched from `cls` up through its native super classes
const ReflFunction* FunctionInHierarchy(std::string_view cls, std::string_view name, std::string* owner = nullptr);
const ReflEnum* Enum(std::string_view name);
bool Loaded();
}
