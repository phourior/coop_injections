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

bool SetupGameHooks()
{
    HMODULE hModule = GetModuleBase("SC2_x64.exe");
    if (!hModule)
    {
        Log("[!] SetupGameHooks: SC2_x64.exe not found\n");
        return false;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(hModule);
    uintptr_t targetAddr = base + 0x20C2120;

    Log("[*] Hooking OnGameLobbyUpdate at 0x%llX\n", (unsigned long long)targetAddr);

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
