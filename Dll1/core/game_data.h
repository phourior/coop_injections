#pragma once
#include <string>

struct ArtifactCoords
{
    float x;
    float y;
    bool  valid;    // 读取链路是否全部成功
};

struct LobbyInfo
{
    uintptr_t    pBattleNet;   // CBattleNet*
    uintptr_t    pEvtNode;     // 事件链节点
    uintptr_t    pLobby;       // GameLobby*
    std::string  mapPath;      // 地图路径/handle
    std::string  mapName;      // 地图显示名
    bool         valid;
};

// 读取泽拉图神器坐标
ArtifactCoords ReadArtifactCoords();

// 读取大厅/地图信息（通过事件链）
LobbyInfo ReadLobbyInfo();
