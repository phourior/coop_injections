#include "pch.h"
#include "hooks/game_hook.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include <MinHook.h>

// ─── 全局原始数据（纯 C，零初始化） ───
RawLobbyData g_rawLobby = {};

// ─── 原函数类型 ───
using OnGameLobbyUpdateFn = __int64(__fastcall*)(__int64 a1, __int64 a2);
static OnGameLobbyUpdateFn g_origOnGameLobbyUpdate = nullptr;

// ─── 纯 C 的 SEH 安全 SC2 字符串读取 ───
static bool ReadSc2StringRaw(uintptr_t strObj, char* out, size_t outCap)
{
    out[0] = '\0';
    if (!strObj || outCap < 2) return false;

    DWORD len = 0, cf = 0;
    if (!SafeMemcpy(&len, reinterpret_cast<const void*>(strObj), 4)) return false;
    if (!SafeMemcpy(&cf, reinterpret_cast<const void*>(strObj + 4), 4)) return false;
    if (len == 0 || len >= outCap) return false;

    uintptr_t dataAddr;
    if (cf & 2)
    {
        dataAddr = 0;
        if (!SafeMemcpy(&dataAddr, reinterpret_cast<const void*>(strObj + 8), 8)) return false;
    }
    else
    {
        dataAddr = strObj + 8;
    }
    if (!dataAddr) return false;
    if (!SafeMemcpy(out, reinterpret_cast<const void*>(dataAddr), len)) return false;
    out[len] = '\0';
    return true;
}

// ─── 纯 C + SEH 的 hook 后处理（写入 g_rawLobby） ───
static void SafeCollectData(uintptr_t a1)
{
    __try
    {
        // 读到临时缓冲区
        char mapPath[512] = {};
        char mapDesc[512] = {};
        DWORD gp1 = 0, gp2 = 0, lobbyFlags = 0;
        DWORD playerIds[16] = {};

        ReadSc2StringRaw(a1 + 0x08, mapPath, sizeof(mapPath));
        ReadSc2StringRaw(a1 + 0x18, mapDesc, sizeof(mapDesc));
        SafeMemcpy(&gp1, reinterpret_cast<const void*>(a1 + 0x48), 4);
        SafeMemcpy(&gp2, reinterpret_cast<const void*>(a1 + 0x4C), 4);
        SafeMemcpy(&lobbyFlags, reinterpret_cast<const void*>(a1 + 0x1EC8), 4);
        SafeMemcpy(playerIds, reinterpret_cast<const void*>(a1 + 0x1C10), sizeof(playerIds));

        // 一次性写入全局结构（纯 memcpy，无堆分配）
        memcpy(g_rawLobby.mapPath, mapPath, sizeof(mapPath));
        memcpy(g_rawLobby.mapDesc, mapDesc, sizeof(mapDesc));
        g_rawLobby.gameParam1  = gp1;
        g_rawLobby.gameParam2  = gp2;
        g_rawLobby.flags       = lobbyFlags;
        memcpy(g_rawLobby.playerIds, playerIds, sizeof(playerIds));
        g_rawLobby.pGameLobby  = a1;

        // 序号最后写（内存屏障），让读取端知道数据已就绪
        InterlockedIncrement(&g_rawLobby.seq);

        // 日志（非 ASCII 替换为 '?'）
        for (int i = 0; mapPath[i]; ++i)
            if ((unsigned char)mapPath[i] > 0x7E || (unsigned char)mapPath[i] < 0x20)
                mapPath[i] = '?';

        int activePlayers = 0;
        for (int i = 0; i < 16; ++i)
            if (playerIds[i]) ++activePlayers;

        Log("[LOBBY #%ld] map='%.200s' p=%u/%u f=0x%X players=%d\n",
            g_rawLobby.seq, mapPath, gp1, gp2, lobbyFlags, activePlayers);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[!] SafeCollectData exception 0x%08X\n", GetExceptionCode());
    }
}

// ─── Hooked OnGameLobbyUpdate ───
static __int64 __fastcall HookedOnGameLobbyUpdate(__int64 a1, __int64 a2)
{
    // ① 立即调用原函数——零干扰
    __int64 ret = g_origOnGameLobbyUpdate(a1, a2);

    // ② 纯 C + SEH 收集数据（无 C++ 对象，无堆分配）
    if (a1)
        SafeCollectData(static_cast<uintptr_t>(a1));

    return ret;
}

// ─── UI 线程调用：从纯 C 数据生成 C++ 快照 ───
LobbyData SnapshotLobbyData()
{
    LobbyData out;
    LONG seq = g_rawLobby.seq;
    if (seq == 0)
        return out;

    out.mapPath        = g_rawLobby.mapPath;
    out.mapDisplayName = g_rawLobby.mapDesc;
    out.gameParam1     = g_rawLobby.gameParam1;
    out.gameParam2     = g_rawLobby.gameParam2;
    out.flags          = g_rawLobby.flags;

    out.playerCount = 0;
    for (int i = 0; i < 16; ++i)
    {
        out.playerIds[i] = g_rawLobby.playerIds[i];
        if (out.playerIds[i]) out.playerCount++;
    }

    out.valid = true;
    return out;
}

// ─── Public API ───

// ─── OnGameLobbyUpdate 特征码定位（版本无关） ───
//
// 历史教训：早期版本硬编码 RVA `base + 0x20C2120` 来 hook `OnGameLobbyUpdate`。
// 客户端更新（>=Base96999）后，该 RVA 落入 sub_1420C1E60 的中段（函数实际入口
// 0x1420C1E60，旧 RVA 偏移 +0x2C0），MinHook 覆盖中段指令导致游戏闪退。
//
// 解决：扫描函数序言 + 特征常数 `add r8, 0x110`（指针链 a2->24+0x110）锁定入口。
// 在 Base96999 中匹配唯一地址，第 11 字节（栈帧大小 0xA8）通配以兼容未来版本。
//
//   48 8B C4              mov rax, rsp
//   48 89 48 08           mov [rax+8], rcx
//   48 81 EC ?? 00 00 00  sub rsp, ??           ; 栈帧（通配，可能随编译器调整）
//   4C 8B 42 18           mov r8, [rdx+0x18]    ; 取 a2->payload
//   48 89 58 10           mov [rax+0x10], rbx
//   49 81 C0 10 01 00 00  add r8, 0x110         ; 唯一特征常数
static constexpr const char* ON_GAME_LOBBY_UPDATE_PATTERN =
    "48 8B C4 48 89 48 08 48 81 EC ?? 00 00 00 "
    "4C 8B 42 18 48 89 58 10 49 81 C0 10 01 00 00";

bool SetupGameHooks()
{
    HMODULE hModule = GetModuleBase("SC2_x64.exe");
    if (!hModule)
    {
        Log("[!] SetupGameHooks: SC2_x64.exe not found\n");
        return false;
    }

    uintptr_t targetAddr = PatternScan("SC2_x64.exe", ON_GAME_LOBBY_UPDATE_PATTERN);
    if (!targetAddr)
    {
        Log("[!] SetupGameHooks: OnGameLobbyUpdate pattern not found, skipping hook\n");
        return false;
    }

    Log("[*] Hooking OnGameLobbyUpdate at 0x%llX (pattern scan)\n",
        (unsigned long long)targetAddr);

    MH_STATUS st = MH_CreateHook(
        reinterpret_cast<LPVOID>(targetAddr),
        &HookedOnGameLobbyUpdate,
        reinterpret_cast<LPVOID*>(&g_origOnGameLobbyUpdate));

    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook failed: %s\n", MH_StatusToString(st));
        return false;
    }

    Log("[+] OnGameLobbyUpdate hook OK\n");
    return true;
}

// ════════════════════════════════════════════════════════════════
//  180 精通补丁（特征码定位 + 内联 shellcode）
// ════════════════════════════════════════════════════════════════
//
//  IDA 验证（SC2_x64.exe Base96921，基址 0x140000000）：
//    0x140EFD6FB  66 89 97 68 01 00 00  mov [rdi+168h], dx   ← 精通值①
//    0x140EFD702  66 89 8F 6A 01 00 00  mov [rdi+16Ah], cx   ← 精通值②
//    0x140EFD709  41 FF C7              inc r15d              ← 返回点
//
//  特征码（14 字节，全二进制唯一）：
//    66 89 97 68 01 00 00  66 89 8F 6A 01 00 00
//
//  Hook 原理：
//    将 14 字节替换为 JMP QWORD [RIP+0] + shellcode 地址，
//    shellcode 将两个字段强制写为 0x7FFF 后跳回 +14 处。
// ────────────────────────────────────────────────────────────────

static constexpr const char* MASTERY_PATTERN =
    "66 89 97 68 01 00 00 66 89 8F 6A 01 00 00";

static uintptr_t g_masteryPatchAddr         = 0;
static uint8_t   g_masteryOrigBytes[14]     = {};
static void*     g_masteryShellcode         = nullptr;
static bool      g_masteryEnabled           = false;

bool EnableMasteryMax()
{
    if (g_masteryEnabled) return true;

    // ① 特征码定位（懒初始化）
    if (!g_masteryPatchAddr)
    {
        g_masteryPatchAddr = PatternScan("SC2_x64.exe", MASTERY_PATTERN);
        if (!g_masteryPatchAddr)
        {
            Log("[!] EnableMasteryMax: pattern not found\n");
            return false;
        }
        Log("[*] Mastery patch addr: 0x%llX\n", (unsigned long long)g_masteryPatchAddr);
    }

    // ② 备份原始 14 字节（用于还原）
    if (!SafeMemcpy(g_masteryOrigBytes,
                    reinterpret_cast<const void*>(g_masteryPatchAddr), 14))
    {
        Log("[!] EnableMasteryMax: backup failed\n");
        return false;
    }

    // ③ 分配可执行 shellcode 区域（32 字节）
    if (!g_masteryShellcode)
    {
        g_masteryShellcode = VirtualAlloc(nullptr, 32,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_masteryShellcode)
        {
            Log("[!] EnableMasteryMax: VirtualAlloc failed\n");
            return false;
        }

        // 返回地址 = patchAddr + 14（inc r15d 处）
        const uintptr_t returnAddr = g_masteryPatchAddr + 14;

        uint8_t sc[32] = {
            // MOV WORD PTR [RDI+0x168], 0x7FFF  (9 bytes)
            0x66, 0xC7, 0x87, 0x68, 0x01, 0x00, 0x00, 0xFF, 0x7F,
            // MOV WORD PTR [RDI+0x16A], 0x7FFF  (9 bytes)
            0x66, 0xC7, 0x87, 0x6A, 0x01, 0x00, 0x00, 0xFF, 0x7F,
            // JMP QWORD PTR [RIP+0]  (6 bytes)
            0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
            // 64-bit return address  (8 bytes, LE)
            static_cast<uint8_t>(returnAddr),
            static_cast<uint8_t>(returnAddr >> 8),
            static_cast<uint8_t>(returnAddr >> 16),
            static_cast<uint8_t>(returnAddr >> 24),
            static_cast<uint8_t>(returnAddr >> 32),
            static_cast<uint8_t>(returnAddr >> 40),
            static_cast<uint8_t>(returnAddr >> 48),
            static_cast<uint8_t>(returnAddr >> 56),
        };
        memcpy(g_masteryShellcode, sc, 32);
        FlushInstructionCache(GetCurrentProcess(), g_masteryShellcode, 32);
    }

    // ④ 写入 14 字节跳转：FF 25 00 00 00 00 <8-byte shellcode ptr>
    uint8_t jmp14[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    const uintptr_t scAddr = reinterpret_cast<uintptr_t>(g_masteryShellcode);
    memcpy(jmp14 + 6, &scAddr, 8);

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(g_masteryPatchAddr), 14,
                        PAGE_EXECUTE_READWRITE, &oldProt))
    {
        Log("[!] EnableMasteryMax: VirtualProtect failed\n");
        return false;
    }
    memcpy(reinterpret_cast<void*>(g_masteryPatchAddr), jmp14, 14);
    VirtualProtect(reinterpret_cast<LPVOID>(g_masteryPatchAddr), 14, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<LPCVOID>(g_masteryPatchAddr), 14);

    g_masteryEnabled = true;
    Log("[+] Mastery MAX enabled (pattern-based)\n");
    return true;
}

void DisableMasteryMax()
{
    if (!g_masteryEnabled || !g_masteryPatchAddr) return;

    DWORD oldProt = 0;
    if (VirtualProtect(reinterpret_cast<LPVOID>(g_masteryPatchAddr), 14,
                       PAGE_EXECUTE_READWRITE, &oldProt))
    {
        memcpy(reinterpret_cast<void*>(g_masteryPatchAddr), g_masteryOrigBytes, 14);
        VirtualProtect(reinterpret_cast<LPVOID>(g_masteryPatchAddr), 14, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<LPCVOID>(g_masteryPatchAddr), 14);
    }
    g_masteryEnabled = false;
    Log("[-] Mastery MAX disabled\n");
}

bool IsMasteryMaxEnabled()
{
    return g_masteryEnabled;
}

// ════════════════════════════════════════════════════════════════
//  NNet UDP 包捕获 hook
//  IDA 基址 0x140000000，运行时偏移：
//    sub_142179870  +0x2179870  — UDP 发送适配器（TX）
//    sub_1401DBE20  +0x01DBE20  — recvfrom 包装（RX）
//    sub_1413A6FE0  +0x13A6FE0  — 游戏事件发送（事件对象层，TX）
// ════════════════════════════════════════════════════════════════

volatile LONG  g_nnetHead    = 0;
NNetPacket     g_nnetRing[NNET_RING_SIZE] = {};
volatile LONG  g_nnetCapture = 0;

// ─── 原函数指针 ───
using NNetTxFn   = __int64(__fastcall*)(__int64, const char*, int, const void*);
using NNetRxFn   = __int64(__fastcall*)(__int64, char*, int, void*, int*);
using NNetEvtFn  = __int64(__fastcall*)(__int64, void***, const void*);

static NNetTxFn   g_origNNetTx  = nullptr;
static NNetRxFn   g_origNNetRx  = nullptr;
static NNetEvtFn  g_origNNetEvt = nullptr;

// ─── 辅助：提取 sockaddr 的 IP/Port 字符串 ───
// sockaddr 内存布局（AF_INET）：
//   [0-1] sa_family (小端)   [2-3] port (大端)   [4-7] IPv4
static void FormatSockAddr(const void* pSa, char* out, int outCap)
{
    if (!pSa) { out[0] = '\0'; return; }
    const unsigned char* sa = static_cast<const unsigned char*>(pSa);
    int family = sa[0] | (sa[1] << 8);
    if (family == 2)   // AF_INET
    {
        int port = (sa[2] << 8) | sa[3];
        _snprintf_s(out, outCap, _TRUNCATE,
            "%d.%d.%d.%d:%d", sa[4], sa[5], sa[6], sa[7], port);
    }
    else
    {
        _snprintf_s(out, outCap, _TRUNCATE, "fam%d", family);
    }
}

// ─── 辅助：向环形缓冲区写入一条记录 ───
static void NNetRingPush(bool isTx, const char* data, int len, const void* pSa)
{
    if (!g_nnetCapture) return;
    __try
    {
        LONG idx = InterlockedIncrement(&g_nnetHead);
        NNetPacket& p = g_nnetRing[idx % NNET_RING_SIZE];
        p.isTx   = isTx;
        p.tickMs = GetTickCount();
        p.len    = len;
        int cpLen = len < (int)sizeof(p.data) ? len : (int)sizeof(p.data);
        if (data && cpLen > 0)
            memcpy(p.data, data, static_cast<size_t>(cpLen));
        if (cpLen < (int)sizeof(p.data))
            memset(p.data + cpLen, 0, sizeof(p.data) - cpLen);
        FormatSockAddr(pSa, p.addrStr, sizeof(p.addrStr));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ─── TX Hook：sub_142179870 ───
static __int64 __fastcall HookedNNetTx(__int64 socketCtx, const char* data, int len, const void* addr)
{
    NNetRingPush(true, data, len, addr);
    return g_origNNetTx(socketCtx, data, len, addr);
}

// ─── RX Hook：sub_1401DBE20 ───
static __int64 __fastcall HookedNNetRx(__int64 socketObj, char* buf, int bufLen, void* from, int* bytesRecv)
{
    __int64 ret = g_origNNetRx(socketObj, buf, bufLen, from, bytesRecv);
    if (ret == 1 && buf && bytesRecv)   // ret==1 表示接收成功
        NNetRingPush(false, buf, *bytesRecv, from);
    return ret;
}

// ─── Event Hook：sub_1413A6FE0  (事件对象层) ───
static __int64 __fastcall HookedNNetEvt(__int64 a1, void*** a2, const void* a3)
{
    __try
    {
        if (g_nnetCapture && a2)
        {
            uintptr_t vtable = reinterpret_cast<uintptr_t>(*a2);
            Log("[NNET EVT TX] vtable=0x%llX addr=0x%p\n",
                (unsigned long long)vtable, a3);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_origNNetEvt(a1, a2, a3);
}

// ─── NNet hook 目标地址（用于运行时启用/禁用） ───
static uintptr_t g_nnetHookTargets[3] = {};

// ─── 公开安装函数：仅创建 trampoline，不实际启用 ───
bool SetupNNetHooks()
{
    HMODULE hMod = GetModuleBase("SC2_x64.exe");
    if (!hMod)
    {
        Log("[!] SetupNNetHooks: SC2_x64.exe not found\n");
        return false;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(hMod);

    struct HookEntry {
        uintptr_t   offset;
        LPVOID      hookFn;
        LPVOID*     origFn;
        const char* name;
    };

    HookEntry entries[] = {
        { 0x2179870, reinterpret_cast<LPVOID>(&HookedNNetTx),
          reinterpret_cast<LPVOID*>(&g_origNNetTx),  "NNetUdpSend" },
        { 0x01DBE20, reinterpret_cast<LPVOID>(&HookedNNetRx),
          reinterpret_cast<LPVOID*>(&g_origNNetRx),  "NNetUdpRecv" },
        { 0x13A6FE0, reinterpret_cast<LPVOID>(&HookedNNetEvt),
          reinterpret_cast<LPVOID*>(&g_origNNetEvt), "NNetEvtSend" },
    };

    bool allOk = true;
    int idx = 0;
    for (auto& e : entries)
    {
        uintptr_t target = base + e.offset;
        MH_STATUS st = MH_CreateHook(
            reinterpret_cast<LPVOID>(target), e.hookFn, e.origFn);
        if (st != MH_OK)
        {
            Log("[!] %s hook failed (0x%llX): %s\n",
                e.name, (unsigned long long)target, MH_StatusToString(st));
            g_nnetHookTargets[idx] = 0;
            allOk = false;
        }
        else
        {
            Log("[+] %s hook created (lazy) at 0x%llX\n",
                e.name, (unsigned long long)target);
            g_nnetHookTargets[idx] = target;
        }
        ++idx;
    }
    return allOk;
}

// 启动时调用：MH_EnableHook(MH_ALL_HOOKS) 会把 NNet hook 也启用，这里立即关掉
void DisableNNetHooksAtStartup()
{
    for (uintptr_t t : g_nnetHookTargets)
    {
        if (t)
            MH_DisableHook(reinterpret_cast<LPVOID>(t));
    }
}

// 运行时开关：用户在 UI 勾选/取消勾选时调用
void EnableNNetCapture(bool on)
{
    if (on)
    {
        // 先标记后启用：trampoline 命中时立即可写入环形缓冲
        InterlockedExchange(&g_nnetCapture, 1);
        for (uintptr_t t : g_nnetHookTargets)
            if (t) MH_EnableHook(reinterpret_cast<LPVOID>(t));
        Log("[+] NNet capture enabled\n");
    }
    else
    {
        // 先禁用 trampoline 后清标记：彻底回到零开销路径
        for (uintptr_t t : g_nnetHookTargets)
            if (t) MH_DisableHook(reinterpret_cast<LPVOID>(t));
        InterlockedExchange(&g_nnetCapture, 0);
        Log("[-] NNet capture disabled\n");
    }
}
