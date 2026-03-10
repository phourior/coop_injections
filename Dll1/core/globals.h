#pragma once
#include <windows.h>
#include <d3d9.h>

// DLL module handle
extern HMODULE g_hModule;

// D3D9 state
extern IDirect3DDevice9*    g_device;
extern IDirect3DSwapChain9* g_swapChain;

// Window state
extern HWND     g_hWnd;
extern WNDPROC  g_origWndProc;

// UI state
extern volatile bool g_imguiInitialized;
extern bool          g_showMenu;
