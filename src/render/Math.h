#pragma once
#include "ue/Mesh.h"
#include <cmath>

// Row-vector math (v' = v * M) in Unreal space: left handed, X forward, Y right, Z up, centimetres.
constexpr float kPi = 3.14159265358979f;

inline Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const Vec3& a) { return sqrtf(Dot(a, a)); }
inline Vec3 Normalize(const Vec3& a) { float l = Length(a); return l > 1e-12f ? a * (1.f / l) : Vec3{0, 0, 1}; }

struct Mat4 {
    float m[4][4];
    static Mat4 Identity() {
        Mat4 r{};
        for (int i = 0; i < 4; i++) r.m[i][i] = 1;
        return r;
    }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
        return r;
    }
    Vec3 TransformPoint(const Vec3& v) const {
        return {v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0], v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1],
                v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2]};
    }
    Vec3 TransformDir(const Vec3& v) const {
        return {v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0], v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1], v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]};
    }
    Vec3 Translation() const { return {m[3][0], m[3][1], m[3][2]}; }
    float MaxScale() const {
        float a = m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2];
        float b = m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2];
        float c = m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2];
        return sqrtf(std::max(a, std::max(b, c)));
    }
    Mat4 Transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = m[j][i];
        return r;
    }
    // UE FTransform semantics: scale, then rotate (quaternion), then translate
    static Mat4 FromTRS(const Vec3& t, const Vec4& q, const Vec3& s) {
        float x = q.x, y = q.y, z = q.z, w = q.w;
        float x2 = x + x, y2 = y + y, z2 = z + z;
        float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2, wx = w * x2, wy = w * y2, wz = w * z2;
        Mat4 r{};
        r.m[0][0] = (1 - (yy + zz)) * s.x; r.m[0][1] = (xy + wz) * s.x; r.m[0][2] = (xz - wy) * s.x;
        r.m[1][0] = (xy - wz) * s.y; r.m[1][1] = (1 - (xx + zz)) * s.y; r.m[1][2] = (yz + wx) * s.y;
        r.m[2][0] = (xz + wy) * s.z; r.m[2][1] = (yz - wx) * s.z; r.m[2][2] = (1 - (xx + yy)) * s.z;
        r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z; r.m[3][3] = 1;
        return r;
    }
    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {   // left handed view
        Vec3 f = Normalize(target - eye), s = Normalize(Cross(up, f)), u = Cross(f, s);
        Mat4 r = Identity();
        r.m[0][0] = s.x; r.m[1][0] = s.y; r.m[2][0] = s.z;
        r.m[0][1] = u.x; r.m[1][1] = u.y; r.m[2][1] = u.z;
        r.m[0][2] = f.x; r.m[1][2] = f.y; r.m[2][2] = f.z;
        r.m[3][0] = -Dot(s, eye); r.m[3][1] = -Dot(u, eye); r.m[3][2] = -Dot(f, eye);
        return r;
    }
    // reversed-Z infinite perspective (depth 1 at near, 0 at infinity)
    static Mat4 PerspectiveReversed(float fovY, float aspect, float zNear) {
        float h = 1.f / tanf(fovY * 0.5f);
        Mat4 r{};
        r.m[0][0] = h / aspect;
        r.m[1][1] = h;
        r.m[2][3] = 1;
        r.m[3][2] = zNear;
        return r;
    }
};

// UE FRotator (degrees) -> quaternion, matching FRotator::Quaternion()
inline Vec4 RotatorToQuat(float pitch, float yaw, float roll) {
    const float d2r = kPi / 180.f / 2.f;
    float sp = sinf(pitch * d2r), cp = cosf(pitch * d2r), sy = sinf(yaw * d2r), cy = cosf(yaw * d2r), sr = sinf(roll * d2r), cr = cosf(roll * d2r);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}

struct Frustum {
    float planes[6][4];
    void FromViewProj(const Mat4& vp) {
        // row-vector clip = v * VP: plane coefficients come from the columns
        auto col = [&](int c, float* o) { for (int r = 0; r < 4; r++) o[r] = vp.m[r][c]; };
        float c0[4], c1[4], c2[4], c3[4];
        col(0, c0); col(1, c1); col(2, c2); col(3, c3);
        for (int i = 0; i < 4; i++) {
            planes[0][i] = c3[i] + c0[i];
            planes[1][i] = c3[i] - c0[i];
            planes[2][i] = c3[i] + c1[i];
            planes[3][i] = c3[i] - c1[i];
            planes[4][i] = c2[i];          // reversed Z: far plane at infinity -> only z >= 0
            planes[5][i] = c3[i] - c2[i];  // near
        }
        for (auto& p : planes) {
            float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (l > 0) for (int i = 0; i < 4; i++) p[i] /= l;
        }
    }
    bool SphereVisible(const Vec3& c, float r) const {
        for (int i = 0; i < 6; i++)
            if (planes[i][0] * c.x + planes[i][1] * c.y + planes[i][2] * c.z + planes[i][3] < -r) return false;
        return true;
    }
};
