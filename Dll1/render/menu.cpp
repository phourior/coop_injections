#include "pch.h"
#include "render/menu.h"
#include "core/globals.h"
#include "core/game_data.h"
#include "core/memory.h"
#include "hooks/game_hook.h"

#include <imgui.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <string>

// ─── 1920x1080 下的小地图基准参数 ───
static constexpr float kRefWidth  = 1920.0f;
static constexpr float kRefHeight = 1080.0f;
static constexpr float kMiniMapW_ref  = 262.0f;  // 小地图区域宽
static constexpr float kMiniMapH_ref  = 257.0f;  // 小地图区域高
static constexpr float kMiniMapX_ref  = 28.0f;   // 小地图左下角 X
static constexpr float kMiniMapBY_ref = 1065.0f; // 小地图左下角 Y

// ─── 微调偏移（菜单中可调） ───
static float s_adjustX = 29.0f;
static float s_adjustY = 6.0f;
static bool  s_configLoaded = false;

// ─── JSON 配置文件路径 ───
static std::string GetConfigPath()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_hModule, path, MAX_PATH);
    // 将 DLL 文件名替换为配置文件名
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    strcat_s(path, "sc2_offset_config.json");
    return std::string(path);
}

static void SaveOffsetConfig()
{
    std::string path = GetConfigPath();
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "w");
    if (!f) return;
    fprintf(f, "{\n  \"adjustX\": %.2f,\n  \"adjustY\": %.2f\n}\n", s_adjustX, s_adjustY);
    fclose(f);
}

static void LoadOffsetConfig()
{
    std::string path = GetConfigPath();
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (!f) return;
    char buf[256] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    // 简单解析 "adjustX": value, "adjustY": value
    const char* px = strstr(buf, "\"adjustX\"");
    const char* py = strstr(buf, "\"adjustY\"");
    if (px) { px = strchr(px, ':'); if (px) s_adjustX = (float)atof(px + 1); }
    if (py) { py = strchr(py, ':'); if (py) s_adjustY = (float)atof(py + 1); }
}

// ─── 匹配当前地图 ───
// Current map parameters are read from runtime bounds instead of map names.

// ─── 坐标转换：神器地图坐标 → 屏幕像素坐标 ───
// 返回 true 表示转换成功，outX/outY 是屏幕坐标
static bool ArtifactToScreen(const MapBounds& bounds, float artX, float artY,
                             float& outX, float& outY)
{
    // 缩放因子
    float scaleX = g_screenWidth  / kRefWidth;
    float scaleY = g_screenHeight / kRefHeight;

    // 当前分辨率下的小地图区域
    float miniW  = kMiniMapW_ref * scaleX;
    float miniH  = kMiniMapH_ref * scaleY;
    float miniLeft   = (kMiniMapX_ref + s_adjustX);  // X 不缩放（实测固定28）
    float miniBottom = kMiniMapBY_ref * scaleY + s_adjustY;

    // Step A: 镜头界限按 1.01:1 拉伸后等比适配小地图区域
    float scaledCamW = bounds.width * 1.01f;
    float scaledCamH = bounds.height * 1.0f;

    float kFit = (std::min)(miniW / scaledCamW, miniH / scaledCamH);
    float projW = scaledCamW * kFit;
    float projH = scaledCamH * kFit;

    // Step B: 神器坐标 → 在投影矩形内的位置
    float relX = (artX - bounds.left) / bounds.width * projW;
    float relY = (artY - bounds.top) / bounds.height * projH;

    // Step C: 居中偏移 + 基准坐标
    float offsetX = (miniW - projW) / 2.0f;
    float offsetY = (miniH - projH) / 2.0f;

    outX = miniLeft + offsetX + relX;
    // Y轴反转：小地图左下角是高Y值，屏幕坐标从上往下增加
    outY = miniBottom - offsetY - relY;

    return true;
}

void DrawDebugOverlay()
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.4f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("##DebugOverlay", nullptr, flags);

    // ─── 基础信息 ───
    HMODULE hMod = GetModuleBase("SC2_x64.exe");
    ImGui::Text("[SC2] 基址: %p", hMod);
    ImGui::Text("分辨率: %.0f x %.0f", g_screenWidth, g_screenHeight);

    ImGui::Separator();

    // ─── 神器坐标（指针链） ───
    ArtifactCoords ac = ReadArtifactCoords();
    if (ac.valid)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.3f, 1.0f),
            "神器坐标: %.1f, %.1f", ac.x, ac.y);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "神器坐标: 无效");

    ImGui::Separator();

    LobbyInfo info = ReadLobbyInfo();
    std::string displayName;
    if (info.valid)
        displayName = !info.mapName.empty() ? info.mapName : info.mapPath;

    if (displayName.empty())
    {
        LobbyData lobby = SnapshotLobbyData();
        if (lobby.valid)
            displayName = !lobby.mapDisplayName.empty() ? lobby.mapDisplayName : lobby.mapPath;
    }

    if (!displayName.empty())
        ImGui::TextWrapped("地图名: %s", displayName.c_str());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            "地图名: 未读取");

    MapBounds bounds = ReadCurrentMapBounds();
    if (bounds.valid)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
            "Bounds: L%.1f T%.1f R%.1f B%.1f  %.1f x %.1f",
            bounds.left, bounds.top, bounds.right, bounds.bottom,
            bounds.width, bounds.height);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
            "Bounds: invalid");

    ImGui::End();
}

void DrawMenu()
{
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("泽拉图外挂", &g_showMenu, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("NumPad- : 开关菜单");
    ImGui::Text("END     : 卸载 DLL");
    ImGui::Separator();

    bool masteryMax = IsMasteryMaxEnabled();
    if (ImGui::Checkbox("180 精通", &masteryMax))
    {
        if (masteryMax)
            EnableMasteryMax();
        else
            DisableMasteryMax();
    }
    ImGui::Separator();

    ArtifactCoords acMenu = ReadArtifactCoords();
    if (acMenu.valid)
        ImGui::Text("神器坐标: (%.1f, %.1f)", acMenu.x, acMenu.y);
    else
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "神器坐标: 无效");

    ImGui::Separator();
    ImGui::Text("分辨率: %.0f x %.0f", g_screenWidth, g_screenHeight);

    // 首次加载配置
    if (!s_configLoaded)
    {
        LoadOffsetConfig();
        s_configLoaded = true;
    }

    ImGui::Separator();
    ImGui::Text("小地图校准:");
    bool changed = false;
    changed |= ImGui::SliderFloat("X 偏移", &s_adjustX, -75.0f, 75.0f, "%.0f px");
    changed |= ImGui::SliderFloat("Y 偏移", &s_adjustY, -75.0f, 75.0f, "%.0f px");
    if (ImGui::Button("重置"))
    {
        s_adjustX = 29.0f;
        s_adjustY = 6.0f;
        changed = true;
    }
    if (changed)
        SaveOffsetConfig();

    ImGui::End();
}

// ─── 小地图神器点渲染 ───

void DrawMinimapOverlay()
{
    // 获取神器坐标
    ArtifactCoords ac = ReadArtifactCoords();
    if (!ac.valid)
        return;

    // Read runtime map bounds instead of matching map names.
    MapBounds bounds = ReadCurrentMapBounds();
    if (!bounds.valid)
        return;

    // 坐标转换
    float screenX, screenY;
    if (!ArtifactToScreen(bounds, ac.x, ac.y, screenX, screenY))
        return;

    // 画黄色圆点
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircleFilled(ImVec2(screenX, screenY), 4.0f,
                          IM_COL32(255, 255, 0, 255));
}
