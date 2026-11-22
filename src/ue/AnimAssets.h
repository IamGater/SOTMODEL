#pragma once
#include "ue/Package.h"
#include "render/Math.h"
#include "ue/Mesh.h"

// Montages, blend spaces and animation blueprints (their baked state machines).

struct AnimNotifyInfo {
    std::string name;      // NotifyName
    std::string cls;       // notify / notify-state class
    float time = 0, duration = 0;
    int track = 0;
    std::string detail;    // a few interesting properties of the notify object (particle, sound, socket)
    // what the viewer plays: a WwiseEvent / sound (AnimNotify_WwiseSound, AnimNotify_PlaySound) or a particle system
    // (AnimNotify_PlayParticleEffect, AnimNotify_AthenaPlayParticleEffect) at a socket, following it when attached
    std::string sound, particle, socket;
    bool attached = true;
    Vec3 locationOffset, rotationOffset;   // rotation: pitch, yaw, roll (degrees)
};
bool ReadAnimNotifies(const Package& pkg, const PropertyList& props, std::vector<AnimNotifyInfo>& out);

struct MontageInfo {
    std::string skeleton;
    float length = 0, rateScale = 1, blendIn = 0, blendOut = 0;
    struct Section { std::string name, next; float start = 0; };
    struct Segment { std::string anim; float startPos = 0, animStart = 0, animEnd = 0, rate = 1; int loops = 1; float Length() const; };
    struct Slot { std::string name; std::vector<Segment> segments; };
    std::vector<Section> sections;
    std::vector<Slot> slots;
    std::vector<AnimNotifyInfo> notifies;
};
bool ReadMontage(const Package& pkg, size_t exportIdx, MontageInfo& out, std::string* error = nullptr);

struct BlendSpaceInfo {
    std::string skeleton;
    int dims = 2;   // 1 (BlendSpace1D / AimOffset1D) or 2
    struct Axis { std::string name; float min = 0, max = 100; int grid = 4; } axes[2];
    struct Sample { std::string anim; float x = 0, y = 0; };
    std::vector<Sample> samples;
    struct Grid { int index[3] = {-1, -1, -1}; float weight[3] = {}; };
    std::vector<Grid> grid;   // (axes[0].grid + 1) * (axes[1].grid + 1) precomputed triangulation weights
    // Sample weights for an input point (UE 4.10: bilinear over the 4 surrounding grid points' triangle weights).
    void Weights(float x, float y, std::vector<float>& out) const;
};
bool IsBlendSpaceClass(std::string_view cls);
bool ReadBlendSpace(const Package& pkg, size_t exportIdx, BlendSpaceInfo& out, std::string* error = nullptr);

struct StateMachineInfo {
    std::string name;
    int initial = 0;
    struct State { std::string name; bool conduit = false; std::vector<int> players; std::vector<int> transitions; };
    struct Transition { int from = 0, to = 0; float crossfade = 0; std::string blendMode, logic; int priority = 0; bool bidirectional = false; };
    std::vector<State> states;
    std::vector<Transition> transitions;
};
struct AnimBlueprintInfo {
    std::string skeleton;
    std::vector<StateMachineInfo> machines;
    struct Player { std::string node, asset; };   // sequence / blend space players on the class defaults
    std::vector<Player> players;
    std::vector<std::string> notifies;            // AnimNotifies event names
};
bool ReadAnimBlueprint(const Package& pkg, AnimBlueprintInfo& out, std::string* error = nullptr);

// Positions for drawing a state machine: states on a grid in breadth-first order from the initial state.
void LayoutStateMachine(const StateMachineInfo& sm, std::vector<Vec2>& positions);

// Weighted blend of local poses of the same bones (lerp translation / scale, sign-aligned quaternion nlerp).
void BlendPoses(const std::vector<std::vector<Bone>>& poses, const std::vector<float>& weights, std::vector<Bone>& out);
