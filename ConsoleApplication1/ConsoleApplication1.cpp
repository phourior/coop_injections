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
static constexpr const wchar_t* kInstanceMutexName =
    L"Local\\CoopInjections_SC2_Dll1_Injector";

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

enum class ModuleState
{
    Loaded,
    NotLoaded,
    Unknown,
};

static ModuleState GetModuleState(DWORD pid, const wchar_t* moduleName)
{
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            break;
    }

    if (snapshot == INVALID_HANDLE_VALUE)
        return ModuleState::Unknown;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    ModuleState state = ModuleState::NotLoaded;
    if (Module32FirstW(snapshot, &module))
    {
        do
        {
            if (_wcsicmp(module.szModule, moduleName) == 0)
            {
                state = ModuleState::Loaded;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    else
    {
        state = ModuleState::Unknown;
    }

    CloseHandle(snapshot);
    return state;
}

// --- DLL injection via remote LoadLibraryW thread ------------------------------
enum class InjectResult
{
    Succeeded,
    Pending,
    Failed,
};

static InjectResult InjectDll(DWORD pid, const std::wstring& dllPath)
{
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess)
        return InjectResult::Failed;

    SIZE_T byteSize = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, byteSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProcess);
        return InjectResult::Failed;
    }

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), byteSize, nullptr)) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return InjectResult::Failed;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return InjectResult::Failed;
    }

    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return InjectResult::Failed;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
        remoteMem, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return InjectResult::Failed;
    }

    // 审查修复 #5：只有远程 LoadLibraryW 完成且返回非零才报告成功；超时
    // 时保留远程参数并标记 Pending，避免释放后使用和重复创建远程线程。
    const DWORD waitResult = WaitForSingleObject(hThread, 8000);
    if (waitResult != WAIT_OBJECT_0)
    {
        const DWORD error = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        // The remote thread may still be reading dllPath. Leave the allocation in
        // the target process; it is reclaimed when that process exits.
        CloseHandle(hThread);
        CloseHandle(hProcess);
        SetLastError(error);
        return InjectResult::Pending;
    }

    DWORD remoteResult = 0;
    const BOOL gotResult = GetExitCodeThread(hThread, &remoteResult);
    const DWORD resultError = gotResult ? ERROR_SUCCESS : GetLastError();
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (!gotResult || remoteResult == 0)
    {
        SetLastError(gotResult ? ERROR_DLL_INIT_FAILED : resultError);
        return InjectResult::Failed;
    }
    return InjectResult::Succeeded;
}

// --- main ----------------------------------------------------------------------
int main()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (!instanceMutex)
    {
        Logf("[ERROR] Cannot create injector mutex  err=%lu", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log("[ERROR] Another injector instance is already running.");
        CloseHandle(instanceMutex);
        return 1;
    }

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

    // This injector instance handles each SC2 PID once. END may unload the DLL,
    // but only a newly started injector instance is allowed to inject it again.
    std::unordered_set<DWORD> injectedPids;
    std::unordered_set<DWORD> observedLoadedPids;
    DWORD lastPid = 0;

    while (g_running)
    {
        DWORD pid = FindProcess(kTargetExe);

        if (pid == 0)
        {
            if (lastPid != 0) {
                Log("SC2_x64.exe exited.");
                injectedPids.erase(lastPid);
                observedLoadedPids.erase(lastPid);
                lastPid = 0;
            }
        }
        else
        {
            if (pid != lastPid) {
                if (lastPid != 0)
                {
                    injectedPids.erase(lastPid);
                    observedLoadedPids.erase(lastPid);
                }
                lastPid = pid;
                Logf("Found SC2_x64.exe  PID=%lu", pid);
            }

            if (injectedPids.find(pid) == injectedPids.end())
            {
                const ModuleState moduleState = GetModuleState(pid, kDllName);
                if (moduleState == ModuleState::Loaded)
                {
                    if (observedLoadedPids.insert(pid).second)
                        Logf("dll1.dll already loaded  PID=%lu; waiting for unload", pid);
                }
                else if (moduleState == ModuleState::Unknown)
                {
                    Logf("[WARN] Cannot enumerate modules  PID=%lu; injection skipped", pid);
                }
                else
                {
                    observedLoadedPids.erase(pid);
                    Logf("Injecting dll1.dll -> PID %lu ...", pid);
                    const InjectResult result = InjectDll(pid, dllPath);
                    if (result == InjectResult::Succeeded) {
                        injectedPids.insert(pid);
                        Logf("Injection OK  PID=%lu", pid);
                    } else if (result == InjectResult::Pending) {
                        injectedPids.insert(pid);
                        Logf("[WARN] Injection still pending  PID=%lu; automatic retry disabled", pid);
                    } else {
                        DWORD err = GetLastError();
                        Logf("[ERROR] Injection failed  err=%lu  (run as Administrator)", err);
                    }
                }
            }
        }

        // Sleep in short slices so Ctrl+C is handled quickly
        for (DWORD i = 0; i < kPollIntervalMs / 100 && g_running; ++i)
            Sleep(100);
    }

    Log("Monitor stopped.");
    CloseHandle(instanceMutex);
    return 0;
}
