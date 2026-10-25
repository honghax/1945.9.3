// 最小 3D 数学库（列主序，与 OpenGL 一致）。不引入 glm，保持零额外依赖。
#pragma once

#include <cmath>

struct Vec2 { float x = 0.f, y = 0.f; };

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(const Vec3& a)
{
    const float len = Length(a);
    return len > 1e-8f ? a * (1.f / len) : Vec3(0.f, 1.f, 0.f);
}

struct Vec4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

// 列主序 4x4：m[col * 4 + row]
struct Mat4 {
    float m[16] = { 1.f, 0.f, 0.f, 0.f,
                    0.f, 1.f, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f,
                    0.f, 0.f, 0.f, 1.f };

    static Mat4 Identity() { return Mat4{}; }

    static Mat4 Perspective(float fovyRad, float aspect, float znear, float zfar)
    {
        Mat4 r;
        const float f = 1.f / std::tan(fovyRad * 0.5f);
        for (int i = 0; i < 16; ++i) r.m[i] = 0.f;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zfar + znear) / (znear - zfar);
        r.m[11] = -1.f;
        r.m[14] = (2.f * zfar * znear) / (znear - zfar);
        return r;
    }

    static Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up)
    {
        const Vec3 f = Normalize(center - eye);
        const Vec3 s = Normalize(Cross(f, up));
        const Vec3 u = Cross(s, f);
        Mat4 r;
        r.m[0] = s.x;  r.m[1] = u.x;  r.m[2] = -f.x; r.m[3] = 0.f;
        r.m[4] = s.y;  r.m[5] = u.y;  r.m[6] = -f.y; r.m[7] = 0.f;
        r.m[8] = s.z;  r.m[9] = u.z;  r.m[10] = -f.z; r.m[11] = 0.f;
        r.m[12] = -Dot(s, eye);
        r.m[13] = -Dot(u, eye);
        r.m[14] = Dot(f, eye);
        r.m[15] = 1.f;
        return r;
    }

    static Mat4 Translate(const Vec3& t)
    {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 Scale(float sx, float sy, float sz)
    {
        Mat4 r;
        r.m[0] = sx;
        r.m[5] = sy;
        r.m[10] = sz;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

inline Vec4 Mul(const Mat4& m, const Vec4& v)
{
    Vec4 r;
    r.x = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w;
    r.y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w;
    r.z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w;
    r.w = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w;
    return r;
}
