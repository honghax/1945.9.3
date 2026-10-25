#include "water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

#include "math3d.h"

namespace {

std::vector<std::string> SplitCsv(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (quoted) {
            if (c == '"') quoted = false;
            else cur.push_back(c);
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
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

}  // namespace

bool WaterSurface::LoadBodies(const std::string& csvPath)
{
    bodies_.clear();

    std::ifstream in(csvPath, std::ios::binary);
    if (!in) {
        std::printf("[water] 打不开水体表：%s\n", csvPath.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    struct Group {
        std::vector<std::pair<int, std::pair<float, float>>> pts;
    };
    std::map<std::string, Group> grouped;
    std::vector<std::string> order;

    std::istringstream lines(text);
    std::string line;
    bool header = false;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = SplitCsv(line);
        if (f.size() < 4) continue;
        if (!header) {
            header = true;
            if (Trim(f[0]) == "body") continue;
        }
        const std::string name = Trim(f[0]);
        const int idx = std::atoi(Trim(f[1]).c_str());
        const float lon = static_cast<float>(std::atof(Trim(f[2]).c_str()));
        const float lat = static_cast<float>(std::atof(Trim(f[3]).c_str()));
        if (grouped.find(name) == grouped.end()) order.push_back(name);
        grouped[name].pts.emplace_back(idx, std::make_pair(lon, lat));
    }

    for (const std::string& name : order) {
        Group& g = grouped[name];
        std::sort(g.pts.begin(), g.pts.end(),
                  [](const std::pair<int, std::pair<float, float>>& a,
                     const std::pair<int, std::pair<float, float>>& b) { return a.first < b.first; });
        if (g.pts.size() < 3) continue;
        WaterBody body;
        body.name = name;
        for (const auto& p : g.pts) {
            body.lons.push_back(p.second.first);
            body.lats.push_back(p.second.second);
        }
        bodies_.push_back(body);
    }

    std::printf("[water] 已加载 %zu 个水体多边形\n", bodies_.size());
    return !bodies_.empty();
}

void WaterSurface::Destroy()
{
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    indexCount_ = 0;
}

void WaterSurface::Build(const HeightField& hf, const GeoBounds& bounds, float heightExaggeration,
                         const TerrainMesh& terrain, float waterLevelMeters)
{
    Destroy();
    waterLevel_ = waterLevelMeters;
    if (bodies_.empty()) return;

    float lonMin = 1e9f, lonMax = -1e9f, latMin = 1e9f, latMax = -1e9f;
    for (const WaterBody& b : bodies_) {
        for (size_t i = 0; i < b.lons.size(); ++i) {
            lonMin = std::min(lonMin, b.lons[i]);
            lonMax = std::max(lonMax, b.lons[i]);
            latMin = std::min(latMin, b.lats[i]);
            latMax = std::max(latMax, b.lats[i]);
        }
    }
    const float margin = 0.10f;
    lonMin -= margin;
    lonMax += margin;
    latMin -= margin;
    latMax += margin;

    constexpr int kN = 256;
    const float waterY = waterLevelMeters * 0.001f * heightExaggeration;

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(kN) * static_cast<size_t>(kN) * 6);

    for (int j = 0; j < kN; ++j) {
        const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(kN);
        const float lat = latMax - v * (latMax - latMin);
        for (int i = 0; i < kN; ++i) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(kN);
            const float lon = lonMin + u * (lonMax - lonMin);

            float x = 0.f, z = 0.f;
            terrain.LonLatToWorld(hf, bounds, lon, lat, heightExaggeration, &x, nullptr, &z);

            const float hfU = (lon - bounds.lonMin) / bounds.LonSpan();
            const float hfV = (bounds.latMax - lat) / bounds.LatSpan();
            const float terrainM = hf.Sample(hfU, hfV);
            const float depthM = waterLevelMeters - terrainM;

            // 湖区掩码：多边形内为正，边界外 3km 内渐变到 0
            float mask = 0.f;
            for (const WaterBody& b : bodies_) {
                const bool inside = PointInPolygon(b, lon, lat);
                const float dist = DistToBoundary(b, lon, lat);
                const float sdist = inside ? dist : -dist;
                const float a = std::max(0.f, std::min(1.f, sdist / 0.015f));
                if (a > mask) mask = a;
            }

            verts.push_back(x);
            verts.push_back(waterY);
            verts.push_back(z);
            verts.push_back(depthM);
            verts.push_back(mask);
            verts.push_back(0.f);   // riverMix：湖面为 0
        }
    }

    std::vector<unsigned> idx;
    idx.reserve(static_cast<size_t>(kN - 1) * static_cast<size_t>(kN - 1) * 6);
    for (int j = 0; j < kN - 1; ++j) {
        for (int i = 0; i < kN - 1; ++i) {
            const unsigned a = static_cast<unsigned>(j * kN + i);
            const unsigned b = a + 1;
            const unsigned c = a + static_cast<unsigned>(kN);
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

    std::printf("[water] 水面网格 %d x %d，水位 %.0f m，三角形 %d\n", kN, kN, waterLevelMeters,
                indexCount_ / 3);
}

void WaterSurface::Draw() const
{
    if (vao_ == 0) return;
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}
