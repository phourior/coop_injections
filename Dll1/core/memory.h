#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

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
