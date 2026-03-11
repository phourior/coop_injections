#include "pch.h"
#include "core/game_data.h"
#include "core/memory.h"
#include "core/log.h"

// ─── 泽拉图神器坐标 ───
// SC2_x64.exe + 0x0447F388 -> +0x0 -> +0x0 -> X(+0x68) / Y(+0x6C)

ArtifactCoords ReadArtifactCoords()
{
    ArtifactCoords result{ 0.f, 0.f, false };

    HMODULE hModule = GetModuleBase("SC2_x64.exe");
    if (!hModule)
        return result;

    uintptr_t base = reinterpret_cast<uintptr_t>(hModule);

    UINT64 addr = ReadMemory<UINT64>(base + 0x0447F388);
    if (!addr)
        return result;

    addr = ReadMemory<UINT64>(addr + 0x0);
    if (!addr)
        return result;

    addr = ReadMemory<UINT64>(addr + 0x0);
    if (!addr)
        return result;

    result.x = ReadMemory<float>(addr + 0x68);
    result.y = ReadMemory<float>(addr + 0x6C) + 29.0f;
    result.valid = true;

    return result;
}

// ─── 大厅/地图信息（事件链） ───
// SC2_x64.exe + 0x5E97018 → CBattleNet*
//   +0x5A8 → 事件链节点
//     +0x08 & ~1 → GameLobby*
//       +0x08 → 地图字符串1 (路径)
//       +0x18 → 地图字符串2 (显示名)

LobbyInfo ReadLobbyInfo()
{
    LobbyInfo info{};

    HMODULE hModule = GetModuleBase("SC2_x64.exe");
    if (!hModule)
        return info;

    uintptr_t base = reinterpret_cast<uintptr_t>(hModule);

    // CBattleNet*
    info.pBattleNet = ReadMemory<uintptr_t>(base + 0x5E97018);
    if (!info.pBattleNet)
        return info;

    // 事件链头节点
    info.pEvtNode = ReadMemory<uintptr_t>(info.pBattleNet + 0x5A8);
    if (!info.pEvtNode)
        return info;

    // GameLobby* (去掉最低位标志位)
    uintptr_t raw = ReadMemory<uintptr_t>(info.pEvtNode + 0x08);
    info.pLobby = raw & ~1ULL;
    if (!info.pLobby)
        return info;

    // 地图字符串1 (+0x08 处的字符串对象)
    info.mapPath = ReadSc2String(info.pLobby + 0x08);

    // 地图字符串2 (+0x18 处的字符串对象)
    info.mapName = ReadSc2String(info.pLobby + 0x18);

    info.valid = true;
    return info;
}
