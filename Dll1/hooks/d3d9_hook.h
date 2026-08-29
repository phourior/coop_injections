#pragma once

// Set up all hooks (currently D3D9 SwapChain::Present).
// Call from MainThread after DLL injection.
bool SetupHooks();

// True after the installed D3D9 SwapChain::Present hook receives its first call.
bool HasD3D9PresentHookFired();

// Disable all hooks, shut down overlay, uninitialize MinHook.
void CleanupHooks();
