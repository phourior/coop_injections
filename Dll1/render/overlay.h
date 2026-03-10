#pragma once

struct IDirect3DSwapChain9;

// Initialize ImGui overlay from a swap chain pointer.
// Returns true on success, false if init should be retried later.
bool InitializeOverlay(IDirect3DSwapChain9* pSwapChain);

// Shut down ImGui and restore window state.
void ShutdownOverlay();

// Call BEFORE IDirect3DDevice9::Reset — releases ImGui GPU resources.
void OnDeviceLost();

// Call AFTER a successful IDirect3DDevice9::Reset — recreates ImGui GPU resources.
void OnDeviceReset();

// Render one ImGui frame (call between Present).
void RenderOverlayFrame();

// Check if overlay is ready.
bool IsOverlayReady();
