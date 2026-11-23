#pragma once
#include "ue/Package.h"
#include "render/Math.h"
#include <random>

// Cascade particle systems (UParticleSystem): emitters, their modules with the baked distribution tables, and a small
// CPU simulation of the common modules for sprite previews.

// A baked distribution (FRawDistribution lookup table, or the distribution object when the table is missing).
struct ParticleCurve {
    int op = 0;              // 1 value, 2 random between min and max, 3 either min or max (RDO_*)
    int dims = 0;            // values per entry (1 float, 3 vector)
    int count = 0;           // entries over time
    float timeScale = 0, timeBias = 0;
    std::vector<float> values;   // count * (op == 1 ? dims : 2 * dims)
    std::string source;          // "table" or the distribution class
    bool Valid() const { return count > 0 && dims > 0; }
    // channel `ch` at time t (0..1 of life / emitter loop), rnd 0..1 picks inside a random range
    float Eval(float t, int ch = 0, float rnd = 0.5f) const;
    Vec3 Eval3(float t, float rnd = 0.5f) const;
    void Range(int ch, float& mn, float& mx) const;   // over all entries
    std::string Describe() const;                       // "5", "0.3 .. 0.6", "curve (8 keys) 0 .. 1"
};

// UVectorFieldStatic: SizeX x SizeY x SizeZ vectors (4 x half per voxel in the bulk data) over Bounds.
struct VectorFieldData {
    int sx = 0, sy = 0, sz = 0;
    Vec3 boundsMin, boundsMax;
    std::vector<Vec3> v;
    // trilinear sample at a position in the field's local space (Bounds units); outside: clamped, or wrapped per tiled axis
    Vec3 Sample(Vec3 local, bool tileX = false, bool tileY = false, bool tileZ = false) const;
};
bool ReadVectorField(PackageCache& cache, const std::string& path, VectorFieldData& out, std::string* error = nullptr);

struct ParticleModuleInfo {
    std::string cls, name;
    std::shared_ptr<const VectorFieldData> vectorField;   // ParticleModuleVectorFieldLocal
    bool enabled = true;
    std::vector<std::pair<std::string, ParticleCurve>> curves;
    std::vector<std::pair<std::string, std::string>> values;   // other simple properties
    bool simulated = false;                                    // understood by the preview simulation
    const char* previewNote = nullptr;                         // why a standalone preview cannot show it (needs the game world, ...)
    const ParticleCurve* Curve(std::string_view n) const;
    std::string Value(std::string_view n) const;
};

struct ParticleEmitterInfo {
    std::string name, cls, type;   // type: Sprite, Mesh, GPU sprites, Beam, Ribbon, AnimTrail
    std::string material, mesh;
    std::string screenAlignment, blendMode;
    bool localSpace = false, enabled = true;
    float duration = 1, delay = 0;
    int loops = 0;                 // 0 = forever
    int subH = 1, subV = 1;
    std::string subUVMethod;
    int peakParticles = 0;
    struct Burst { int count = 0, countLow = -1; float time = 0; };
    std::vector<Burst> bursts;
    ParticleCurve rate, rateScale;
    ParticleCurve loopTime, subUVLoopTime;   // SoT: particles with no lifetime loop their relative time over these seconds
    Vec3 origin;
    int axisLock = 0;              // ParticleModuleOrientationAxisLock: 0 none, 1..3 EPAL_X/Y/Z, 4..6 EPAL_NEGATIVE_*, 7..9 EPAL_ROTATE_*
    bool alignToVelocity = false;  // ParticleModuleAlignRotationToVelocity: texture top along the velocity
    std::vector<ParticleModuleInfo> modules;   // required + spawn + type data + the module stack
};

struct ParticleSystemInfo {
    std::string path;
    std::vector<ParticleEmitterInfo> emitters;
    std::vector<float> lodDistances;
    Vec3 boundsMin, boundsMax;
    bool fixedBounds = false;
    float warmup = 0;
};
bool ReadParticleSystem(PackageCache& cache, const Package& pkg, size_t exportIdx, ParticleSystemInfo& out, std::string* error = nullptr);
// ParticleModulePivotOffset of an emitter (UV units added to the centred pivot; (0, -0.5) puts the sprite's bottom edge on the particle).
Vec2 ParticlePivot(const ParticleEmitterInfo& e);

struct ParticleSprite {
    Vec3 pos, velocity;
    Vec2 size;
    Vec4 color;     // rgb + alpha
    float rotation = 0;   // radians
    float subImage = 0;
    Vec3 meshRot;         // mesh emitters: roll, pitch, yaw in turns (ParticleModuleMeshRotation / RotationRate)
    float sizeZ = 1;      // mesh emitters: the third scale axis
    int emitter = 0;
};

// Deterministic simulation (fixed 30 Hz steps) of every emitter from t = 0.
class ParticleSim {
public:
    void Init(const ParticleSystemInfo& ps, u32 seed = 1);
    void Reset();
    void Step(float dt);
    void AdvanceTo(float seconds);   // resets when going back in time
    float Time() const { return m_time; }
    void Sprites(std::vector<ParticleSprite>& out) const;
    int Count() const;

private:
    struct Particle {
        Vec3 pos, vel, accel, meshRot, meshRotRate;
        Vec3 orbitOffset, orbitRot, orbitRate;   // ParticleModuleOrbit: rendered offset, rotation (turns) and its rate
        Vec3 baseSize, baseColor;
        float vfScale = 1;   // ParticleModuleVectorFieldScale (at spawn)
        float speed0 = 0;    // speed at spawn (SizeOverVelocity input)
        float baseAlpha = 1, age = 0, life = 1, loopTime = 0, subLoopTime = 0, rot = 0, rotRate = 0, subRnd = 0, drag = 0, rnd = 0.5f;
    };
    struct Emitter {
        const ParticleEmitterInfo* info = nullptr;
        float time = 0, spawnAcc = 0;
        int loop = 0;
        size_t burstNext = 0;
        bool done = false;
        std::vector<Particle> parts;
    };
    void Spawn(Emitter& e, float emitterT, int n);
    const ParticleSystemInfo* m_ps = nullptr;
    std::vector<Emitter> m_emitters;
    std::mt19937 m_rng;
    u32 m_seed = 1;
    float m_time = 0;
    float Rnd();
};

// Simple software render of sprites (textured, subUV, alpha or additive) for CLI previews.
struct Image;
struct ParticleRenderEmitter { const Image* texture = nullptr; int subH = 1, subV = 1; bool additive = false, frameBlend = false; std::string alignment; Vec2 pivot; int axisLock = 0; bool alignToVelocity = false; };
// Unit axes of a sprite quad (ux right, uy = texture top) for a camera basis: screen facing and turned by `rotation`,
// locked to an axis plane / rotating about an axis (axisLock), or with its top along the velocity.
void ParticleQuadAxes(int axisLock, bool velocityAligned, float rotation, Vec3 velocity, Vec3 right, Vec3 up, Vec3 fwd, Vec3& ux, Vec3& uy);
void RenderParticleSprites(const std::vector<ParticleSprite>& sprites, const std::vector<ParticleRenderEmitter>& emitters, Vec3 target, float distance,
                           float yawDeg, float pitchDeg, int width, int height, Image& out);
// Camera basis shared by the GUI and the software renderer (UE axes: X forward, Y right, Z up).
void ParticleCamera(Vec3 target, float distance, float yawDeg, float pitchDeg, Vec3& eye, Vec3& right, Vec3& up, Vec3& fwd);

// The texture a particle material most likely draws (guessed diffuse, else the first texture parameter).
std::string ParticleTexturePath(PackageCache& cache, const std::string& material);
bool LoadParticleTexture(PackageCache& cache, const std::string& material, int maxDim, Image& out);
// Camera target / distance that frame the simulated sprites up to `upTo` seconds.
void ParticleFrame(const ParticleSystemInfo& ps, float upTo, Vec3& centre, float& dist);
