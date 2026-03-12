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

// ─── 地图参数表 ───
// keyword:  关键字匹配（子串）
// mapW/H:  地图界限 (地图编辑器)
// camW/H:  镜头界限
// marginX/Y: 镜头边距 (边距-7=X偏移, 边距-4=Y偏移 — 已预计算在表中直接存原始边距)

struct MapParams
{
    const char* keyword;
    float mapW, mapH;
    float camW, camH;
    float marginX, marginY;
};

static const MapParams s_mapTable[] = {
    { "\xe8\x99\x9a\xe7\xa9\xba\xe6\x92\x95\xe8\xa3\x82",   208, 200, 175, 160, 16, 12 }, // 虚空撕裂
    { "\xe5\x85\x8b\xe5\x93\x88\xe8\xa3\x82\xe7\x97\x95",   216, 216, 196, 173, 10, 13 }, // 克哈裂痕
    { "\xe7\x86\x94\xe7\x81\xab\xe5\x8d\xb1\xe6\x9c\xba",   200, 200, 180, 172, 10,  8 }, // 熔火危机
    { "\xe8\x81\x9a\xe9\x93\x81\xe6\x88\x90\xe5\x85\xb5",   216, 200, 178, 176, 20,  6 }, // 聚铁成兵
    { "\xe8\x90\xa5\xe6\x95\x91\xe7\x9f\xbf\xe5\xb7\xa5",   200, 184, 180, 156, 10,  8 }, // 营救矿工
    { "\xe9\xbb\x91\xe6\x9a\x97\xe6\x9d\x80\xe6\x98\x9f",   216, 192, 181, 164, 18,  9 }, // 黑暗杀星
    { "\xe6\xad\xbb\xe4\xba\xa1\xe6\x91\x87\xe7\xaf\xae",   256, 256, 218, 193, 20, 30 }, // 死亡摇篮
    { "\xe5\xa4\xa9\xe7\x95\x8c\xe5\xb0\x81\xe9\x94\x81",   208, 216, 172, 180, 18, 16 }, // 天界封锁
    { "\xe5\x8d\x87\xe6\xa0\xbc\xe4\xb9\x8b\xe9\x93\xbe",   216, 184, 196, 164, 10,  8 }, // 升格之链
    { "\xe8\x99\x9a\xe7\xa9\xba\xe9\x99\x8d\xe4\xb8\xb4",   152, 192, 132, 164, 10,  8 }, // 虚空降临
    { "\xe6\x9c\xba\xe4\xbc\x9a\xe6\xb8\xba\xe8\x8c\xab",   168, 184, 148, 156, 10,  8 }, // 机会渺茫
    { "\xe5\x87\x80\xe7\xbd\x91\xe8\xa1\x8c\xe5\x8a\xa8",   208, 192, 192, 164, 10,  8 }, // 净网行动
    { "\xe4\xba\xa1\xe8\x80\x85\xe4\xb9\x8b\xe5\xa4\x9c",   192, 192, 160, 160, 16,  8 }, // 亡者之夜
    { "\xe5\xbe\x80\xe6\x97\xa5\xe7\xa5\x9e\xe5\xba\x99",   192, 200, 176, 172,  8,  8 }, // 往日神庙
    { "\xe6\xb9\xae\xe7\x81\xad\xe5\xbf\xab\xe8\xbd\xa6",   224, 160, 192, 144, 16,  8 }, // 湮灭快车
};
static constexpr int s_mapCount = sizeof(s_mapTable) / sizeof(s_mapTable[0]);

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

// ─── 匹配当前地图 ───
static const MapParams* FindMapByKeyword(const std::string& mapStr)
{
    for (int i = 0; i < s_mapCount; ++i)
    {
        if (mapStr.find(s_mapTable[i].keyword) != std::string::npos)
            return &s_mapTable[i];
    }
    return nullptr;
}

// ─── 坐标转换：神器地图坐标 → 屏幕像素坐标 ───
// 返回 true 表示转换成功，outX/outY 是屏幕坐标
static bool ArtifactToScreen(const MapParams& mp, float artX, float artY,
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
    float scaledCamW = mp.camW * 1.01f;
    float scaledCamH = mp.camH * 1.0f;

    float kFit = (std::min)(miniW / scaledCamW, miniH / scaledCamH);
    float projW = scaledCamW * kFit;
    float projH = scaledCamH * kFit;

    // Step B: 神器坐标 → 在投影矩形内的位置
    float relX = (artX - mp.marginX) / mp.camW * projW;
    float relY = (artY - mp.marginY) / mp.camH * projH;

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
    ImGui::Text("[SC2] \xe5\x9f\xba\xe5\x9d\x80: %p", hMod);
    ImGui::Text("\xe5\x88\x86\xe8\xbe\xa8\xe7\x8e\x87: %.0f x %.0f", g_screenWidth, g_screenHeight);

    ImGui::Separator();

    // ─── 神器坐标（指针链） ───
    ArtifactCoords ac = ReadArtifactCoords();
    if (ac.valid)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.3f, 1.0f),
            "\xe7\xa5\x9e\xe5\x99\xa8\xe5\x9d\x90\xe6\xa0\x87: %.1f, %.1f", ac.x, ac.y);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "\xe7\xa5\x9e\xe5\x99\xa8\xe5\x9d\x90\xe6\xa0\x87: \xe6\x97\xa0\xe6\x95\x88");

    ImGui::Separator();

    // ─── 大厅信息（来自 hook） ───
    LobbyData lobby = SnapshotLobbyData();

    if (lobby.valid)
    {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
            "\xe5\x9c\xb0\xe5\x9b\xbe: %s", lobby.mapPath.c_str());

        if (!lobby.mapDisplayName.empty())
            ImGui::TextWrapped("\xe6\x8f\x8f\xe8\xbf\xb0: %s", lobby.mapDisplayName.c_str());

        // ─── 地图匹配结果 ───
        const MapParams* matched = FindMapByKeyword(lobby.mapPath);
        if (!matched)
            matched = FindMapByKeyword(lobby.mapDisplayName);
        if (matched)
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                "\xe5\x91\xbd\xe4\xb8\xad\xe5\x9c\xb0\xe5\x9b\xbe: %s", matched->keyword);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                "\xe5\x91\xbd\xe4\xb8\xad\xe5\x9c\xb0\xe5\x9b\xbe: \xe6\x9c\xaa\xe5\x8c\xb9\xe9\x85\x8d");

        ImGui::Text("\xe5\x8f\x82\xe6\x95\xb0: %u / %u  \xe6\xa0\x87\xe5\xbf\x97: 0x%X",
            lobby.gameParam1, lobby.gameParam2, lobby.flags);

        ImGui::Text("\xe7\x8e\xa9\xe5\xae\xb6: %u", lobby.playerCount);
        for (DWORD i = 0; i < 16; ++i)
        {
            if (lobby.playerIds[i])
                ImGui::Text("  [%u] id=%u", i, lobby.playerIds[i]);
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "\xe5\xa4\xa7\xe5\x8e\x85: \xe7\xad\x89\xe5\xbe\x85\xe6\x95\xb0\xe6\x8d\xae...");
    }

    ImGui::End();
}

void DrawMenu()
{
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("\xe6\xb3\xbd\xe6\x8b\x89\xe5\x9b\xbe\xe5\xa4\x96\xe6\x8c\x82", &g_showMenu, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("NumPad- : \xe5\xbc\x80\xe5\x85\xb3\xe8\x8f\x9c\xe5\x8d\x95");
    ImGui::Text("END     : \xe5\x8d\xb8\xe8\xbd\xbd DLL");
    ImGui::Separator();

    ArtifactCoords acMenu = ReadArtifactCoords();
    if (acMenu.valid)
        ImGui::Text("\xe7\xa5\x9e\xe5\x99\xa8\xe5\x9d\x90\xe6\xa0\x87: (%.1f, %.1f)", acMenu.x, acMenu.y);
    else
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "\xe7\xa5\x9e\xe5\x99\xa8\xe5\x9d\x90\xe6\xa0\x87: \xe6\x97\xa0\xe6\x95\x88");

    ImGui::Separator();
    ImGui::Text("\xe5\x88\x86\xe8\xbe\xa8\xe7\x8e\x87: %.0f x %.0f", g_screenWidth, g_screenHeight);

    ImGui::Separator();
    ImGui::Text("\xe5\xb0\x8f\xe5\x9c\xb0\xe5\x9b\xbe\xe6\xa0\xa1\xe5\x87\x86:");
    ImGui::SliderFloat("X \xe5\x81\x8f\xe7\xa7\xbb", &s_adjustX, -50.0f, 50.0f, "%.0f px");
    ImGui::SliderFloat("Y \xe5\x81\x8f\xe7\xa7\xbb", &s_adjustY, -50.0f, 50.0f, "%.0f px");
    if (ImGui::Button("\xe9\x87\x8d\xe7\xbd\xae"))
    {
        s_adjustX = 29.0f;
        s_adjustY = 6.0f;
    }

    ImGui::End();
}

// ─── 小地图神器点渲染 ───

void DrawMinimapOverlay()
{
    // 获取神器坐标
    ArtifactCoords ac = ReadArtifactCoords();
    if (!ac.valid)
        return;

    // 获取地图信息，匹配参数
    LobbyData lobby = SnapshotLobbyData();
    if (!lobby.valid)
        return;

    // 优先用 mapPath 匹配，其次 mapDisplayName
    const MapParams* mp = FindMapByKeyword(lobby.mapPath);
    if (!mp)
        mp = FindMapByKeyword(lobby.mapDisplayName);
    if (!mp)
        return;

    // 坐标转换
    float screenX, screenY;
    if (!ArtifactToScreen(*mp, ac.x, ac.y, screenX, screenY))
        return;

    // 画黄色圆点
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircleFilled(ImVec2(screenX, screenY), 4.0f,
                          IM_COL32(255, 255, 0, 255));
}
