#pragma once
#include "ue/Package.h"

// Blueprint decompiler for UE4.10 (Athena) BlueprintGeneratedClass packages: class layout (variables, functions with
// parameters and locals) plus every function's Kismet bytecode turned into C++-like statements. Jumps keep their
// in-memory offsets as labels (Label_123), so flow matches the engine's script frame.
bool IsBlueprintPackage(const Package& pkg);
bool DecompileBlueprint(const Package& pkg, std::string& out, std::string* error = nullptr);
