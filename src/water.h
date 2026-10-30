// 水面：轮廓来自 World_Topo_Map 提取的水系掩码，不再使用手绘多边形或河道折线。
// 网格只在水域范围内生成，岸线由片元着色器采样掩码纹理裁出，实现真正的自然岸线。
#pragma once

#include <glad/glad.h>

#include <cstdint>
#include <string>
#include <vector>

#include "terrain.h"

struct WaterBody {
    std::string name;
    std::vector<float> lons;
    std::vector<float> lats;
};

class WaterSurface {
public:
    ~WaterSurface() { Destroy(); }

    bool LoadBodies(const std::string& csvPath);
    // 掩码：size x size uint8，行序自北向南，与地形包围盒一致
    bool LoadMask(const std::string& rawPath, int size);
    // 水深场：到岸距离（像素层数），0 岸边、越大越深
    bool LoadDepth(const std::string& rawPath, int size);

    // 计算水位场并把 DEM 在掩码区域压到水位以下（必须在 TerrainMesh::Build 之前）
    void Prepare(HeightField& hf, const GeoBounds& bounds);
    // 依据掩码与水位场生成水面网格（必须在 Prepare 之后）
    void Build(const HeightField& hf, const GeoBounds& bounds, float heightExaggeration);

    void Draw() const;
    void Destroy();

    GLuint MaskTexture() const { return maskTex_; }
    bool HasMask() const { return !mask_.empty(); }
    size_t BodyCount() const { return bodies_.size(); }
    const std::vector<WaterBody>& Bodies() const { return bodies_; }
    float WaterLevelMeters() const { return meanLevel_; }

private:
    float MaskAt(float u, float v) const;
    float DepthAt(float u, float v) const;
    void UploadMaskTexture();

    std::vector<WaterBody> bodies_;

    int maskSize_ = 0;
    std::vector<uint8_t> mask_;
    std::vector<uint8_t> depth_;
    GLuint maskTex_ = 0;

    // 与 DEM 同分辨率的水位场（米）和掩码覆盖度
    std::vector<float> level_;
    std::vector<float> coverage_;
    float meanLevel_ = 30.f;

    GLuint vao_ = 0, vbo_ = 0;
    int n_ = 0;
};
