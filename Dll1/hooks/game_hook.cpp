#include "pch.h"
#include "hooks/game_hook.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include <MinHook.h>
#include <cstring>

// ─── 原函数类型 ───
// sub_1420BFBE0(__int64 a1, __int64 a2)
// a1 = GameLobby*, a2 = NetworkPacket wrapper
using OnGameLobbyUpdateFn = __int64(__fastcall*)(__int64 a1, __int64 a2);
static OnGameLobbyUpdateFn g_origOnGameLobbyUpdate = nullptr;

// ─── 安全读取 C 字符串（SEH 保护） ───
static std::string SafeReadCString(uintptr_t addr, size_t maxLen = 512)
{
    if (!addr)
        return {};

    char buf[512]{};
    size_t cap = (maxLen < sizeof(buf)) ? maxLen : sizeof(buf) - 1;
    if (!SafeMemcpy(buf, reinterpret_cast<const void*>(addr), cap))
        return {};

    buf[cap] = '\0';
    return std::string(buf);
}

// ─── Hooked OnGameLobbyUpdate ───
static __int64 __fastcall HookedOnGameLobbyUpdate(__int64 a1, __int64 a2)
{
    // 先调用原函数，让它把数据写入 GameLobby (a1)
    __int64 ret = g_origOnGameLobbyUpdate(a1, a2);

    // 从网络包提取数据
    uintptr_t pkt = ReadMemory<uintptr_t>(static_cast<uintptr_t>(a2 + 24));
    if (pkt)
    {
        LobbyData data{};
        data.pGameLobby = static_cast<uintptr_t>(a1);
        data.pPacket    = pkt;

        // 房间名: pkt+16 是 null-terminated char*
        data.roomName = SafeReadCString(pkt + 16);

        // 游戏参数
        data.gameParam1 = ReadMemory<DWORD>(pkt + 300);
        data.gameParam2 = ReadMemory<DWORD>(pkt + 316);

        // 布尔标志
        data.flag1 = ReadMemory<BYTE>(pkt + 368) != 0;
        data.flag2 = ReadMemory<BYTE>(pkt + 369) != 0;

        // 玩家数量
        UINT64 playerCount = ReadMemory<UINT64>(pkt + 376);
        data.playerCount = (playerCount <= 16) ? static_cast<DWORD>(playerCount) : 0;

        // 玩家数据 (每项5字节: 1字节存在标志 + 4字节ID)
        for (DWORD i = 0; i < data.playerCount && i < 16; ++i)
        {
            uintptr_t entry = pkt + 384 + i * 5;
            data.playerPresent[i] = ReadMemory<BYTE>(entry) != 0;
            data.playerIds[i]     = data.playerPresent[i] ? ReadMemory<DWORD>(entry + 1) : 0;
        }

        // 字符串数组 (地图名列表)
        UINT64 strCount = ReadMemory<UINT64>(pkt + 472);
        uintptr_t strArr = ReadMemory<uintptr_t>(pkt + 464);
        data.mapStringCount = (strCount <= 32) ? static_cast<DWORD>(strCount) : 0;
        if (strArr)
        {
            for (DWORD i = 0; i < data.mapStringCount && i < 8; ++i)
            {
                // 每个字符串条目224字节，BSN::String<50> 结构
                uintptr_t strEntry = strArr + i * 224;
                data.mapStrings[i] = SafeReadCString(strEntry, 200);
            }
        }

        // 原函数已经把 sub_1420BC740 处理后的数据写入 a1+0x08 和 a1+0x18
        // 直接从 GameLobby 读取处理后的地图字符串
        data.mapPath = ReadSc2String(static_cast<uintptr_t>(a1 + 0x08));
        data.mapDisplayName = ReadSc2String(static_cast<uintptr_t>(a1 + 0x18));

        // 原子更新全局数据
        {
            std::lock_guard<std::mutex> lock(g_lobbyMutex);
            g_lobbyData = std::move(data);
            g_lobbyData.valid = true;
            ++g_lobbyUpdateCount;
        }

        Log("[+] LobbyUpdate #%u: room='%s' map='%s' display='%s' players=%u param1=%u param2=%u\n",
            g_lobbyUpdateCount,
            g_lobbyData.roomName.c_str(),
            g_lobbyData.mapPath.c_str(),
            g_lobbyData.mapDisplayName.c_str(),
            g_lobbyData.playerCount,
            g_lobbyData.gameParam1,
            g_lobbyData.gameParam2);
    }

    return ret;
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

    // sub_1420BFBE0 → RVA = 0x20BFBE0
    uintptr_t targetAddr = base + 0x20BFBE0;

    Log("[*] Hooking OnGameLobbyUpdate at 0x%llX\n",
        static_cast<unsigned long long>(targetAddr));

    MH_STATUS st = MH_CreateHook(
        reinterpret_cast<LPVOID>(targetAddr),
        &HookedOnGameLobbyUpdate,
        reinterpret_cast<LPVOID*>(&g_origOnGameLobbyUpdate));

    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook OnGameLobbyUpdate failed: %s\n", MH_StatusToString(st));
        return false;
    }

    Log("[+] OnGameLobbyUpdate hook created OK\n");
    return true;
}
