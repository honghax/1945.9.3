// 河流水面：沿河道折线生成贴地水带，复用湖面的水体着色器（深度/Fresnel/波纹）
// Prepare 阶段会先把 DEM 沿河道压成河槽，让水面嵌在河谷里而不是浮在地表
#pragma once

#include <glad/glad.h>

#include <vector>

#include "math3d.h"
#include "routes.h"
#include "terrain.h"
#include "water.h"

class RiverSurface {
public:
    ~RiverSurface() { Destroy(); }

    // 采样河道剖面并下切 DEM（必须在 TerrainMesh::Build 之前调用）
    // lakeLevelMeters / bodies 用于把入湖段的水面衔接到湖面高度
    void Prepare(const std::vector<MapRoute>& routes, HeightField& hf, const GeoBounds& bounds,
                 float heightExaggeration, const std::vector<WaterBody>& bodies,
                 float lakeLevelMeters);
    // 用 Prepare 缓存的剖面生成水面网格（在 TerrainMesh::Build 之后调用）
    void Build();
    void Draw() const;
    void Destroy();

    int VertexCount() const { return vertexCount_; }

private:
    struct Profile {
        std::vector<Vec3> pts;   // 世界坐标，y 为水面高度
        std::vector<float> mix;  // 0 = 湖面材质，1 = 河道材质
        float widthKm = 1.f;
    };

    std::vector<Profile> profiles_;
    GLuint vao_ = 0, vbo_ = 0;
    int vertexCount_ = 0;
};
