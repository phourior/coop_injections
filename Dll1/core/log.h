#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>

extern HMODULE g_hModule;
inline SRWLOCK g_logLock = SRWLOCK_INIT;

inline void Log(const char* fmt, ...)
{
    char message[1024]{};
    va_list args;
    va_start(args, fmt);
    const int formatResult = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (formatResult < 0)
        strcpy_s(message, "[!] Log message formatting failed\n");

    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[1280];
    snprintf(line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] %s",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId(), message);
    AcquireSRWLockExclusive(&g_logLock);
    OutputDebugStringA(line);

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
                WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
                CloseHandle(file);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
}
