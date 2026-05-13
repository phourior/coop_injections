// ConsoleApplication1.cpp : SC2 DLL injector / monitor
// Loads dll1.dll from the same directory as this exe into SC2_x64.exe.
// Must be run as Administrator.

#include <iostream>
#include <string>
#include <unordered_set>
#include <ctime>

#include <Windows.h>
#include <TlHelp32.h>

#pragma comment(lib, "kernel32.lib")

// --- Config --------------------------------------------------------------------
static constexpr DWORD       kPollIntervalMs = 2000;
static constexpr const wchar_t* kTargetExe   = L"SC2_x64.exe";
static constexpr const wchar_t* kDllName     = L"dll1.dll";

// Global exit flag (set by Ctrl+C handler)
static volatile bool g_running = true;

// --- Ctrl+C handler -----------------------------------------------------------
static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT ||
        dwCtrlType == CTRL_CLOSE_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// --- Timestamped log -----------------------------------------------------------
static void Log(const char* msg)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
}

static void Logf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(buf);
}

// --- Get directory of this exe -------------------------------------------------
static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ws(path);
    size_t pos = ws.rfind(L'\\');
    if (pos != std::wstring::npos)
        ws.resize(pos);
    return ws;
}

// --- Find process PID ----------------------------------------------------------
static DWORD FindProcess(const wchar_t* exeName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    DWORD found = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return found;
}

// --- DLL injection via remote LoadLibraryW thread ------------------------------
static bool InjectDll(DWORD pid, const std::wstring& dllPath)
{
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess)
        return false;

    SIZE_T byteSize = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, byteSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), byteSize, nullptr)) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
        remoteMem, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 8000);

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);
    return true;
}

// --- main ----------------------------------------------------------------------
int main()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Build full DLL path: <exe dir>\dll1.dll
    std::wstring dllPath = GetExeDir() + L'\\' + kDllName;

    // Verify DLL exists
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        char narrow[MAX_PATH]{};
        WideCharToMultiByte(CP_ACP, 0, dllPath.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
        Logf("[ERROR] DLL not found: %s", narrow);
        Logf("Place dll1.dll next to this exe and try again.");
        system("pause");
        return 1;
    }

    {
        char narrow[MAX_PATH]{};
        WideCharToMultiByte(CP_ACP, 0, dllPath.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
        Logf("DLL: %s", narrow);
    }

    Log("Monitoring SC2_x64.exe ... (Ctrl+C to quit)");

    std::unordered_set<DWORD> injectedPids;
    DWORD lastPid = 0;

    while (g_running)
    {
        DWORD pid = FindProcess(kTargetExe);

        if (pid == 0)
        {
            if (lastPid != 0) {
                Log("SC2_x64.exe exited.");
                lastPid = 0;
            }
        }
        else
        {
            if (pid != lastPid) {
                lastPid = pid;
                Logf("Found SC2_x64.exe  PID=%lu", pid);
            }

            if (injectedPids.find(pid) == injectedPids.end())
            {
                Logf("Injecting dll1.dll -> PID %lu ...", pid);
                if (InjectDll(pid, dllPath)) {
                    injectedPids.insert(pid);
                    Logf("Injection OK  PID=%lu", pid);
                } else {
                    DWORD err = GetLastError();
                    Logf("[ERROR] Injection failed  err=%lu  (run as Administrator)", err);
                }
            }
        }

        // Sleep in short slices so Ctrl+C is handled quickly
        for (DWORD i = 0; i < kPollIntervalMs / 100 && g_running; ++i)
            Sleep(100);
    }

    Log("Monitor stopped.");
    return 0;
}
