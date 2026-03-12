#pragma once

// Draw the main ImGui menu. Called each frame when menu is visible.
void DrawMenu();

// Draw debug overlay in top-left corner. Called every frame (always visible).
void DrawDebugOverlay();

// Draw artifact dot on minimap. Called every frame (always visible).
void DrawMinimapOverlay();
