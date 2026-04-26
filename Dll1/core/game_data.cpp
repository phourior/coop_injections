#include "pch.h"
#include "core/game_data.h"
#include "core/memory.h"
#include "core/log.h"

#include <algorithm>
#include <cstdlib>

// ─── 泽拉图神器坐标：特征码扫描定位 CameraManager 全局指针 ───
//
// 不再硬编码偏移，流程：
//   1. 在 SC2_x64.exe 镜像内找 "CameraClearChannel\0" 字节串
//   2. 找 LEA reg,[rip+disp32] 引用该字符串的指令
//   3. 在 LEA 之前 0x200 字节内找 CALL rel32，被调函数形如
//        48 8B ?5 xx xx xx xx C3   ; MOV r64,[RIP+disp32] ; RET
//   4. 从 MOV 指令中提取 RIP-relative 偏移，得到 qword 全局地址
//
// 指针链（版本无关）:  *global -> +0x0 -> +0x0 -> X@+0x68, Y@+0x6C

static uintptr_t ScanCameraManagerGlobal()
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA("SC2_x64.exe"));
    if (!base) return 0;

    auto dos    = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt     = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    size_t imgSz = nt->OptionalHeader.SizeOfImage;

    // 1. 找 "CameraClearChannel\0"
    static const char kStr[] = "CameraClearChannel";
    constexpr size_t kStrLen = sizeof(kStr); // 含 '\0'
    uintptr_t strAddr = 0;
    for (size_t i = 0; i + kStrLen <= imgSz; ++i)
    {
        if (memcmp(base + i, kStr, kStrLen) == 0)
        {
            strAddr = reinterpret_cast<uintptr_t>(base + i);
            break;
        }
    }
    if (!strAddr) return 0;

    // 2. 找 LEA reg,[rip+disp32] 指向该字符串
    //    编码: 48 8D [ModRM: mod=00, rm=101] [disp32]
    for (size_t i = 7; i + 7 <= imgSz; ++i)
    {
        if (base[i] != 0x48 || base[i + 1] != 0x8D) continue;
        if ((base[i + 2] & 0x07) != 0x05) continue; // rm=101 => RIP-relative

        int32_t disp = *reinterpret_cast<const int32_t*>(base + i + 3);
        if (reinterpret_cast<uintptr_t>(base + i + 7) + disp != strAddr) continue;

        // 3. 在 LEA 前 0x200 字节内找 CALL rel32 (E8)
        //    验证被调函数是 getter: MOV r64,[RIP+disp32]; RET (8字节)
        size_t lo = (i > 0x200) ? (i - 0x200) : 0;
        for (size_t j = lo; j < i; ++j)
        {
            if (base[j] != 0xE8) continue;

            int32_t rel = *reinterpret_cast<const int32_t*>(base + j + 1);
            auto callee = base + j + 5 + rel;
            if (callee < base || callee + 8 > base + imgSz) continue;

            // 48 8B [ModRM: mod=00,rm=101] [disp32] C3
            if (callee[0] == 0x48 && callee[1] == 0x8B &&
                (callee[2] & 0xC7) == 0x05 && callee[7] == 0xC3)
            {
                int32_t ripDisp = *reinterpret_cast<const int32_t*>(callee + 3);
                return reinterpret_cast<uintptr_t>(callee + 7) + ripDisp;
            }
        }
    }
    return 0;
}

ArtifactCoords ReadArtifactCoords()
{
    ArtifactCoords result{ 0.f, 0.f, false };

    // 首次调用时扫描，结果缓存（C++11 静态局部变量线程安全初始化）
    static uintptr_t sCamGlobal = ScanCameraManagerGlobal();
    if (!sCamGlobal)
        return result;

    UINT64 addr = ReadMemory<UINT64>(sCamGlobal);
    if (!addr) return result;

    addr = ReadMemory<UINT64>(addr);
    if (!addr) return result;

    addr = ReadMemory<UINT64>(addr);
    if (!addr) return result;

    result.x = ReadMemory<float>(addr + 0x68);
    result.y = ReadMemory<float>(addr + 0x6C) + 29.0f;
    result.valid = true;
    return result;
}

// Runtime camera/map bounds.
//
// Base96921 IDA notes:
//   CameraSetBounds resolves the clamp rectangle through sub_14055CB00.
//   The getter returns an int32 rect array: minX, minY, maxX, maxY.
//   Passing 16 selects the global/current map camera clamp used by the native.
static uintptr_t ScanCameraBoundsGetter()
{
    static constexpr const char* kPattern =
        "8B 05 ?? ?? ?? ?? 2B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? "
        "05 E3 6F 3E 6B 03 15 ?? ?? ?? ?? 89 44 24 14 0F B6 C1 "
        "48 83 C0 1F 89 54 24 10 48 C1 E0 04 48 03 44 24 10 C3";

    return PatternScan("SC2_x64.exe", kPattern);
}

static bool BuildBoundsFromRect(uintptr_t rect, MapBounds& out)
{
    int32_t raw[4] = {};
    if (!rect || !SafeMemcpy(raw, reinterpret_cast<const void*>(rect), sizeof(raw)))
        return false;

    int32_t maxAbs = 0;
    for (int v : raw)
        maxAbs = (std::max)(maxAbs, abs(v));

    const float scale = (maxAbs > 4096) ? (1.0f / 4096.0f) : 1.0f;
    const float left   = raw[0] * scale;
    const float top    = raw[1] * scale;
    const float right  = raw[2] * scale;
    const float bottom = raw[3] * scale;
    const float width  = right - left;
    const float height = bottom - top;

    if (width < 32.0f || width > 512.0f || height < 32.0f || height > 512.0f)
        return false;
    if (left < -64.0f || top < -64.0f || right > 640.0f || bottom > 640.0f)
        return false;

    out.left = left;
    out.top = top;
    out.right = right;
    out.bottom = bottom;
    out.width = width;
    out.height = height;
    out.valid = true;
    return true;
}

using BoundsGetterFn = uintptr_t(__fastcall*)(uint8_t index);

static uintptr_t CallBoundsGetter(uintptr_t getterAddr, uint8_t index)
{
    __try
    {
        return reinterpret_cast<BoundsGetterFn>(getterAddr)(index);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

MapBounds ReadCurrentMapBounds()
{
    MapBounds result{};

    static uintptr_t sGetter = ScanCameraBoundsGetter();
    if (!sGetter)
        return result;

    auto tryIndex = [&](uint8_t index) -> bool
    {
        uintptr_t rect = CallBoundsGetter(sGetter, index);
        return BuildBoundsFromRect(rect, result);
    };

    if (tryIndex(16))
        return result;

    for (uint8_t i = 0; i < 16; ++i)
    {
        if (tryIndex(i))
            return result;
    }

    return MapBounds{};
}

// ─── 大厅/地图信息：特征码扫描定位 CBattleNet 全局指针 ───
//
// 不再硬编码偏移，流程：
//   1. 在 SC2_x64.exe 镜像内找 "CBattleNet::Initialize()\0" 字节串
//   2. 找 LEA R8,[rip+disp32] 引用该字符串的指令 (4C 8D 05 ...)
//   3. 在 LEA 前 0x100 字节内找最后一条 MOV [RIP+disp32],RSI (48 89 35 ...)
//   4. 从该 MOV 指令提取 RIP-relative 偏移，得到 qword 全局地址
//
// 指针链（版本无关）: *global → CBattleNet*
//   +0x5A8 → 事件链节点 → +0x08 & ~1 → GameLobby*
//     +0x08 → mapPath 字符串对象
//     +0x18 → mapName 字符串对象

static uintptr_t ScanCBattleNetGlobal()
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA("SC2_x64.exe"));
    if (!base) return 0;

    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    size_t imgSz = nt->OptionalHeader.SizeOfImage;

    // 1. 找 "CBattleNet::Initialize()\0"
    static const char kStr[] = "CBattleNet::Initialize()";
    constexpr size_t kStrLen = sizeof(kStr); // 含 '\0'
    uintptr_t strAddr = 0;
    for (size_t i = 0; i + kStrLen <= imgSz; ++i)
    {
        if (memcmp(base + i, kStr, kStrLen) == 0)
        {
            strAddr = reinterpret_cast<uintptr_t>(base + i);
            break;
        }
    }
    if (!strAddr) return 0;

    // 2. 找 LEA R8,[rip+disp32] 指向该字符串  (4C 8D 05 [disp32])
    for (size_t i = 7; i + 7 <= imgSz; ++i)
    {
        if (base[i] != 0x4C || base[i + 1] != 0x8D || base[i + 2] != 0x05) continue;
        int32_t disp = *reinterpret_cast<const int32_t*>(base + i + 3);
        if (reinterpret_cast<uintptr_t>(base + i + 7) + disp != strAddr) continue;

        // 3. 在 LEA 前 0x100 字节内找最后一条 MOV [RIP+disp32],RSI (48 89 35 ...)
        size_t lo = (i > 0x100) ? (i - 0x100) : 0;
        size_t movIdx = SIZE_MAX;
        for (size_t j = lo; j + 7 <= i; ++j)
        {
            if (base[j] == 0x48 && base[j + 1] == 0x89 && base[j + 2] == 0x35)
                movIdx = j;
        }
        if (movIdx == SIZE_MAX) continue;

        // 4. 提取 RIP-relative 偏移
        int32_t ripDisp = *reinterpret_cast<const int32_t*>(base + movIdx + 3);
        return reinterpret_cast<uintptr_t>(base + movIdx + 7) + ripDisp;
    }
    return 0;
}

LobbyInfo ReadLobbyInfo()
{
    LobbyInfo info{};

    // 首次调用时扫描，结果缓存（C++11 静态局部变量线程安全初始化）
    static uintptr_t sCBattleNetGlobal = ScanCBattleNetGlobal();
    if (!sCBattleNetGlobal)
        return info;

    // CBattleNet*
    info.pBattleNet = ReadMemory<uintptr_t>(sCBattleNetGlobal);
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
