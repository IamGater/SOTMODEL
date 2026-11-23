#include "ue/BehaviorTree.h"
#include "ue/Properties.h"
#include <functional>
#include <set>

namespace {

std::string StripPrefix(const std::string& cls) {
    for (const char* p : {"BTComposite_", "BTTask_", "BTDecorator_", "BTService_", "BlackboardKeyType_"})
        if (IStartsWith(cls, p)) return cls.substr(strlen(p));
    return cls;
}

// properties every node has (tree links, editor caches) and the ones shown elsewhere
bool Hidden(const std::string& n) {
    static const std::set<std::string> h = {"TreeAsset", "ParentNode", "Children", "Services", "NodeName", "CachedDescription",
                                            "bCreateNodeInstance", "BehaviorAsset", "ExecutionIndex", "TreeDepth"};
    return h.count(n) > 0;
}

std::string Num(double f) {
    std::string s = Fmt("%.4g", f);
    return s;
}

std::string Brief(const Package& pkg, const PropValue& v, int depth);

std::string BriefProps(const Package& pkg, const PropertyList& props, int depth, bool skipHidden) {
    std::string out;
    for (auto& p : props) {
        if (skipHidden && Hidden(p.name)) continue;
        std::string b = Brief(pkg, p.value, depth + 1);
        if (b.empty()) continue;
        if (!out.empty()) out += ", ";
        out += p.name + (p.arrayIndex ? Fmt("[%d]", p.arrayIndex) : "") + "=" + b;
    }
    return out;
}

std::string Brief(const Package& pkg, const PropValue& v, int depth) {
    if (depth > 4) return "...";
    switch (v.type) {
    case PropType::Bool: return v.b ? "true" : "false";
    case PropType::Float: case PropType::Double: return Num(v.f);
    case PropType::Int: case PropType::Int8: case PropType::Int16: case PropType::Int64: case PropType::Byte: return std::to_string(v.i);
    case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: return std::to_string(v.u);
    case PropType::Enum: {
        size_t c = v.str.rfind("::");
        return c == std::string::npos ? v.str : v.str.substr(c + 2);
    }
    case PropType::Name: return v.str == "None" ? "" : v.str;
    case PropType::Str: case PropType::Text: return v.str.empty() ? "" : "\"" + v.str + "\"";
    case PropType::SoftObject: return v.str;
    case PropType::Object: {
        i32 o = v.ObjIndex();
        if (!o) return "";
        std::string name = pkg.ObjectName(o);
        if (o > 0) {   // an inline sub-object (data providers, key types): its class and values
            std::string err;
            size_t end = 0;
            PropertyList sub = pkg.ReadProperties((size_t)o - 1, &end, &err);
            std::string inner = BriefProps(pkg, sub, depth, true);
            return StripPrefix(pkg.ExportClass((size_t)o - 1)) + (inner.empty() ? "" : "(" + inner + ")");
        }
        return name;
    }
    case PropType::Struct: {
        if (!v.fields) return "";
        if (const PropValue* k = v.Field("SelectedKeyName")) return k->str == "None" ? "" : "@" + k->str;   // blackboard key selector
        std::string inner = BriefProps(pkg, *v.fields, depth, false);
        return inner.empty() ? "" : "{" + inner + "}";
    }
    case PropType::Array: {
        if (v.items.empty()) return "";
        std::string out;
        for (size_t i = 0; i < v.items.size() && i < 6; i++) out += (i ? ", " : "") + Brief(pkg, v.items[i], depth + 1);
        if (v.items.size() > 6) out += Fmt(", ... (%zu)", v.items.size());
        return "[" + out + "]";
    }
    default: return "";
    }
}

std::string DisplayName(const PropertyList& props, const std::string& cls) {
    std::string n = PropStr(props, "NodeName");
    return n.empty() ? cls : n;
}

std::string Details(const Package& pkg, const PropertyList& props) {
    std::string desc = PropStr(props, "CachedDescription");   // the editor's own description of the node, when cooked
    std::string rest = BriefProps(pkg, props, 0, true);
    if (!desc.empty()) {
        for (auto& c : desc) if (c == '\n' || c == '\r') c = ' ';
        return rest.empty() ? desc : desc + "  {" + rest + "}";
    }
    return rest;
}

BtAux ReadAux(const Package& pkg, i32 obj) {
    BtAux a;
    if (obj <= 0) return a;
    size_t end = 0;
    std::string err;
    PropertyList props = pkg.ReadProperties((size_t)obj - 1, &end, &err);
    a.cls = StripPrefix(pkg.ExportClass((size_t)obj - 1));
    a.name = DisplayName(props, a.cls);
    a.details = Details(pkg, props);
    return a;
}

void ReadBlackboard(PackageCache* cache, const std::string& path, BehaviorTreeInfo& out, bool parent, int depth) {
    if (!cache || path.empty() || depth > 8) return;
    std::string err;
    auto bb = cache->LoadPath(path, &err);
    if (!bb) return;
    i32 m = bb->MainExport();
    if (!m) return;
    size_t end = 0;
    PropertyList props = bb->ReadProperties((size_t)m - 1, &end, &err);
    if (i32 par = PropObj(props, "Parent")) ReadBlackboard(cache, bb->FullPath(par), out, true, depth + 1);
    if (const PropValue* keys = FindValue(props, "Keys"))
        for (auto& k : keys->items) {
            if (!k.fields) continue;
            BlackboardKey key;
            if (const PropValue* n = k.Field("EntryName")) key.name = n->str;
            if (const PropValue* t = k.Field("KeyType"); t && t->ObjIndex()) {
                i32 o = t->ObjIndex();
                key.type = StripPrefix(bb->ClassName(o));
                if (o > 0) {
                    PropertyList kp = bb->ReadProperties((size_t)o - 1, &end, &err);
                    if (i32 bc = PropObj(kp, "BaseClass")) key.detail = bb->ObjectName(bc);
                    if (i32 et = PropObj(kp, "EnumType")) key.detail = bb->ObjectName(et);
                    std::string en = PropStr(kp, "EnumName");
                    if (key.detail.empty() && !en.empty()) key.detail = en;
                }
            }
            if (parent) key.from = bb->ShortName();
            out.keys.push_back(std::move(key));
        }
}

}   // namespace

bool ReadBehaviorTree(const Package& pkg, BehaviorTreeInfo& out, PackageCache* cache, std::string* error) {
    i32 main = pkg.MainExport();
    if (!main || pkg.ExportClass((size_t)main - 1) != "BehaviorTree") {
        if (error) *error = "not a BehaviorTree package";
        return false;
    }
    out = {};
    out.name = pkg.Exports()[(size_t)main - 1].objectName;
    size_t end = 0;
    std::string err;
    PropertyList tree = pkg.ReadProperties((size_t)main - 1, &end, &err);
    if (i32 bb = PropObj(tree, "BlackboardAsset")) {
        out.blackboard = pkg.FullPath(bb);
        ReadBlackboard(cache, out.blackboard, out, false, 0);
    }
    i32 root = PropObj(tree, "RootNode");
    if (root <= 0) {
        if (error) *error = "the tree has no root node";
        return false;
    }
    std::set<i32> seen;
    std::function<int(i32, int)> add = [&](i32 obj, int depth) -> int {
        if (obj <= 0 || !seen.insert(obj).second || depth > 64) return -1;
        size_t e2 = 0;
        PropertyList props = pkg.ReadProperties((size_t)obj - 1, &e2, &err);
        BtNode n;
        std::string cls = pkg.ExportClass((size_t)obj - 1);
        n.composite = IStartsWith(cls, "BTComposite_") || FindValue(props, "Children") != nullptr;
        n.cls = StripPrefix(cls);
        n.name = DisplayName(props, n.cls);
        n.details = Details(pkg, props);
        n.depth = depth;
        if (i32 sub = PropObj(props, "BehaviorAsset")) {
            n.subtree = pkg.FullPath(sub);
            out.subtrees.push_back(n.subtree);
        }
        if (const PropValue* sv = FindValue(props, "Services"))
            for (auto& s : sv->items) n.services.push_back(ReadAux(pkg, s.ObjIndex()));
        int idx = (int)out.nodes.size();
        out.nodes.push_back(std::move(n));
        if (const PropValue* ch = FindValue(props, "Children"))
            for (auto& c : ch->items) {
                if (!c.fields) continue;
                i32 target = 0;
                if (const PropValue* cc = c.Field("ChildComposite")) target = cc->ObjIndex();
                if (!target)
                    if (const PropValue* ct = c.Field("ChildTask")) target = ct->ObjIndex();
                int ci = add(target, depth + 1);
                if (ci < 0) continue;
                if (const PropValue* decs = c.Field("Decorators"))
                    for (auto& dv : decs->items) out.nodes[ci].decorators.push_back(ReadAux(pkg, dv.ObjIndex()));
                // DecoratorOps: a prefix notation over the decorators (And / Or / Not / Test); only shown when not all AND
                if (const PropValue* ops = c.Field("DecoratorOps"); ops && !ops->items.empty()) {
                    std::string logic;
                    bool plain = true;
                    for (auto& op : ops->items) {
                        if (!op.fields) continue;
                        std::string o = Brief(pkg, op.Field("Operation") ? *op.Field("Operation") : PropValue(), 0);
                        i64 num = op.Field("Number") ? op.Field("Number")->AsInt() : 0;
                        if (o == "Or" || o == "Not") plain = false;
                        logic += (logic.empty() ? "" : " ") + (o == "Test" ? Fmt("#%lld", (long long)num) : o + (o == "And" || o == "Or" ? Fmt("(%lld)", (long long)num) : ""));
                    }
                    if (!plain) out.nodes[ci].decoratorLogic = logic;
                }
                out.nodes[idx].children.push_back(ci);
            }
        return idx;
    };
    add(root, 0);
    return !out.nodes.empty();
}

std::string BehaviorTreeText(const BehaviorTreeInfo& bt) {
    std::string s = "BehaviorTree " + bt.name + "\n";
    if (!bt.blackboard.empty()) s += "Blackboard " + bt.blackboard + "\n";
    if (!bt.keys.empty()) {
        s += "Keys:\n";
        for (auto& k : bt.keys)
            s += "  " + k.name + " : " + k.type + (k.detail.empty() ? "" : "<" + k.detail + ">") + (k.from.empty() ? "" : "   (from " + k.from + ")") + "\n";
    }
    s += "\n";
    std::function<void(int, const std::string&, bool, bool)> line = [&](int i, const std::string& prefix, bool last, bool root) {
        const BtNode& n = bt.nodes[(size_t)i];
        std::string branch = root ? "" : prefix + (last ? "\xE2\x94\x94\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80 ");   // └─ / ├─
        std::string cont = root ? "" : prefix + (last ? "   " : "\xE2\x94\x82  ");                                       // │
        std::string deco = prefix + "\xE2\x94\x82  ";   // conditions sit on the link into the node, above it
        for (auto& d : n.decorators) s += root ? "" : deco + "? " + d.name + (d.details.empty() || d.details == d.name ? "" : "  " + d.details) + "\n";
        if (!n.decoratorLogic.empty()) s += deco + "? logic " + n.decoratorLogic + "\n";
        s += branch + (n.composite ? "[" + n.cls + "] " : "") + n.name + (n.name != n.cls && !n.composite ? "  (" + n.cls + ")" : "");
        if (!n.subtree.empty()) s += "  -> " + n.subtree;
        if (!n.details.empty()) s += "  " + n.details;
        s += "\n";
        std::string childPrefix = root ? "" : cont;
        for (auto& sv : n.services) s += childPrefix + "  * " + sv.name + (sv.details.empty() ? "" : "  " + sv.details) + "\n";
        for (size_t c = 0; c < n.children.size(); c++) line(n.children[c], childPrefix, c + 1 == n.children.size(), false);
    };
    if (!bt.nodes.empty()) line(0, "", true, true);
    return s;
}
