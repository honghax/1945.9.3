// war3d —— 抗日战争三维战史演示（第一阶段：第一次长沙会战，1939.9–10）
//
// 构建方式与 fitsplat_git/fitsplat_gl 一致：
//   MSVC + Ninja + 内置 CMake（全绝对路径，不依赖 PATH）
//   依赖：third_party/glfw-3.4.bin.WIN64、glad43（OpenGL 4.3 core）
//
// 操作：
//   左键拖拽 = 旋转视角    右键拖拽 = 平移     滚轮 = 缩放
//   空格 = 播放/暂停       ←/→ = ±1 天（Shift 加速）     Home = 回到战役开始
//   1/2/3 = 全景 / 湘北 / 长沙 预设视角

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "events.h"
#include "gl_common.h"
#include "map_symbols.h"
#include "math3d.h"
#include "narration.h"
#include "river_water.h"
#include "routes.h"
#include "terrain.h"
#include "text_atlas.h"
#include "water.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 演示时间范围：1939-09-01 ~ 1939-10-31
const int kDemoStart = 19390901;
const int kDemoEnd = 19391031;
// 战役起点：1939-09-13 冈村宁次下达作战命令
const int kBattleStart = 19390913;

constexpr float kFovY = 35.f;
constexpr float kCamNear = 0.5f;
constexpr float kCamFar = 3000.f;

struct Camera {
    Vec3 target;
    float yaw = 0.f;       // 弧度，0 = 从南向北看
    float pitch = 0.66f;   // 弧度
    float distance = 210.f;
};

struct AppState {
    int winW = 1600;
    int winH = 900;
    Camera cam;
    TerrainMesh terrain;
    HeightField heights;
    GeoBounds bounds;
    EventTable events;
    RouteMesh routes;
    WaterSurface water;
    RiverSurface riverWater;
    MapSymbols symbols;
    Narration narration;
    // 真实 DEM 下 8 倍夸张（487km 跨度上，1500m 山地抬到 12km，接近微缩沙盘手感）
    float heightExaggeration = 8.f;
    int meshRes = 768;

    int currentDay = 0;
    float currentDayExact = 0.f;   // 连续时间轴：箭头生长与播放都基于它
    int demoStartDay = 0;
    int demoEndDay = 0;
    bool playing = false;
    float uiSpeed = 1.0f;          // 演示倍速（UI 滑杆），实际天/秒 = 1.0 * uiSpeed
    int selectedEvent = -1;        // UI 中选中的事件下标
    float waterLevelMeters = 31.f;
    bool waterDirty = false;

    // 相机平滑与点击拾取
    Vec3 camGoal;
    bool camAnimating = false;
    Mat4 lastViewProj;
    bool clickPress = false;
    bool clickPendingPick = false;
    double clickX = 0.0;
    double clickY = 0.0;
    float clickMove = 0.f;

    bool dragLeft = false;
    bool dragRight = false;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;

    GLuint albedoTex = 0;
    GLuint progTerrain = 0;
    GLuint progPoints = 0;
    GLuint progFlat = 0;
    GLuint progText = 0;
    GLuint progRoute = 0;
    GLuint progSky = 0;
    GLuint progWater = 0;
    GLuint progSymbol = 0;
    GLuint progFire = 0;
    GLuint vaoSky = 0;
    GLuint vaoFire = 0, vboFire = 0;
    GLuint vaoPoints = 0, vboPoints = 0;
    GLuint vaoFlat = 0, vboFlat = 0;
    GLuint vaoText = 0, vboText = 0;
    TextAtlas textAtlas;
    int placeLabelCount = 0;
    int unitLabelBase = 0;
    int lastTitleDay = -1;
};

// 固定地名标注（坐标为已核对的地点中心，用于建立地理参照）
struct PlaceLabel {
    const char* name;
    float lon;
    float lat;
};

const PlaceLabel kPlaces[] = {
    { "咸宁", 114.3169f, 29.8438f },
    { "岳阳", 113.1000f, 29.3600f },
    { "新墙河", 113.2270f, 29.1197f },
    { "通城", 113.8116f, 29.2482f },
    { "汨罗", 113.0707f, 28.8171f },
    { "湘阴", 112.8022f, 28.6989f },
    { "平江", 113.7036f, 28.7730f },
    { "修水", 114.3948f, 29.0021f },
    { "武宁", 115.0476f, 29.2778f },
    { "铜鼓", 114.3878f, 28.6293f },
    { "奉新", 115.1646f, 28.7257f },
    { "高安", 115.3798f, 28.4742f },
    { "长沙", 112.9823f, 28.1946f },
    { "浏阳", 113.7002f, 28.2179f },
    { "株洲", 113.1517f, 27.8388f },
};

constexpr int kPlaceCount = static_cast<int>(sizeof(kPlaces) / sizeof(kPlaces[0]));

AppState g_app;

// ImGui 只保存字形范围数组的指针，必须活到字体图集构建完成之后
ImVector<ImWchar> g_fontGlyphRanges;

std::string ExeDir()
{
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

// 底图纹理：2048x2048 RGB uint8，行序自北向南
GLuint LoadAlbedoTexture(const std::string& path)
{
    constexpr int kW = 2048;
    constexpr int kH = 2048;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) {
        std::printf("[albedo] 打不开底图：%s\n", path.c_str());
        return 0;
    }
    std::vector<unsigned char> rgb(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 3);
    const size_t got = std::fread(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    if (got != rgb.size()) {
        std::printf("[albedo] 底图尺寸不符：期望 %zu，读到 %zu\n", rgb.size(), got);
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, kW, kH, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::printf("[albedo] 底图 %dx%d 已加载\n", kW, kH);
    return tex;
}

// 优先从可执行文件旁边向上找 data/，兼容 build 与安装两种布局
std::string FindDataDir()
{
    std::filesystem::path p(ExeDir());
    for (int i = 0; i < 4; ++i) {
        const std::filesystem::path candidate = p / "data";
        if (std::filesystem::exists(candidate / "events_changsha1939.csv")) {
            return candidate.string();
        }
        if (!p.has_parent_path()) break;
        p = p.parent_path();
    }
    return (std::filesystem::path(ExeDir()) / "data").string();
}

void FramebufferSizeCallback(GLFWwindow*, int w, int h)
{
    g_app.winW = w > 0 ? w : 1;
    g_app.winH = h > 0 ? h : 1;
    glViewport(0, 0, w, h);
}

void ScrollCallback(GLFWwindow*, double, double yoffset)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) return;
    const float factor = std::pow(0.88f, static_cast<float>(yoffset));
    // 最近只到 65km：再近就会把 1km 网格的粗糙感暴露出来
    g_app.cam.distance = std::max(65.f, std::min(900.f, g_app.cam.distance * factor));
}

void MouseButtonCallback(GLFWwindow*, int button, int action, int)
{
    const bool uiCaptures = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            if (!uiCaptures) {
                g_app.dragLeft = true;
                g_app.camAnimating = false;
                g_app.clickPress = true;
                g_app.clickMove = 0.f;
                g_app.clickX = g_app.lastMouseX;
                g_app.clickY = g_app.lastMouseY;
            }
        } else {
            g_app.dragLeft = false;
            if (g_app.clickPress) {
                if (g_app.clickMove < 6.f) g_app.clickPendingPick = true;
                g_app.clickPress = false;
            }
        }
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        g_app.dragRight = (action == GLFW_PRESS) && !uiCaptures;
        if (action == GLFW_PRESS) g_app.camAnimating = false;
    }
}

void CursorCallback(GLFWwindow*, double x, double y)
{
    const bool uiCaptures = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    if (uiCaptures) {
        g_app.lastMouseX = x;
        g_app.lastMouseY = y;
        g_app.dragLeft = false;
        g_app.dragRight = false;
        return;
    }

    const float dx = static_cast<float>(x - g_app.lastMouseX);
    const float dy = static_cast<float>(y - g_app.lastMouseY);
    g_app.lastMouseX = x;
    g_app.lastMouseY = y;

    if (g_app.dragLeft) {
        g_app.clickMove += std::fabs(dx) + std::fabs(dy);
        g_app.cam.yaw -= dx * 0.005f;
        g_app.cam.pitch = std::max(0.12f, std::min(1.52f, g_app.cam.pitch + dy * 0.004f));
    }
    if (g_app.dragRight) {
        const float s = g_app.cam.distance * 0.0016f;
        const Vec3 fwd(std::sin(g_app.cam.yaw), 0.f, std::cos(g_app.cam.yaw));
        const Vec3 right(std::cos(g_app.cam.yaw), 0.f, -std::sin(g_app.cam.yaw));
        g_app.cam.target = g_app.cam.target - right * (dx * s) + fwd * (dy * s);
    }
}

Vec3 CameraEye(const Camera& c)
{
    const float cp = std::cos(c.pitch);
    return c.target + Vec3(std::sin(c.yaw) * cp, std::sin(c.pitch), std::cos(c.yaw) * cp)
                          * c.distance;
}

Vec3 EventColor(const std::string& category)
{
    if (category == "战斗") return Vec3(0.94f, 0.34f, 0.26f);
    if (category == "失守") return Vec3(0.90f, 0.58f, 0.20f);
    if (category == "收复") return Vec3(0.36f, 0.80f, 0.45f);
    if (category == "登陆") return Vec3(0.72f, 0.48f, 0.95f);
    if (category == "调动") return Vec3(0.40f, 0.65f, 0.95f);
    return Vec3(0.72f, 0.72f, 0.72f);
}

const char* kTerrainVS = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProj;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vUV;
void main()
{
    vWorld = aPos;
    vNormal = aNormal;
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* kTerrainFS = R"(#version 430 core
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
uniform vec3 uEye;
uniform vec3 uSunDir;
uniform sampler2D uAlbedo;
uniform float uHasAlbedo;
out vec4 fragColor;

float hash21(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float vnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main()
{
    float h = vWorld.y;          // km
    vec3 n = normalize(vNormal);

    float dist = length(uEye - vWorld);
    float detailFade = 1.0 - smoothstep(120.0, 380.0, dist);

    // 细节起伏：低频噪声轻微扰动法线，仅在近处生效
    vec2 q = vUV * 260.0;
    float nb1 = vnoise(q);
    float nb2 = vnoise(q * 2.7 + 31.0);
    n = normalize(n + vec3((nb1 - 0.5) * 0.26, 0.0, (nb2 - 0.5) * 0.26) * detailFade);

    // 分层设色：平原—丘陵—山地（湘北亚热带，山地取绿褐而非砂岩色）
    vec3 plainLo = vec3(0.24, 0.33, 0.19);
    vec3 plainHi = vec3(0.30, 0.39, 0.23);
    vec3 hill   = vec3(0.39, 0.41, 0.25);
    vec3 mount  = vec3(0.47, 0.47, 0.36);
    float t1 = clamp(h / 0.35, 0.0, 1.0);
    float t2 = clamp((h - 0.35) / 0.9, 0.0, 1.0);
    vec3 albedo = mix(mix(plainLo, plainHi, vnoise(vUV * 170.0)), hill, t1);
    albedo = mix(albedo, mount, t2);

    // 底图只借它的明暗细节（山脊、河谷、田块），色相仍保持战史图的设色
    if (uHasAlbedo > 0.5) {
        vec3 photo = texture(uAlbedo, vec2(vUV.x, 1.0 - vUV.y)).rgb;
        float lum = dot(photo, vec3(0.299, 0.587, 0.114));
        float rel = clamp(lum / 0.40, 0.45, 1.45);
        albedo *= mix(1.0, rel, 0.62);
    }

    // 极缓的经纬网（每 0.5 度一条），暗示地图属性
    float gu = abs(fract(vUV.x * 10.0) - 0.5);
    float gv = abs(fract(vUV.y * 6.0) - 0.5);
    float grid = smoothstep(0.494, 0.5, max(gu, gv));
    albedo = mix(albedo, vec3(0.76, 0.79, 0.72), grid * 0.16);

    // 地表颗粒（复用 nb1，控制采样次数）
    albedo *= 0.985 + 0.03 * nb1 * detailFade;

    if (h < 0.0) {
        float depth = clamp(-h / 0.05, 0.0, 1.0);
        vec3 water = mix(vec3(0.17, 0.29, 0.37), vec3(0.08, 0.16, 0.24), depth);
        vec3 v = normalize(uEye - vWorld);
        float fres = pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 3.0);
        fragColor = vec4(mix(water, vec3(0.58, 0.66, 0.70), fres * 0.40), 1.0);
        return;
    }

    // 半球环境光 + 斜射太阳
    vec3 amb = mix(vec3(0.11, 0.11, 0.09), vec3(0.24, 0.30, 0.38),
                   clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
    float ndl = max(dot(n, normalize(uSunDir)), 0.0);
    float wrap = smoothstep(0.0, 1.0, ndl) * 0.95;
    vec3 color = albedo * (amb + vec3(1.12, 1.02, 0.86) * wrap);

    // 大气透视
    float fog = clamp((dist - 380.0) / 560.0, 0.0, 1.0);
    color = mix(color, vec3(0.60, 0.66, 0.70), fog * 0.72);

    color = pow(color, vec3(0.95));
    fragColor = vec4(color, 1.0);
}
)";

const char* kSkyVS = R"(#version 430 core
out vec2 vUV;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.99999, 1.0);
}
)";

const char* kSkyFS = R"(#version 430 core
in vec2 vUV;
out vec4 fragColor;
void main()
{
    vec3 top = vec3(0.30, 0.45, 0.60);
    vec3 mid = vec3(0.52, 0.62, 0.70);
    vec3 bot = vec3(0.70, 0.75, 0.76);
    float t = clamp((vUV.y - 0.45) / 0.55, 0.0, 1.0);
    vec3 col = mix(mix(bot, mid, smoothstep(0.0, 0.5, t)), top, smoothstep(0.45, 1.0, t));
    fragColor = vec4(col, 1.0);
}
)";

const char* kPointsVS = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
uniform mat4 uViewProj;
uniform float uPxPerUnit;
out vec4 vColor;
void main()
{
    gl_Position = uViewProj * vec4(aPos, 1.0);
    float w = max(gl_Position.w, 0.001);
    gl_PointSize = clamp(aSize * uPxPerUnit / w, 3.0, 56.0);
    vColor = aColor;
}
)";

const char* kPointsFS = R"(#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main()
{
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(p, p);
    if (r2 > 1.0) discard;
    float edge = smoothstep(1.0, 0.78, r2);
    float core = smoothstep(0.30, 0.0, r2);
    vec3 col = mix(vColor.rgb, vec3(1.0), core * 0.45);
    fragColor = vec4(col, vColor.a * edge);
}
)";

const char* kFlatVS = R"(#version 430 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main()
{
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kFlatFS = R"(#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

const char* kTextVS = R"(#version 430 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
out vec2 vUV;
out vec4 vColor;
void main()
{
    vUV = aUV;
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kTextFS = R"(#version 430 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main()
{
    float a = texture(uTex, vUV).a;
    if (a < 0.04) discard;
    fragColor = vec4(vColor.rgb, vColor.a * a);
}
)";

const char* kRouteVS = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aDist;
layout(location = 3) in float aRail;
uniform mat4 uViewProj;
out vec4 vColor;
out float vDist;
out float vRail;
void main()
{
    vColor = aColor;
    vDist = aDist;
    vRail = aRail;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* kRouteFS = R"(#version 430 core
in vec4 vColor;
in float vDist;
in float vRail;
out vec4 fragColor;
void main()
{
    if (vRail > 0.5) {
        // 铁路按 2.2 km 周期打断，形成地图上的黑白相间符号
        float t = fract(vDist / 2.2);
        if (t > 0.58) discard;
    }
    fragColor = vColor;
}
)";

const char* kWaterVS = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aDepthM;
layout(location = 2) in float aMask;
layout(location = 3) in float aRiverMix;
uniform mat4 uViewProj;
out vec3 vWorld;
out float vDepthM;
out float vMask;
out float vRiverMix;
void main()
{
    vWorld = aPos;
    vDepthM = aDepthM;
    vMask = aMask;
    vRiverMix = aRiverMix;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* kWaterFS = R"(#version 430 core
in vec3 vWorld;
in float vDepthM;
in float vMask;
in float vRiverMix;
uniform vec3 uEye;
uniform vec3 uSunDir;
uniform float uTime;
out vec4 fragColor;

void main()
{
    // 地形高于水位的地方不出水；太浅则露出洲滩
    if (vDepthM <= 0.2 || vMask <= 0.02) discard;
    float shore = smoothstep(0.15, 1.4, vDepthM);
    if (shore <= 0.02) discard;

    vec3 V = normalize(uEye - vWorld);

    // 程序化波纹：交叉正弦 + 相位扰动，避免规则条纹
    float dist = length(uEye - vWorld);
    float rippleFade = 1.0 - smoothstep(250.0, 500.0, dist);
    vec2 p = vWorld.xz * 2.2;
    float t = uTime;
    float phase = sin(p.x * 0.63 + p.y * 0.41) * 2.6;
    p += vec2(phase, phase * 0.73);
    vec3 n = normalize(vec3(
        (sin(p.x * 2.0 + t * 1.1) * 0.10 + sin(p.y * 3.3 - t * 0.75) * 0.06) * rippleFade,
        1.0,
        (cos(p.y * 2.4 + t * 0.9) * 0.10 + cos(p.x * 3.1 + t * 1.25) * 0.06) * rippleFade));

    float ndv = max(dot(n, V), 0.0);
    // 给一个反射下限，俯视时水面也有天色反光，不至于像一块塑料
    float fres = 0.14 + 0.86 * pow(1.0 - ndv, 5.0);

    // 洞庭湖含沙量高，水色偏黄绿；河道取偏青蓝，便于在地形上辨认
    float depthScale = mix(8.0, 3.0, vRiverMix);
    float d = clamp(vDepthM / depthScale, 0.0, 1.0);
    vec3 shallow = mix(vec3(0.30, 0.34, 0.27), vec3(0.22, 0.36, 0.42), vRiverMix);
    vec3 deep = mix(vec3(0.07, 0.15, 0.18), vec3(0.08, 0.21, 0.28), vRiverMix);
    vec3 body = mix(shallow, deep, pow(d, 0.55));

    vec3 skyCol = mix(vec3(0.46, 0.55, 0.63), vec3(0.66, 0.74, 0.80), fres);
    vec3 color = mix(body, skyCol, fres * 0.62);

    vec3 H = normalize(V + normalize(uSunDir));
    float spec = pow(max(dot(n, H), 0.0), 130.0) * 0.75;
    color += vec3(1.0, 0.94, 0.82) * spec;

    // 岸边浪花：极浅的一圈泛白，帮助读出岸线
    float foam = smoothstep(1.1, 0.25, vDepthM) * 0.32;
    color = mix(color, vec3(0.76, 0.79, 0.77), foam);

    float fog = clamp((dist - 380.0) / 560.0, 0.0, 1.0);
    color = mix(color, vec3(0.60, 0.66, 0.70), fog * 0.72);

    // 浅水略透，深水近实；河道更实以保持可读性
    float baseAlpha = mix(0.82, 0.98, d);
    float alpha = vMask * shore * mix(baseAlpha, 0.96, vRiverMix);
    fragColor = vec4(color, alpha);
}
)";

// ---------------- 战史图符号与炮火 ----------------

const char* kSymbolVS = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
out vec4 vColor;
void main()
{
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* kSymbolFS = R"(#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

const char* kFireVS = R"(#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aPhase;
layout(location = 2) in float aSize;
uniform mat4 uViewProj;
uniform float uPxPerUnit;
uniform float uTime;
out float vLife;
void main()
{
    float cycle = fract(uTime * 0.5 + aPhase);
    gl_Position = uViewProj * vec4(aPos, 1.0);
    float w = max(gl_Position.w, 0.001);
    float grow = 0.35 + cycle * 1.6;
    gl_PointSize = clamp(aSize * grow * uPxPerUnit / w, 4.0, 240.0);
    vLife = cycle;
}
)";

const char* kFireFS = R"(#version 430 core
in float vLife;
out vec4 fragColor;
void main()
{
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r = length(p);
    if (r > 1.0) discard;

    // 中心炽白核 + 向外扩散的烟环
    float core = smoothstep(0.62, 0.0, r) * (1.0 - vLife * 0.85);
    float ring = smoothstep(0.82, 0.62, r) * (1.0 - smoothstep(0.62, 0.82, r));
    ring *= smoothstep(0.0, 0.30, vLife) * (1.0 - vLife) * 0.9;

    vec3 hot = mix(vec3(1.0, 0.62, 0.20), vec3(1.0, 0.97, 0.80), core);
    float a = core + ring * 0.7;
    if (a < 0.01) discard;
    fragColor = vec4(hot, a);
}
)";

// ---------------- 界面 ----------------

void ApplyPreset(int preset)
{
    if (preset == 0) {
        g_app.cam.yaw = 0.f;
        g_app.cam.pitch = 0.95f;
        g_app.cam.distance = 420.f;
        g_app.camGoal = Vec3(0.f, 0.f, 0.f);
    } else if (preset == 1) {
        g_app.cam.yaw = 0.f;
        g_app.cam.pitch = 0.62f;
        g_app.cam.distance = 150.f;
        g_app.camGoal = Vec3(0.f, 0.f, -40.f);
    } else {
        float cx = 0.f, cy = 0.f, cz = 0.f;
        g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, 112.94f, 28.23f,
                                    g_app.heightExaggeration, &cx, &cy, &cz);
        g_app.cam.yaw = 0.f;
        g_app.cam.pitch = 0.72f;
        g_app.cam.distance = 90.f;
        g_app.camGoal = Vec3(cx, cy, cz);
    }
    g_app.camAnimating = true;
}

void UpdateCameraSmoothing(float dt)
{
    if (!g_app.camAnimating) return;
    const Vec3 delta = g_app.camGoal - g_app.cam.target;
    if (Length(delta) < 0.4f) {
        g_app.cam.target = g_app.camGoal;
        g_app.camAnimating = false;
        return;
    }
    const float k = std::min(1.f, dt * 3.6f);
    g_app.cam.target = g_app.cam.target + delta * k;
}

// 点击画面上的地名或事件点，把镜头移过去
void PickAndFocus(double mx, double my)
{
    const int W = g_app.winW;
    const int H = g_app.winH;
    if (W <= 0 || H <= 0) return;

    float bestDist = 48.f;
    float bestX = 0.f, bestY = 0.f, bestZ = 0.f;
    bool found = false;

    auto test = [&](float lon, float lat) {
        float x = 0.f, y = 0.f, z = 0.f;
        g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, lon, lat,
                                    g_app.heightExaggeration, &x, &y, &z);
        const Vec4 clip = Mul(g_app.lastViewProj, Vec4(x, y, z, 1.f));
        if (clip.w <= 0.01f) return;
        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(W);
        const float sy = (1.f - (clip.y / clip.w * 0.5f + 0.5f)) * static_cast<float>(H);
        if (sx < -40.f || sx > static_cast<float>(W) + 40.f) return;
        if (sy < -40.f || sy > static_cast<float>(H) + 40.f) return;

        const float dx = sx - static_cast<float>(mx);
        const float dy = sy - static_cast<float>(my);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < bestDist) {
            bestDist = d;
            bestX = x;
            bestY = y;
            bestZ = z;
            found = true;
        }
    };

    for (int i = 0; i < kPlaceCount; ++i) test(kPlaces[i].lon, kPlaces[i].lat);
    for (const WarEvent& ev : g_app.events.All()) {
        if (!ev.hasCoord) continue;
        test(ev.lon, ev.lat);
    }

    if (found) {
        // 只切视角，不打断播放，也不改变当前缩放
        g_app.camGoal = Vec3(bestX, bestY, bestZ);
        g_app.camAnimating = true;
    }
}

void JumpToEvent(int index)
{
    const std::vector<WarEvent>& evs = g_app.events.All();
    if (index < 0 || index >= static_cast<int>(evs.size())) return;
    const WarEvent& ev = evs[static_cast<size_t>(index)];
    g_app.selectedEvent = index;
    g_app.currentDayExact =
        static_cast<float>(std::clamp(ev.startDay, g_app.demoStartDay, g_app.demoEndDay));
    g_app.playing = false;
    if (ev.hasCoord) {
        float x = 0.f, y = 0.f, z = 0.f;
        g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, ev.lon, ev.lat,
                                    g_app.heightExaggeration, &x, &y, &z);
        g_app.camGoal = Vec3(x, y, z);
        g_app.camAnimating = true;
        g_app.cam.distance = std::max(90.f, g_app.cam.distance);
        g_app.cam.pitch = 0.70f;
    }
}

std::string ShortDate(int dayIndex) { return DaysToDateString(dayIndex).substr(5); }

void DrawUI()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const std::vector<WarEvent>& evs = g_app.events.All();

    const ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                      | ImGuiWindowFlags_NoSavedSettings
                                      | ImGuiWindowFlags_NoBringToFrontOnFocus;
    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                        | ImGuiWindowFlags_NoCollapse
                                        | ImGuiWindowFlags_NoSavedSettings;

    // ---- 底部控制栏 ----
    constexpr float kBarH = 96.f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kBarH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kBarH));
    ImGui::Begin("##controlbar", nullptr, barFlags);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 7.f));
        if (ImGui::Button(g_app.playing ? "暂停" : "播放", ImVec2(84.f, 0.f))) {
            g_app.playing = !g_app.playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("回到开战", ImVec2(96.f, 0.f))) {
            g_app.currentDayExact = static_cast<float>(DateToDays(kBattleStart));
            g_app.playing = false;
        }
        ImGui::SameLine(0.f, 20.f);
        ImGui::TextColored(ImVec4(0.96f, 0.91f, 0.74f, 1.f), "%s",
                           DaysToDateString(g_app.currentDay).c_str());
        ImGui::SameLine(0.f, 20.f);
        ImGui::TextDisabled("1939-09-01 — 1939-10-31");
        ImGui::SameLine(0.f, 24.f);
        ImGui::SetNextItemWidth(190.f);
        ImGui::SliderFloat("##speed", &g_app.uiSpeed, 0.25f, 4.f, "速度 %.2fx");
        ImGui::PopStyleVar();

        int day = g_app.currentDay;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderInt("##timeline", &day, g_app.demoStartDay, g_app.demoEndDay, "")) {
            g_app.currentDayExact = static_cast<float>(day);
            g_app.playing = false;
        }
    }
    ImGui::End();

    // ---- 右侧战况面板 ----
    constexpr float kPanelW = 340.f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kPanelW, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(kPanelW, vp->WorkSize.y - kBarH));
    ImGui::Begin("第一次长沙会战", nullptr, panelFlags);
    {
        ImGui::TextUnformatted("1939 年 9—10 月 · 湘北 / 鄂南 / 赣北");
        ImGui::Separator();
        if (ImGui::Button("全景", ImVec2(72.f, 0.f))) ApplyPreset(0);
        ImGui::SameLine();
        if (ImGui::Button("湘北", ImVec2(72.f, 0.f))) ApplyPreset(1);
        ImGui::SameLine();
        if (ImGui::Button("长沙", ImVec2(72.f, 0.f))) ApplyPreset(2);
        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.42f, 1.f), "本日战况");
        bool anyOngoing = false;
        for (size_t i = 0; i < evs.size(); ++i) {
            const WarEvent& ev = evs[i];
            if (g_app.currentDay < ev.startDay || g_app.currentDay > ev.endDay) continue;
            anyOngoing = true;
            char label[256];
            std::snprintf(label, sizeof(label), "%s##ongoing%zu", ev.name.c_str(), i);
            if (ImGui::Selectable(label)) JumpToEvent(static_cast<int>(i));
        }
        if (!anyOngoing) ImGui::TextDisabled("（本日无记录事件）");

        ImGui::Separator();
        ImGui::TextUnformatted("全部事件");
        ImGui::BeginChild("##allevents", ImVec2(0.f, 168.f), true);
        for (size_t i = 0; i < evs.size(); ++i) {
            const WarEvent& ev = evs[i];
            char label[256];
            std::snprintf(label, sizeof(label), "%s  %s##ev%zu", ShortDate(ev.startDay).c_str(),
                          ev.name.c_str(), i);
            if (ImGui::Selectable(label, g_app.selectedEvent == static_cast<int>(i))) {
                JumpToEvent(static_cast<int>(i));
            }
        }
        ImGui::EndChild();

        if (g_app.selectedEvent >= 0
            && g_app.selectedEvent < static_cast<int>(evs.size())) {
            const WarEvent& ev = evs[static_cast<size_t>(g_app.selectedEvent)];
            ImGui::Separator();
            ImGui::TextWrapped("%s", ev.name.c_str());
            ImGui::TextDisabled("%s — %s · %s · %s", ShortDate(ev.startDay).c_str(),
                                ShortDate(ev.endDay).c_str(), ev.place.c_str(), ev.category.c_str());
            ImGui::TextWrapped("%s", ev.summary.c_str());
            ImGui::TextColored(ImVec4(0.62f, 0.70f, 0.62f, 1.f), "来源 %s · 置信度 %s",
                               ev.source.c_str(), ev.confidence.c_str());
        }

        ImGui::Separator();
        ImGui::TextDisabled("环境");
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderFloat("##waterlevel", &g_app.waterLevelMeters, 20.f, 42.f, "水位 %.0f m")) {
            g_app.waterDirty = true;
        }
    }
    ImGui::End();

    // ---- 解说字幕（控制栏上方） ----
    if (const std::string* line = g_app.narration.TextFor(g_app.currentDay)) {
        const float subW = vp->WorkSize.x - kPanelW - 80.f;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 40.f,
                                       vp->WorkPos.y + vp->WorkSize.y - kBarH - 94.f));
        ImGui::SetNextWindowSize(ImVec2(subW, 86.f));
        ImGui::Begin("##narration", nullptr, barFlags);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.95f, 0.88f, 1.f));
        ImGui::PushTextWrapPos(subW - 30.f);
        ImGui::TextUnformatted(line->c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::End();
    }
}

// 离屏截图：把当前帧缓冲写成 24 位 BMP（OpenGL 与 BMP 都是自底向上）
bool SaveBmp24(const std::string& path)
{
    const int w = g_app.winW;
    const int h = g_app.winH;
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int stride = rowBytes + pad;
    const unsigned imgSize = static_cast<unsigned>(stride) * static_cast<unsigned>(h);
    const unsigned fileSize = 54u + imgSize;

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) return false;

    unsigned char hdr[54] = {};
    hdr[0] = 'B';
    hdr[1] = 'M';
    std::memcpy(&hdr[2], &fileSize, 4);
    const unsigned offset = 54u;
    std::memcpy(&hdr[10], &offset, 4);
    const unsigned dibSize = 40u;
    std::memcpy(&hdr[14], &dibSize, 4);
    const int width = w;
    const int height = h;
    std::memcpy(&hdr[18], &width, 4);
    std::memcpy(&hdr[22], &height, 4);
    const unsigned short planes = 1;
    const unsigned short bpp = 24;
    std::memcpy(&hdr[26], &planes, 2);
    std::memcpy(&hdr[28], &bpp, 2);
    std::memcpy(&hdr[34], &imgSize, 4);
    std::fwrite(hdr, 1, 54, f);

    std::vector<unsigned char> row(static_cast<size_t>(stride), 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
        for (int x = 0; x < w; ++x) {
            row[static_cast<size_t>(x) * 3 + 0] = src[static_cast<size_t>(x) * 3 + 2];
            row[static_cast<size_t>(x) * 3 + 1] = src[static_cast<size_t>(x) * 3 + 1];
            row[static_cast<size_t>(x) * 3 + 2] = src[static_cast<size_t>(x) * 3 + 0];
        }
        std::fwrite(row.data(), 1, static_cast<size_t>(stride), f);
    }
    std::fclose(f);
    return true;
}

void PushTextQuad(std::vector<float>& out, float left, float top, float w, float h,
                  float u0, float v0, float u1, float v1, int winW, int winH,
                  const Vec3& color, float alpha)
{
    const float nx0 = left / static_cast<float>(winW) * 2.f - 1.f;
    const float nx1 = (left + w) / static_cast<float>(winW) * 2.f - 1.f;
    const float ny0 = 1.f - top / static_cast<float>(winH) * 2.f;
    const float ny1 = 1.f - (top + h) / static_cast<float>(winH) * 2.f;

    const float v[6][8] = {
        { nx0, ny0, u0, v0, color.x, color.y, color.z, alpha },
        { nx1, ny0, u1, v0, color.x, color.y, color.z, alpha },
        { nx1, ny1, u1, v1, color.x, color.y, color.z, alpha },
        { nx0, ny0, u0, v0, color.x, color.y, color.z, alpha },
        { nx1, ny1, u1, v1, color.x, color.y, color.z, alpha },
        { nx0, ny1, u0, v1, color.x, color.y, color.z, alpha },
    };
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < 8; ++k) out.push_back(v[i][k]);
    }
}

void DrawLabels(const Mat4& viewProj)
{
    if (g_app.progText == 0 || g_app.textAtlas.entries.empty()) return;

    const int W = g_app.winW;
    const int H = g_app.winH;
    const float scale = 0.95f;
    std::vector<float> verts;
    verts.reserve(512 * 8);

    auto emit = [&](float lon, float lat, int labelIndex, const Vec3& color, float alpha,
                    float lift) {
        if (labelIndex < 0 || labelIndex >= static_cast<int>(g_app.textAtlas.entries.size())) return;
        float x = 0.f, y = 0.f, z = 0.f;
        g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, lon, lat,
                                    g_app.heightExaggeration, &x, &y, &z);
        y += lift;
        const Vec4 clip = Mul(viewProj, Vec4(x, y, z, 1.f));
        if (clip.w <= 0.01f) return;
        const float ndcX = clip.x / clip.w;
        const float ndcY = clip.y / clip.w;
        if (ndcX < -1.05f || ndcX > 1.05f || ndcY < -1.05f || ndcY > 1.05f) return;

        const float sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(W);
        const float sy = (1.f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(H);

        const TextAtlas::Entry& e = g_app.textAtlas.entries[static_cast<size_t>(labelIndex)];
        const float tw = e.pixelW * scale;
        const float th = e.pixelH * scale;
        const float left = sx - tw * 0.5f;
        const float top = sy - th - 4.f;

        PushTextQuad(verts, left + 2.f, top + 2.f, tw, th, e.u0, e.v0, e.u1, e.v1, W, H,
                     Vec3(0.f, 0.f, 0.f), alpha * 0.8f);
        PushTextQuad(verts, left, top, tw, th, e.u0, e.v0, e.u1, e.v1, W, H, color, alpha);
    };

    // 固定地名
    for (int i = 0; i < kPlaceCount; ++i) {
        emit(kPlaces[i].lon, kPlaces[i].lat, i, Vec3(1.f, 0.96f, 0.86f), 0.90f, 1.0f);
    }

    // 仅显示当前正在发生的事件，避免标签堆叠
    const std::vector<WarEvent>& evs = g_app.events.All();
    for (size_t i = 0; i < evs.size(); ++i) {
        const WarEvent& ev = evs[i];
        if (!ev.hasCoord) continue;
        if (g_app.currentDay < ev.startDay || g_app.currentDay > ev.endDay) continue;
        emit(ev.lon, ev.lat, kPlaceCount + static_cast<int>(i), Vec3(1.f, 0.92f, 0.50f), 0.98f,
             1.7f);
    }

    // 行军单位标签：沿箭头当前进度点标注
    {
        const std::vector<SymbolSet>& movs = g_app.symbols.Movements();
        for (size_t i = 0; i < movs.size(); ++i) {
            const SymbolSet& m = movs[i];
            if (g_app.currentDay < m.startDay || g_app.currentDay > m.endDay + 20) continue;
            if (m.lon.size() < 2) continue;

            float progress = 1.f;
            if (m.endDay > m.startDay) {
                progress = static_cast<float>(g_app.currentDay - m.startDay)
                           / static_cast<float>(m.endDay - m.startDay);
                progress = std::max(0.08f, std::min(1.f, progress));
            }

            float total = 0.f;
            for (size_t k = 0; k + 1 < m.lon.size(); ++k) {
                const float dx = (m.lon[k + 1] - m.lon[k]) * 0.98f;
                const float dy = m.lat[k + 1] - m.lat[k];
                total += std::sqrt(dx * dx + dy * dy);
            }
            const float target = total * progress;
            float acc = 0.f;
            float lon = m.lon.back();
            float lat = m.lat.back();
            for (size_t k = 0; k + 1 < m.lon.size(); ++k) {
                const float dx = (m.lon[k + 1] - m.lon[k]) * 0.98f;
                const float dy = m.lat[k + 1] - m.lat[k];
                const float seg = std::sqrt(dx * dx + dy * dy);
                if (acc + seg >= target) {
                    const float t = (target - acc) / std::max(seg, 1e-6f);
                    lon = m.lon[k] + (m.lon[k + 1] - m.lon[k]) * t;
                    lat = m.lat[k] + (m.lat[k + 1] - m.lat[k]) * t;
                    break;
                }
                acc += seg;
            }

            Vec3 col(1.f, 0.86f, 0.42f);
            if (m.side == "cn") col = Vec3(0.62f, 0.80f, 1.f);
            emit(lon, lat, g_app.unitLabelBase + static_cast<int>(i), col, 0.96f, 4.2f);
        }
    }

    // 当前日期与时间轴由 ImGui 显示，此处不再绘制

    if (verts.empty()) return;

    glUseProgram(g_app.progText);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_app.textAtlas.tex);
    glUniform1i(glGetUniformLocation(g_app.progText, "uTex"), 0);
    glBindVertexArray(g_app.vaoText);
    glBindBuffer(GL_ARRAY_BUFFER, g_app.vboText);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void UpdateTitle(GLFWwindow* window)
{
    if (g_app.currentDay == g_app.lastTitleDay) return;
    g_app.lastTitleDay = g_app.currentDay;

    int active = 0, total = 0;
    for (const WarEvent& ev : g_app.events.All()) {
        ++total;
        if (g_app.currentDay >= ev.startDay && g_app.currentDay <= ev.endDay) ++active;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "war3d | 第一次长沙会战 %s | 进行中 %d / 共 %d 条 | %s",
                  DaysToDateString(g_app.currentDay).c_str(), active, total,
                  g_app.playing ? "播放中" : "已暂停");
    glfwSetWindowTitle(window, buf);
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 输出不缓冲，便于重定向后立即看到

    bool shotMode = false;
    bool nearShot = false;
    bool benchMode = false;
    std::string shotPath = "war3d_shot.bmp";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shotMode = true;
            shotPath = argv[++i];
        } else if (std::strcmp(argv[i], "--near") == 0) {
            nearShot = true;
        } else if (std::strcmp(argv[i], "--bench") == 0) {
            benchMode = true;
        }
    }

    g_app.demoStartDay = DateToDays(kDemoStart);
    g_app.demoEndDay = DateToDays(kDemoEnd);
    g_app.currentDay = DateToDays(kBattleStart);
    if (shotMode) g_app.currentDay = DateToDays(19390926);
    g_app.currentDayExact = static_cast<float>(g_app.currentDay);

    glfwSetErrorCallback([](int code, const char* desc) {
        std::printf("[glfw] 错误 %d: %s\n", code, desc);
    });
    if (!glfwInit()) {
        std::printf("[fatal] glfwInit 失败\n");
        return 1;
    }

    // OpenGL 4.3 core（与 third_party 的 glad43 一致）
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    if (shotMode || benchMode) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // 离屏模式不显示窗口

    GLFWwindow* window = glfwCreateWindow(g_app.winW, g_app.winH, "war3d", nullptr, nullptr);
    if (!window) {
        std::printf("[fatal] 创建窗口失败（需要支持 OpenGL 4.3 的驱动）\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval((shotMode || benchMode) ? 0 : 1);
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
    glfwSetScrollCallback(window, ScrollCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorCallback);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        std::printf("[fatal] glad 加载失败\n");
        glfwTerminate();
        return 1;
    }
    std::printf("[gl] %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    // 数据
    g_app.bounds = GeoBounds{};
    const std::string dataDir = FindDataDir();
    std::printf("[path] 数据目录：%s\n", dataDir.c_str());

    const std::string rawPath = dataDir + "/terrain_hunan_float32.raw";
    if (!g_app.heights.LoadRaw(rawPath, 1024, 1024)) {
        g_app.heights.GenerateProcedural(1024, 1024);
    }
    g_app.events.Load(dataDir + "/events_changsha1939.csv");

    // 先加载河道与湖区，河面入湖段要抬到湖面高度才能连成一片
    g_app.routes.Load(dataDir + "/routes.csv");
    g_app.water.LoadBodies(dataDir + "/water_bodies.csv");
    g_app.riverWater.Prepare(g_app.routes.All(), g_app.heights, g_app.bounds,
                             g_app.heightExaggeration, g_app.water.Bodies(),
                             g_app.waterLevelMeters);

    g_app.terrain.Build(g_app.heights, g_app.bounds, g_app.meshRes, g_app.heightExaggeration);

    g_app.routes.Build(g_app.heights, g_app.bounds, g_app.heightExaggeration, g_app.terrain);
    g_app.riverWater.Build();

    g_app.water.Build(g_app.heights, g_app.bounds, g_app.heightExaggeration, g_app.terrain,
                      g_app.waterLevelMeters);

    g_app.symbols.Load(dataDir);
    g_app.narration.Load(dataDir + "/narration.csv");
    g_app.albedoTex = LoadAlbedoTexture(dataDir + "/terrain_albedo_rgb.raw");

    // 着色器
    g_app.progTerrain = BuildProgram(kTerrainVS, kTerrainFS, "terrain");
    g_app.progPoints = BuildProgram(kPointsVS, kPointsFS, "points");
    g_app.progFlat = BuildProgram(kFlatVS, kFlatFS, "flat");
    g_app.progText = BuildProgram(kTextVS, kTextFS, "text");
    g_app.progRoute = BuildProgram(kRouteVS, kRouteFS, "route");
    g_app.progSky = BuildProgram(kSkyVS, kSkyFS, "sky");
    g_app.progWater = BuildProgram(kWaterVS, kWaterFS, "water");
    g_app.progSymbol = BuildProgram(kSymbolVS, kSymbolFS, "symbol");
    g_app.progFire = BuildProgram(kFireVS, kFireFS, "fire");
    if (!g_app.progTerrain || !g_app.progPoints || !g_app.progFlat || !g_app.progText
        || !g_app.progRoute || !g_app.progSky || !g_app.progWater || !g_app.progSymbol
        || !g_app.progFire) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    glGenVertexArrays(1, &g_app.vaoSky);

    // 文字图集：地名 → 事件名 → 行军单位名
    {
        std::vector<std::string> texts;
        texts.reserve(static_cast<size_t>(kPlaceCount) + g_app.events.Count()
                      + g_app.symbols.Movements().size());
        for (int i = 0; i < kPlaceCount; ++i) texts.emplace_back(kPlaces[i].name);
        for (const WarEvent& ev : g_app.events.All()) texts.push_back(ev.name);
        g_app.unitLabelBase = static_cast<int>(texts.size());
        for (const SymbolSet& m : g_app.symbols.Movements()) texts.push_back(m.name);
        g_app.textAtlas.Build(texts);
        g_app.placeLabelCount = kPlaceCount;
    }

    // 事件点 VAO
    glGenVertexArrays(1, &g_app.vaoPoints);
    glGenBuffers(1, &g_app.vboPoints);
    glBindVertexArray(g_app.vaoPoints);
    glBindBuffer(GL_ARRAY_BUFFER, g_app.vboPoints);
    const GLsizei pstride = static_cast<GLsizei>(8 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, pstride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, pstride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, pstride,
                          reinterpret_cast<void*>(7 * sizeof(float)));
    glBindVertexArray(0);

    // 2D 平色 VAO
    glGenVertexArrays(1, &g_app.vaoFlat);
    glGenBuffers(1, &g_app.vboFlat);
    glBindVertexArray(g_app.vaoFlat);
    glBindBuffer(GL_ARRAY_BUFFER, g_app.vboFlat);
    const GLsizei fstride = static_cast<GLsizei>(6 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, fstride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, fstride,
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);

    // 文字 VAO（2D 屏幕空间 + 图集 UV）
    glGenVertexArrays(1, &g_app.vaoText);
    glGenBuffers(1, &g_app.vboText);
    glBindVertexArray(g_app.vaoText);
    glBindBuffer(GL_ARRAY_BUFFER, g_app.vboText);
    const GLsizei tstride = static_cast<GLsizei>(8 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, tstride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, tstride,
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glBindVertexArray(0);

    // 炮火点精灵 VAO
    glGenVertexArrays(1, &g_app.vaoFire);
    glGenBuffers(1, &g_app.vboFire);
    glBindVertexArray(g_app.vaoFire);
    glBindBuffer(GL_ARRAY_BUFFER, g_app.vboFire);
    const GLsizei fireStride = static_cast<GLsizei>(5 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, fireStride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, fireStride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, fireStride,
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- ImGui 初始化 ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& guiIo = ImGui::GetIO();
    guiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.f;
        style.ChildRounding = 4.f;
        style.FrameRounding = 4.f;
        style.GrabRounding = 3.f;
        style.WindowPadding = ImVec2(14.f, 11.f);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.09f, 0.90f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.12f, 0.95f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.16f, 0.96f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.24f, 0.26f, 0.95f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.34f, 0.35f, 0.30f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.58f, 0.34f, 0.22f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.74f, 0.57f, 0.31f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.88f, 0.68f, 0.36f, 1.00f);
    }
    {
        const std::string fontPath = "C:/Windows/Fonts/msyh.ttc";
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        if (std::filesystem::exists(fontPath)) {
            // 只加载常用 2500 字会把「浯」「汨」这类地名字渲染成问号，
            // 这里把实际用到的全部文本喂进 builder，再叠常用字兜底
            ImFontGlyphRangesBuilder builder;
            builder.AddRanges(guiIo.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            for (const WarEvent& ev : g_app.events.All()) {
                builder.AddText(ev.name.c_str());
                builder.AddText(ev.place.c_str());
                builder.AddText(ev.summary.c_str());
                builder.AddText(ev.source.c_str());
                builder.AddText(ev.category.c_str());
                builder.AddText(ev.confidence.c_str());
            }
            for (const SymbolSet& m : g_app.symbols.Movements()) builder.AddText(m.name.c_str());
            for (const WaterBody& b : g_app.water.Bodies()) builder.AddText(b.name.c_str());
            for (const NarrationLine& n : g_app.narration.All()) builder.AddText(n.text.c_str());
            for (int i = 0; i < kPlaceCount; ++i) builder.AddText(kPlaces[i].name);
            builder.AddText("播放暂停回到开战速度全景湘北长沙本日战况全部事件来源置信度水位环境"
                            "第一次长沙会战年月日湘鄂赣日军中国无记录");
            for (int d = g_app.demoStartDay; d <= g_app.demoEndDay; ++d) {
                builder.AddText(DaysToDateString(d).c_str());
            }
            ImVector<ImWchar> ranges;
            builder.BuildRanges(&ranges);
            g_fontGlyphRanges = ranges;
            guiIo.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.f, &cfg,
                                            g_fontGlyphRanges.Data);
        } else {
            guiIo.Fonts->AddFontDefault();
        }
    }
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 430 core");

    // 相机初始视角：从南向北俯瞰湘北
    g_app.cam.target = Vec3(0.f, 0.f, -40.f);
    g_app.cam.yaw = 0.f;
    g_app.cam.pitch = 0.66f;
    g_app.cam.distance = 210.f;

    if (shotMode && nearShot) {
        // 近景核对河槽与水面（仅用于开发截图，正常运行的最近距离是 65km）
        float cx = 0.f, cy = 0.f, cz = 0.f;
        g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, 113.10f, 28.86f,
                                    g_app.heightExaggeration, &cx, &cy, &cz);
        g_app.cam.target = Vec3(cx, cy, cz);
        g_app.cam.pitch = 0.40f;
        g_app.cam.distance = 72.f;
    }

    double lastTime = glfwGetTime();
    bool spaceLatched = false;
    int frameNo = 0;
    double benchStart = 0.0;

    while (!glfwWindowShouldClose(window)) {
        const double now = glfwGetTime();
        const float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        // ---- ImGui 帧（界面改动立即影响本帧） ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawUI();

        // 水位滑杆改动后重建水面
        if (g_app.waterDirty) {
            g_app.waterDirty = false;
            g_app.water.Build(g_app.heights, g_app.bounds, g_app.heightExaggeration,
                              g_app.terrain, g_app.waterLevelMeters);
        }

        // ---- 输入 ----
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);

        const bool spaceDown = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (spaceDown && !spaceLatched) g_app.playing = !g_app.playing;
        spaceLatched = spaceDown;

        const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                           || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        const int step = shift ? 7 : 1;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
            g_app.currentDayExact += static_cast<float>(step);
            g_app.playing = false;
        }
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) {
            g_app.currentDayExact -= static_cast<float>(step);
            g_app.playing = false;
        }
        if (glfwGetKey(window, GLFW_KEY_HOME) == GLFW_PRESS) {
            g_app.currentDayExact = static_cast<float>(DateToDays(kBattleStart));
            g_app.playing = false;
        }

        if (g_app.playing) {
            // 连续推进：箭头生长与日期跳变都按亚天精度插值
            g_app.currentDayExact += dt * g_app.uiSpeed;
            if (g_app.currentDayExact >= static_cast<float>(g_app.demoEndDay)) {
                g_app.currentDayExact = static_cast<float>(g_app.demoEndDay);
                g_app.playing = false;
            }
        }
        g_app.currentDayExact = std::max(static_cast<float>(g_app.demoStartDay),
                                        std::min(static_cast<float>(g_app.demoEndDay),
                                                 g_app.currentDayExact));
        g_app.currentDay = static_cast<int>(std::floor(g_app.currentDayExact));

        UpdateCameraSmoothing(dt);
        if (g_app.clickPendingPick) {
            g_app.clickPendingPick = false;
            PickAndFocus(g_app.lastMouseX, g_app.lastMouseY);
        }

        // ---- 相机矩阵 ----
        const Vec3 eye = CameraEye(g_app.cam);
        const float aspect = static_cast<float>(g_app.winW) / static_cast<float>(g_app.winH);
        const Mat4 proj = Mat4::Perspective(kFovY * kPi / 180.f, aspect, kCamNear, kCamFar);
        const Mat4 view = Mat4::LookAt(eye, g_app.cam.target, Vec3(0.f, 1.f, 0.f));
        const Mat4 viewProj = proj * view;
        g_app.lastViewProj = viewProj;

        // ---- 事件点数据（按当前日期筛选） ----
        std::vector<float> points;
        points.reserve(g_app.events.Count() * 8);
        for (const WarEvent& ev : g_app.events.All()) {
            if (!ev.hasCoord || g_app.currentDay < ev.startDay) continue;

            float x = 0.f, y = 0.f, z = 0.f;
            g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, ev.lon, ev.lat,
                                        g_app.heightExaggeration, &x, &y, &z);
            y += 0.7f;   // 抬高，避免被地形覆盖

            const Vec3 c = EventColor(ev.category);
            const bool ongoing = g_app.currentDay <= ev.endDay;
            const bool question = (ev.confidence == "存疑");
            const float alpha = ongoing ? 1.0f : 0.26f;
            const float size = ongoing ? (question ? 4.5f : 6.0f) : 3.2f;

            points.push_back(x); points.push_back(y); points.push_back(z);
            points.push_back(c.x); points.push_back(c.y); points.push_back(c.z);
            points.push_back(alpha);
            points.push_back(size);
        }

        glBindBuffer(GL_ARRAY_BUFFER, g_app.vboPoints);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(points.size() * sizeof(float)),
                     points.empty() ? nullptr : points.data(), GL_STREAM_DRAW);

        // ---- 绘制 ----
        glClearColor(0.60f, 0.66f, 0.70f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 渐变天空（全屏三角形，不参与深度）
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUseProgram(g_app.progSky);
        glBindVertexArray(g_app.vaoSky);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(g_app.progTerrain);
        glUniformMatrix4fv(glGetUniformLocation(g_app.progTerrain, "uViewProj"), 1, GL_FALSE,
                           viewProj.m);
        glUniform3f(glGetUniformLocation(g_app.progTerrain, "uEye"), eye.x, eye.y, eye.z);
        glUniform3f(glGetUniformLocation(g_app.progTerrain, "uSunDir"), -0.58f, 0.55f, 0.60f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_app.albedoTex);
        glUniform1i(glGetUniformLocation(g_app.progTerrain, "uAlbedo"), 0);
        glUniform1f(glGetUniformLocation(g_app.progTerrain, "uHasAlbedo"),
                    g_app.albedoTex != 0 ? 1.f : 0.f);
        g_app.terrain.Draw();

        // 水面（半透明，不写深度）
        glUseProgram(g_app.progWater);
        glUniformMatrix4fv(glGetUniformLocation(g_app.progWater, "uViewProj"), 1, GL_FALSE,
                           viewProj.m);
        glUniform3f(glGetUniformLocation(g_app.progWater, "uEye"), eye.x, eye.y, eye.z);
        glUniform3f(glGetUniformLocation(g_app.progWater, "uSunDir"), -0.58f, 0.55f, 0.60f);
        glUniform1f(glGetUniformLocation(g_app.progWater, "uTime"), static_cast<float>(now));
        glDepthMask(GL_FALSE);
        // 河与湖共用一套材质，交界处用顶点里的 riverMix 平滑过渡
        g_app.riverWater.Draw();
        g_app.water.Draw();
        glDepthMask(GL_TRUE);

        // 地理骨架：河流与铁路（贴地色带，不写深度）
        glUseProgram(g_app.progRoute);
        glUniformMatrix4fv(glGetUniformLocation(g_app.progRoute, "uViewProj"), 1, GL_FALSE,
                           viewProj.m);
        glDepthMask(GL_FALSE);
        g_app.routes.Draw();
        glDepthMask(GL_TRUE);

        // 事件点（混合，不写深度）
        glUseProgram(g_app.progPoints);
        glUniformMatrix4fv(glGetUniformLocation(g_app.progPoints, "uViewProj"), 1, GL_FALSE,
                           viewProj.m);
        const float pxPerUnit = static_cast<float>(g_app.winH)
                                / (2.f * std::tan(kFovY * 0.5f * kPi / 180.f));
        glUniform1f(glGetUniformLocation(g_app.progPoints, "uPxPerUnit"), pxPerUnit);
        glDepthMask(GL_FALSE);
        glBindVertexArray(g_app.vaoPoints);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(points.size() / 8));
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);

        // 战史图符号：进攻箭头 / 占领区 / 防御阵地
        g_app.symbols.Draw(g_app.progSymbol, viewProj, g_app.currentDayExact, g_app.heights,
                           g_app.bounds, g_app.heightExaggeration, g_app.terrain);

        // 炮火：当前正在进行的战斗事件
        {
            std::vector<float> fire;
            const std::vector<WarEvent>& evs = g_app.events.All();
            for (size_t i = 0; i < evs.size(); ++i) {
                const WarEvent& ev = evs[i];
                if (!ev.hasCoord || ev.category != "战斗") continue;
                if (g_app.currentDay < ev.startDay || g_app.currentDay > ev.endDay) continue;
                float fx = 0.f, fy = 0.f, fz = 0.f;
                g_app.terrain.LonLatToWorld(g_app.heights, g_app.bounds, ev.lon, ev.lat,
                                            g_app.heightExaggeration, &fx, &fy, &fz);
                fire.push_back(fx);
                fire.push_back(fy + 2.0f);
                fire.push_back(fz);
                fire.push_back(static_cast<float>((i * 37) % 100) / 100.f);
                fire.push_back(10.0f);
            }
            if (!fire.empty()) {
                glBindBuffer(GL_ARRAY_BUFFER, g_app.vboFire);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(fire.size() * sizeof(float)),
                             fire.data(), GL_STREAM_DRAW);
                glUseProgram(g_app.progFire);
                glUniformMatrix4fv(glGetUniformLocation(g_app.progFire, "uViewProj"), 1, GL_FALSE,
                                   viewProj.m);
                glUniform1f(glGetUniformLocation(g_app.progFire, "uPxPerUnit"), pxPerUnit);
                glUniform1f(glGetUniformLocation(g_app.progFire, "uTime"), static_cast<float>(now));
                glDepthMask(GL_FALSE);
                glBindVertexArray(g_app.vaoFire);
                glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(fire.size() / 5));
                glBindVertexArray(0);
                glDepthMask(GL_TRUE);
            }
        }

        // 文字标注（2D 覆盖层）
        glDisable(GL_DEPTH_TEST);
        DrawLabels(viewProj);
        glEnable(GL_DEPTH_TEST);

        UpdateTitle(window);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ++frameNo;
        if (shotMode && frameNo >= 3) {
            const bool ok = SaveBmp24(shotPath);
            std::printf("[shot] %s：%s（%dx%d）\n", ok ? "已保存" : "保存失败",
                        shotPath.c_str(), g_app.winW, g_app.winH);
        }
        glfwSwapBuffers(window);
        glfwPollEvents();
        if (shotMode && frameNo >= 3) break;

        if (benchMode) {
            if (frameNo == 20) {
                glFinish();
                benchStart = glfwGetTime();
            }
            if (frameNo == 140) {
                glFinish();
                const double elapsed = glfwGetTime() - benchStart;
                std::printf("[bench] %.2f ms/帧  ≈ %.1f fps（网格 %d，窗口 %dx%d）\n",
                            elapsed * 1000.0 / 120.0, 120.0 / elapsed, g_app.meshRes, g_app.winW,
                            g_app.winH);
                break;
            }
        }
    }

    g_app.terrain.Destroy();
    g_app.routes.Destroy();
    g_app.water.Destroy();
    g_app.riverWater.Destroy();
    g_app.symbols.Destroy();
    if (g_app.vboFire) glDeleteBuffers(1, &g_app.vboFire);
    if (g_app.vaoFire) glDeleteVertexArrays(1, &g_app.vaoFire);
    if (g_app.vboPoints) glDeleteBuffers(1, &g_app.vboPoints);
    if (g_app.vaoPoints) glDeleteVertexArrays(1, &g_app.vaoPoints);
    if (g_app.vboFlat) glDeleteBuffers(1, &g_app.vboFlat);
    if (g_app.vaoFlat) glDeleteVertexArrays(1, &g_app.vaoFlat);
    if (g_app.vboText) glDeleteBuffers(1, &g_app.vboText);
    if (g_app.vaoText) glDeleteVertexArrays(1, &g_app.vaoText);
    g_app.textAtlas.Destroy();
    if (g_app.progTerrain) glDeleteProgram(g_app.progTerrain);
    if (g_app.progPoints) glDeleteProgram(g_app.progPoints);
    if (g_app.progFlat) glDeleteProgram(g_app.progFlat);
    if (g_app.progText) glDeleteProgram(g_app.progText);
    if (g_app.progRoute) glDeleteProgram(g_app.progRoute);
    if (g_app.progSky) glDeleteProgram(g_app.progSky);
    if (g_app.progWater) glDeleteProgram(g_app.progWater);
    if (g_app.progSymbol) glDeleteProgram(g_app.progSymbol);
    if (g_app.progFire) glDeleteProgram(g_app.progFire);
    if (g_app.vaoSky) glDeleteVertexArrays(1, &g_app.vaoSky);
    if (g_app.albedoTex) glDeleteTextures(1, &g_app.albedoTex);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
