#include "pch.h"
#include "core/globals.h"

HMODULE g_hModule = nullptr;

IDirect3DDevice9*    g_device = nullptr;
IDirect3DSwapChain9* g_swapChain = nullptr;

HWND     g_hWnd = nullptr;
WNDPROC  g_origWndProc = nullptr;

float g_screenWidth  = 1920.0f;
float g_screenHeight = 1080.0f;

volatile bool g_imguiInitialized = false;
bool          g_showMenu = true;

SRWLOCK       g_hookCallbackLock = SRWLOCK_INIT;
volatile LONG g_unloading = 0;
