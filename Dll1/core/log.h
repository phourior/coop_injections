#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>

extern HMODULE g_hModule;

inline void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);

    const bool lifecycleMessage = strstr(buf, "MainThread") ||
        strstr(buf, "hooks installed") || strstr(buf, "cleanup done");
    if ((buf[0] == '[' && buf[1] == '!') || lifecycleMessage)
    {
        char path[MAX_PATH]{};
        if (g_hModule && GetModuleFileNameA(g_hModule, path, MAX_PATH))
        {
            char* fileName = strrchr(path, '\\');
            if (fileName)
            {
                strcpy_s(fileName + 1, MAX_PATH - static_cast<size_t>(fileName + 1 - path),
                    "dll1_runtime.log");
                HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;
                    WriteFile(file, buf, static_cast<DWORD>(strlen(buf)), &written, nullptr);
                    CloseHandle(file);
                }
            }
        }
    }
}
