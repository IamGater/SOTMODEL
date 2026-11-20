// Cooked AnimSequence decoding for Athena's UE 4.10 fork. Follows the Athena toolchain's decode_cooked_animation.py:
//  - AKF_PerTrackCompression: per-track header (key count:24, component flags:4, format:4), keys, optional frame table
//  - AKF_ConstantKeyLerp / AKF_VariableKeyLerp: headerless tracks (offset, count) using the sequence-wide formats
//  - Athena writes a 16-byte GUID after the zero object-GUID marker
#include "ue/Animation.h"
#include <cmath>

bool IsAnimSequenceClass(std::string_view c) { return c == "AnimSequence"; }

namespace {
enum KeyFormat { F_None = 0, F_Float96NoW = 1, F_Fixed48NoW = 2, F_IntervalFixed32NoW = 3, F_Fixed32NoW = 4, F_Float32NoW = 5, F_Identity = 6 };

int FormatFromName(const std::string& s) {
    static const char* names[] = {"ACF_None", "ACF_Float96NoW", "ACF_Fixed48NoW", "ACF_IntervalFixed32NoW", "ACF_Fixed32NoW", "ACF_Float32NoW", "ACF_Identity"};
    for (int i = 0; i < 7; i++) if (s == names[i]) return i;
    return F_None;
}

u32 Rd32(const Bytes& s, size_t o) {
    if (o + 4 > s.size()) throw ParseError("animation stream overrun");
    u32 v;
    memcpy(&v, &s[o], 4);
    return v;
}
float RdF(const Bytes& s, size_t o) { u32 v = Rd32(s, o); float f; memcpy(&f, &v, 4); return f; }
u16 Rd16(const Bytes& s, size_t o) {
    if (o + 2 > s.size()) throw ParseError("animation stream overrun");
    return (u16)(s[o] | (s[o + 1] << 8));
}

bool HeaderOk(const Bytes& s, i64 off, int frames) {
    if (off < 0 || (size_t)off + 4 > s.size()) return false;
    u32 h = Rd32(s, (size_t)off);
    u32 n = h & 0xffffff, f = h >> 28;
    return n >= 1 && (int)n <= frames && (f == 1 || f == 2 || f == 3 || f == 4 || f == 6);
}

size_t BlockSize(const Bytes& s, size_t off, int frames) {
    u32 h = Rd32(s, off);
    u32 count = h & 0xffffff, flags = (h >> 24) & 15, fmt = h >> 28;
    int axes = 0;
    for (int i = 0; i < 3; i++) if (flags & (1 << i)) axes++;
    if (!axes) axes = 3;
    size_t size = 4;
    if (fmt == 1) size += (size_t)count * 4 * axes;
    else if (fmt == 2) size += (size_t)count * 2 * axes;
    else if (fmt == 3) size += 8 * axes + (size_t)count * 4;
    else size += (size_t)count * 4;
    size = (size + 3) & ~(size_t)3;
    if (count > 1 && (flags & 8)) {
        size += (size_t)count * (frames > 255 ? 2 : 1);
        size = (size + 3) & ~(size_t)3;
    }
    return size;
}

// keys at `times` (frame numbers) -> one value per frame, linear (nlerp for rotations)
template <typename V> void Resample(const std::vector<V>& keys, const std::vector<float>& times, int frames, std::vector<V>& out, bool quat) {
    out.resize(frames);
    if (keys.size() == 1) { std::fill(out.begin(), out.end(), keys[0]); return; }
    size_t k = 0;
    for (int f = 0; f < frames; f++) {
        while (k + 1 < keys.size() - 1 && times[k + 1] <= f) k++;
        float t0 = times[k], t1 = times[k + 1];
        float a = t1 > t0 ? std::clamp((f - t0) / (t1 - t0), 0.f, 1.f) : 0.f;
        const V& x = keys[k];
        const V& y = keys[k + 1];
        if constexpr (std::is_same_v<V, Vec4>) {
            Vec4 r{x.x + (y.x - x.x) * a, x.y + (y.y - x.y) * a, x.z + (y.z - x.z) * a, x.w + (y.w - x.w) * a};
            float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (l > 1e-8f) { r.x /= l; r.y /= l; r.z /= l; r.w /= l; }
            out[f] = r;
        } else {
            (void)quat;
            out[f] = x + (y - x) * a;
        }
    }
}

std::vector<float> EvenTimes(size_t count, int frames) {
    std::vector<float> t(count);
    for (size_t i = 0; i < count; i++) t[i] = count > 1 ? (float)i * (frames - 1) / (count - 1) : 0.f;
    return t;
}

std::vector<float> FrameTable(const Bytes& s, size_t pos, size_t count, int frames) {
    pos = (pos + 3) & ~(size_t)3;   // aligned within the compressed stream
    std::vector<float> t(count);
    for (size_t i = 0; i < count; i++) t[i] = frames > 255 ? Rd16(s, pos + i * 2) : (pos + i < s.size() ? s[pos + i] : throw ParseError("frame table overrun"));
    for (size_t i = 1; i < count; i++)
        if (t[i] <= t[i - 1]) throw ParseError("frame table not increasing");
    return t;
}

void FixQuatSigns(std::vector<Vec4>& q) {
    for (size_t i = 1; i < q.size(); i++) {
        auto& a = q[i - 1];
        auto& b = q[i];
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    }
}

Vec4 QuatFromXYZ(float x, float y, float z) {
    float w = sqrtf(std::max(0.f, 1.f - (x * x + y * y + z * z)));
    float l = sqrtf(x * x + y * y + z * z + w * w);
    return l > 1e-8f ? Vec4{x / l, y / l, z / l, w / l} : Vec4{0, 0, 0, 1};
}

// AKF_PerTrackCompression track; rotation selects the quaternion packing. Writes per-frame values.
void PerTrack(const Bytes& s, size_t off, int frames, bool rotation, std::vector<Vec3>* vec, std::vector<Vec4>* quat) {
    u32 h = Rd32(s, off);
    u32 count = h & 0xffffff, flags = (h >> 24) & 15, fmt = h >> 28;
    if (count < 1 || (int)count > frames) throw ParseError(Fmt("bad key count %u", count));
    int axes[3], na = 0;
    for (int i = 0; i < 3; i++) if (flags & (1 << i)) axes[na++] = i;
    if (fmt == 1 && !na) { axes[0] = 0; axes[1] = 1; axes[2] = 2; na = 3; }
    size_t p = off + 4;
    float mins[3] = {}, ranges[3] = {};
    if (fmt == 3)
        for (int i = 0; i < na; i++) { mins[axes[i]] = RdF(s, p); ranges[axes[i]] = RdF(s, p + 4); p += 8; }
    std::vector<Vec3> v3(count);
    std::vector<Vec4> v4(count);
    for (u32 k = 0; k < count; k++) {
        float c[3] = {0, 0, 0};
        if (fmt == 1) {
            for (int i = 0; i < na; i++) { c[axes[i]] = RdF(s, p); p += 4; }
        } else if (fmt == 2) {
            for (int i = 0; i < na; i++) {
                float raw = Rd16(s, p);
                c[axes[i]] = rotation ? (raw - 32767.f) / 32767.f : raw - 255.f;
                p += 2;
            }
        } else if (fmt == 3 || fmt == 4) {
            u32 pk = Rd32(s, p);
            p += 4;
            if (rotation) {
                c[0] = ((float)(pk >> 21) - 1023.f) / 1023.f;
                c[1] = ((float)((pk >> 10) & 2047) - 1023.f) / 1023.f;
                c[2] = ((float)(pk & 1023) - 511.f) / 511.f;
            } else {
                c[0] = ((float)(pk & 1023) - 511.f) / 511.f;
                c[1] = ((float)((pk >> 10) & 2047) - 1023.f) / 1023.f;
                c[2] = ((float)(pk >> 21) - 1023.f) / 1023.f;
            }
            if (fmt == 3) for (int i = 0; i < 3; i++) c[i] = c[i] * ranges[i] + mins[i];
        }
        if (rotation) v4[k] = QuatFromXYZ(c[0], c[1], c[2]);
        else v3[k] = {c[0], c[1], c[2]};
    }
    std::vector<float> times = count > 1 && (flags & 8) ? FrameTable(s, p, count, frames) : EvenTimes(count, frames);
    if (rotation) {
        FixQuatSigns(v4);
        Resample(v4, times, frames, *quat, true);
    } else {
        Resample(v3, times, frames, *vec, false);
    }
}

// AKF_ConstantKeyLerp / AKF_VariableKeyLerp: headerless keys in the sequence-wide format
void KeyLerpTrack(const Bytes& s, size_t off, int count, int frames, bool rotation, int fmt, bool variable, std::vector<Vec3>* vec, std::vector<Vec4>* quat) {
    if (count < 1 || count > frames) throw ParseError(Fmt("bad key count %d", count));
    if (count == 1) fmt = rotation ? F_Float96NoW : F_None;
    size_t p = off;
    float mins[3] = {}, ranges[3] = {};
    if (fmt == F_IntervalFixed32NoW) {
        for (int i = 0; i < 3; i++) mins[i] = RdF(s, p + i * 4);
        for (int i = 0; i < 3; i++) ranges[i] = RdF(s, p + 12 + i * 4);
        p += 24;
    }
    std::vector<Vec3> v3(count);
    std::vector<Vec4> v4(count);
    for (int k = 0; k < count; k++) {
        float c[4] = {0, 0, 0, 1};
        switch (fmt) {
        case F_None:
            if (rotation) { for (int i = 0; i < 4; i++) c[i] = RdF(s, p + i * 4); p += 16; }
            else { for (int i = 0; i < 3; i++) c[i] = RdF(s, p + i * 4); p += 12; }
            break;
        case F_Float96NoW:
            for (int i = 0; i < 3; i++) c[i] = RdF(s, p + i * 4);
            p += 12;
            if (rotation) c[3] = -2;   // reconstruct w
            break;
        case F_Fixed48NoW:
            for (int i = 0; i < 3; i++) c[i] = rotation ? ((float)Rd16(s, p + i * 2) - 32767.f) / 32767.f : (float)Rd16(s, p + i * 2) - 255.f;
            p += 6;
            c[3] = -2;
            break;
        case F_IntervalFixed32NoW:
        case F_Fixed32NoW: {
            u32 pk = Rd32(s, p);
            p += 4;
            if (rotation) {
                c[0] = ((float)(pk >> 21) - 1023.f) / 1023.f;
                c[1] = ((float)((pk >> 10) & 2047) - 1023.f) / 1023.f;
                c[2] = ((float)(pk & 1023) - 511.f) / 511.f;
            } else {
                c[0] = ((float)(pk & 1023) - 511.f) / 511.f;
                c[1] = ((float)((pk >> 10) & 2047) - 1023.f) / 1023.f;
                c[2] = ((float)(pk >> 21) - 1023.f) / 1023.f;
            }
            if (fmt == F_IntervalFixed32NoW) for (int i = 0; i < 3; i++) c[i] = c[i] * ranges[i] + mins[i];
            c[3] = -2;
            break;
        }
        case F_Float32NoW: throw ParseError("ACF_Float32NoW key-lerp tracks are not supported");
        default: break;   // identity
        }
        if (rotation) {
            if (c[3] == -2) v4[k] = QuatFromXYZ(c[0], c[1], c[2]);
            else {
                float l = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
                v4[k] = l > 1e-8f ? Vec4{c[0] / l, c[1] / l, c[2] / l, c[3] / l} : Vec4{0, 0, 0, 1};
            }
        } else {
            v3[k] = {c[0], c[1], c[2]};
        }
    }
    std::vector<float> times = variable && count > 1 ? FrameTable(s, p, count, frames) : EvenTimes(count, frames);
    if (rotation) {
        FixQuatSigns(v4);
        Resample(v4, times, frames, *quat, true);
    } else {
        Resample(v3, times, frames, *vec, false);
    }
}
}

bool ReadAnimSequence(const Package& pkg, size_t exportIdx, AnimSequenceData& a, std::string* error) {
    try {
        a.name = pkg.Exports()[exportIdx].objectName;
        size_t end = 0;
        PropertyList props = pkg.ReadProperties(exportIdx, &end);
        a.numFrames = (i32)PropInt(props, "NumFrames");
        a.length = (float)PropFloat(props, "SequenceLength");
        if (i32 sk = PropObj(props, "Skeleton")) a.skeletonPath = pkg.FullPath(sk);
        a.encoding = FindValue(props, "KeyEncodingFormat") ? PropStr(props, "KeyEncodingFormat") : "AKF_ConstantKeyLerp";
        a.translationFormat = FindValue(props, "TranslationCompressionFormat") ? PropStr(props, "TranslationCompressionFormat") : "ACF_None";
        a.rotationFormat = FindValue(props, "RotationCompressionFormat") ? PropStr(props, "RotationCompressionFormat") : "ACF_None";
        a.additiveType = FindValue(props, "AdditiveAnimType") ? PropStr(props, "AdditiveAnimType") : "AAT_None";
        int frames = a.numFrames;
        if (frames < 1) throw ParseError("animation has no frames");

        std::vector<i32> map;
        if (auto t = FindValue(props, "TrackToSkeletonMapTable"))
            for (auto& v : t->items) {
                auto b = v.Field("BoneTreeIndex");
                map.push_back(b ? (i32)b->AsInt() : -1);
            }
        std::vector<i64> offsets;
        if (auto o = FindValue(props, "CompressedTrackOffsets"))
            for (auto& v : o->items) offsets.push_back(v.AsInt());
        std::vector<i64> scaleOffsets;
        int scaleStrip = 1;
        if (auto so = FindValue(props, "CompressedScaleOffsets")) {
            if (auto od = so->Field("OffsetData")) for (auto& v : od->items) scaleOffsets.push_back(v.AsInt());
            if (auto ss = so->Field("StripSize")) scaleStrip = std::max(1, (int)ss->AsInt());
        }
        bool perTrack = a.encoding == "AKF_PerTrackCompression";
        bool variable = a.encoding == "AKF_VariableKeyLerp";
        if (!perTrack && !variable && a.encoding != "AKF_ConstantKeyLerp") throw ParseError("unsupported key encoding " + a.encoding);
        size_t stride = perTrack ? 2 : 4;
        if (offsets.size() != map.size() * stride) throw ParseError(Fmt("track offsets %zu do not match %zu tracks", offsets.size(), map.size()));

        // native part: object GUID marker (+ Athena GUID), strip flags, cooked flag, compressed byte stream
        Reader r = pkg.ExportReader(exportIdx);
        r.Seek(end);
        if (r.U32() != 0) throw ParseError("unexpected object GUID");
        r.Skip(16);
        const Bytes& d = pkg.Data();
        size_t pos = r.Pos();
        static const u8 kMarker[6] = {1, 0, 1, 0, 0, 0};   // strip flags (editor data stripped) + bCooked = 1
        size_t serialEnd = pos + r.Remaining();
        if (pos + 6 <= d.size() && memcmp(&d[pos], kMarker, 6) != 0) {
            // some sequences carry an extra bone index array here: find the strip flags + cooked flag whose
            // stream size runs exactly to the end of the export
            bool found = false;
            for (size_t j = pos; j + 10 <= std::min(serialEnd, pos + 65536); j++)
                if (memcmp(&d[j], kMarker, 6) == 0) {
                    i32 sz;
                    memcpy(&sz, &d[j + 6], 4);
                    if (sz >= 0 && j + 10 + (size_t)sz == serialEnd) { r.Seek(j); found = true; break; }
                }
            if (!found) throw ParseError("compressed stream header not found");
        }
        r.Skip(2);   // strip flags
        if (r.I32() != 1) throw ParseError("animation is not cooked");
        i32 size = r.I32();
        if (size < 0 || (size_t)size > r.Remaining()) throw ParseError("bad compressed stream size");
        Bytes stream(size);
        if (size) r.ReadInto(stream.data(), size);

        // per-track: stored offsets can drift for sequences with float scale keys -> recompute in stream order
        if (perTrack) {
            bool ok = true;
            for (i64 o : offsets) if (o >= 0 && !HeaderOk(stream, o, frames)) { ok = false; break; }
            if (!ok) {
                std::vector<i64> fixed(offsets.size(), -1), fixedScale(map.size() * scaleStrip, -1);
                size_t p = 0;
                bool fits = true;
                for (size_t t = 0; t < map.size() && fits; t++) {
                    i64 sc = t * scaleStrip < scaleOffsets.size() ? scaleOffsets[t * scaleStrip] : -1;
                    bool present[3] = {offsets[t * 2] >= 0, offsets[t * 2 + 1] >= 0, sc >= 0};
                    for (int k = 0; k < 3; k++) {
                        if (!present[k]) continue;
                        if (!HeaderOk(stream, (i64)p, frames)) { fits = false; break; }
                        if (k < 2) fixed[t * 2 + k] = (i64)p;
                        else fixedScale[t * scaleStrip] = (i64)p;
                        p += BlockSize(stream, p, frames);
                    }
                }
                if (!fits || p != stream.size()) throw ParseError("neither stored nor sequential track offsets fit the stream");
                offsets = fixed;
                scaleOffsets = fixedScale;
                a.sequentialOffsets = true;
            }
        }

        int tf = FormatFromName(a.translationFormat), rf = FormatFromName(a.rotationFormat);
        a.tracks.resize(map.size());
        for (size_t t = 0; t < map.size(); t++) {
            AnimTrack& tr = a.tracks[t];
            tr.bone = map[t];
            if (perTrack) {
                i64 to = offsets[t * 2], ro = offsets[t * 2 + 1];
                if (to >= 0) PerTrack(stream, (size_t)to, frames, false, &tr.translation, nullptr);
                if (ro >= 0) PerTrack(stream, (size_t)ro, frames, true, nullptr, &tr.rotation);
                i64 so = t * scaleStrip < scaleOffsets.size() ? scaleOffsets[t * scaleStrip] : -1;
                if (so >= 0 && HeaderOk(stream, so, frames)) PerTrack(stream, (size_t)so, frames, false, &tr.scale, nullptr);
            } else {
                i64 to = offsets[t * 4], tc = offsets[t * 4 + 1], ro = offsets[t * 4 + 2], rc = offsets[t * 4 + 3];
                if (to >= 0 && tc > 0) KeyLerpTrack(stream, (size_t)to, (int)tc, frames, false, tf, variable, &tr.translation, nullptr);
                if (ro >= 0 && rc > 0) KeyLerpTrack(stream, (size_t)ro, (int)rc, frames, true, rf, variable, nullptr, &tr.rotation);
            }
        }
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

void SampleAnimation(const AnimSequenceData& anim, const std::vector<Bone>& ref, float frame, std::vector<Bone>& pose) {
    pose = ref;
    if (anim.numFrames < 1) return;
    frame = std::clamp(frame, 0.f, (float)(anim.numFrames - 1));
    int f0 = (int)floorf(frame), f1 = std::min(f0 + 1, anim.numFrames - 1);
    float a = frame - f0;
    for (auto& t : anim.tracks) {
        if (t.bone < 0 || t.bone >= (i32)pose.size()) continue;
        Bone& b = pose[t.bone];
        if (!t.translation.empty()) b.translation = t.translation[f0] + (t.translation[f1] - t.translation[f0]) * a;
        if (!t.rotation.empty()) {
            Vec4 x = t.rotation[f0], y = t.rotation[f1];
            if (x.x * y.x + x.y * y.y + x.z * y.z + x.w * y.w < 0) y = {-y.x, -y.y, -y.z, -y.w};
            Vec4 r{x.x + (y.x - x.x) * a, x.y + (y.y - x.y) * a, x.z + (y.z - x.z) * a, x.w + (y.w - x.w) * a};
            float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (l > 1e-8f) b.rotation = {r.x / l, r.y / l, r.z / l, r.w / l};
        }
        if (!t.scale.empty()) b.scale = t.scale[f0] + (t.scale[f1] - t.scale[f0]) * a;
    }
}
