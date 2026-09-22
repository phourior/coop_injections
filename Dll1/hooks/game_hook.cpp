#include "pch.h"
#include "hooks/game_hook.h"
#include "hooks/game_hook_internal.h"
#include "core/game_data.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <intrin.h>
#include <tlhelp32.h>
#include <vector>

// ─── 全局原始数据（纯 C，零初始化） ───
RawLobbyData g_rawLobby = {};
static SRWLOCK g_rawLobbyLock = SRWLOCK_INIT;

// ─── 原函数类型 ───
using OnGameLobbyUpdateFn = __int64(__fastcall*)(__int64 a1, __int64 a2);
static OnGameLobbyUpdateFn g_origOnGameLobbyUpdate = nullptr;

// 反向扫描状态：0=尚未扫描，1=某个 Hook 线程正在扫描，2=扫描完成。
// 使用 Interlocked API 防止大厅更新由多个线程同时触发时重复遍历内存。
static volatile LONG g_lobbyReverseScanState = 0;
static volatile LONG g_lobbyLogPathReported = 0;
static SRWLOCK g_lobbyLogOpenLock = SRWLOCK_INIT;
static bool g_lobbyLogInitialized = false;
// 反向扫描动态确认的 CBattleNet 当前大厅字段地址。渲染线程只读取该字段，
// 不把某次客户端版本中观察到的 +0x60 固化为长期偏移。
static volatile LONG64 g_currentLobbyFieldAddress = 0;
static volatile LONG64 g_lastLobbyPublishTick = 0;

static HANDLE OpenLobbyScanLog(wchar_t* actualPath, size_t actualPathCount)
{
    AcquireSRWLockExclusive(&g_lobbyLogOpenLock);

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

    // 每次 DLL 加载后的首次成功打开使用 CREATE_ALWAYS 清空旧日志；后续写入继续追加。
    // 独占锁防止多个大厅 Hook 线程同时首写时重复截断文件。
    const DWORD creationDisposition = g_lobbyLogInitialized ? OPEN_ALWAYS : CREATE_ALWAYS;
    HANDLE file = path[0]
        ? CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      creationDisposition, FILE_ATTRIBUTE_NORMAL, nullptr)
        : INVALID_HANDLE_VALUE;

    // DLL 所在目录不可写时回退到当前用户的临时目录。
    if (file == INVALID_HANDLE_VALUE)
    {
        DWORD length = GetTempPathW(MAX_PATH, path);
        static constexpr wchar_t kFallbackName[] = L"Dll1_lobby_offsets.log";
        if (!length || length + _countof(kFallbackName) > MAX_PATH)
        {
            ReleaseSRWLockExclusive(&g_lobbyLogOpenLock);
            return INVALID_HANDLE_VALUE;
        }
        wcscpy_s(path + length, MAX_PATH - length, kFallbackName);
        file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           creationDisposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    if (file != INVALID_HANDLE_VALUE)
    {
        g_lobbyLogInitialized = true;
        if (actualPath && actualPathCount)
            wcscpy_s(actualPath, actualPathCount, path);
    }

    ReleaseSRWLockExclusive(&g_lobbyLogOpenLock);
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
            LobbyScanLog("[LOBBY-SCAN] INDIRECT member=CBattleNet+0x%zX -> node=0x%llX, "
                "GameLobby at node+0x%zX raw=0x%llX\r\n",
                fieldOffset,
                static_cast<unsigned long long>(node),
                valueOffset,
                static_cast<unsigned long long>(raw));
        }
    }

    LobbyScanLog("[LOBBY-SCAN] complete: direct=%d indirect=%d\r\n", directHits, indirectHits);

    // 唯一的直接命中足以标识 CBattleNet 中保存当前 GameLobby* 的字段；间接
    // 命中可能只是其他成员指向同一对象内部，不能用于否定这个直接字段。
    if (directHits == 1 && directFieldCandidate != SIZE_MAX)
    {
        const uintptr_t fieldAddress = battleNet + directFieldCandidate;
        InterlockedExchange64(&g_currentLobbyFieldAddress,
            static_cast<LONG64>(fieldAddress));
        LobbyScanLog("[LOBBY-SCAN] current lobby field=0x%llX\r\n",
            static_cast<unsigned long long>(fieldAddress));
    }

    if (directHits != 1)
        LobbyScanLog("[LOBBY-SCAN] current lobby field unresolved; lifecycle polling disabled\r\n");

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

        // 审查修复 #8：Hook 写入与 UI 快照通过同一 SRW 锁同步，保证一次
        // 快照中的地图、参数和玩家字段来自同一次发布。
        AcquireSRWLockExclusive(&g_rawLobbyLock);
        memcpy(g_rawLobby.mapPath, mapPath, sizeof(mapPath));
        memcpy(g_rawLobby.mapDesc, mapDesc, sizeof(mapDesc));
        g_rawLobby.gameParam1  = gp1;
        g_rawLobby.gameParam2  = gp2;
        g_rawLobby.flags       = lobbyFlags;
        memcpy(g_rawLobby.playerIds, playerIds, sizeof(playerIds));
        g_rawLobby.pGameLobby  = a1;

        // 序号最后写（内存屏障），让读取端知道数据已就绪
        InterlockedIncrement(&g_rawLobby.seq);
        InterlockedExchange64(&g_lastLobbyPublishTick,
            static_cast<LONG64>(GetTickCount64()));
        const LONG publishedSeq = g_rawLobby.seq;
        ReleaseSRWLockExclusive(&g_rawLobbyLock);

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
            publishedSeq, mapPath, gp1, gp2, lobbyFlags, activePlayers);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[!] SafeCollectData exception 0x%08X\n", GetExceptionCode());
    }
}

// ─── Hooked OnGameLobbyUpdate ───
static __int64 __fastcall HookedOnGameLobbyUpdate(__int64 a1, __int64 a2)
{
    EnterHookCallback();
    // ① 立即调用原函数——零干扰
    __int64 ret = g_origOnGameLobbyUpdate(a1, a2);

    // ② 原函数完成后，以它传入的 GameLobby* 做一次性诊断扫描。
    // ③ 继续执行原有的数据收集；扫描不修改 a1 指向的对象和 g_rawLobby。
    if (!IsDllUnloading() && a1)
    {
        ReverseScanLobbyPointer(static_cast<uintptr_t>(a1));
        SafeCollectData(static_cast<uintptr_t>(a1));
    }

    LeaveHookCallback();
    return ret;
}

// ─── UI 线程调用：从纯 C 数据生成 C++ 快照 ───
LobbyData SnapshotLobbyData()
{
    LobbyData out;
    RawLobbyData snapshot{};
    AcquireSRWLockShared(&g_rawLobbyLock);
    memcpy(&snapshot, &g_rawLobby, sizeof(snapshot));
    ReleaseSRWLockShared(&g_rawLobbyLock);

    if (snapshot.seq == 0 || snapshot.pGameLobby == 0)
        return out;

    out.mapPath        = snapshot.mapPath;
    out.mapDisplayName = snapshot.mapDesc;
    out.gameParam1     = snapshot.gameParam1;
    out.gameParam2     = snapshot.gameParam2;
    out.flags          = snapshot.flags;

    out.playerCount = 0;
    for (int i = 0; i < 16; ++i)
    {
        out.playerIds[i] = snapshot.playerIds[i];
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
    // 在大厅 Hook 启用前立即触发本次 DLL 生命周期的首次打开，确保即使没有进入
    // 大厅，启动时也会清空上次运行留下的 lobby_offsets.log。
    HANDLE lobbyLog = OpenLobbyScanLog(nullptr, 0);
    if (lobbyLog != INVALID_HANDLE_VALUE)
        CloseHandle(lobbyLog);

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

bool CleanupGameFeatures()
{
    // 必须在 MH_Uninitialize 和 DLL 卸载前执行：先阻止 detour 修改 payload，
    // 再恢复所有直接写入 SC2 代码段的补丁。
    EnableExperienceMultiplier(false);
    DisableFullMapVision();
    DisableMasteryMax();
    const bool leaderRestored = CleanupLeaderPanelFeature();
    return !HasFullMapVisionError() && !IsMasteryMaxEnabled() && leaderRestored;
}
