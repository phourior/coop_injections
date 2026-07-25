#include "pch.h"
#include "hooks/game_hook.h"
#include "core/game_data.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <limits>

// ─── 全局原始数据（纯 C，零初始化） ───
RawLobbyData g_rawLobby = {};

// ─── 原函数类型 ───
using OnGameLobbyUpdateFn = __int64(__fastcall*)(__int64 a1, __int64 a2);
static OnGameLobbyUpdateFn g_origOnGameLobbyUpdate = nullptr;

// 全图视野由地图状态机驱动。声明放在大厅 Hook 前，具体补丁实现在下方。
static void UpdateFullMapVisionMapState(const char* mapPath, const char* mapDesc);

// 反向扫描状态：0=尚未扫描，1=某个 Hook 线程正在扫描，2=扫描完成。
// 使用 Interlocked API 防止大厅更新由多个线程同时触发时重复遍历内存。
static volatile LONG g_lobbyReverseScanState = 0;
static volatile LONG g_lobbyLogPathReported = 0;

static HANDLE OpenLobbyScanLog(wchar_t* actualPath, size_t actualPathCount)
{
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;

    // 通过本 DLL 中静态变量的地址取得模块路径，日志默认与注入 DLL 放在一起。
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(const_cast<LONG*>(&g_lobbyReverseScanState)), &self))
    {
        DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
        wchar_t* slash = length ? wcsrchr(path, L'\\') : nullptr;
        static constexpr wchar_t kFileName[] = L"lobby_offsets.log";
        if (slash && static_cast<size_t>(slash - path + 1) + _countof(kFileName) <= MAX_PATH)
            wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), kFileName);
        else
            path[0] = L'\0';
    }

    HANDLE file = path[0]
        ? CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
        : INVALID_HANDLE_VALUE;

    // DLL 所在目录不可写时回退到当前用户的临时目录。
    if (file == INVALID_HANDLE_VALUE)
    {
        DWORD length = GetTempPathW(MAX_PATH, path);
        static constexpr wchar_t kFallbackName[] = L"Dll1_lobby_offsets.log";
        if (!length || length + _countof(kFallbackName) > MAX_PATH)
            return INVALID_HANDLE_VALUE;
        wcscpy_s(path + length, MAX_PATH - length, kFallbackName);
        file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    if (file != INVALID_HANDLE_VALUE && actualPath && actualPathCount)
        wcscpy_s(actualPath, actualPathCount, path);
    return file;
}

// 偏移扫描日志同时发送到 DebugView，并追加写入本地 lobby_offsets.log。
static void LobbyScanLog(const char* format, ...)
{
    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    OutputDebugStringA(buffer);

    wchar_t path[MAX_PATH] = {};
    HANDLE file = OpenLobbyScanLog(path, _countof(path));
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, buffer, static_cast<DWORD>(strlen(buffer)), &written, nullptr);
    CloseHandle(file);

    // 每次 DLL 生命周期只在 DebugView 中提示一次实际落盘路径。
    if (InterlockedCompareExchange(&g_lobbyLogPathReported, 1, 0) == 0)
    {
        char utf8Path[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8Path, sizeof(utf8Path), nullptr, nullptr);
        Log("[LOBBY-SCAN] local log: %s\n", utf8Path);
    }
}

static bool IsLikelyObjectPointer(uintptr_t value)
{
    // SC2 的节点指针可能借用最低位保存标志，判断地址范围前先移除该标志。
    // 这里只过滤明显无效的值；真正读取仍由 ReadMemory/SafeMemcpy 通过 SEH 保护。
    value &= ~uintptr_t{1};
    return value >= 0x10000 && value <= 0x00007FFFFFFFFFFFULL;
}

// gameLobby 来自 OnGameLobbyUpdate 的 RCX，因此它是已知正确的 GameLobby*。
// 以该地址为目标反查 CBattleNet 对象，区分以下两种可能布局：
//   1. [CBattleNet+X] & ~1 == GameLobby                 （直接字段/内嵌节点字段）
//   2. node=[CBattleNet+X], [node+Y] & ~1 == GameLobby （成员保存节点指针）
static void ReverseScanLobbyPointer(uintptr_t gameLobby)
{
    // 只有第一个线程能把状态从 0 改为 1 并执行扫描；已完成时直接返回。
    if (!gameLobby || InterlockedCompareExchange(&g_lobbyReverseScanState, 1, 0) != 0)
        return;

    // ScanCBattleNetGlobal 返回的是全局槽地址，槽中保存的值才是 CBattleNet*。
    const uintptr_t globalSlot = ScanCBattleNetGlobal();
    const uintptr_t battleNet = globalSlot ? ReadMemory<uintptr_t>(globalSlot) : 0;
    if (!battleNet)
    {
        LobbyScanLog("[LOBBY-SCAN] CBattleNet global/object unavailable; will retry\r\n");
        // 初始化时序过早不算永久失败，恢复为 0，让下一次大厅更新重试。
        InterlockedExchange(&g_lobbyReverseScanState, 0);
        return;
    }

    // sub_14205C7D0 以 0xAA0 分配 CBattleNet；节点内部只探测前 0x40 字节。
    constexpr size_t kBattleNetSize = 0xAA0;
    constexpr size_t kMaxNodeValueOffset = 0x40;
    int directHits = 0;
    int indirectHits = 0;
    size_t directFieldCandidate = SIZE_MAX;
    size_t indirectMemberCandidate = SIZE_MAX;
    size_t indirectValueCandidate = SIZE_MAX;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    LobbyScanLog("\r\n=== Lobby offset scan %04u-%02u-%02u %02u:%02u:%02u ===\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    LobbyScanLog("[LOBBY-SCAN] slot=0x%llX CBattleNet=0x%llX GameLobby=0x%llX size=0x%zX\r\n",
        static_cast<unsigned long long>(globalSlot),
        static_cast<unsigned long long>(battleNet),
        static_cast<unsigned long long>(gameLobby),
        kBattleNetSize);

    for (size_t fieldOffset = 0; fieldOffset + sizeof(uintptr_t) <= kBattleNetSize;
         fieldOffset += sizeof(uintptr_t))
    {
        // 第一层：逐个读取 CBattleNet 中按 8 字节对齐的成员值。
        const uintptr_t memberValue = ReadMemory<uintptr_t>(battleNet + fieldOffset);

        // 直接命中也可能表示 EventChainNode 内嵌于 CBattleNet：
        // 若 GameLobby 位于 node+0x08，则节点起始偏移为 fieldOffset-0x08。
        if ((memberValue & ~uintptr_t{1}) == gameLobby)
        {
            ++directHits;
            directFieldCandidate = fieldOffset;
            LobbyScanLog("[LOBBY-SCAN] DIRECT field=CBattleNet+0x%zX raw=0x%llX; "
                "embedded-node candidate at +0x%zX when value offset is +0x8\r\n",
                fieldOffset,
                static_cast<unsigned long long>(memberValue),
                fieldOffset >= 8 ? fieldOffset - 8 : 0);
        }

        if (!IsLikelyObjectPointer(memberValue))
            continue;

        // 第二层：把成员值视为外部节点指针，寻找节点内保存 GameLobby* 的偏移 Y。
        const uintptr_t node = memberValue & ~uintptr_t{1};
        for (size_t valueOffset = 0; valueOffset <= kMaxNodeValueOffset;
             valueOffset += sizeof(uintptr_t))
        {
            const uintptr_t raw = ReadMemory<uintptr_t>(node + valueOffset);
            if ((raw & ~uintptr_t{1}) != gameLobby)
                continue;

            ++indirectHits;
            indirectMemberCandidate = fieldOffset;
            indirectValueCandidate = valueOffset;
            LobbyScanLog("[LOBBY-SCAN] INDIRECT member=CBattleNet+0x%zX -> node=0x%llX, "
                "GameLobby at node+0x%zX raw=0x%llX\r\n",
                fieldOffset,
                static_cast<unsigned long long>(node),
                valueOffset,
                static_cast<unsigned long long>(raw));
        }
    }

    LobbyScanLog("[LOBBY-SCAN] complete: direct=%d indirect=%d\r\n", directHits, indirectHits);

    // 只有唯一布局时才给出明确替换值；多个候选必须结合 IDA xref 再确认。
    if (directHits == 1 && indirectHits == 0 && directFieldCandidate >= 8)
    {
        LobbyScanLog("[LOBBY-SCAN] RECOMMENDATION (embedded node):\r\n"
            "  ReadLobbyInfo CBattleNet node offset: 0x%zX\r\n"
            "  EventChainNode GameLobby offset:      0x08\r\n"
            "  GameLobby mapPath/mapName offsets:    0x08 / 0x18 (unchanged)\r\n"
            "  pEvtNode = pBattleNet + 0x%zX; // do not dereference here\r\n",
            directFieldCandidate - 8, directFieldCandidate - 8);
    }
    else if (indirectHits == 1 && directHits == 0)
    {
        LobbyScanLog("[LOBBY-SCAN] RECOMMENDATION (pointer node):\r\n"
            "  ReadLobbyInfo CBattleNet member offset: 0x%zX\r\n"
            "  Node GameLobby pointer offset:          0x%zX\r\n"
            "  GameLobby mapPath/mapName offsets:      0x08 / 0x18 (unchanged)\r\n"
            "  pEvtNode = ReadMemory<uintptr_t>(pBattleNet + 0x%zX);\r\n",
            indirectMemberCandidate, indirectValueCandidate, indirectMemberCandidate);
    }
    else if (directHits == 0 && indirectHits == 0)
    {
        LobbyScanLog("[LOBBY-SCAN] RECOMMENDATION: no matching offset found; do not change ReadLobbyInfo yet.\r\n");
    }
    else
    {
        LobbyScanLog("[LOBBY-SCAN] RECOMMENDATION: multiple candidates found; do not change ReadLobbyInfo until IDA xrefs confirm one.\r\n");
    }

    // 标记永久完成，后续大厅更新不再进行内存遍历。
    InterlockedExchange(&g_lobbyReverseScanState, 2);
}

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

        // 目标 DLL 同时检查地图路径和显示标识。状态机只在匹配结果发生变化时
        // 应用或恢复 Fog 补丁，因此频繁的大厅更新不会反复写代码页。
        UpdateFullMapVisionMapState(mapPath, mapDesc);

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

    // ② 原函数完成后，以它传入的 GameLobby* 做一次性诊断扫描。
    // ③ 继续执行原有的数据收集；扫描不修改 a1 指向的对象和 g_rawLobby。
    if (a1)
    {
        ReverseScanLobbyPointer(static_cast<uintptr_t>(a1));
        SafeCollectData(static_cast<uintptr_t>(a1));
    }

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
//  全图视野补丁
// ════════════════════════════════════════════════════════════════
//
// Base97579 中目标位于 SC2_x64.exe+0x1CD7A79：
//   48 8D 0D xx xx xx xx    lea rcx, [visibilityTable]
//   8B 34 81                mov esi, [rcx+rax*4]
// 补丁把这 10 字节替换为 `mov esi,0FFFFFFFFh` + 5 个 NOP，使后续代码
// 看到全位掩码。RIP 位移、栈偏移和短跳转距离会随版本变化，因此 pattern
// 只保留表达控制流/数据流的 opcode。升级流程见 docs/sc2_feature_update_guide.md。

static constexpr const char* FULL_MAP_VISION_PATTERN =
    "48 8D 0D ?? ?? ?? ?? 8B 34 81 48 89 6C 24 ?? "
    "41 80 FD 10 74 ?? 44 0F A3 EE";

static constexpr uint8_t FULL_MAP_VISION_PATCH[10] = {
    0xBE, 0xFF, 0xFF, 0xFF, 0xFF, // mov esi, 0xFFFFFFFF
    0x90, 0x90, 0x90, 0x90, 0x90,
};

// 原字节只在首次启用时保存。后续关闭必须恢复同一进程、同一版本的字节，
// 不能把 Base97579 的原字节硬编码进 DLL。
static uintptr_t g_fullMapVisionAddr = 0;
static uint8_t   g_fullMapVisionOrigBytes[10] = {};
static bool      g_fullMapVisionOrigSaved = false;
static bool      g_fullMapVisionPatched = false;
static volatile LONG g_fullMapVisionRequested = 0;
static volatile LONG g_fullMapVisionMapIndex = -1;
static SRWLOCK   g_fullMapVisionLock = SRWLOCK_INIT;

// 与逆向目标 DLL 一致，只在这些合作任务地图中自动应用 Fog 补丁。SC2 的两段
// 标识可能分别保存内部路径和本地化显示名，所以每个名称都同时匹配两段字符串。
static constexpr const char* FULL_MAP_VISION_MAPS[] = {
    "虚空撕裂", "克哈裂痕", "虚空降临", "往日神庙", "湮灭快车",
    "天界封锁", "升格之链", "熔火危机", "机会渺茫", "营救矿工",
    "亡者之夜", "黑暗杀星", "净网行动", "聚铁成兵", "死亡摇篮",
};

static int FindFullMapVisionMap(const char* mapPath, const char* mapDesc)
{
    const char* path = mapPath ? mapPath : "";
    const char* desc = mapDesc ? mapDesc : "";
    for (size_t index = 0; index < _countof(FULL_MAP_VISION_MAPS); ++index)
    {
        const char* name = FULL_MAP_VISION_MAPS[index];
        if (strstr(path, name) || strstr(desc, name))
            return static_cast<int>(index);
    }
    return -1;
}

static bool WriteCodeBytes(uintptr_t address, const void* bytes, size_t size)
{
    DWORD oldProtect = 0;
    if (!address || !bytes || !size ||
        !VirtualProtect(reinterpret_cast<LPVOID>(address), size,
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    // 修改代码页后恢复原保护，并刷新 CPU 指令缓存；缺少任一步都可能导致
    // 当前线程继续执行旧指令或留下永久可写的代码页。
    memcpy(reinterpret_cast<void*>(address), bytes, size);

    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), size, oldProtect, &unused);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), size);
    return true;
}

static bool ApplyFullMapVisionPatch()
{
    if (g_fullMapVisionPatched)
        return true;

    if (!g_fullMapVisionAddr)
    {
        // 特征失配时保持关闭。不要回退到固定 RVA：RVA 只用于分析和日志对照，
        // SC2 更新后不能证明该位置仍是同一条可见性读取指令。
        g_fullMapVisionAddr = PatternScan("SC2_x64.exe", FULL_MAP_VISION_PATTERN);
        if (!g_fullMapVisionAddr)
        {
            Log("[!] FullMapVision: pattern not found\n");
            return false;
        }
        Log("[*] FullMapVision patch addr: 0x%llX\n",
            static_cast<unsigned long long>(g_fullMapVisionAddr));
    }

    if (!g_fullMapVisionOrigSaved)
    {
        if (!SafeMemcpy(g_fullMapVisionOrigBytes,
                        reinterpret_cast<const void*>(g_fullMapVisionAddr),
                        sizeof(g_fullMapVisionOrigBytes)))
        {
            Log("[!] FullMapVision: backup failed\n");
            return false;
        }
        g_fullMapVisionOrigSaved = true;
    }

    if (!WriteCodeBytes(g_fullMapVisionAddr, FULL_MAP_VISION_PATCH,
                        sizeof(FULL_MAP_VISION_PATCH)))
    {
        Log("[!] FullMapVision: patch failed\n");
        return false;
    }

    g_fullMapVisionPatched = true;
    Log("[+] FullMapVision enabled\n");
    return true;
}

static void RestoreFullMapVisionPatch()
{
    if (!g_fullMapVisionPatched || !g_fullMapVisionAddr || !g_fullMapVisionOrigSaved)
        return;

    if (!WriteCodeBytes(g_fullMapVisionAddr, g_fullMapVisionOrigBytes,
                        sizeof(g_fullMapVisionOrigBytes)))
    {
        Log("[!] FullMapVision: restore failed\n");
        return;
    }

    g_fullMapVisionPatched = false;
    Log("[-] FullMapVision disabled\n");
}

static void UpdateFullMapVisionMapState(const char* mapPath, const char* mapDesc)
{
    const int nextMap = FindFullMapVisionMap(mapPath, mapDesc);
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    const LONG previousMap = InterlockedExchange(
        &g_fullMapVisionMapIndex, static_cast<LONG>(nextMap));
    if (previousMap == nextMap)
    {
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return;
    }

    if (nextMap >= 0)
    {
        if (previousMap >= 0)
            Log("[*] FullMapVision map left: %s\n", FULL_MAP_VISION_MAPS[previousMap]);
        Log("[*] FullMapVision map entered: %s\n", FULL_MAP_VISION_MAPS[nextMap]);
        if (InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0) != 0)
            ApplyFullMapVisionPatch();
    }
    else
    {
        if (previousMap >= 0)
            Log("[*] FullMapVision map left: %s\n", FULL_MAP_VISION_MAPS[previousMap]);
        RestoreFullMapVisionPatch();
    }
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
}

bool EnableFullMapVision()
{
    InterlockedExchange(&g_fullMapVisionRequested, 1);

    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    const LONG mapIndex = InterlockedCompareExchange(&g_fullMapVisionMapIndex, 0, 0);
    const bool result = mapIndex < 0 || ApplyFullMapVisionPatch();
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);

    if (mapIndex < 0)
        Log("[*] FullMapVision armed; waiting for a supported co-op map\n");
    return result;
}

void DisableFullMapVision()
{
    InterlockedExchange(&g_fullMapVisionRequested, 0);
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    RestoreFullMapVisionPatch();
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
}

bool IsFullMapVisionEnabled()
{
    return InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0) != 0;
}

bool IsFullMapVisionApplied()
{
    AcquireSRWLockShared(&g_fullMapVisionLock);
    const bool applied = g_fullMapVisionPatched;
    ReleaseSRWLockShared(&g_fullMapVisionLock);
    return applied;
}

// ════════════════════════════════════════════════════════════════
//  经验倍率 Hook
// ════════════════════════════════════════════════════════════════
//
// Base97579 有 5 个同源事件处理入口（实现最多容纳 8 个）。这些入口的第三个
// 参数均为经验 payload，所以每个 detour 只负责选择对应 trampoline，实际修改
// 统一交给 ScaleExperiencePayload。函数签名、参数位置和 payload 偏移都是
// 版本契约；新版不能只看 pattern 命中就认定仍然兼容。

using ExperienceFn = __int64(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static ExperienceFn g_origExperience[8] = {};
static uintptr_t     g_experienceTargets[8] = {};
static size_t        g_experienceHookCount = 0;
static volatile LONG g_experienceEnabled = 0;
static volatile LONG g_experienceMultiplier = 30;

static void ScaleExperiencePayload(uintptr_t payload)
{
    if (!payload || InterlockedCompareExchange(&g_experienceEnabled, 0, 0) == 0)
        return;

    // Base97579 payload：+0x18 为条目数组，+0x20 为条目数。条目数上限是
    // 逆向得到的结构约束，也是防止错误 Hook 命中后遍历任意内存的保护条件。
    uintptr_t entries = 0;
    uint64_t count = 0;
    if (!SafeMemcpy(&entries, reinterpret_cast<const void*>(payload + 0x18), sizeof(entries)) ||
        !SafeMemcpy(&count, reinterpret_cast<const void*>(payload + 0x20), sizeof(count)) ||
        !entries || count == 0 || count > 16)
        return;

    const uint64_t multiplier = static_cast<uint64_t>(
        InterlockedCompareExchange(&g_experienceMultiplier, 0, 0));

    for (uint64_t index = 0; index < count; ++index)
    {
        // 每项 0x80 字节；+0x08 是有效标志，+0x09 是非对齐 uint64 经验值。
        // SC2 升级后若任一偏移变化，必须先更新结构验证，再更新这里。
        const uintptr_t entry = entries + index * 0x80;
        uint8_t active = 0;
        uint64_t value = 0;
        if (!SafeMemcpy(&active, reinterpret_cast<const void*>(entry + 0x08), sizeof(active)) ||
            !active ||
            !SafeMemcpy(&value, reinterpret_cast<const void*>(entry + 0x09), sizeof(value)) ||
            value == 0)
            continue;

        // 在整数域做乘法前判断溢出，保持目标 DLL 的 UINT64_MAX 饱和行为。
        const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
        const uint64_t scaled = value > maximum / multiplier
            ? maximum
            : value * multiplier;
        if (scaled != value)
            SafeMemcpy(reinterpret_cast<void*>(entry + 0x09), &scaled, sizeof(scaled));
    }
}

static __int64 CallExperienceOriginal(size_t index, uintptr_t a1, uintptr_t a2,
                                      uintptr_t payload, uintptr_t a4)
{
    // 目标实现是在原函数消费 payload 前修改数据；改成先调用 original 会让
    // 当前事件仍使用未放大的经验值。
    ScaleExperiencePayload(payload);
    ExperienceFn original = g_origExperience[index];
    return original ? original(a1, a2, payload, a4) : 0;
}

#define DEFINE_EXPERIENCE_DETOUR(index) \
    static __int64 __fastcall ExperienceDetour##index( \
        uintptr_t a1, uintptr_t a2, uintptr_t payload, uintptr_t a4) \
    { \
        return CallExperienceOriginal(index, a1, a2, payload, a4); \
    }

DEFINE_EXPERIENCE_DETOUR(0)
DEFINE_EXPERIENCE_DETOUR(1)
DEFINE_EXPERIENCE_DETOUR(2)
DEFINE_EXPERIENCE_DETOUR(3)
DEFINE_EXPERIENCE_DETOUR(4)
DEFINE_EXPERIENCE_DETOUR(5)
DEFINE_EXPERIENCE_DETOUR(6)
DEFINE_EXPERIENCE_DETOUR(7)

#undef DEFINE_EXPERIENCE_DETOUR

static LPVOID const EXPERIENCE_DETOURS[8] = {
    reinterpret_cast<LPVOID>(&ExperienceDetour0),
    reinterpret_cast<LPVOID>(&ExperienceDetour1),
    reinterpret_cast<LPVOID>(&ExperienceDetour2),
    reinterpret_cast<LPVOID>(&ExperienceDetour3),
    reinterpret_cast<LPVOID>(&ExperienceDetour4),
    reinterpret_cast<LPVOID>(&ExperienceDetour5),
    reinterpret_cast<LPVOID>(&ExperienceDetour6),
    reinterpret_cast<LPVOID>(&ExperienceDetour7),
};

static size_t FindExperienceTargets(uintptr_t* targets, size_t capacity)
{
    HMODULE module = GetModuleBase("SC2_x64.exe");
    if (!module || !targets || capacity == 0)
        return 0;

    auto* base = reinterpret_cast<const uint8_t*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    // Base97579 的共享序言。中间 CALL rel32 的 4 字节目标必然受链接布局影响，
    // 所以扫描器只要求 opcode 为 E8，并从 CALL 后继续匹配稳定的数据流。
    // 这段模板在当前镜像中有 164 个实例，不能直接取前 8 个；偏移 +0x2B 的
    // `41 B0 0E` 把经验事件类型 0x0E 装入 r8b，能精确筛出 5 个经验入口。
    // 0x4480 是当前函数栈帧大小，若编译器重排局部变量，这组特征会安全失配。
    static constexpr uint8_t prefix[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
        0x56, 0x57, 0x41, 0x56, 0xB8, 0x80, 0x44, 0x00, 0x00,
    };
    static constexpr uint8_t suffix[] = {
        0x48, 0x2B, 0xE0, 0x49, 0x8B, 0xE8, 0x0F, 0xB6, 0xFA,
        0x48, 0x8B, 0xF1, 0x48, 0x81, 0xC1, 0x90, 0x01,
    };
    static constexpr uint8_t experienceEventType[] = { 0x41, 0xB0, 0x0E };
    constexpr size_t experienceEventTypeOffset = 0x2B;

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    constexpr size_t requiredSize = experienceEventTypeOffset + sizeof(experienceEventType);
    size_t found = 0;
    for (size_t offset = 0; offset + requiredSize <= imageSize && found < capacity; ++offset)
    {
        const uint8_t* candidate = base + offset;
        if (memcmp(candidate, prefix, sizeof(prefix)) != 0 ||
            candidate[sizeof(prefix)] != 0xE8 ||
            memcmp(candidate + sizeof(prefix) + 5, suffix, sizeof(suffix)) != 0 ||
            memcmp(candidate + experienceEventTypeOffset, experienceEventType,
                   sizeof(experienceEventType)) != 0)
            continue;

        // 同一入口不得重复收集；跳过完整特征长度还能避免在当前函数序言内部
        // 再次匹配。当前已验证版本应得到 5 个目标，数量变化必须查看日志复核。
        targets[found++] = reinterpret_cast<uintptr_t>(candidate);
        offset += requiredSize - 1;
    }
    return found;
}

bool SetupExperienceHooks()
{
    uintptr_t targets[8] = {};
    const size_t targetCount = FindExperienceTargets(targets, _countof(targets));
    if (targetCount == 0)
    {
        Log("[!] Experience: no hook targets found\n");
        return false;
    }

    Log("[*] Experience: found %zu candidate hook target(s)\n", targetCount);

    for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
    {
        const size_t slot = g_experienceHookCount;
        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(targets[targetIndex]),
            EXPERIENCE_DETOURS[slot],
            reinterpret_cast<LPVOID*>(&g_origExperience[slot]));
        if (status != MH_OK)
        {
            Log("[!] Experience hook[%zu] failed at 0x%llX: %s\n",
                targetIndex, static_cast<unsigned long long>(targets[targetIndex]),
                MH_StatusToString(status));
            continue;
        }

        g_experienceTargets[slot] = targets[targetIndex];
        ++g_experienceHookCount;
        Log("[+] Experience hook[%zu] created at 0x%llX\n", slot,
            static_cast<unsigned long long>(targets[targetIndex]));
    }

    return g_experienceHookCount != 0;
}

void EnableExperienceMultiplier(bool on)
{
    if (on && g_experienceHookCount == 0)
    {
        Log("[!] Experience multiplier unavailable: no hooks installed\n");
        return;
    }

    if (on)
    {
        // SetupHooks() 已调用 MH_EnableHook(MH_ALL_HOOKS)，但某些运行环境中实际
        // 只观察到第一个经验入口被改写。这里逐个启用已创建的 Hook，确保所有
        // 同源事件路径都进入 detour；MH_ERROR_ENABLED 表示该入口本来就已启用。
        for (size_t index = 0; index < g_experienceHookCount; ++index)
        {
            MH_STATUS status = MH_EnableHook(
                reinterpret_cast<LPVOID>(g_experienceTargets[index]));
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                Log("[!] Experience hook[%zu] enable failed at 0x%llX: %s\n",
                    index,
                    static_cast<unsigned long long>(g_experienceTargets[index]),
                    MH_StatusToString(status));
            }
        }
    }

    // 关闭时只清原子状态，不在游戏线程可能执行 trampoline 时移除 Hook。
    // 所有 trampoline 统一在 CleanupHooks() 中由 MinHook 安全清理。
    InterlockedExchange(&g_experienceEnabled, on ? 1 : 0);
    Log("[%c] Experience multiplier %s (%ldx)\n", on ? '+' : '-',
        on ? "enabled" : "disabled",
        InterlockedCompareExchange(&g_experienceMultiplier, 0, 0));
}

bool IsExperienceMultiplierEnabled()
{
    return InterlockedCompareExchange(&g_experienceEnabled, 0, 0) != 0;
}

void SetExperienceMultiplier(float multiplier)
{
    LONG value = static_cast<LONG>(multiplier + 0.5f);
    if (value < 1) value = 1;
    if (value > 30) value = 30;
    InterlockedExchange(&g_experienceMultiplier, value);
}

float GetExperienceMultiplier()
{
    return static_cast<float>(InterlockedCompareExchange(&g_experienceMultiplier, 0, 0));
}

void CleanupGameFeatures()
{
    // 必须在 MH_Uninitialize 和 DLL 卸载前执行：先阻止 detour 修改 payload，
    // 再恢复所有直接写入 SC2 代码段的补丁。
    EnableExperienceMultiplier(false);
    DisableFullMapVision();
    DisableMasteryMax();
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
