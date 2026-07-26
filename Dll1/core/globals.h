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

// Screen resolution (set during overlay init)
extern float g_screenWidth;
extern float g_screenHeight;

// UI state
extern volatile bool g_imguiInitialized;
extern bool          g_showMenu;

// Hook callbacks hold this gate for their complete lifetime. Unload first closes
// the entry points, then takes the gate exclusively before freeing DLL resources.
extern SRWLOCK      g_hookCallbackLock;
extern volatile LONG g_unloading;

inline void EnterHookCallback()
{
	AcquireSRWLockShared(&g_hookCallbackLock);
}

inline void LeaveHookCallback()
{
	ReleaseSRWLockShared(&g_hookCallbackLock);
}

inline bool IsDllUnloading()
{
	return InterlockedCompareExchange(&g_unloading, 0, 0) != 0;
}

inline void BeginDllUnload()
{
	InterlockedExchange(&g_unloading, 1);
}

inline void WaitForHookCallbacks()
{
	AcquireSRWLockExclusive(&g_hookCallbackLock);
	ReleaseSRWLockExclusive(&g_hookCallbackLock);
}
