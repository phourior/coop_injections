#include "pch.h"
#include "render/menu.h"
#include "core/globals.h"

#include <imgui.h>

void DrawMenu()
{
    ImGui::Begin("SC2 Overlay", &g_showMenu, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("ImGui attached (D3D9)");
    ImGui::Text("NumPad-: Toggle menu");
    ImGui::Text("END: Unload DLL");
    ImGui::End();
}
