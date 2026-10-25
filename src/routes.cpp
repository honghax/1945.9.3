#include "routes.h"

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

// 贴地抬升（世界单位 km）：略高于地形，避免与网格穿插
constexpr float kLift = 1.6f;

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

}  // namespace

bool RouteMesh::Load(const std::string& csvPath)
{
    routes_.clear();

    std::ifstream in(csvPath, std::ios::binary);
    if (!in) {
        std::printf("[routes] 打不开折线表：%s\n", csvPath.c_str());
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
        std::string kind;
        float widthKm = 1.f;
        std::vector<std::pair<int, std::pair<float, float>>> pts;
    };

    std::map<std::string, Group> grouped;
    std::vector<std::string> orderNames;

    std::istringstream lines(text);
    std::string line;
    bool header = false;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = SplitCsv(line);
        if (f.size() < 6) continue;
        if (!header) {
            header = true;
            if (Trim(f[0]) == "name") continue;
        }

        const std::string name = Trim(f[0]);
        const std::string kind = Trim(f[1]);
        const float widthKm = static_cast<float>(std::atof(Trim(f[2]).c_str()));
        const int order = std::atoi(Trim(f[3]).c_str());
        const float lon = static_cast<float>(std::atof(Trim(f[4]).c_str()));
        const float lat = static_cast<float>(std::atof(Trim(f[5]).c_str()));

        if (grouped.find(name) == grouped.end()) orderNames.push_back(name);
        Group& g = grouped[name];
        g.kind = kind;
        g.widthKm = widthKm;
        g.pts.emplace_back(order, std::make_pair(lon, lat));
    }

    for (const std::string& name : orderNames) {
        Group& g = grouped[name];
        std::sort(g.pts.begin(), g.pts.end(),
                  [](const std::pair<int, std::pair<float, float>>& a,
                     const std::pair<int, std::pair<float, float>>& b) { return a.first < b.first; });

        MapRoute route;
        route.name = name;
        route.kind = g.kind;
        route.widthKm = g.widthKm;
        for (const auto& p : g.pts) {
            route.lons.push_back(p.second.first);
            route.lats.push_back(p.second.second);
        }
        routes_.push_back(route);
    }

    std::printf("[routes] 已加载 %zu 条折线\n", routes_.size());
    return !routes_.empty();
}

void RouteMesh::Destroy()
{
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    vertexCount_ = 0;
}

void RouteMesh::Build(const HeightField& hf, const GeoBounds& bounds, float heightExaggeration,
                      const TerrainMesh& terrain)
{
    Destroy();

    std::vector<float> verts;

    for (const MapRoute& route : routes_) {
        if (route.lons.size() < 2) continue;
        // 河流由 RiverSurface 用同一套水体着色器单独绘制，这里只处理铁路
        if (route.kind == "河流") continue;

        const bool isRail = (route.kind == "铁路");
        Vec3 color(0.20f, 0.41f, 0.57f);
        if (isRail) color = Vec3(0.88f, 0.85f, 0.78f);

        // 先取每个控制点的世界坐标
        std::vector<Vec3> pts;
        pts.reserve(route.lons.size());
        for (size_t i = 0; i < route.lons.size(); ++i) {
            float x = 0.f, y = 0.f, z = 0.f;
            terrain.LonLatToWorld(hf, bounds, route.lons[i], route.lats[i], heightExaggeration,
                                  &x, &y, &z);
            pts.emplace_back(x, y, z);
        }

        // 每段线性细分，采样足够密才能沿山势起伏而不被地形切断
        constexpr int kSubdiv = 24;
        std::vector<Vec3> dense;
        std::vector<float> dists;
        float acc = 0.f;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            for (int s = 0; s < kSubdiv; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(kSubdiv);
                const Vec3 p = pts[i] + (pts[i + 1] - pts[i]) * t;
                // 高度按世界位置重新采样，避免在坡上悬空
                const float u = (p.x / terrain.WorldWidth()) + 0.5f;
                const float v = (p.z / terrain.WorldDepth()) + 0.5f;
                Vec3 q = p;
                q.y = hf.Sample(u, v) * 0.001f * heightExaggeration + kLift;
                dense.push_back(q);
                if (dense.size() >= 2) {
                    acc += Length(q - dense[dense.size() - 2]);
                }
                dists.push_back(acc);
            }
        }
        {
            Vec3 last = pts.back();
            const float u = (last.x / terrain.WorldWidth()) + 0.5f;
            const float v = (last.z / terrain.WorldDepth()) + 0.5f;
            last.y = hf.Sample(u, v) * 0.001f * heightExaggeration + kLift;
            dense.push_back(last);
            acc += Length(last - dense[dense.size() - 2]);
            dists.push_back(acc);
        }

        const float half = route.widthKm * 0.5f;
        for (size_t i = 0; i + 1 < dense.size(); ++i) {
            const Vec3 a = dense[i];
            const Vec3 b = dense[i + 1];
            Vec3 dir(b.x - a.x, 0.f, b.z - a.z);
            const float len = Length(dir);
            if (len < 1e-4f) continue;
            dir = dir * (1.f / len);
            const Vec3 n(-dir.z, 0.f, dir.x);

            const Vec3 a0 = a + n * half;
            const Vec3 a1 = a - n * half;
            const Vec3 b0 = b + n * half;
            const Vec3 b1 = b - n * half;

            const float railFlag = isRail ? 1.f : 0.f;

            const float quad[6][9] = {
                { a0.x, a0.y, a0.z, color.x, color.y, color.z, 0.96f, dists[i], railFlag },
                { b0.x, b0.y, b0.z, color.x, color.y, color.z, 0.96f, dists[i + 1], railFlag },
                { b1.x, b1.y, b1.z, color.x, color.y, color.z, 0.96f, dists[i + 1], railFlag },
                { a0.x, a0.y, a0.z, color.x, color.y, color.z, 0.96f, dists[i], railFlag },
                { b1.x, b1.y, b1.z, color.x, color.y, color.z, 0.96f, dists[i + 1], railFlag },
                { a1.x, a1.y, a1.z, color.x, color.y, color.z, 0.96f, dists[i], railFlag },
            };
            for (int k = 0; k < 6; ++k) {
                for (int c = 0; c < 9; ++c) verts.push_back(quad[k][c]);
            }
        }
    }

    vertexCount_ = static_cast<int>(verts.size() / 9);
    if (vertexCount_ == 0) {
        std::printf("[routes] 没有生成任何顶点\n");
        return;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);

    const GLsizei stride = static_cast<GLsizei>(9 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(7 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);

    std::printf("[routes] 折线网格 %d 顶点\n", vertexCount_);
}

void RouteMesh::Draw() const
{
    if (vao_ == 0) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount_);
    glBindVertexArray(0);
}
