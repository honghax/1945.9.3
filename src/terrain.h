#pragma once

#include <glad/glad.h>

#include <string>
#include <vector>

// 第一次长沙会战覆盖范围（湘北、鄂南、赣北）
// 经度 111.0E ~ 116.0E，纬度 27.4N ~ 30.4N
struct GeoBounds {
    float lonMin = 111.0f, lonMax = 116.0f;
    float latMin = 27.4f, latMax = 30.4f;
    float LonSpan() const { return lonMax - lonMin; }
    float LatSpan() const { return latMax - latMin; }
};

// 高程场：单位米，第 0 行对应最北；u 向东为正，v 向南为正
struct HeightField {
    int w = 0, h = 0;
    float minH = 0.f, maxH = 0.f;
    std::vector<float> data;

    float Sample(float u, float v) const;                       // 双线性，u,v ∈ [0,1]
    bool LoadRaw(const std::string& path, int width, int height);  // 单通道 float32 小端
    void GenerateProcedural(int width, int height);             // 无真实 DEM 时的占位地形
};

class TerrainMesh {
public:
    ~TerrainMesh();
    void Build(const HeightField& hf, const GeoBounds& bounds, int meshRes,
               float heightExaggeration);
    void Draw() const;
    void Destroy();

    // 世界坐标：x 向东，z 向南，y 向上；1 世界单位 = 1 km
    float WorldWidth() const { return worldW_; }
    float WorldDepth() const { return worldD_; }

    void LonLatToWorld(const HeightField& hf, const GeoBounds& bounds, float lon, float lat,
                       float heightExaggeration, float* outX, float* outY, float* outZ) const;

private:
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
    float worldW_ = 1.f, worldD_ = 1.f;
};
