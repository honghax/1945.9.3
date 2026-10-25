// 湖面水体：按水位线与地形高差生成水面网格，着色器实现深度渐变 / Fresnel / 波纹
// 岸线由湖区多边形 + 地形高度共同决定，不使用任何贴图
#pragma once

#include <glad/glad.h>

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
    void Build(const HeightField& hf, const GeoBounds& bounds, float heightExaggeration,
               const TerrainMesh& terrain, float waterLevelMeters);
    void Draw() const;
    void Destroy();

    float WaterLevelMeters() const { return waterLevel_; }
    size_t BodyCount() const { return bodies_.size(); }
    const std::vector<WaterBody>& Bodies() const { return bodies_; }

private:
    std::vector<WaterBody> bodies_;
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
    float waterLevel_ = 30.f;
};
