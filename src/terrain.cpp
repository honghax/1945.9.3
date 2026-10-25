#include "terrain.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979323846f;

float Clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
float SmoothStep(float a, float b, float x)
{
    const float t = Clamp01((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}
float Gauss(float x, float mu, float sigma)
{
    const float d = (x - mu) / sigma;
    return std::exp(-0.5f * d * d);
}
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// 确定性哈希与值噪声（占位地形用，接入真实 DEM 后即被替换）
unsigned HashU(unsigned x, unsigned y, unsigned seed)
{
    unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float HashF(int x, int y, unsigned seed)
{
    return static_cast<float>(HashU(static_cast<unsigned>(x), static_cast<unsigned>(y), seed) & 0xFFFFu)
           / 65535.f;
}
float ValueNoise(float x, float y, unsigned seed)
{
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3.f - 2.f * fx);
    const float sy = fy * fy * (3.f - 2.f * fy);
    const float v00 = HashF(xi, yi, seed);
    const float v10 = HashF(xi + 1, yi, seed);
    const float v01 = HashF(xi, yi + 1, seed);
    const float v11 = HashF(xi + 1, yi + 1, seed);
    return Lerp(Lerp(v00, v10, sx), Lerp(v01, v11, sx), sy);
}
float Fbm(float x, float y, unsigned seed, int octaves)
{
    float sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * ValueNoise(x * freq, y * freq, seed + static_cast<unsigned>(o) * 131u);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return norm > 0.f ? sum / norm : 0.f;
}

}  // namespace

float HeightField::Sample(float u, float v) const
{
    if (w <= 0 || h <= 0 || data.empty()) return 0.f;
    const float x = Clamp01(u) * static_cast<float>(w - 1);
    const float y = Clamp01(v) * static_cast<float>(h - 1);
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = x0 + 1 < w ? x0 + 1 : x0;
    const int y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float v00 = data[static_cast<size_t>(y0) * static_cast<size_t>(w) + static_cast<size_t>(x0)];
    const float v10 = data[static_cast<size_t>(y0) * static_cast<size_t>(w) + static_cast<size_t>(x1)];
    const float v01 = data[static_cast<size_t>(y1) * static_cast<size_t>(w) + static_cast<size_t>(x0)];
    const float v11 = data[static_cast<size_t>(y1) * static_cast<size_t>(w) + static_cast<size_t>(x1)];
    return Lerp(Lerp(v00, v10, fx), Lerp(v01, v11, fx), fy);
}

bool HeightField::LoadRaw(const std::string& path, int width, int height)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) {
        std::printf("[terrain] 打不开高程文件：%s\n", path.c_str());
        return false;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
    data.resize(expected);
    const size_t got = fread(data.data(), sizeof(float), expected, f);
    fclose(f);
    if (got != expected) {
        std::printf("[terrain] 高程文件尺寸不符：期望 %zu 个 float，读到 %zu\n", expected, got);
        data.clear();
        return false;
    }
    w = width;
    h = height;
    minH = maxH = data[0];
    for (float value : data) {
        if (value < minH) minH = value;
        if (value > maxH) maxH = value;
    }
    std::printf("[terrain] 已加载高程 %dx%d，范围 %.0f ~ %.0f m\n", w, h, minH, maxH);
    return true;
}

void HeightField::GenerateProcedural(int width, int height)
{
    w = width;
    h = height;
    data.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0.f);

    for (int j = 0; j < h; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(h - 1);
        for (int i = 0; i < w; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(w - 1);
            const float lon = 111.0f + u * 5.0f;

            // 平原基底 + 分形起伏
            float elev = 35.f + 140.f * Fbm(u * 6.f, v * 6.f, 7u, 4);
            elev += 40.f * Fbm(u * 18.f, v * 18.f, 23u, 3);

            // 东侧罗霄山、幕阜山（经度 114.3 以东）
            const float east = SmoothStep(114.2f, 115.6f, lon) * (0.55f + 0.45f * Gauss(v, 0.25f, 0.30f));
            elev += 900.f * east;
            // 东南山地
            elev += 650.f * SmoothStep(0.72f, 1.0f, v) * SmoothStep(114.6f, 113.4f, lon);
            // 西侧雪峰山（经度 111.6 以西）
            elev += 800.f * SmoothStep(111.6f, 111.0f, lon);
            // 北部幕阜山系（通城、修水一带）
            elev += 620.f * Gauss(lon, 113.9f, 0.45f) * Gauss(v, 0.05f, 0.22f);
            // 湘江—洞庭湖低地：长沙、湘阴、岳阳一线
            const float corridor = Gauss(lon, 112.95f, 0.30f) * Gauss(v, 0.55f, 0.42f);
            elev = Lerp(elev, 22.f + 18.f * Fbm(u * 12.f, v * 12.f, 41u, 2), corridor * 0.85f);
            // 洞庭湖水域
            const float lake = Gauss(lon, 112.80f, 0.16f) * Gauss(v, 0.30f, 0.13f);
            elev = Lerp(elev, -18.f, SmoothStep(0.25f, 0.75f, lake));

            data[static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)] = elev;
        }
    }

    minH = maxH = data[0];
    for (float value : data) {
        if (value < minH) minH = value;
        if (value > maxH) maxH = value;
    }
    std::printf("[terrain] 占位高程 %dx%d，范围 %.0f ~ %.0f m（未接入真实 DEM）\n", w, h, minH, maxH);
}

TerrainMesh::~TerrainMesh() { Destroy(); }

void TerrainMesh::Destroy()
{
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    indexCount_ = 0;
}

void TerrainMesh::Build(const HeightField& hf, const GeoBounds& bounds, int meshRes,
                        float heightExaggeration)
{
    Destroy();

    const int n = meshRes < 2 ? 2 : meshRes;
    const float midLat = 0.5f * (bounds.latMin + bounds.latMax);
    const float kmPerLon = 111.32f * std::cos(midLat * kPi / 180.f);
    worldW_ = bounds.LonSpan() * kmPerLon;
    worldD_ = bounds.LatSpan() * 110.9f;

    std::vector<float> heights(static_cast<size_t>(n) * static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(n - 1);
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            // 世界单位 km
            heights[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)] =
                hf.Sample(u, v) * 0.001f * heightExaggeration;
        }
    }

    const float dx = worldW_ / static_cast<float>(n - 1);
    const float dz = worldD_ / static_cast<float>(n - 1);

    std::vector<float> verts(static_cast<size_t>(n) * static_cast<size_t>(n) * 8);
    size_t k = 0;
    for (int j = 0; j < n; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(n - 1);
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            const size_t ci = static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i);
            const float y = heights[ci];

            const int il = i > 0 ? i - 1 : i;
            const int ir = i + 1 < n ? i + 1 : i;
            const int ju = j > 0 ? j - 1 : j;
            const int jd = j + 1 < n ? j + 1 : j;
            const float hl = heights[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(il)];
            const float hr = heights[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(ir)];
            const float hu = heights[static_cast<size_t>(ju) * static_cast<size_t>(n) + static_cast<size_t>(i)];
            const float hd = heights[static_cast<size_t>(jd) * static_cast<size_t>(n) + static_cast<size_t>(i)];
            const float dhdx = (hr - hl) / (static_cast<float>(ir - il) * dx);
            const float dhdz = (hd - hu) / (static_cast<float>(jd - ju) * dz);
            const float len = std::sqrt(dhdx * dhdx + 1.f + dhdz * dhdz);

            verts[k++] = (u - 0.5f) * worldW_;
            verts[k++] = y;
            verts[k++] = (v - 0.5f) * worldD_;
            verts[k++] = -dhdx / len;
            verts[k++] = 1.f / len;
            verts[k++] = -dhdz / len;
            verts[k++] = u;
            verts[k++] = v;
        }
    }

    std::vector<unsigned> idx;
    idx.reserve(static_cast<size_t>(n - 1) * static_cast<size_t>(n - 1) * 6);
    for (int j = 0; j < n - 1; ++j) {
        for (int i = 0; i < n - 1; ++i) {
            const unsigned a = static_cast<unsigned>(j * n + i);
            const unsigned b = a + 1;
            const unsigned c = a + static_cast<unsigned>(n);
            const unsigned d = c + 1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }
    indexCount_ = static_cast<int>(idx.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idx.size() * sizeof(unsigned)),
                 idx.data(), GL_STATIC_DRAW);

    const GLsizei stride = static_cast<GLsizei>(8 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(6 * sizeof(float)));
    glBindVertexArray(0);

    std::printf("[terrain] 网格 %dx%d，三角形 %d，世界尺寸 %.1f x %.1f km\n",
                n, n, indexCount_ / 3, worldW_, worldD_);
}

void TerrainMesh::Draw() const
{
    if (vao_ == 0) return;
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void TerrainMesh::LonLatToWorld(const HeightField& hf, const GeoBounds& bounds, float lon,
                                float lat, float heightExaggeration, float* outX, float* outY,
                                float* outZ) const
{
    const float u = (lon - bounds.lonMin) / bounds.LonSpan();
    const float v = (bounds.latMax - lat) / bounds.LatSpan();
    const float ground = hf.Sample(u, v) * 0.001f * heightExaggeration;
    if (outX) *outX = (u - 0.5f) * worldW_;
    if (outY) *outY = ground;
    if (outZ) *outZ = (v - 0.5f) * worldD_;
}
