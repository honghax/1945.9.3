#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "text_atlas.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

}  // namespace

bool TextAtlas::Build(const std::vector<std::string>& labels, int atlasWidth)
{
    Destroy();
    if (labels.empty()) return false;

    constexpr int kRowH = 30;
    constexpr int kFontH = 21;
    rowHeight = static_cast<float>(kRowH);

    width = atlasWidth;
    const int needed = static_cast<int>(labels.size()) * kRowH + 4;
    height = 32;
    while (height < needed) height *= 2;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;   // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!hbmp || !memdc || !bits) {
        if (hbmp) DeleteObject(hbmp);
        if (memdc) DeleteDC(memdc);
        std::printf("[text] 创建 DIB 失败\n");
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memdc, hbmp);
    std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    SetBkMode(memdc, TRANSPARENT);
    SetTextColor(memdc, RGB(255, 255, 255));

    HFONT font = CreateFontW(-kFontH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(memdc, font);

    entries.reserve(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        const std::wstring w = Utf8ToWide(labels[i]);
        const int rowTop = 2 + static_cast<int>(i) * kRowH;

        RECT measure = { 0, 0, width - 8, kRowH };
        DrawTextW(memdc, w.c_str(), -1, &measure,
                  DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);

        const int textW = std::min(static_cast<int>(measure.right - measure.left) + 2, width - 4);
        RECT rc = { 0, rowTop, textW + 2, rowTop + kRowH };
        DrawTextW(memdc, w.c_str(), -1, &rc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        Entry e;
        e.pixelW = static_cast<float>(textW);
        e.pixelH = static_cast<float>(kRowH);
        e.u0 = 0.f;
        e.v0 = static_cast<float>(rowTop) / static_cast<float>(height);
        e.u1 = static_cast<float>(textW) / static_cast<float>(width);
        e.v1 = static_cast<float>(rowTop + kRowH) / static_cast<float>(height);
        entries.push_back(e);
    }

    SelectObject(memdc, oldFont);
    DeleteObject(font);

    // 从 DIB 拷出亮度，构造白色 RGBA（GDI 不写 alpha，用亮度代替）
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const unsigned char* src = static_cast<const unsigned char*>(bits);
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixels; ++i) {
        const unsigned char lum = src[i * 4];   // BGRA 的 B 通道，黑底白字即亮度
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = lum;
    }

    SelectObject(memdc, oldBmp);
    DeleteDC(memdc);
    DeleteObject(hbmp);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::printf("[text] 图集 %dx%d，%zu 个标签\n", width, height, entries.size());
    return true;
}

void TextAtlas::Destroy()
{
    if (tex) {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    entries.clear();
}
