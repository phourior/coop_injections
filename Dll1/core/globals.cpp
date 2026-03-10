#include "pch.h"
#include "core/globals.h"

HMODULE g_hModule = nullptr;

IDirect3DDevice9*    g_device = nullptr;
IDirect3DSwapChain9* g_swapChain = nullptr;

HWND     g_hWnd = nullptr;
WNDPROC  g_origWndProc = nullptr;

volatile bool g_imguiInitialized = false;
bool          g_showMenu = true;
