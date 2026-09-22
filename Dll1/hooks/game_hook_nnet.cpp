#include "pch.h"
#include "hooks/game_hook.h"
#include "core/globals.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

// ════════════════════════════════════════════════════════════════
//  NNet UDP 包捕获 hook
//  IDA 基址 0x140000000，运行时偏移：
//    sub_142179870  +0x2179870  — UDP 发送适配器（TX）
//    sub_1401DBE20  +0x01DBE20  — recvfrom 包装（RX）
//    sub_1413A6FE0  +0x13A6FE0  — 游戏事件发送（事件对象层，TX）
// ════════════════════════════════════════════════════════════════

volatile LONG  g_nnetHead = 0;
NNetPacket g_nnetRing[NNET_RING_SIZE] = {};
volatile LONG  g_nnetCapture = 0;

using NNetTxFn = __int64(__fastcall*)(__int64, const char*, int, const void*);
using NNetRxFn = __int64(__fastcall*)(__int64, char*, int, void*, int*);
using NNetEvtFn = __int64(__fastcall*)(__int64, void***, const void*);

static NNetTxFn g_origNNetTx = nullptr;
static NNetRxFn g_origNNetRx = nullptr;
static NNetEvtFn g_origNNetEvt = nullptr;
static uintptr_t g_nnetHookTargets[3] = {};

static void FormatSockAddr(const void* pSa, char* out, int outCap)
{
    if (!pSa)
    {
        out[0] = '\0';
        return;
    }

    const unsigned char* sa = static_cast<const unsigned char*>(pSa);
    const int family = sa[0] | (sa[1] << 8);
    if (family == 2)
    {
        const int port = (sa[2] << 8) | sa[3];
        _snprintf_s(out, outCap, _TRUNCATE,
            "%d.%d.%d.%d:%d", sa[4], sa[5], sa[6], sa[7], port);
    }
    else
    {
        _snprintf_s(out, outCap, _TRUNCATE, "fam%d", family);
    }
}

static void NNetRingPush(bool isTx, const char* data, int len, const void* pSa)
{
    if (!g_nnetCapture)
        return;

    __try
    {
        const LONG index = InterlockedIncrement(&g_nnetHead);
        NNetPacket& packet = g_nnetRing[index % NNET_RING_SIZE];
        packet.isTx = isTx;
        packet.tickMs = GetTickCount64();
        packet.len = len;

        const int copyLength = len < static_cast<int>(sizeof(packet.data))
            ? len
            : static_cast<int>(sizeof(packet.data));
        if (data && copyLength > 0)
            memcpy(packet.data, data, static_cast<size_t>(copyLength));
        if (copyLength < static_cast<int>(sizeof(packet.data)))
            memset(packet.data + copyLength, 0, sizeof(packet.data) - copyLength);

        FormatSockAddr(pSa, packet.addrStr, sizeof(packet.addrStr));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static __int64 __fastcall HookedNNetTx(__int64 socketCtx, const char* data, int len, const void* addr)
{
    EnterHookCallback();
    if (!IsDllUnloading())
        NNetRingPush(true, data, len, addr);
    const __int64 result = g_origNNetTx(socketCtx, data, len, addr);
    LeaveHookCallback();
    return result;
}

static __int64 __fastcall HookedNNetRx(__int64 socketObj, char* buf, int bufLen, void* from, int* bytesRecv)
{
    EnterHookCallback();
    const __int64 result = g_origNNetRx(socketObj, buf, bufLen, from, bytesRecv);
    if (!IsDllUnloading() && result == 1 && buf && bytesRecv)
        NNetRingPush(false, buf, *bytesRecv, from);
    LeaveHookCallback();
    return result;
}

static __int64 __fastcall HookedNNetEvt(__int64 a1, void*** a2, const void* a3)
{
    EnterHookCallback();
    __try
    {
        if (!IsDllUnloading() && g_nnetCapture && a2)
        {
            const uintptr_t vtable = reinterpret_cast<uintptr_t>(*a2);
            Log("[NNET EVT TX] vtable=0x%llX addr=0x%p\n",
                static_cast<unsigned long long>(vtable), a3);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    const __int64 result = g_origNNetEvt(a1, a2, a3);
    LeaveHookCallback();
    return result;
}

bool SetupNNetHooks()
{
    // 审查修复 #4：旧实现的固定 RVA 没有函数签名验证；客户端更新后即使
    // MH_CreateHook 成功也可能按错误原型调用，因此缺少唯一特征码时安全禁用。
    // The previous implementation used three Base97579 RVAs without validating
    // the function bodies. A client update can leave those addresses executable
    // while changing their signatures, making a successful MH_CreateHook unsafe.
    // Keep capture unavailable until unique byte signatures are recorded and
    // verified for all three entry points.
    Log("[!] NNet capture disabled: hook signatures are not verified for this client\n");
    return false;
}

void DisableNNetHooksAtStartup()
{
    for (uintptr_t target : g_nnetHookTargets)
    {
        if (target)
            MH_DisableHook(reinterpret_cast<LPVOID>(target));
    }
}

void EnableNNetCapture(bool on)
{
    if (on)
    {
        if (!g_nnetHookTargets[0] || !g_nnetHookTargets[1] || !g_nnetHookTargets[2])
        {
            InterlockedExchange(&g_nnetCapture, 0);
            Log("[!] NNet capture unavailable: no verified hooks installed\n");
            return;
        }

        InterlockedExchange(&g_nnetCapture, 1);
        for (uintptr_t target : g_nnetHookTargets)
        {
            if (target)
                MH_EnableHook(reinterpret_cast<LPVOID>(target));
        }
        Log("[+] NNet capture enabled\n");
        return;
    }

    for (uintptr_t target : g_nnetHookTargets)
    {
        if (target)
            MH_DisableHook(reinterpret_cast<LPVOID>(target));
    }
    InterlockedExchange(&g_nnetCapture, 0);
    Log("[-] NNet capture disabled\n");
}