#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdlib>   // strtol

// 获取指定模块基址（注入后在目标进程内调用）
inline HMODULE GetModuleBase(const char* moduleName)
{
    return GetModuleHandleA(moduleName);
}

// SEH 安全复制（纯 C 函数，无 C++ 对象）
inline bool SafeMemcpy(void* dst, const void* src, size_t size)
{
    __try
    {
        memcpy(dst, src, size);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

// 安全读取内存，失败返回默认值
template<typename T>
inline T ReadMemory(uintptr_t address)
{
    T value{};
    SafeMemcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// 读取 SC2 内部字符串结构体
// strObj 指向字符串对象起始地址，布局:
//   +0x00: size (4 bytes)
//   +0x04: cap_flags (4 bytes)  —— bit1 表示堆分配
//   +0x08: 内联缓冲区 / 堆指针
inline std::string ReadSc2String(uintptr_t strObj)
{
    if (!strObj)
        return {};

    DWORD cap_flags = ReadMemory<DWORD>(strObj + 0x04);
    DWORD len = ReadMemory<DWORD>(strObj + 0x00);

    if (len == 0 || len > 4096)
        return {};

    uintptr_t dataAddr;
    if (cap_flags & 2)
        dataAddr = ReadMemory<uintptr_t>(strObj + 0x08);  // 堆分配
    else
        dataAddr = strObj + 0x08;                          // 内联

    if (!dataAddr)
        return {};

    std::string result(len, '\0');
    if (!SafeMemcpy(result.data(), reinterpret_cast<const void*>(dataAddr), len))
        return {};

    return result;
}

// ─── 特征码扫描（IDA 风格，支持 ?? 通配符） ───
// 用法: PatternScan("SC2_x64.exe", "66 89 97 68 01 00 00 66 ?? 8F 6A 01")
// 返回匹配的第一个地址，失败返回 0
inline uintptr_t PatternScan(const char* moduleName, const char* pattern)
{
    HMODULE hMod = GetModuleHandleA(moduleName);
    if (!hMod) return 0;

    // 通过 PE 头获取模块大小，无需 PSAPI
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(hMod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;

    // 解析 pattern："66 89 ?? 68 01 00 00"
    struct PatByte { uint8_t val; bool wild; };
    PatByte pat[256];
    int patLen = 0;
    const char* p = pattern;
    while (*p && patLen < 256)
    {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?')
        {
            pat[patLen++] = {0, true};
            ++p;
            if (*p == '?') ++p;
        }
        else
        {
            pat[patLen++] = {static_cast<uint8_t>(strtol(p, const_cast<char**>(&p), 16)), false};
        }
    }
    if (patLen == 0) return 0;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(hMod);
    for (size_t i = 0; i + patLen <= imageSize; ++i)
    {
        bool ok = true;
        for (int j = 0; j < patLen; ++j)
        {
            if (!pat[j].wild && base[i + j] != pat[j].val) { ok = false; break; }
        }
        if (ok) return reinterpret_cast<uintptr_t>(base + i);
    }
    return 0;
}
