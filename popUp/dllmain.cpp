// dllmain.cpp : 简单注入测试 - 弹窗确认DLL是否成功加载
#include "pch.h"
#include <cstdio>
#include <string>

static HMODULE g_hModule = nullptr;

static DWORD WINAPI TestThread(LPVOID)
{
    // 1) 弹窗确认注入成功
    MessageBoxA(nullptr, "DLL injected OK!", "popUp Test", MB_OK | MB_TOPMOST);

    // 2) 检测 d3d11.dll 是否已加载
    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    HMODULE hDXGI  = GetModuleHandleA("dxgi.dll");

    char buf[512];
    snprintf(buf, sizeof(buf),
        "d3d11.dll = %p\n"
        "dxgi.dll  = %p\n\n"
        "Process ID = %u\n"
        "Module base = %p",
        hD3D11, hDXGI,
        GetCurrentProcessId(),
        g_hModule);

    MessageBoxA(nullptr, buf, "Module Info", MB_OK | MB_TOPMOST);

    // 3) 枚举所有窗口找到目标进程的主窗口
    struct FindData {
        DWORD pid;
        HWND  hwnd;
    } fd{ GetCurrentProcessId(), nullptr };

    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        auto* p = reinterpret_cast<FindData*>(lParam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == p->pid && IsWindowVisible(hWnd)) {
            p->hwnd = hWnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fd));

    char buf2[256];
    if (fd.hwnd) {
        char title[128]{};
        GetWindowTextA(fd.hwnd, title, 128);
        RECT rc{};
        GetClientRect(fd.hwnd, &rc);
        snprintf(buf2, sizeof(buf2),
            "Main HWND = %p\n"
            "Title = %s\n"
            "Client = %dx%d",
            fd.hwnd, title, rc.right, rc.bottom);
    } else {
        snprintf(buf2, sizeof(buf2), "No visible window found for PID %u", fd.pid);
    }
    MessageBoxA(nullptr, buf2, "Window Info", MB_OK | MB_TOPMOST);

    // 不自动卸载，按 END 键卸载
    while (!(GetAsyncKeyState(VK_END) & 0x8000))
        Sleep(100);

    MessageBoxA(nullptr, "Unloading...", "popUp Test", MB_OK | MB_TOPMOST);
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        HANDLE h = CreateThread(nullptr, 0, TestThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}

