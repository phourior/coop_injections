#pragma once

struct ArtifactCoords
{
    float x;
    float y;
    bool  valid;    // 读取链路是否全部成功
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
