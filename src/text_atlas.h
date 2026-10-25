// 文字图集：用 GDI 把 UTF-8 标签画进一张 32 位 DIB，取亮度作 alpha，上传为 GL 纹理
// 这样不必引入 freetype / ImGui，也能在 OpenGL 4.3 里显示中文地名
#pragma once

#include <glad/glad.h>

#include <string>
#include <vector>

struct TextAtlas {
    struct Entry {
        float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
        float pixelW = 0.f;   // 该标签在图集里的实际宽度（像素）
        float pixelH = 0.f;
    };

    GLuint tex = 0;
    int width = 0;
    int height = 0;
    float rowHeight = 30.f;
    std::vector<Entry> entries;

    ~TextAtlas() { Destroy(); }
    bool Build(const std::vector<std::string>& labels, int atlasWidth = 1024);
    void Destroy();
};
