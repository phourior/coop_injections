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

// 从纯 C 结构拷贝到 C++ 结构（UI 线程调用）
LobbyData SnapshotLobbyData();

// Hook sub_1420BFBE0 = CBattleNet::OnGameLobbyUpdate
bool SetupGameHooks();

// ─── NNet UDP 包捕获 ───
// 钩住 sub_142179870 (TX) 和 sub_1401DBE20 (RX) 以及 sub_1413A6FE0 (事件对象层)
// 捕获的包写入环形缓冲区，可在 ImGui 菜单中显示

static constexpr int NNET_RING_SIZE = 64;   // 环形缓冲区容量

struct NNetPacket
{
    bool   isTx;          // true=发送，false=接收
    int    len;           // 数据长度
    BYTE   data[64];      // 前 64 字节原始数据
    char   addrStr[32];   // "ip:port" 字符串
    DWORD  tickMs;        // GetTickCount() 时间戳
};

extern volatile LONG  g_nnetHead;            // 环形缓冲区写入头（原子）
extern NNetPacket     g_nnetRing[NNET_RING_SIZE];
extern volatile LONG  g_nnetCapture;         // 非零 = 捕获中

// 安装 NNet hook（在 SetupGameHooks 之后调用）
// 仅 MH_CreateHook，不立即启用——调用方需在 MH_EnableHook 之后再 DisableNNetHooks
// 默认启动时禁用，避免热路径上每帧都被命中导致游戏卡顿
bool SetupNNetHooks();

// 启动时禁用 NNet hook（在 MH_EnableHook(MH_ALL_HOOKS) 之后调用）
void DisableNNetHooksAtStartup();

// 运行时开关：实际启用/禁用 MinHook 的 trampoline，零开销切换
void EnableNNetCapture(bool on);
inline bool IsNNetCaptureEnabled()      { return g_nnetCapture != 0; }

// ─── 180 精通补丁（特征码定位，动态开关） ───
// EnableMasteryMax: 扫描特征码并写入 shellcode，强制精通值 = 0x7FFF
// DisableMasteryMax: 恢复原始字节
// IsMasteryMaxEnabled: 查询当前状态
bool EnableMasteryMax();
void DisableMasteryMax();
bool IsMasteryMaxEnabled();
