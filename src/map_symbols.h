// 战史图符号层：进攻/撤退箭头、占领区色块、防御阵地线
// 按二战作战地图惯例：粗实线箭头示进攻轴线，虚线示撤退与争夺中的战线，连续线加梳齿示阵地
#pragma once

#include <glad/glad.h>

#include <string>
#include <vector>

#include "math3d.h"
#include "terrain.h"

class EventTable;

struct SymbolSet {
    std::string name;
    std::string side;    // jp / cn
    std::string style;   // attack / retreat / pursuit
    int startDay = 0;
    int endDay = 0;
    float widthKm = 2.f;
    std::vector<float> lon;
    std::vector<float> lat;
};

class MapSymbols {
public:
    ~MapSymbols() { Destroy(); }

    bool Load(const std::string& dataDir);
    void Draw(GLuint program, const Mat4& viewProj, float currentDayExact, const HeightField& hf,
              const GeoBounds& bounds, float heightExaggeration, const TerrainMesh& terrain,
              const EventTable& events);
    void Destroy();

    size_t MovementCount() const { return movements_.size(); }
    size_t AreaCount() const { return areas_.size(); }
    size_t PositionCount() const { return positions_.size(); }
    const std::vector<SymbolSet>& Movements() const { return movements_; }
    const std::vector<SymbolSet>& Positions() const { return positions_; }

private:
    std::vector<SymbolSet> movements_;
    std::vector<SymbolSet> areas_;
    std::vector<SymbolSet> positions_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    size_t vboCapacity_ = 0;
};
