#pragma once
// Dependency graph around one package: who imports it (referencers, from the ReferenceIndex) and what it imports
// (references, read from its import table), a few levels deep.
#include "core/Vfs.h"
#include <string>
#include <vector>

class PackageCache;
class Package;
class ReferenceIndex;

struct DepNode {
    u32 entry = 0;
    std::string path;    // game path
    std::string cls;     // main asset class (StaticMesh, Material, ...), empty if unknown
    int depth = 0;       // 0 root, 1.. levels away
    int side = 0;        // -1 referencer, 0 root, +1 reference
    int hiddenReferencers = 0, hiddenReferences = 0;   // neighbours left out by the per-node limit
};

struct DepGraph {
    u32 root = UINT32_MAX;
    std::vector<DepNode> nodes;               // nodes[0] is the root
    std::vector<std::pair<int, int>> edges;   // (importer, imported) node indices
};

struct DepImport { u32 entry; std::string cls; };
// /Game packages a package imports, with the class of the object it imports from each (Default__ objects skipped)
std::vector<DepImport> PackageImports(const Package& pkg, const Vfs& vfs, u32 self);

// depth 1..4; limit = neighbours per node and side; maxNodes caps the whole graph. idx may be null (no referencers).
DepGraph BuildDepGraph(const Vfs& vfs, PackageCache& cache, const ReferenceIndex* idx, u32 root, int depth = 1,
                       bool referencers = true, bool references = true, int limit = 40, int maxNodes = 400);
