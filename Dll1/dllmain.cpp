// dllmain.cpp : DLL entry point
#include "pch.h"
#include "core/globals.h"
#include "core/log.h"
#include "hooks/d3d9_hook.h"

static DWORD WINAPI MainThread(LPVOID)
{
    Log("[+] MainThread started. PID=%u\n", GetCurrentProcessId());

    if (!SetupHooks())
    {
        Log("[!] SetupHooks failed\n");
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    while (!(GetAsyncKeyState(VK_END) & 0x8000))
        Sleep(100);

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
