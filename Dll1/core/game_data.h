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

enum class MapBoundsStatus
{
    PatternNotFound,
    GetterReturnedNull,
    RectReadFailed,
    RectRejected,
    Valid,
};

struct MapBounds
{
    float left;
    float top;
    float right;
    float bottom;
    float width;
    float height;
    bool  valid;
    MapBoundsStatus status;
    uintptr_t getter;
    uintptr_t rect;
    int32_t raw[4];
    uint8_t index;
};

// 读取泽拉图神器坐标
ArtifactCoords ReadArtifactCoords();

MapBounds ReadCurrentMapBounds();

// Resolve and cache module scan results on the DLL worker thread before rendering.
void WarmUpGameDataScans();

// 定位保存 CBattleNet* 的全局槽地址；返回值需要再解引用一次才能得到对象地址。
uintptr_t ScanCBattleNetGlobal();

// 读取大厅/地图信息（通过事件链）
LobbyInfo ReadLobbyInfo();
