#pragma once
#include "ue/Package.h"
#include <map>

// Blueprint decompiler for UE4.10 (Athena) BlueprintGeneratedClass packages: the class (variables, functions with
// parameters and locals) and every function's Kismet bytecode as C++. Types use their C++ names (AActor*, UObject*),
// native calls are matched to the game's reflected signatures (ue/SotReflection), the event graph is split into the
// events that enter it, and control flow is rebuilt (if / else, loops, sequences) from the script jumps.
struct BlueprintStats {
    size_t calls = 0, callsWithSignature = 0;   // function calls, and those matched to a signature (names / out parameters known)
    std::map<std::string, int> unresolved;       // owner::function of calls without one
};
struct BlueprintOptions {
    BlueprintStats* stats = nullptr;
    PackageCache* cache = nullptr;   // resolves parent Blueprints, other Blueprints' functions and Blueprint enums
    bool raw = false;                // keep the bytecode's jumps as labels (no event split, no inlining)
};

bool IsBlueprintPackage(const Package& pkg);
bool DecompileBlueprint(const Package& pkg, std::string& out, std::string* error = nullptr, const BlueprintOptions& options = {});
