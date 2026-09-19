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
    Log("[*] Graphics modules at injection: d3d9=%p d3d11=%p dxgi=%p opengl32=%p\n",
        GetModuleHandleW(L"d3d9.dll"), GetModuleHandleW(L"d3d11.dll"),
        GetModuleHandleW(L"dxgi.dll"), GetModuleHandleW(L"opengl32.dll"));

    // 审查修复 #9：在安装渲染 Hook 前由工作线程完成并缓存模块扫描，避免
    // 第一次 Present 在游戏渲染线程上承担全镜像扫描开销。
    WarmUpGameDataScans();

    if (!SetupHooks())
    {
        Log("[!] SetupHooks failed\n");
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    const ULONGLONG hooksInstalledAt = GetTickCount64();
    bool presentTimeoutLogged = false;
    while (!(GetAsyncKeyState(VK_END) & 0x8000))
    {
        if (!presentTimeoutLogged &&
            GetTickCount64() - hooksInstalledAt >= 10000 &&
            !HasD3D9PresentHookFired())
        {
            Log("[!] No D3D9 Present hook received calls within 10 seconds. "
                "Checked SwapChain::Present, Device::Present, Device9Ex::PresentEx "
                "and Device::EndScene. "
                "Current modules: d3d11=%p dxgi=%p\n",
                GetModuleHandleW(L"d3d11.dll"), GetModuleHandleW(L"dxgi.dll"));
            presentTimeoutLogged = true;
        }
        Sleep(100);
    }

    // END 同时请求唯一注入器实例正常退出，然后再执行 DLL 的安全卸载流程。
    RequestInjectorExit();
    if (!CleanupHooks())
    {
        Log("[!] DLL unload aborted after cleanup failure; restart the game before reinjecting\n");
        return 1;
    }
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
