#pragma once

struct IDirect3DSwapChain9;

// Initialize ImGui overlay from a swap chain pointer.
// Returns true on success, false if init should be retried later.
bool InitializeOverlay(IDirect3DSwapChain9* pSwapChain);

// Shut down ImGui and restore window state.
void ShutdownOverlay();

// Render one ImGui frame (call between Present).
void RenderOverlayFrame();

// Check if overlay is ready.
bool IsOverlayReady();
