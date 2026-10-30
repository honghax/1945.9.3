#include "water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 可分离滑动窗口均值滤波，用于把水位场抹平
void BoxBlur(std::vector<float>& img, int w, int h, int r)
{
    if (r <= 0 || w < 2 || h < 2) return;
    std::vector<float> tmp(img.size(), 0.f);

    for (int j = 0; j < h; ++j) {
        const float* row = &img[static_cast<size_t>(j) * static_cast<size_t>(w)];
        float* out = &tmp[static_cast<size_t>(j) * static_cast<size_t>(w)];
        float sum = 0.f;
        int count = 0;
        for (int i = 0; i <= r && i < w; ++i) {
            sum += row[i];
            ++count;
        }
        for (int i = 0; i < w; ++i) {
            out[i] = count > 0 ? sum / static_cast<float>(count) : 0.f;
            const int add = i + r + 1;
            const int rem = i - r;
            if (add < w) {
                sum += row[add];
                ++count;
            }
            if (rem >= 0) {
                sum -= row[rem];
                --count;
            }
        }
    }

    for (int i = 0; i < w; ++i) {
        float sum = 0.f;
        int count = 0;
        for (int j = 0; j <= r && j < h; ++j) {
            sum += tmp[static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)];
            ++count;
        }
        for (int j = 0; j < h; ++j) {
            img[static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)] =
                count > 0 ? sum / static_cast<float>(count) : 0.f;
            const int add = j + r + 1;
            const int rem = j - r;
            if (add < h) {
                sum += tmp[static_cast<size_t>(add) * static_cast<size_t>(w) + static_cast<size_t>(i)];
                ++count;
            }
            if (rem >= 0) {
                sum -= tmp[static_cast<size_t>(rem) * static_cast<size_t>(w) + static_cast<size_t>(i)];
                --count;
            }
        }
    }
}

}  // namespace

bool WaterSurface::LoadBodies(const std::string& csvPath)
{
    std::ifstream in(csvPath);
    if (!in) {
        std::printf("[water] 打不开 %s\n", csvPath.c_str());
        return false;
    }

    bodies_.clear();
    std::string line;
    std::getline(in, line);   // 表头
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> cells;
        while (std::getline(ss, cell, ',')) cells.push_back(cell);
        if (cells.size() < 3) continue;

        WaterBody body;
        body.name = cells[0];
        for (size_t k = 1; k + 1 < cells.size(); k += 2) {
            body.lons.push_back(static_cast<float>(std::atof(cells[k].c_str())));
            body.lats.push_back(static_cast<float>(std::atof(cells[k + 1].c_str())));
        }
        if (!body.lons.empty()) bodies_.push_back(body);
    }
    std::printf("[water] 水体名称 %zu 条\n", bodies_.size());
    return true;
}

bool WaterSurface::LoadMask(const std::string& rawPath, int size)
{
    FILE* f = nullptr;
    if (fopen_s(&f, rawPath.c_str(), "rb") != 0 || f == nullptr) {
        std::printf("[water] 打不开水系掩码 %s\n", rawPath.c_str());
        return false;
    }
    mask_.resize(static_cast<size_t>(size) * static_cast<size_t>(size));
    const size_t got = std::fread(mask_.data(), 1, mask_.size(), f);
    std::fclose(f);
    if (got != mask_.size()) {
        std::printf("[water] 掩码尺寸不符：期望 %zu，读到 %zu\n", mask_.size(), got);
        mask_.clear();
        return false;
    }
    maskSize_ = size;
    UploadMaskTexture();

    size_t wet = 0;
    for (uint8_t v : mask_) {
        if (v > 127) ++wet;
    }
    std::printf("[water] 水系掩码 %dx%d，水域占 %.2f%%\n", size, size,
                100.0 * static_cast<double>(wet) / static_cast<double>(mask_.size()));
    return true;
}

void WaterSurface::UploadMaskTexture()
{
    if (maskTex_) glDeleteTextures(1, &maskTex_);
    glGenTextures(1, &maskTex_);
    glBindTexture(GL_TEXTURE_2D, maskTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, maskSize_, maskSize_, 0, GL_RED,
                 GL_UNSIGNED_BYTE, mask_.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool WaterSurface::LoadDepth(const std::string& rawPath, int size)
{
    FILE* f = nullptr;
    if (fopen_s(&f, rawPath.c_str(), "rb") != 0 || f == nullptr) {
        std::printf("[water] 打不开水深场 %s（水深将退化为离岸程度）\n", rawPath.c_str());
        return false;
    }
    depth_.resize(static_cast<size_t>(size) * static_cast<size_t>(size));
    const size_t got = std::fread(depth_.data(), 1, depth_.size(), f);
    std::fclose(f);
    if (got != depth_.size()) {
        std::printf("[water] 水深场尺寸不符\n");
        depth_.clear();
        return false;
    }
    uint8_t maxD = 0;
    for (uint8_t v : depth_) {
        if (v > maxD) maxD = v;
    }
    std::printf("[water] 水深场 %dx%d，最深层数 %u\n", size, size,
                static_cast<unsigned>(maxD));
    return true;
}

float WaterSurface::DepthAt(float u, float v) const
{
    if (depth_.empty() || maskSize_ < 2) return 0.f;
    const float fx = std::clamp(u, 0.f, 1.f) * static_cast<float>(maskSize_ - 1);
    const float fy = std::clamp(v, 0.f, 1.f) * static_cast<float>(maskSize_ - 1);
    const int x0 = std::min(static_cast<int>(fx), maskSize_ - 2);
    const int y0 = std::min(static_cast<int>(fy), maskSize_ - 2);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    auto at = [&](int x, int y) {
        return static_cast<float>(depth_[static_cast<size_t>(y) * static_cast<size_t>(maskSize_)
                                         + static_cast<size_t>(x)]) / 24.f;
    };
    const float a = at(x0, y0) * (1.f - tx) + at(x0 + 1, y0) * tx;
    const float b = at(x0, y0 + 1) * (1.f - tx) + at(x0 + 1, y0 + 1) * tx;
    return std::clamp(a * (1.f - ty) + b * ty, 0.f, 1.f);
}

float WaterSurface::MaskAt(float u, float v) const
{
    if (mask_.empty() || maskSize_ < 2) return 0.f;
    const float fx = std::clamp(u, 0.f, 1.f) * static_cast<float>(maskSize_ - 1);
    const float fy = std::clamp(v, 0.f, 1.f) * static_cast<float>(maskSize_ - 1);
    const int x0 = std::min(static_cast<int>(fx), maskSize_ - 2);
    const int y0 = std::min(static_cast<int>(fy), maskSize_ - 2);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    auto at = [&](int x, int y) {
        return static_cast<float>(mask_[static_cast<size_t>(y) * static_cast<size_t>(maskSize_)
                                        + static_cast<size_t>(x)]) / 255.f;
    };
    const float a = at(x0, y0) * (1.f - tx) + at(x0 + 1, y0) * tx;
    const float b = at(x0, y0 + 1) * (1.f - tx) + at(x0 + 1, y0 + 1) * tx;
    return a * (1.f - ty) + b * ty;
}

void WaterSurface::Prepare(HeightField& hf, const GeoBounds& bounds)
{
    (void)bounds;
    const int w = hf.w;
    const int h = hf.h;
    if (w < 2 || h < 2 || mask_.empty()) return;

    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    coverage_.assign(n, 0.f);
    level_.assign(n, 0.f);
    const std::vector<float> dem(hf.data.begin(), hf.data.end());

    double weightSum = 0.0;
    double elevSum = 0.0;
    size_t wet = 0;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(w);
            const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(h);
            const float m = MaskAt(u, v);
            const size_t idx = static_cast<size_t>(j) * static_cast<size_t>(w)
                               + static_cast<size_t>(i);
            coverage_[idx] = m;
            if (m > 0.5f && dem[idx] < 500.f) {
                weightSum += 1.0;
                elevSum += dem[idx];
                ++wet;
            }
        }
    }
    meanLevel_ = weightSum > 0.0 ? static_cast<float>(elevSum / weightSum) : 30.f;

    // 用覆盖度加权的强平滑，得到连续的水位场：湖区接近水平，河流随地形缓降
    std::vector<float> num(n, 0.f);
    std::vector<float> den(n, 0.f);
    for (size_t k = 0; k < n; ++k) {
        num[k] = dem[k] * coverage_[k];
        den[k] = coverage_[k];
    }
    const int radius = std::max(4, w / 70);
    for (int pass = 0; pass < 3; ++pass) {
        BoxBlur(num, w, h, radius);
        BoxBlur(den, w, h, radius);
    }
    for (size_t k = 0; k < n; ++k) {
        const float lv = den[k] > 0.03f ? num[k] / den[k] : meanLevel_;
        // 水位不得高出本地地面太多，否则窄河段的水面会浮到山坡上去
        level_[k] = std::min(lv, dem[k] + 6.f);
    }

    // 把掩码区域的地形压到水位以下；按覆盖度渐变，避免岸边出现垂直陡坎
    for (size_t k = 0; k < n; ++k) {
        const float m = coverage_[k];
        if (m <= 0.02f) continue;
        const float target = level_[k] - 12.f;
        const float v = hf.data[k] + (target - hf.data[k]) * m;
        if (v < hf.data[k]) hf.data[k] = v;
    }

    std::printf("[water] 水位 %.1fm，DEM 网格水域 %.2f%%\n", meanLevel_,
                100.0 * static_cast<double>(wet) / static_cast<double>(n));
}

void WaterSurface::Build(const HeightField& hf, const GeoBounds& bounds,
                         float heightExaggeration)
{
    Destroy();
    if (level_.empty()) return;

    const int w = hf.w;
    const int h = hf.h;
    if (w < 2 || h < 2) return;

    const float midLat = 0.5f * (bounds.latMin + bounds.latMax);
    const float kmPerLon = 111.32f * std::cos(midLat * kPi / 180.f);
    const float worldW = bounds.LonSpan() * kmPerLon;
    const float worldD = bounds.LatSpan() * 110.9f;
    const float scale = 0.001f * heightExaggeration;

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(n_));

    auto pushV = [&](int i, int j) {
        const size_t idx = static_cast<size_t>(j) * static_cast<size_t>(w)
                           + static_cast<size_t>(i);
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(w);
        const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(h);
        const float x = (u - 0.5f) * worldW;
        const float z = (v - 0.5f) * worldD;
        // 水面压在岸边地面之下几米，看起来才像嵌在地形里而不是浮着一块板
        const float y = (level_[idx] - 3.f) * scale;
        const float dep = depth_.empty() ? coverage_[idx] : DepthAt(u, v);
        verts.push_back(x);
        verts.push_back(y);
        verts.push_back(z);
        verts.push_back(dep);  // depth：到岸距离，湖心 1 / 窄河接近 0
        verts.push_back(coverage_[idx]);   // mask：岸线裁剪
        verts.push_back(0.f);
    };

    for (int j = 0; j + 1 < h; ++j) {
        for (int i = 0; i + 1 < w; ++i) {
            const size_t i00 = static_cast<size_t>(j) * static_cast<size_t>(w)
                               + static_cast<size_t>(i);
            const size_t i10 = i00 + 1;
            const size_t i01 = i00 + static_cast<size_t>(w);
            const size_t i11 = i01 + 1;
            const float peak = std::max(std::max(coverage_[i00], coverage_[i10]),
                                        std::max(coverage_[i01], coverage_[i11]));
            if (peak < 0.12f) continue;

            pushV(i, j);
            pushV(i, j + 1);
            pushV(i + 1, j + 1);
            pushV(i, j);
            pushV(i + 1, j + 1);
            pushV(i + 1, j);
        }
    }

    n_ = static_cast<int>(verts.size() / 6);
    if (n_ == 0) return;

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

    std::printf("[water] 水面 %d 顶点 / %d 三角形\n", n_, n_ / 3);
}

void WaterSurface::Draw() const
{
    if (vao_ == 0) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, n_);
    glBindVertexArray(0);
}

void WaterSurface::Destroy()
{
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    n_ = 0;
}
