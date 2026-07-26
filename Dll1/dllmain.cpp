// dllmain.cpp : DLL entry point
#include "pch.h"
#include "core/globals.h"
#include "core/game_data.h"
#include "core/log.h"
#include "hooks/d3d9_hook.h"

static constexpr const wchar_t* kInjectorExitEventName =
    L"Local\\CoopInjections_SC2_Dll1_InjectorExit";

static void RequestInjectorExit()
{
    // 注入器持有命名事件；DLL 只发送退出通知，不枚举或强制终止其他进程。
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kInjectorExitEventName);
    if (!event)
    {
        Log("[!] Injector exit event unavailable  err=%lu\n", GetLastError());
        return;
    }

    SetEvent(event);
    CloseHandle(event);
}

static DWORD WINAPI MainThread(LPVOID)
{
    Log("[+] MainThread started. PID=%u\n", GetCurrentProcessId());

    // 审查修复 #9：在安装渲染 Hook 前由工作线程完成并缓存模块扫描，避免
    // 第一次 Present 在游戏渲染线程上承担全镜像扫描开销。
    WarmUpGameDataScans();

    if (!SetupHooks())
    {
        Log("[!] SetupHooks failed\n");
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    while (!(GetAsyncKeyState(VK_END) & 0x8000))
        Sleep(100);

    // END 同时请求唯一注入器实例正常退出，然后再执行 DLL 的安全卸载流程。
    RequestInjectorExit();
    CleanupHooks();
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;

        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
