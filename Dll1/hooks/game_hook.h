#pragma once
#include <string>
#include <mutex>
#include <cstdint>
#include <windows.h>

// ─── 纯 C 的原始大厅数据（hook 回调中写入，无 C++ 对象） ───
struct RawLobbyData
{
    volatile LONG  seq;             // 写入序号（原子递增）
    char           mapPath[512];    // a1+0x08
    char           mapDesc[512];    // a1+0x18
    DWORD          gameParam1;      // a1+0x48
    DWORD          gameParam2;      // a1+0x4C
    DWORD          flags;           // a1+0x1EC8
    DWORD          playerIds[16];   // a1+0x1C10
    uintptr_t      pGameLobby;     // a1
};

// ─── 供 UI 使用的 C++ 大厅数据 ───
struct LobbyData
{
    bool         valid = false;
    std::string  mapPath;
    std::string  mapDisplayName;
    DWORD        gameParam1 = 0;
    DWORD        gameParam2 = 0;
    DWORD        flags = 0;
    DWORD        playerCount = 0;
    DWORD        playerIds[16]{};
};

// 全局原始数据（hook 写，纯 C）
extern RawLobbyData g_rawLobby;

// 从 g_rawLobby 拷贝到 C++ 结构（UI 线程调用）
LobbyData SnapshotLobbyData();

// Hook sub_1420BFBE0 = CBattleNet::OnGameLobbyUpdate
bool SetupGameHooks();
