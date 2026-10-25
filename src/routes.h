// 地理骨架折线：河流与铁路
// 点位以已核实地名坐标为准，缺失处按河道走向插值，note 列标明来源性质
#pragma once

#include <glad/glad.h>

#include <string>
#include <vector>

#include "terrain.h"

struct MapRoute {
    std::string name;
    std::string kind;      // 河流 / 铁路
    float widthKm = 1.f;
    std::vector<float> lons;
    std::vector<float> lats;
};

class RouteMesh {
public:
    ~RouteMesh() { Destroy(); }

    bool Load(const std::string& csvPath);
    void Build(const HeightField& hf, const GeoBounds& bounds, float heightExaggeration,
               const TerrainMesh& terrain);
    void Draw() const;
    void Destroy();

    size_t RouteCount() const { return routes_.size(); }
    const std::vector<MapRoute>& All() const { return routes_; }

private:
    std::vector<MapRoute> routes_;
    GLuint vao_ = 0, vbo_ = 0;
    int vertexCount_ = 0;
};
