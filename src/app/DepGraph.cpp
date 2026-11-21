#include "app/DepGraph.h"
#include "app/RefIndex.h"
#include "ue/Package.h"
#include <unordered_map>

std::vector<DepImport> PackageImports(const Package& pkg, const Vfs& vfs, u32 self) {
    std::vector<DepImport> out;
    const auto& im = pkg.Imports();
    for (size_t i = 0; i < im.size(); i++) {
        if (im[i].outerIndex != 0 || im[i].className != "Package" || !IStartsWith(im[i].objectName, "/Game/")) continue;
        i64 t = vfs.FindPackage(im[i].objectName);
        if (t < 0 || (u32)t == self) continue;
        bool dup = false;
        for (auto& o : out) dup |= o.entry == (u32)t;
        if (dup) continue;
        // the object(s) imported from that package: the asset itself, or a Blueprint's generated class
        std::string cls;
        for (auto& o : im)
            if (o.outerIndex == -(i32)i - 1 && !IStartsWith(o.objectName, "Default__")) {
                cls = o.className;
                if (cls != "Class" && cls != "Function") break;
            }
        out.push_back({(u32)t, cls});
    }
    return out;
}

namespace {

std::string MainClass(PackageCache& cache, u32 entry) {
    auto pkg = cache.Load(entry);
    if (!pkg) return {};
    i32 m = pkg->MainExport();
    return m > 0 ? pkg->ExportClass(m - 1) : std::string();
}

}   // namespace

DepGraph BuildDepGraph(const Vfs& vfs, PackageCache& cache, const ReferenceIndex* idx, u32 root, int depth, bool referencers,
                       bool references, int limit, int maxNodes) {
    DepGraph g;
    g.root = root;
    depth = std::clamp(depth, 1, 4);
    limit = std::max(1, limit);
    std::unordered_map<u64, int> index;   // (side, entry) -> node: the same package may sit on both sides
    auto key = [](int side, u32 e) { return ((u64)(u32)(side + 1) << 32) | e; };
    auto add = [&](u32 e, int side, int d, const std::string& cls) -> int {
        auto it = index.find(key(side, e));
        if (it != index.end()) return it->second;
        if ((int)g.nodes.size() >= maxNodes) return -1;
        DepNode n;
        n.entry = e;
        n.path = std::string(vfs.Path(e));
        n.cls = cls;
        n.depth = d;
        n.side = side;
        g.nodes.push_back(std::move(n));
        int id = (int)g.nodes.size() - 1;
        index[key(side, e)] = id;
        if (side == 0) {   // the root counts as either side, so a cycle back to it links to the centre
            index[key(-1, e)] = id;
            index[key(1, e)] = id;
        }
        return id;
    };
    add(root, 0, 0, MainClass(cache, root));
    auto addEdge = [&](int from, int to) {
        if (from < 0 || to < 0 || from == to) return;
        for (auto& e : g.edges)
            if (e.first == from && e.second == to) return;
        g.edges.push_back({from, to});
    };

    if (references) {
        std::vector<int> frontier = {0};
        for (int d = 1; d <= depth && !frontier.empty(); d++) {
            std::vector<int> next;
            for (int n : frontier) {
                auto pkg = cache.Load(g.nodes[n].entry);
                if (!pkg) continue;
                auto deps = PackageImports(*pkg, vfs, g.nodes[n].entry);
                for (size_t i = 0; i < deps.size(); i++) {
                    if ((int)i >= limit) { g.nodes[n].hiddenReferences += (int)(deps.size() - i); break; }
                    size_t before = g.nodes.size();
                    int id = add(deps[i].entry, 1, d, deps[i].cls);
                    addEdge(n, id);
                    if (id >= 0 && g.nodes.size() > before) next.push_back(id);
                }
            }
            frontier = std::move(next);
        }
    }
    if (referencers && idx) {
        std::vector<int> frontier = {0};
        for (int d = 1; d <= depth && !frontier.empty(); d++) {
            std::vector<int> next;
            for (int n : frontier) {
                const std::vector<u32>* refs = idx->Referencers(g.nodes[n].entry);
                if (!refs) continue;
                for (size_t i = 0; i < refs->size(); i++) {
                    if ((int)i >= limit) { g.nodes[n].hiddenReferencers += (int)(refs->size() - i); break; }
                    u32 e = (*refs)[i];
                    size_t before = g.nodes.size();
                    int id = index.count(key(-1, e)) ? index[key(-1, e)] : add(e, -1, d, MainClass(cache, e));
                    addEdge(id, n);
                    if (id >= 0 && g.nodes.size() > before) next.push_back(id);
                }
            }
            frontier = std::move(next);
        }
    }
    return g;
}
