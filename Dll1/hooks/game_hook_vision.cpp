#include "pch.h"
#include "hooks/game_hook.h"
#include "hooks/game_hook_internal.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

#include <tlhelp32.h>
#include <vector>

static constexpr const char* FULL_MAP_VISION_PATTERN =
    "48 8D 0D ?? ?? ?? ?? 8B 34 81 48 89 6C 24 ?? "
    "41 80 FD 10 74 ?? 44 0F A3 EE";

static constexpr uint8_t FULL_MAP_VISION_PATCH[10] = {
    0xBE, 0xFF, 0xFF, 0xFF, 0xFF,
    0x90, 0x90, 0x90, 0x90, 0x90,
};

static constexpr uint8_t EnhancedVisionResult(uint8_t original, uint64_t flags)
{
    return original >= 8 ? original :
        static_cast<uint8_t>(8 + (((flags >> 30) & 1) && !((flags >> 29) & 1)));
}

static_assert(EnhancedVisionResult(0, 0) == 8);
static_assert(EnhancedVisionResult(7, uint64_t{1} << 30) == 9);
static_assert(EnhancedVisionResult(0, uint64_t{3} << 29) == 8);
static_assert(EnhancedVisionResult(12, uint64_t{1} << 30) == 12);

static constexpr bool PreserveExploredVisibility(uint8_t original, uint8_t cached)
{
    return original == 4 || original == 5 ||
        ((cached == 4 || cached == 5) && original <= 5);
}

static_assert(PreserveExploredVisibility(4, 8));
static_assert(PreserveExploredVisibility(5, 5));
static_assert(PreserveExploredVisibility(2, 4));
static_assert(!PreserveExploredVisibility(8, 4));
static_assert(!PreserveExploredVisibility(2, 2));
static_assert(!PreserveExploredVisibility(9, 5));

static uintptr_t g_fullMapVisionAddr = 0;
static uint8_t g_fullMapVisionOrigBytes[10] = {};
static bool g_fullMapVisionOrigSaved = false;
static bool g_fullMapVisionPatched = false;
static volatile LONG g_fullMapVisionRequested = 0;
static volatile LONG g_fullMapVisionMapIndex = -1;
static SRWLOCK g_fullMapVisionLock = SRWLOCK_INIT;
static bool g_enhancedVisionSelected = false;
static bool g_observerVisionSelected = false;
static bool g_fullMapVisionFailed = false;
static constexpr uint8_t ENHANCED_VISION_MASK_ORIGINAL[] = {
    0x48, 0x8D, 0x0D, 0x90, 0x0E, 0x37, 0x02, 0x8B, 0x34, 0x81,
};
static constexpr uint8_t VISION_ENTRY_ORIGINAL[] = {
    0x44, 0x88, 0x44, 0x24, 0x18, 0x88, 0x54, 0x24, 0x10,
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x56,
};

bool VisionBytesEqual(uintptr_t address, const uint8_t* expected, size_t size)
{
    uint8_t current[32] = {};
    return address && size <= sizeof(current) &&
        SafeMemcpy(current, reinterpret_cast<const void*>(address), size) &&
        memcmp(current, expected, size) == 0;
}


static constexpr const char* FULL_MAP_VISION_MAPS[] = {
    "虚空撕裂", "克哈裂痕", "虚空降临", "往日神庙", "往曰神庙", "湮灭快车",
    "天界封锁", "升格之链", "熔火危机", "机会渺茫", "营救矿工",
    "亡者之夜", "黑暗杀星", "净网行动", "聚铁成兵", "死亡摇篮", "往昔神庙", "湮灭之源", "旧忆神庙",
    "Cradle of Death", "Part and Parcel", "Rifts to Korhal", "Scythe of Amon",
    "Void Thrashing", "Chain of Ascension", "Lock & Load", "Malwarfare",
    "Mist Opportunities", "Void Launch", "The Vermillion Problem", "Dead of Night",
    "Oblivion Express", "Miner Evacuation", "Temple of the Past", "LnL",
};

static int FindFullMapVisionMap(const char* mapPath, const char* mapDesc)
{
    const char* path = mapPath ? mapPath : "";
    const char* desc = mapDesc ? mapDesc : "";
    for (size_t index = 0; index < _countof(FULL_MAP_VISION_MAPS); ++index)
    {
        const char* name = FULL_MAP_VISION_MAPS[index];
        if (strstr(path, name) || strstr(desc, name))
            return static_cast<int>(index);
    }
    return -1;
}

bool IsFullMapVisionMapAllowed()
{
    return !FULL_MAP_VISION_MAP_LIMIT_ENABLED ||
        InterlockedCompareExchange(&g_fullMapVisionMapIndex, 0, 0) >= 0;
}

static void ResumeAndCloseThreads(std::vector<HANDLE>& threads)
{
    for (auto it = threads.rbegin(); it != threads.rend(); ++it)
    {
        ResumeThread(*it);
        CloseHandle(*it);
    }
    threads.clear();
}

static bool SuspendOtherThreads(uintptr_t protectedAddress, size_t protectedSize,
    std::vector<HANDLE>& threads)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    const DWORD processId = GetCurrentProcessId();
    const DWORD currentThreadId = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool success = Thread32First(snapshot, &entry) != FALSE;

    while (success)
    {
        if (entry.th32OwnerProcessID == processId && entry.th32ThreadID != currentThreadId)
        {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread)
            {
                if (SuspendThread(thread) == static_cast<DWORD>(-1))
                {
                    CloseHandle(thread);
                    success = false;
                    break;
                }

                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (!GetThreadContext(thread, &context))
                {
                    ResumeThread(thread);
                    CloseHandle(thread);
                    success = false;
                    break;
                }

#ifdef _WIN64
                const uintptr_t instructionPointer = static_cast<uintptr_t>(context.Rip);
#else
                const uintptr_t instructionPointer = static_cast<uintptr_t>(context.Eip);
#endif
                if (instructionPointer >= protectedAddress &&
                    instructionPointer < protectedAddress + protectedSize)
                {
                    ResumeThread(thread);
                    CloseHandle(thread);
                    success = false;
                    break;
                }
                threads.push_back(thread);
            }
            else if (GetLastError() != ERROR_INVALID_PARAMETER)
            {
                success = false;
                break;
            }
        }

        if (!Thread32Next(snapshot, &entry))
            break;
    }

    CloseHandle(snapshot);
    if (!success)
        ResumeAndCloseThreads(threads);
    return success;
}

bool WriteCodeBytes(uintptr_t address, const void* bytes, size_t size)
{
    std::vector<HANDLE> suspendedThreads;
    if (!address || !bytes || !size ||
        !SuspendOtherThreads(address, size, suspendedThreads))
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), size,
        PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        ResumeAndCloseThreads(suspendedThreads);
        return false;
    }

    memcpy(reinterpret_cast<void*>(address), bytes, size);

    DWORD unused = 0;
    const bool protectionRestored = VirtualProtect(
        reinterpret_cast<LPVOID>(address), size, oldProtect, &unused) != FALSE;
    const bool cacheFlushed = FlushInstructionCache(
        GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), size) != FALSE;
    ResumeAndCloseThreads(suspendedThreads);
    if (!protectionRestored)
        Log("[!] Code patch committed but page protection restore failed at 0x%llX\n",
            static_cast<unsigned long long>(address));
    if (!cacheFlushed)
        Log("[!] Code patch committed but instruction cache flush failed at 0x%llX\n",
            static_cast<unsigned long long>(address));
    return true;
}

static bool ApplyLegacyFullMapVisionPatch()
{
    if (g_fullMapVisionPatched)
        return true;

    if (!g_fullMapVisionAddr)
    {
        g_fullMapVisionAddr = PatternScan("SC2_x64.exe", FULL_MAP_VISION_PATTERN);
        if (!g_fullMapVisionAddr)
        {
            Log("[!] FullMapVision: pattern not found\n");
            return false;
        }
        Log("[*] FullMapVision patch addr: 0x%llX\n",
            static_cast<unsigned long long>(g_fullMapVisionAddr));
    }

    if (!VisionBytesEqual(g_fullMapVisionAddr - 0x49, VISION_ENTRY_ORIGINAL,
        sizeof(VISION_ENTRY_ORIGINAL)))
    {
        Log("[!] FullMapVision: visibility entry is owned by another hook\n");
        return false;
    }

    if (!g_fullMapVisionOrigSaved)
    {
        if (!SafeMemcpy(g_fullMapVisionOrigBytes,
            reinterpret_cast<const void*>(g_fullMapVisionAddr), sizeof(g_fullMapVisionOrigBytes)))
        {
            Log("[!] FullMapVision: backup failed\n");
            return false;
        }
        g_fullMapVisionOrigSaved = true;
    }

    if (!VisionBytesEqual(g_fullMapVisionAddr, g_fullMapVisionOrigBytes,
        sizeof(g_fullMapVisionOrigBytes)))
        return false;

    if (!WriteCodeBytes(g_fullMapVisionAddr, FULL_MAP_VISION_PATCH, sizeof(FULL_MAP_VISION_PATCH)))
    {
        Log("[!] FullMapVision: patch failed\n");
        return false;
    }

    g_fullMapVisionPatched = true;
    Log("[+] FullMapVision enabled\n");
    return true;
}

static bool RestoreLegacyFullMapVisionPatch()
{
    if (!g_fullMapVisionPatched || !g_fullMapVisionAddr || !g_fullMapVisionOrigSaved)
        return true;

    if (!VisionBytesEqual(g_fullMapVisionAddr, FULL_MAP_VISION_PATCH, sizeof(FULL_MAP_VISION_PATCH)) ||
        !WriteCodeBytes(g_fullMapVisionAddr, g_fullMapVisionOrigBytes, sizeof(g_fullMapVisionOrigBytes)))
    {
        Log("[!] FullMapVision: restore failed\n");
        return false;
    }

    g_fullMapVisionPatched = false;
    Log("[-] FullMapVision disabled\n");
    return true;
}

static bool ApplyEnhancedVision()
{
    return ApplyEnhancedVisionMode(g_observerVisionSelected);
}

static bool ApplyFullMapVisionPatch()
{
    if (!IsFullMapVisionMapAllowed())
    {
        g_fullMapVisionFailed = false;
        return true;
    }
    if (g_observerVisionSelected && !IsObserverVisionRuntimeReady())
    {
        g_fullMapVisionFailed = false;
        return true;
    }
    const bool result = !IsDllUnloading() &&
        ((g_enhancedVisionSelected || g_observerVisionSelected)
            ? ApplyEnhancedVision() : ApplyLegacyFullMapVisionPatch());
    g_fullMapVisionFailed = !result;
    return result;
}

static bool RestoreFullMapVisionPatch()
{
    const bool enhanced = RestoreEnhancedVisionMode();
    const bool legacy = RestoreLegacyFullMapVisionPatch();
    g_fullMapVisionFailed = !enhanced || !legacy;
    return !g_fullMapVisionFailed;
}

void UpdateFullMapVisionMapState(const char* mapPath, const char* mapDesc)
{
    const int nextMap = FindFullMapVisionMap(mapPath, mapDesc);
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    const LONG previousMap = InterlockedExchange(&g_fullMapVisionMapIndex, static_cast<LONG>(nextMap));
    if (previousMap == nextMap)
    {
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return;
    }

    if (nextMap >= 0)
    {
        if (previousMap >= 0)
            Log("[*] FullMapVision map left: %s\n", FULL_MAP_VISION_MAPS[previousMap]);
        Log("[*] FullMapVision map entered: %s\n", FULL_MAP_VISION_MAPS[nextMap]);
        if (InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0) != 0)
            ApplyFullMapVisionPatch();
    }
    else
    {
        if (previousMap >= 0)
            Log("[*] FullMapVision map left: %s\n", FULL_MAP_VISION_MAPS[previousMap]);
        if (FULL_MAP_VISION_MAP_LIMIT_ENABLED)
            RestoreFullMapVisionPatch();
        else if (InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0) != 0)
            ApplyFullMapVisionPatch();
    }
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
}

bool EnableFullMapVision()
{
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    if (IsDllUnloading() || (g_observerVisionSelected && !RestoreFullMapVisionPatch()))
    {
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return false;
    }
    g_observerVisionSelected = false;
    InterlockedExchange(&g_fullMapVisionRequested, 1);
    const LONG mapIndex = InterlockedCompareExchange(&g_fullMapVisionMapIndex, 0, 0);
    const bool result = IsFullMapVisionMapAllowed() ? ApplyFullMapVisionPatch() : true;
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);

    if (FULL_MAP_VISION_MAP_LIMIT_ENABLED && mapIndex < 0)
        Log("[*] FullMapVision armed; waiting for a supported co-op map\n");
    return result;
}

void DisableFullMapVision()
{
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    InterlockedExchange(&g_fullMapVisionRequested, 0);
    RestoreFullMapVisionPatch();
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
}

bool IsFullMapVisionEnabled()
{
    return InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0) == 1;
}

bool IsFullMapVisionApplied()
{
    AcquireSRWLockShared(&g_fullMapVisionLock);
    const bool applied = !g_fullMapVisionFailed &&
        (g_fullMapVisionPatched || IsEnhancedVisionModeApplied(g_observerVisionSelected));
    ReleaseSRWLockShared(&g_fullMapVisionLock);
    return applied;
}

bool SetFullMapVisionEnhanced(bool enhanced)
{
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    if (g_observerVisionSelected && !IsDllUnloading())
    {
        g_enhancedVisionSelected = enhanced;
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return true;
    }
    if (IsDllUnloading() || !RestoreFullMapVisionPatch())
    {
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return false;
    }
    g_enhancedVisionSelected = enhanced;
    g_observerVisionSelected = false;
    const bool shouldApply = IsFullMapVisionEnabled() && IsFullMapVisionMapAllowed();
    const bool result = !shouldApply || ApplyFullMapVisionPatch();
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
    return result;
}

bool SetFullMapVisionObserver(bool observer)
{
    AcquireSRWLockExclusive(&g_fullMapVisionLock);
    if (!observer && !g_observerVisionSelected)
    {
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return true;
    }
    if (IsDllUnloading() || !RestoreFullMapVisionPatch())
    {
        ReleaseSRWLockExclusive(&g_fullMapVisionLock);
        return false;
    }
    g_observerVisionSelected = observer;
    InterlockedExchange(&g_fullMapVisionRequested, observer ? 2 : 0);
    const bool shouldApply = observer && IsFullMapVisionMapAllowed();
    const bool result = !shouldApply || ApplyFullMapVisionPatch();
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
    return result;
}

bool IsFullMapVisionObserver()
{
    AcquireSRWLockShared(&g_fullMapVisionLock);
    const bool selected = g_observerVisionSelected &&
        InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0) == 2;
    ReleaseSRWLockShared(&g_fullMapVisionLock);
    return selected;
}

bool IsFullMapVisionEnhanced()
{
    AcquireSRWLockShared(&g_fullMapVisionLock);
    const bool selected = g_enhancedVisionSelected;
    ReleaseSRWLockShared(&g_fullMapVisionLock);
    return selected;
}

bool HasFullMapVisionError()
{
    AcquireSRWLockShared(&g_fullMapVisionLock);
    const bool failed = g_fullMapVisionFailed;
    ReleaseSRWLockShared(&g_fullMapVisionLock);
    return failed;
}

void UpdateFullMapVisionRuntime()
{
    if (IsDllUnloading() || !TryAcquireSRWLockExclusive(&g_fullMapVisionLock))
        return;
    static ULONGLONG lastCheck = 0;
    const ULONGLONG now = GetTickCount64();
    const bool mapAllowed = IsFullMapVisionMapAllowed();
    if (!IsDllUnloading() && now - lastCheck >= 1000)
    {
        lastCheck = now;
        const LONG requested = InterlockedCompareExchange(&g_fullMapVisionRequested, 0, 0);
        bool applied = true;
        if (g_observerVisionSelected && requested == 2)
        {
            const bool ready = mapAllowed && IsObserverVisionRuntimeReady();
            const bool active = IsEnhancedVisionModeApplied(true);
            if (active && !ready)
                applied = RestoreFullMapVisionPatch();
            else if (!active && ready)
                applied = ApplyFullMapVisionPatch();
        }
        else
        {
            applied = RefreshEnhancedVisionRuntime(
                g_enhancedVisionSelected, requested == 1, mapAllowed);
        }
        if (!applied && !g_fullMapVisionFailed)
            Log("[!] Vision runtime not ready; retrying while enabled\n");
        g_fullMapVisionFailed = !applied;
    }
    ReleaseSRWLockExclusive(&g_fullMapVisionLock);
}