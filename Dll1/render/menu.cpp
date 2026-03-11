#include "pch.h"
#include "render/menu.h"
#include "core/globals.h"
#include "core/game_data.h"
#include "core/memory.h"

#include <imgui.h>

void DrawDebugOverlay()
{
    // 固定在左上角，无边框无背景的调试信息窗口
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("##DebugOverlay", nullptr, flags);

    // 模块基址
    HMODULE hMod = GetModuleBase("SC2_x64.exe");
    ImGui::Text("[SC2] Base: %p", hMod);

    ImGui::Separator();

    // ─── 神器坐标 ───
    ArtifactCoords ac = ReadArtifactCoords();
    if (ac.valid)
    {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.3f, 1.0f),
            "Artifact X: %.2f  Y: %.2f", ac.x, ac.y);
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Artifact: INVALID (pointer chain broken)");
    }

    ImGui::Separator();

    // ─── 大厅/地图信息（事件链） ───
    LobbyInfo lobby = ReadLobbyInfo();
    ImGui::Text("BattleNet*: %p", (void*)lobby.pBattleNet);
    ImGui::Text("EvtNode*:   %p", (void*)lobby.pEvtNode);
    ImGui::Text("Lobby*:     %p", (void*)lobby.pLobby);

    if (lobby.valid)
    {
        if (!lobby.mapPath.empty())
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                "MapPath: %s", lobby.mapPath.c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f),
                "MapPath: (empty)");

        if (!lobby.mapName.empty())
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                "MapName: %s", lobby.mapName.c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f),
                "MapName: (empty)");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Lobby: INVALID (event chain broken)");
    }

    ImGui::End();
}

void DrawMenu()
{
    ImGui::Begin("SC2 Overlay", &g_showMenu, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("ImGui attached (D3D9)");
    ImGui::Text("NumPad-: Toggle menu");
    ImGui::Text("END: Unload DLL");
    ImGui::Separator();

    ArtifactCoords ac = ReadArtifactCoords();
    if (ac.valid)
        ImGui::Text("Artifact: (%.2f, %.2f)", ac.x, ac.y);
    else
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Artifact: N/A");

    ImGui::End();
}
