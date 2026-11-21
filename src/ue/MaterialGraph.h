#pragma once
#include "ue/Package.h"

// Material graph -> readable HLSL, the way UE 4.10's HLSLMaterialTranslator builds a material's shader code: every
// expression node becomes a typed HLSL value, material functions are inlined at their call sites, static switches are
// resolved from the instance's static parameters, and the connected material attributes are assigned in
// CalcPixelMaterialInputs / the vertex functions. Works on a Material, a MaterialInstance (the master's graph with the
// instance's parameter values and static switches) or a MaterialFunction (its outputs).
struct MaterialGraphStats {
    int nodes = 0;                        // expressions translated
    int functions = 0;                    // material function calls inlined
    std::vector<std::string> unsupported; // node types without a translation (they evaluate to 0)
    std::vector<std::string> attributes;  // connected attributes
};

bool MaterialToHlsl(PackageCache& cache, const std::string& path, std::string& hlsl, MaterialGraphStats* stats = nullptr,
                    std::string* error = nullptr);

// The graph as nodes + links (for listing / drawing): one entry per expression of the material itself.
struct MaterialGraphNode {
    std::string name, type;   // export name, expression class without the MaterialExpression prefix
    std::string caption;      // parameter name, constant value, function name, ...
    int x = 0, y = 0;         // editor position
    struct Link { std::string input, from; int output = 0; };
    std::vector<Link> inputs;
    std::vector<std::string> outputs;   // output pin names when they have names (function outputs, RGB / R / G / B / A, ...)
    std::string function;               // MaterialFunctionCall: the function's package path
};
bool ReadMaterialGraph(PackageCache& cache, const std::string& path, std::vector<MaterialGraphNode>& nodes, std::string* error = nullptr);
