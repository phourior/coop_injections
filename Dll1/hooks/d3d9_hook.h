#pragma once

// Set up all hooks (currently D3D9 SwapChain::Present).
// Call from MainThread after DLL injection.
bool SetupHooks();

// Disable all hooks, shut down overlay, uninitialize MinHook.
void CleanupHooks();
