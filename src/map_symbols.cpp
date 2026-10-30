#include "map_symbols.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "events.h"

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

std::string ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

Vec3 SideColor(const std::string& side)
{
    if (side == "jp") return Vec3(0.66f, 0.15f, 0.12f);
    return Vec3(0.17f, 0.38f, 0.70f);
}

Vec3 EventColor(const std::string& side)
{
    if (side == "日") return Vec3(0.74f, 0.20f, 0.15f);
    if (side == "中") return Vec3(0.24f, 0.48f, 0.80f);
    return Vec3(0.94f, 0.64f, 0.24f);
}

void PushTri(std::vector<float>& out, const Vec3& a, const Vec3& b, const Vec3& c,
             const Vec3& col, float alpha)
{
    const Vec3 pts[3] = { a, b, c };
    for (const Vec3& p : pts) {
        out.push_back(p.x);
        out.push_back(p.y);
        out.push_back(p.z);
        out.push_back(col.x);
        out.push_back(col.y);
        out.push_back(col.z);
        out.push_back(alpha);
    }
}

void PushQuad(std::vector<float>& out, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              const Vec3& col, float alpha)
{
    PushTri(out, a, b, c, col, alpha);
    PushTri(out, a, c, d, col, alpha);
}

// 水平细带：a→b，半宽 half（世界单位 km），两端点共用 a.y 高度
void PushSegment(std::vector<float>& out, const Vec3& a, const Vec3& b, float half,
                 const Vec3& col, float alpha)
{
    Vec3 d(b.x - a.x, 0.f, b.z - a.z);
    if (Length(d) < 1e-5f) return;
    d = Normalize(d);
    const Vec3 n(-d.z, 0.f, d.x);
    PushQuad(out, a + n * half, b + n * half, b - n * half, a - n * half, col, alpha);
}

// 缓冲 a0/a1（起点两侧）→ b0/b1（终点两侧）。dashed 时按 2km 周期断开，
// 子段长 0.25km，flow 使虚线沿行军方向连续滑动，长空档期也能看出推进方向
void PushBandQuad(std::vector<float>& out, const Vec3& a0, const Vec3& a1, const Vec3& b0,
                  const Vec3& b1, const Vec3& col, float alpha, bool dashed, float dist0,
                  float segLen, float flow)
{
    if (!dashed) {
        PushQuad(out, a0, b0, b1, a1, col, alpha);
        return;
    }
    constexpr float period = 2.0f;
    constexpr float dashLen = 1.1f;
    const int n = std::max(1, static_cast<int>(std::ceil(segLen / 0.25f)));
    for (int k = 0; k < n; ++k) {
        const float t0 = static_cast<float>(k) / static_cast<float>(n);
        const float t1 = static_cast<float>(k + 1) / static_cast<float>(n);
        float phase = std::fmod(dist0 + segLen * t0 - flow, period);
        if (phase < 0.f) phase += period;
        if (phase > dashLen) continue;
        const Vec3 s0 = a0 + (b0 - a0) * t0;
        const Vec3 s1 = a1 + (b1 - a1) * t0;
        const Vec3 e0 = a0 + (b0 - a0) * t1;
        const Vec3 e1 = a1 + (b1 - a1) * t1;
        PushQuad(out, s0, e0, e1, s1, col, alpha);
    }
}

// 单位框：矩形外框 + 内部叉号（标准步兵符号），中心 c，长轴沿 dir
void PushUnitBox(std::vector<float>& out, const Vec3& c, const Vec3& dir, float halfLen,
                 float halfWid, float thick, const Vec3& col, float alpha)
{
    const Vec3 u = Normalize(Vec3(dir.x, 0.f, dir.z));
    const Vec3 v(-u.z, 0.f, u.x);
    const Vec3 p00 = c - u * halfLen - v * halfWid;
    const Vec3 p10 = c + u * halfLen - v * halfWid;
    const Vec3 p11 = c + u * halfLen + v * halfWid;
    const Vec3 p01 = c - u * halfLen + v * halfWid;
    PushSegment(out, p00, p10, thick, col, alpha);
    PushSegment(out, p10, p11, thick, col, alpha);
    PushSegment(out, p11, p01, thick, col, alpha);
    PushSegment(out, p01, p00, thick, col, alpha);
    PushSegment(out, p00, p11, thick * 0.8f, col, alpha * 0.85f);
    PushSegment(out, p01, p10, thick * 0.8f, col, alpha * 0.85f);
}

}  // namespace

bool MapSymbols::Load(const std::string& dataDir)
{
    movements_.clear();
    areas_.clear();
    positions_.clear();

    // ---- movements ----
    {
        const std::string text = ReadAll(dataDir + "/movements.csv");
        struct Group {
            SymbolSet set;
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
            if (f.size() < 10) continue;
            if (!header) {
                header = true;
                if (Trim(f[0]) == "id") continue;
            }
            const std::string id = Trim(f[0]);
            if (grouped.find(id) == grouped.end()) order.push_back(id);
            Group& g = grouped[id];
            g.set.name = Trim(f[2]);
            g.set.side = Trim(f[1]);
            g.set.style = Trim(f[3]);
            g.set.startDay = DateToDays(std::atoi(Trim(f[4]).c_str()));
            g.set.endDay = DateToDays(std::atoi(Trim(f[5]).c_str()));
            g.set.widthKm = static_cast<float>(std::atof(Trim(f[6]).c_str()));
            g.pts.emplace_back(std::atoi(Trim(f[7]).c_str()),
                               std::make_pair(static_cast<float>(std::atof(Trim(f[8]).c_str())),
                                              static_cast<float>(std::atof(Trim(f[9]).c_str()))));
        }
        for (const std::string& id : order) {
            Group& g = grouped[id];
            std::sort(g.pts.begin(), g.pts.end(),
                      [](const std::pair<int, std::pair<float, float>>& a,
                         const std::pair<int, std::pair<float, float>>& b) {
                          return a.first < b.first;
                      });
            if (g.pts.size() < 2) continue;
            for (const auto& p : g.pts) {
                g.set.lon.push_back(p.second.first);
                g.set.lat.push_back(p.second.second);
            }
            movements_.push_back(g.set);
        }
    }

    // ---- territory / positions ----
    auto loadPolygons = [](const std::string& path, std::vector<SymbolSet>& out) {
        const std::string text = ReadAll(path);
        struct Group {
            SymbolSet set;
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
            if (f.size() < 7) continue;
            if (!header) {
                header = true;
                if (Trim(f[0]) == "name") continue;
            }
            const std::string name = Trim(f[0]);
            if (grouped.find(name) == grouped.end()) order.push_back(name);
            Group& g = grouped[name];
            g.set.name = name;
            g.set.side = Trim(f[1]);
            g.set.startDay = DateToDays(std::atoi(Trim(f[2]).c_str()));
            g.set.endDay = DateToDays(std::atoi(Trim(f[3]).c_str()));
            g.pts.emplace_back(std::atoi(Trim(f[4]).c_str()),
                               std::make_pair(static_cast<float>(std::atof(Trim(f[5]).c_str())),
                                              static_cast<float>(std::atof(Trim(f[6]).c_str()))));
        }
        for (const std::string& name : order) {
            Group& g = grouped[name];
            std::sort(g.pts.begin(), g.pts.end(),
                      [](const std::pair<int, std::pair<float, float>>& a,
                         const std::pair<int, std::pair<float, float>>& b) {
                          return a.first < b.first;
                      });
            if (g.pts.size() < 3) continue;
            for (const auto& p : g.pts) {
                g.set.lon.push_back(p.second.first);
                g.set.lat.push_back(p.second.second);
            }
            out.push_back(g.set);
        }
    };

    loadPolygons(dataDir + "/territory.csv", areas_);
    loadPolygons(dataDir + "/positions.csv", positions_);

    // 把控制点按弧长加密：原来两点之间是直线，几十公里跨度会直接穿过山体。
    // 加密后每段只有 1.5~3km，逐点采样地形就能贴着地表走
    auto densify = [](SymbolSet& s, float stepKm) {
        if (s.lon.size() < 2) return;
        constexpr float kLonKm = 95.f;   // 区域中纬度上的每度经线长度（约）
        constexpr float kLatKm = 111.f;
        std::vector<float> lon;
        std::vector<float> lat;
        for (size_t i = 0; i + 1 < s.lon.size(); ++i) {
            const float dLon = s.lon[i + 1] - s.lon[i];
            const float dLat = s.lat[i + 1] - s.lat[i];
            const float km = std::sqrt((dLon * kLonKm) * (dLon * kLonKm)
                                       + (dLat * kLatKm) * (dLat * kLatKm));
            const int n = std::max(1, static_cast<int>(std::ceil(km / stepKm)));
            for (int k = 0; k < n; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(n);
                lon.push_back(s.lon[i] + dLon * t);
                lat.push_back(s.lat[i] + dLat * t);
            }
        }
        lon.push_back(s.lon.back());
        lat.push_back(s.lat.back());
        s.lon.swap(lon);
        s.lat.swap(lat);
    };
    for (SymbolSet& s : movements_) densify(s, 1.5f);
    for (SymbolSet& s : positions_) densify(s, 3.0f);

    std::printf("[symbols] 箭头 %zu 条，占领区 %zu 个，阵地 %zu 条\n", movements_.size(),
                areas_.size(), positions_.size());
    return !movements_.empty();
}

void MapSymbols::Destroy()
{
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    vboCapacity_ = 0;
}

void MapSymbols::Draw(GLuint program, const Mat4& viewProj, float currentDayExact,
                      const HeightField& hf, const GeoBounds& bounds, float heightExaggeration,
                      const TerrainMesh& terrain, const EventTable& events)
{
    (void)program;
    const int currentDay = static_cast<int>(std::floor(currentDayExact));
    std::vector<float> verts;
    verts.reserve(4096 * 7);

    // 世界坐标 + 贴地抬升
    auto worldAt = [&](float lon, float lat, float lift) {
        const float u = (lon - bounds.lonMin) / bounds.LonSpan();
        const float v = (bounds.latMax - lat) / bounds.LatSpan();
        float x = 0.f, z = 0.f;
        terrain.LonLatToWorld(hf, bounds, lon, lat, heightExaggeration, &x, nullptr, &z);
        const float y = hf.Sample(u, v) * 0.001f * heightExaggeration + lift;
        return Vec3(x, y, z);
    };

    // 虚线流动相位与阵地脉动：时间轴停在长空档时画面仍有推进感
    const float flow = currentDayExact * 1.15f;
    const float pulse = 0.5f + 0.5f * std::sin(currentDayExact * 2.2f);

    // ---- 占领区：多边形扇形填充 ----
    for (const SymbolSet& a : areas_) {
        if (currentDay < a.startDay || currentDay > a.endDay) continue;
        // 进入 / 退出时淡入淡出
        const int fadeIn = std::min(currentDay - a.startDay, 3);
        const int fadeOut = std::min(a.endDay - currentDay, 3);
        const float alpha = 0.11f * (static_cast<float>(fadeIn) / 3.f)
                            * (static_cast<float>(fadeOut) / 3.f);
        if (alpha < 0.01f) continue;

        const Vec3 col = SideColor(a.side);
        std::vector<Vec3> ring;
        ring.reserve(a.lon.size());
        for (size_t i = 0; i < a.lon.size(); ++i) ring.push_back(worldAt(a.lon[i], a.lat[i], 1.2f));

        const Vec3 center = ring[0];
        for (size_t i = 1; i + 1 < ring.size(); ++i) {
            PushTri(verts, center, ring[i], ring[i + 1], col, alpha);
        }
    }

    // ---- 箭头 ----
    for (const SymbolSet& m : movements_) {
        if (currentDay < m.startDay) continue;

        float progress = 1.f;
        if (m.endDay > m.startDay) {
            progress = (currentDayExact - static_cast<float>(m.startDay))
                       / static_cast<float>(m.endDay - m.startDay);
            progress = std::max(0.f, std::min(1.f, progress));
        }
        if (progress <= 0.001f) continue;

        // 到位后 6 天内渐隐到 0.46，作为战役经过保留，不会在某一帧突然消失
        float persist = 1.f;
        if (currentDayExact > static_cast<float>(m.endDay)) {
            const float late = (currentDayExact - static_cast<float>(m.endDay)) / 6.f;
            persist = 0.46f + 0.44f * std::max(0.f, 1.f - late);
        }

        const bool dashed = (m.style == "retreat");
        const Vec3 col = SideColor(m.side);

        std::vector<Vec3> pts;
        pts.reserve(m.lon.size());
        for (size_t i = 0; i < m.lon.size(); ++i) pts.push_back(worldAt(m.lon[i], m.lat[i], 1.4f));

        // 总长
        std::vector<float> segLen(pts.size(), 0.f);
        float total = 0.f;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            segLen[i] = Length(pts[i + 1] - pts[i]);
            total += segLen[i];
        }
        if (total < 0.5f) continue;

        const float target = total * progress;
        float acc = 0.f;
        const float half = m.widthKm * 0.5f * 0.68f;
        float lastDist = 0.f;

        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            if (acc >= target) break;
            const float remain = target - acc;
            const float segT = std::min(1.f, remain / std::max(segLen[i], 1e-4f));

            Vec3 a = pts[i];
            Vec3 b = pts[i] + (pts[i + 1] - pts[i]) * segT;
            Vec3 dir = Normalize(Vec3(b.x - a.x, 0.f, b.z - a.z));
            const Vec3 n(-dir.z, 0.f, dir.x);

            const Vec3 a0 = a + n * half;
            const Vec3 a1 = a - n * half;
            const Vec3 b0 = b + n * half;
            const Vec3 b1 = b - n * half;
            // 仍在推进的实线箭头：一道缓行的明度波，跨度长时也能看出方向
            const float glint = (!dashed && progress < 0.999f)
                                    ? 1.f + 0.16f * std::sin(lastDist * 1.15f - currentDayExact * 2.4f)
                                    : 1.f;
            PushBandQuad(verts, a0, a1, b0, b1, col * glint, 0.90f * persist, dashed, lastDist,
                         segLen[i] * segT, flow);

            lastDist += segLen[i];
            acc += segLen[i];
        }

        // 箭头头部（在进度末端）：高度取末端的实际地面，三个顶点共用，避免扎进山脊
        {
            float cum = 0.f;
            Vec3 tip = pts.back();
            Vec3 dir(0.f, 0.f, 1.f);
            float tipLon = m.lon.back();
            float tipLat = m.lat.back();
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                if (cum + segLen[i] >= target) {
                    const float t = (target - cum) / std::max(segLen[i], 1e-4f);
                    tip = pts[i] + (pts[i + 1] - pts[i]) * t;
                    tipLon = m.lon[i] + (m.lon[i + 1] - m.lon[i]) * t;
                    tipLat = m.lat[i] + (m.lat[i + 1] - m.lat[i]) * t;
                    dir = Normalize(Vec3(pts[i + 1].x - pts[i].x, 0.f, pts[i + 1].z - pts[i].z));
                    break;
                }
                cum += segLen[i];
                tip = pts[i + 1];
                tipLon = m.lon[i + 1];
                tipLat = m.lat[i + 1];
                if (i + 2 < pts.size()) {
                    dir = Normalize(Vec3(pts[i + 1].x - pts[i].x, 0.f, pts[i + 1].z - pts[i].z));
                }
            }
            const Vec3 n(-dir.z, 0.f, dir.x);
            const float headLen = m.widthKm * 2.7f;
            const float headHalf = m.widthKm * 1.25f;
            const float y = worldAt(tipLon, tipLat, 1.4f).y;
            const Vec3 headTip(tip.x + dir.x * headLen, y, tip.z + dir.z * headLen);
            const Vec3 headL(tip.x + n.x * headHalf, y, tip.z + n.z * headHalf);
            const Vec3 headR(tip.x - n.x * headHalf, y, tip.z - n.z * headHalf);
            // 生长中的箭头头部轻微脉动，提示该轴线仍在推进
            const float headAlpha = (progress < 0.999f ? 0.84f + 0.16f * pulse : 0.90f) * persist;
            PushTri(verts, headTip, headL, headR, col, headAlpha);
        }
    }

    // ---- 单位框：进攻 / 追击轴线的出发位置，矩形框 + 叉号（标准步兵符号）----
    {
        std::vector<Vec3> placed;
        for (const SymbolSet& m : movements_) {
            if (m.style != "attack" && m.style != "pursuit") continue;
            if (currentDay < m.startDay || currentDay > m.endDay + 4) continue;
            if (m.lon.size() < 2) continue;

            const Vec3 p0 = worldAt(m.lon[0], m.lat[0], 1.5f);
            bool tooClose = false;
            for (const Vec3& q : placed) {
                if (Length(q - p0) < 7.f) { tooClose = true; break; }
            }
            if (tooClose) continue;

            // 方向必须在世界坐标里算：经度与纬度方向的公里尺度不同
            const Vec3 p1 = worldAt(m.lon[1], m.lat[1], 1.5f);
            const Vec3 dir(p1.x - p0.x, 0.f, p1.z - p0.z);
            if (Length(dir) < 1e-4f) continue;
            placed.push_back(p0);

            const float fadeIn = std::max(0.f, std::min(1.f, (currentDayExact - m.startDay + 0.5f)));
            const float stay = currentDay > m.endDay ? 0.40f : 0.86f;
            const float alpha = stay * fadeIn;
            if (alpha < 0.03f) continue;

            const float hu = m.widthKm * 1.10f + 1.2f;
            const float hv = m.widthKm * 0.58f + 0.6f;
            const Vec3 u = Normalize(dir);
            const Vec3 center = p0 - u * (hu * 0.72f);
            PushUnitBox(verts, center, dir, hu, hv, 0.26f, SideColor(m.side), alpha);
        }
    }

    // ---- 阵地：主线 + 梳齿 ----
    for (const SymbolSet& p : positions_) {
        if (currentDay < p.startDay || currentDay > p.endDay) continue;
        const Vec3 col = SideColor(p.side);
        // 阵地线随时间呼吸，梳齿长度同步起伏
        const float alpha = 0.62f + 0.20f * pulse;
        const float toothLen = 1.05f + 0.32f * pulse;

        std::vector<Vec3> pts;
        pts.reserve(p.lon.size());
        for (size_t i = 0; i < p.lon.size(); ++i) pts.push_back(worldAt(p.lon[i], p.lat[i], 1.2f));

        float travelled = 0.f;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec3 a = pts[i];
            const Vec3 b = pts[i + 1];
            Vec3 dir = Normalize(Vec3(b.x - a.x, 0.f, b.z - a.z));
            if (Length(Vec3(b.x - a.x, 0.f, b.z - a.z)) < 1e-4f) continue;
            const Vec3 n(-dir.z, 0.f, dir.x);

            // 主线：细带
            const float half = 0.30f;
            PushTri(verts, a + n * half, b + n * half, b - n * half, col, alpha * 0.85f);
            PushTri(verts, a + n * half, b - n * half, a - n * half, col, alpha * 0.85f);

            // 梳齿：每 5km 一根垂直短线
            const float segLen = Length(b - a);
            float s = 0.f;
            while (s < segLen) {
                const float t = s / segLen;
                const Vec3 base = a + (b - a) * t;
                const Vec3 tooth = base + n * toothLen;
                const float tw = 0.35f;
                PushTri(verts, base + dir * tw, tooth + dir * tw, tooth - dir * tw, col, alpha);
                PushTri(verts, base + dir * tw, tooth - dir * tw, base - dir * tw, col, alpha);
                s += 5.0f;
            }
            travelled += segLen;
        }
        (void)travelled;
    }

    // ---- 交战点：交叉叉号（标准战场标注），按下当月序脉动 ----
    for (const WarEvent& ev : events.All()) {
        if (!ev.hasCoord) continue;
        if (ev.category != "战斗" && ev.category != "失守" && ev.category != "登陆") continue;
        if (currentDayExact < static_cast<float>(ev.startDay)
            || currentDayExact > static_cast<float>(ev.endDay) + 1.f) continue;

        const float fade = std::min(1.f, (currentDayExact - static_cast<float>(ev.startDay)) / 0.7f);
        if (fade <= 0.f) continue;
        const float beat = 0.5f + 0.5f * std::sin(currentDayExact * 3.4f + ev.lon * 7.f);
        const float s = 2.3f * (0.90f + 0.10f * beat);
        const Vec3 c = worldAt(ev.lon, ev.lat, 2.2f);
        const Vec3 u(0.79f * s, 0.f, 0.61f * s);
        const Vec3 v(-0.61f * s, 0.f, 0.79f * s);
        const Vec3 col = EventColor(ev.side);
        const float tail = currentDay > ev.endDay ? 0.45f : 1.f;
        const float alpha = (0.40f + 0.26f * beat) * fade * tail;
        PushSegment(verts, c - u, c + u, 0.32f, col, alpha);
        PushSegment(verts, c - v, c + v, 0.32f, col, alpha);
    }

    if (verts.empty()) return;

    const size_t floats = verts.size();
    if (vao_ == 0) {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        const GLsizei stride = static_cast<GLsizei>(7 * sizeof(float));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glBindVertexArray(0);
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (floats > vboCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(floats * sizeof(float)), nullptr,
                     GL_DYNAMIC_DRAW);
        vboCapacity_ = floats;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(floats * sizeof(float)),
                    verts.data());

    glUseProgram(program);
    glUniformMatrix4fv(glGetUniformLocation(program, "uViewProj"), 1, GL_FALSE, viewProj.m);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(floats / 7));
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
}
