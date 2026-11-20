#pragma once
#include "ue/Mesh.h"

// One animated bone: per-frame local transforms (Unreal space). Empty arrays = keep the reference pose value.
struct AnimTrack {
    i32 bone = -1;   // skeleton (USkeleton) bone index
    std::vector<Vec3> translation;
    std::vector<Vec4> rotation;
    std::vector<Vec3> scale;
};

struct AnimSequenceData {
    std::string name;
    std::string skeletonPath;
    i32 numFrames = 0;
    float length = 0;   // seconds
    std::string encoding, rotationFormat, translationFormat, additiveType;
    bool sequentialOffsets = false;   // stored offsets did not fit the stream (read in stream order)
    std::vector<AnimTrack> tracks;
    float FrameRate() const { return length > 0 && numFrames > 1 ? (numFrames - 1) / length : 30.f; }
};

bool IsAnimSequenceClass(std::string_view cls);
// Decodes a cooked AnimSequence (Athena legacy per-track / key-lerp compression) to per-frame tracks.
bool ReadAnimSequence(const Package& pkg, size_t exportIdx, AnimSequenceData& out, std::string* error = nullptr);

// Local pose of every skeleton bone at a (fractional) frame: reference pose overridden by the animated tracks.
void SampleAnimation(const AnimSequenceData& anim, const std::vector<Bone>& refPose, float frame, std::vector<Bone>& pose);
