#include "river_water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr float kPi = 3.14159265358979323846f;
// 水面比两岸低多少（米）：视觉下移，让岸线看得出来
constexpr float kWaterDropM = 15.f;
// 河槽底比原始地面低多少（米）：不追求真实深度，只要看起来像条河谷
constexpr float kChannelDepthM = 70.f;
// 河槽影响半径（km）
constexpr float kValleyRadiusKm = 2.4f;
// 河道在数据宽度上再放宽，避免细到看不见
constexpr float kWidthBoost = 1.4f;
// 每段折线的细分数
constexpr int kSubdiv = 10;
// 入湖过渡带宽度（km）
constexpr float kLakeFadeKm = 3.0f;
constexpr float kmPerDegLon = 97.4f;

float Smooth01(float x)
{
    const float t = std::max(0.f, std::min(1.f, x));
    return t * t * (3.f - 2.f * t);
}

bool PointInPolygon(const WaterBody& b, float lon, float lat)
{
    bool inside = false;
    const size_t n = b.lons.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const bool cond = (b.lats[i] > lat) != (b.lats[j] > lat);
        if (!cond) continue;
        const float t = (lat - b.lats[i]) / (b.lats[j] - b.lats[i]);
        if (lon < b.lons[i] + t * (b.lons[j] - b.lons[i])) inside = !inside;
    }
    return inside;
}

float DistToBoundary(const WaterBody& b, float lon, float lat)
{
    float best = 1e9f;
    const size_t n = b.lons.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float ax = b.lons[j];
        const float ay = b.lats[j];
        const float bx = b.lons[i];
        const float by = b.lats[i];
        const float dx = bx - ax;
        const float dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        float t = 0.f;
        if (len2 > 1e-12f) t = ((lon - ax) * dx + (lat - ay) * dy) / len2;
        t = std::max(0.f, std::min(1.f, t));
        const float px = ax + t * dx;
        const float py = ay + t * dy;
        const float d = std::sqrt((lon - px) * (lon - px) + (lat - py) * (lat - py));
        if (d < best) best = d;
    }
    return best;
}

// 湖区权重：多边形内为 1，边界外 kLakeFadeKm 内渐变到 0
float LakeMask(float lon, float lat, const std::vector<WaterBody>& bodies)
{
    float best = 0.f;
    for (const WaterBody& b : bodies) {
        if (b.lons.size() < 3) continue;
        if (PointInPolygon(b, lon, lat)) return 1.f;
        const float distKm = DistToBoundary(b, lon, lat) * kmPerDegLon;
        best = std::max(best, 1.f - std::min(1.f, distKm / kLakeFadeKm));
    }
    return best;
}

// 把 DEM 在 (lon, lat) 附近压到 targetM 以下，按距离衰减
void BurnDem(HeightField& hf, const GeoBounds& bounds, float lon, float lat, float targetM)
{
    constexpr float kmPerDegLat = 110.9f;
    const int w = hf.w;
    const int h = hf.h;
    if (w < 2 || h < 2) return;

    const float ci = (lon - bounds.lonMin) / bounds.LonSpan() * static_cast<float>(w - 1);
    const float cj = (bounds.latMax - lat) / bounds.LatSpan() * static_cast<float>(h - 1);

    const int ri = static_cast<int>(std::ceil(kValleyRadiusKm / kmPerDegLon
                                              / bounds.LonSpan() * static_cast<float>(w - 1))) + 1;
    const int rj = static_cast<int>(std::ceil(kValleyRadiusKm / kmPerDegLat
                                              / bounds.LatSpan() * static_cast<float>(h - 1))) + 1;
    const int i0 = std::max(0, static_cast<int>(ci) - ri);
    const int i1 = std::min(w - 1, static_cast<int>(ci) + ri);
    const int j0 = std::max(0, static_cast<int>(cj) - rj);
    const int j1 = std::min(h - 1, static_cast<int>(cj) + rj);

    for (int j = j0; j <= j1; ++j) {
        const float dLat = (static_cast<float>(j) - cj) * bounds.LatSpan()
                           / static_cast<float>(h - 1) * kmPerDegLat;
        for (int i = i0; i <= i1; ++i) {
            const float dLon = (static_cast<float>(i) - ci) * bounds.LonSpan()
                               / static_cast<float>(w - 1) * kmPerDegLon;
            const float dist = std::sqrt(dLon * dLon + dLat * dLat);
            if (dist > kValleyRadiusKm) continue;
            const float t = 1.f - Smooth01((dist - 0.35f) / (kValleyRadiusKm - 0.35f));
            float& cell = hf.data[static_cast<size_t>(j) * static_cast<size_t>(w)
                                  + static_cast<size_t>(i)];
            const float v = cell + (targetM - cell) * t;
            if (v < cell) cell = v;   // 只降不升
        }
    }
}

}  // namespace

void RiverSurface::Prepare(const std::vector<MapRoute>& routes, HeightField& hf,
                           const GeoBounds& bounds, float heightExaggeration,
                           const std::vector<WaterBody>& bodies, float lakeLevelMeters)
{
    profiles_.clear();

    const float midLat = 0.5f * (bounds.latMin + bounds.latMax);
    const float kmPerLon = 111.32f * std::cos(midLat * kPi / 180.f);
    const float worldW = bounds.LonSpan() * kmPerLon;
    const float worldD = bounds.LatSpan() * 110.9f;
    const float lakeY = lakeLevelMeters * 0.001f * heightExaggeration;

    for (const MapRoute& route : routes) {
        if (route.kind != "河流" || route.lons.size() < 2) continue;

        // 沿折线细分
        std::vector<float> lons;
        std::vector<float> lats;
        for (size_t i = 0; i + 1 < route.lons.size(); ++i) {
            for (int s = 0; s < kSubdiv; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(kSubdiv);
                lons.push_back(route.lons[i] + (route.lons[i + 1] - route.lons[i]) * t);
                lats.push_back(route.lats[i] + (route.lats[i + 1] - route.lats[i]) * t);
            }
        }
        lons.push_back(route.lons.back());
        lats.push_back(route.lats.back());

        // 采样原始高程并做平滑，避免水面随 DEM 噪声起伏
        std::vector<float> elev(lons.size(), 0.f);
        for (size_t i = 0; i < lons.size(); ++i) {
            const float u = (lons[i] - bounds.lonMin) / bounds.LonSpan();
            const float v = (bounds.latMax - lats[i]) / bounds.LatSpan();
            elev[i] = hf.Sample(u, v);
        }
        for (int pass = 0; pass < 4; ++pass) {
            const std::vector<float> tmp = elev;
            for (size_t i = 0; i < elev.size(); ++i) {
                float sum = 0.f;
                int cnt = 0;
                for (int k = -3; k <= 3; ++k) {
                    const int idx = static_cast<int>(i) + k;
                    if (idx < 0 || idx >= static_cast<int>(elev.size())) continue;
                    sum += tmp[static_cast<size_t>(idx)];
                    ++cnt;
                }
                elev[i] = cnt > 0 ? sum / static_cast<float>(cnt) : elev[i];
            }
        }

        Profile prof;
        prof.widthKm = route.widthKm;
        prof.pts.reserve(lons.size());
        prof.mix.reserve(lons.size());

        std::vector<float> masks(lons.size(), 0.f);
        for (size_t i = 0; i < lons.size(); ++i) {
            const float u = (lons[i] - bounds.lonMin) / bounds.LonSpan();
            const float v = (bounds.latMax - lats[i]) / bounds.LatSpan();
            const float x = (u - 0.5f) * worldW;
            const float z = (v - 0.5f) * worldD;

            // 入湖段把水面抬到湖面高度，河与湖才连成一片水
            const float lakeMask = LakeMask(lons[i], lats[i], bodies);
            masks[i] = lakeMask;

            const float riverY = (elev[i] - kWaterDropM) * 0.001f * heightExaggeration;
            const float y = riverY + (lakeY - riverY) * lakeMask;
            prof.pts.emplace_back(x, y, z);
            prof.mix.push_back(1.f - lakeMask);
        }
        profiles_.push_back(prof);

        // 下切河槽；湖区段减弱，避免把湖底挖出深坑
        for (size_t i = 0; i < lons.size(); ++i) {
            float target = elev[i] - kChannelDepthM;
            const float shallowTarget = elev[i] - 3.f;
            target += (shallowTarget - target) * masks[i];
            BurnDem(hf, bounds, lons[i], lats[i], target);
        }
    }

    std::printf("[river] 已下切 %zu 条河道并衔接到湖面\n", profiles_.size());
}

void RiverSurface::Destroy()
{
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    vertexCount_ = 0;
}

void RiverSurface::Build()
{
    Destroy();
    if (profiles_.empty()) return;

    std::vector<float> verts;

    for (const Profile& prof : profiles_) {
        if (prof.pts.size() < 2) continue;
        const float halfWidth = prof.widthKm * 0.5f * kWidthBoost;

        for (size_t i = 0; i + 1 < prof.pts.size(); ++i) {
            const Vec3 a = prof.pts[i];
            const Vec3 b = prof.pts[i + 1];
            Vec3 dir(b.x - a.x, 0.f, b.z - a.z);
            const float len = Length(dir);
            if (len < 1e-5f) continue;
            dir = dir * (1.f / len);
            const Vec3 n(-dir.z, 0.f, dir.x);

            const float mixA = prof.mix[i];
            const float mixB = prof.mix[i + 1];

            // 入湖段逐渐加宽，与湖面自然相接
            const float widenA = 1.f + (1.f - mixA) * 1.6f;
            const float widenB = 1.f + (1.f - mixB) * 1.6f;

            const Vec3 leftA = a + n * (halfWidth * widenA);
            const Vec3 rightA = a - n * (halfWidth * widenA);
            const Vec3 leftB = b + n * (halfWidth * widenB);
            const Vec3 rightB = b - n * (halfWidth * widenB);

            // 截面做成「岸边浅、河心深」，深度插值才有体积感
            constexpr float kEdgeDepth = 0.5f;
            constexpr float kCenterDepth = 3.4f;
            auto pushV = [&](const Vec3& p, float depth, float mixV) {
                verts.push_back(p.x);
                verts.push_back(p.y);
                verts.push_back(p.z);
                verts.push_back(depth);
                verts.push_back(1.f);
                verts.push_back(mixV);
            };

            // 左半条带
            pushV(leftA, kEdgeDepth, mixA);
            pushV(a, kCenterDepth, mixA);
            pushV(b, kCenterDepth, mixB);
            pushV(leftA, kEdgeDepth, mixA);
            pushV(b, kCenterDepth, mixB);
            pushV(leftB, kEdgeDepth, mixB);

            // 右半条带
            pushV(a, kCenterDepth, mixA);
            pushV(rightA, kEdgeDepth, mixA);
            pushV(rightB, kEdgeDepth, mixB);
            pushV(a, kCenterDepth, mixA);
            pushV(rightB, kEdgeDepth, mixB);
            pushV(b, kCenterDepth, mixB);
        }
    }

    vertexCount_ = static_cast<int>(verts.size() / 6);
    if (vertexCount_ == 0) return;

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);

    const GLsizei stride = static_cast<GLsizei>(6 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(5 * sizeof(float)));
    glBindVertexArray(0);

    std::printf("[river] 河道水面 %d 顶点\n", vertexCount_);
}

void RiverSurface::Draw() const
{
    if (vao_ == 0) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount_);
    glBindVertexArray(0);
}
