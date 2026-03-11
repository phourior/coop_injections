#include "pch.h"
#include "render/menu.h"
#include "core/globals.h"
#include "core/game_data.h"
#include "core/memory.h"
#include "hooks/game_hook.h"

#include <imgui.h>

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
    ImGui::Text("[SC2] Base: %p", hMod);

    ImGui::Separator();

    // ─── 神器坐标 ───
    ArtifactCoords ac = ReadArtifactCoords();
    if (ac.valid)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.3f, 1.0f),
            "Artifact X: %.2f  Y: %.2f", ac.x, ac.y);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Artifact: N/A");

    ImGui::Separator();

    // ─── 大厅信息（来自 hook） ───
    LobbyData lobby = SnapshotLobbyData();

    if (lobby.valid)
    {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
            "Map: %s", lobby.mapPath.c_str());

        if (!lobby.mapDisplayName.empty())
            ImGui::TextWrapped("Desc: %s", lobby.mapDisplayName.c_str());

        ImGui::Text("Params: %u / %u  Flags: 0x%X",
            lobby.gameParam1, lobby.gameParam2, lobby.flags);

        ImGui::Text("Players: %u", lobby.playerCount);
        for (DWORD i = 0; i < 16; ++i)
        {
            if (lobby.playerIds[i])
                ImGui::Text("  [%u] id=%u", i, lobby.playerIds[i]);
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Lobby: waiting for data...");
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
