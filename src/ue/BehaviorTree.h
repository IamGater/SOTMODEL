#pragma once
#include "ue/Package.h"

// Behavior trees (AIModule BehaviorTree packages): the node tree from RootNode through the composites' Children, each
// child's decorators (conditions on entering it) and the composites' services, plus the blackboard's keys.
struct BtAux {   // a decorator or a service
    std::string cls, name, details;   // class without its BTDecorator_ / BTService_ prefix, NodeName or that, property summary
};
struct BtNode {
    std::string cls, name, details;   // class without its BTComposite_ / BTTask_ prefix ("Selector", "SwimTo"), NodeName, summary
    bool composite = false;
    std::vector<BtAux> decorators;    // on the link from the parent into this node
    std::string decoratorLogic;       // DecoratorOps when they combine the decorators other than AND ("OR(0,1)")
    std::vector<BtAux> services;      // composites: run while the branch is active
    std::string subtree;              // RunBehavior: the behavior tree it runs (package path)
    std::vector<int> children;        // indices into BehaviorTreeInfo::nodes, in execution order
    int depth = 0;
};
struct BlackboardKey {
    std::string name, type;           // type: Object / Vector / Float / Bool / Enum ... (BlackboardKeyType_ without prefix)
    std::string detail;               // Object base class, Enum type
    std::string from;                 // the parent blackboard that declares it (empty: this one)
};
struct BehaviorTreeInfo {
    std::string name, blackboard;     // blackboard asset path
    std::vector<BlackboardKey> keys;
    std::vector<BtNode> nodes;        // nodes[0] is the root
    std::vector<std::string> subtrees;   // RunBehavior targets
};

// `cache` (optional) loads the blackboard (and its parents) for the keys
bool ReadBehaviorTree(const Package& pkg, BehaviorTreeInfo& out, PackageCache* cache, std::string* error);
// indented text: one line per node with its decorators / services
std::string BehaviorTreeText(const BehaviorTreeInfo& bt);
