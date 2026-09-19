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
    ULONGLONG tickMs;     // GetTickCount64() 时间戳
};

extern volatile LONG  g_nnetHead;            // 环形缓冲区写入头（原子）
extern NNetPacket     g_nnetRing[NNET_RING_SIZE];
extern volatile LONG  g_nnetCapture;         // 非零 = 捕获中

// 安装 NNet hook（在 SetupGameHooks 之后调用）。当前缺少可验证的目标函数
// 特征码，因此安全地返回 false，避免用版本相关固定 RVA 安装 Hook。
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

// ─── 全图视野补丁（特征码定位，动态开关） ───
// 开启后进入 15 张支持的合作地图时自动应用补丁，离开时自动恢复；地图路径
// 和显示标识都会参与子串匹配。找不到特征时不使用固定 RVA 回退。
bool EnableFullMapVision();
void DisableFullMapVision();
bool IsFullMapVisionEnabled();
bool IsFullMapVisionApplied();
bool SetFullMapVisionEnhanced(bool enhanced);
bool IsFullMapVisionEnhanced();
bool HasFullMapVisionError();

// ─── 经验倍率（MinHook，1-30 倍） ───
// SetupExperienceHooks 必须在 MH_Initialize 之后、MH_EnableHook(MH_ALL_HOOKS)
// 之前调用；开启时会确保所有已创建入口均启用，关闭时只改变原子状态。
bool SetupExperienceHooks();
void EnableExperienceMultiplier(bool on);
bool IsExperienceMultiplierEnabled();
void SetExperienceMultiplier(float multiplier);
float GetExperienceMultiplier();

// 卸载 DLL 前恢复所有直接代码补丁和功能状态。
bool CleanupGameFeatures();
