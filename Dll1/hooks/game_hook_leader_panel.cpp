#include "pch.h"
#include "hooks/game_hook.h"
#include "hooks/game_hook_internal.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

static SRWLOCK g_leaderPanelLock = SRWLOCK_INIT;
static volatile LONG g_leaderPanelRequested = 0;
static volatile LONG g_leaderPanelApplied = 0;
static volatile LONG g_leaderPanelFailed = 0;
static uintptr_t g_leaderPanelSaved = 0;
static bool g_leaderPanelWasVisible = false;
using LeaderHotkeyFn = int(__fastcall*)(uintptr_t, uintptr_t);
static LeaderHotkeyFn g_originalLeaderHotkey = nullptr;
static uintptr_t g_leaderHotkeyEntry = 0;
static bool g_leaderHotkeyEnabled = false;

static constexpr bool IsLeaderMenuHotkey(uint32_t eventType, uint32_t action)
{
    return eventType == 0x18 && action >= 0x12 && action <= 0x1D;
}

static_assert(IsLeaderMenuHotkey(0x18, 0x12));
static_assert(IsLeaderMenuHotkey(0x18, 0x1D));
static_assert(!IsLeaderMenuHotkey(0x18, 0x11));
static_assert(!IsLeaderMenuHotkey(0x18, 0x1E));
static_assert(!IsLeaderMenuHotkey(0x17, 0x12));

static int __fastcall HookLeaderHotkey(uintptr_t panel, uintptr_t event)
{
    EnterHookCallback();
    uint32_t eventType = 0;
    uint32_t action = 0;
    const bool ignore = !IsDllUnloading() && IsLeaderPanelEnabled() && event &&
        SafeMemcpy(&eventType, reinterpret_cast<const void*>(event + 0x10), sizeof(eventType)) &&
        SafeMemcpy(&action, reinterpret_cast<const void*>(event + 0x20), sizeof(action)) &&
        IsLeaderMenuHotkey(eventType, action);
    const int result = ignore ? 1 : g_originalLeaderHotkey(panel, event);
    LeaveHookCallback();
    return result;
}

static bool EnableLeaderHotkeyFilter()
{
    if (g_leaderHotkeyEnabled)
        return true;
    if (!g_leaderHotkeyEntry)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
        const uintptr_t entry = base + 0xC2BA40;
        static constexpr uint8_t expected[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
            0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x7A, 0x10, 0x18
        };
        if (!base || !VisionBytesEqual(entry, expected, sizeof(expected)))
            return false;
        const MH_STATUS created = MH_CreateHook(reinterpret_cast<void*>(entry),
            reinterpret_cast<void*>(&HookLeaderHotkey), reinterpret_cast<void**>(&g_originalLeaderHotkey));
        if (created != MH_OK)
        {
            Log("[!] LeaderPanel hotkey hook creation failed: %s\n", MH_StatusToString(created));
            return false;
        }
        g_leaderHotkeyEntry = entry;
    }
    const MH_STATUS enabled = MH_EnableHook(reinterpret_cast<void*>(g_leaderHotkeyEntry));
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        return false;
    g_leaderHotkeyEnabled = true;
    Log("[+] LeaderPanel menu hotkeys disabled; mouse selection unchanged\n");
    return true;
}

static bool DisableLeaderHotkeyFilter()
{
    if (!g_leaderHotkeyEnabled)
        return true;
    const MH_STATUS disabled = MH_DisableHook(reinterpret_cast<void*>(g_leaderHotkeyEntry));
    if (disabled != MH_OK && disabled != MH_ERROR_DISABLED)
        return false;
    g_leaderHotkeyEnabled = false;
    Log("[-] LeaderPanel menu hotkeys restored\n");
    return true;
}

static bool FindLeaderPanel(uintptr_t& panel)
{
    panel = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!base || !SafeMemcpy(&dos, reinterpret_cast<const void*>(base), sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !SafeMemcpy(&nt, reinterpret_cast<const void*>(base + dos.e_lfanew), sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp != 0x6A57F729 || nt.OptionalHeader.SizeOfImage != 0x8204000)
        return false;

    uintptr_t tail = 0;
    uintptr_t node = 0;
    if (!SafeMemcpy(&tail, reinterpret_cast<const void*>(base + 0x443FA40 + 0xD0), sizeof(tail)))
        return false;
    if (!tail)
        return true;
    if (!SafeMemcpy(&node, reinterpret_cast<const void*>(tail), sizeof(node)))
        return false;
    node &= ~uintptr_t{1};
    for (size_t count = 0; node && count < 256; ++count)
    {
        uintptr_t record[3]{};
        if (!SafeMemcpy(record, reinterpret_cast<const void*>(node), sizeof(record)))
            return false;
        const uintptr_t subscriber = record[1] & ~uintptr_t{1};
        uintptr_t interfaceTable = 0;
        uintptr_t objectTable = 0;
        if (record[2] == base + 0xC25740 && subscriber > 0x10000 &&
            SafeMemcpy(&interfaceTable, reinterpret_cast<const void*>(subscriber), sizeof(interfaceTable)) &&
            interfaceTable == base + 0x2B25790 &&
            SafeMemcpy(&objectTable, reinterpret_cast<const void*>(subscriber - 0x10), sizeof(objectTable)) &&
            objectTable == base + 0x2B25538)
        {
            const uintptr_t candidate = subscriber - 0x10;
            constexpr uintptr_t childOffsets[] = {0x148, 0x170};
            constexpr uintptr_t childTables[] = {0x2B24348, 0x2B25018};
            for (size_t index = 0; index < 2; ++index)
            {
                uintptr_t child = 0;
                uintptr_t childTable = 0;
                if (!SafeMemcpy(&child, reinterpret_cast<const void*>(candidate + childOffsets[index]), sizeof(child)) ||
                    !child || !SafeMemcpy(&childTable, reinterpret_cast<const void*>(child), sizeof(childTable)) ||
                    childTable != base + childTables[index])
                    return false;
            }
            panel = candidate;
            return true;
        }
        if (record[0] & 1)
            return true;
        node = record[0];
    }
    return node == 0;
}

static bool SetLeaderPanelVisible(uintptr_t panel, bool visible)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    uintptr_t table = 0;
    uintptr_t function = 0;
    static constexpr uint8_t entry[] = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x40};
    if (!base || !panel ||
        !SafeMemcpy(&table, reinterpret_cast<const void*>(panel), sizeof(table)) ||
        table != base + 0x2B25538 ||
        !SafeMemcpy(&function, reinterpret_cast<const void*>(table + 0x40), sizeof(function)) ||
        function != base + 0x14E48C0 || !VisionBytesEqual(function, entry, sizeof(entry)))
        return false;
    __try
    {
        reinterpret_cast<void(__fastcall*)(uintptr_t, bool)>(function)(panel, visible);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    uint8_t flags = 0;
    return SafeMemcpy(&flags, reinterpret_cast<const void*>(panel + 0x48), sizeof(flags)) &&
        ((flags & 1) != 0) == visible;
}

static bool RestoreLeaderPanel()
{
    if (!g_leaderPanelSaved)
        return true;
    uintptr_t current = 0;
    if (!FindLeaderPanel(current))
        return false;
    if (current == g_leaderPanelSaved && !SetLeaderPanelVisible(current, g_leaderPanelWasVisible))
        return false;
    g_leaderPanelSaved = 0;
    InterlockedExchange(&g_leaderPanelApplied, 0);
    Log("[-] LeaderPanel visibility restored or object removed\n");
    return true;
}

void EnableLeaderPanel(bool enabled)
{
    if (!IsDllUnloading())
        InterlockedExchange(&g_leaderPanelRequested, enabled ? 1 : 0);
}

bool IsLeaderPanelEnabled()
{
    return InterlockedCompareExchange(&g_leaderPanelRequested, 0, 0) != 0;
}

bool IsLeaderPanelApplied()
{
    return InterlockedCompareExchange(&g_leaderPanelApplied, 0, 0) != 0;
}

bool HasLeaderPanelError()
{
    return InterlockedCompareExchange(&g_leaderPanelFailed, 0, 0) != 0;
}

void UpdateLeaderPanel()
{
    if (IsDllUnloading() || !TryAcquireSRWLockExclusive(&g_leaderPanelLock))
        return;
    static ULONGLONG lastCheck = 0;
    const ULONGLONG now = GetTickCount64();
    if (IsDllUnloading() || now - lastCheck < 250)
    {
        ReleaseSRWLockExclusive(&g_leaderPanelLock);
        return;
    }
    lastCheck = now;
    bool success = true;
    if (!IsLeaderPanelEnabled())
    {
        success = RestoreLeaderPanel() && EnableLeaderHotkeyFilter();
    }
    else
    {
        uintptr_t panel = 0;
        success = FindLeaderPanel(panel);
        if (success && panel != g_leaderPanelSaved)
        {
            g_leaderPanelSaved = 0;
            InterlockedExchange(&g_leaderPanelApplied, 0);
        }
        if (success && panel)
        {
            uint8_t flags = 0;
            success = EnableLeaderHotkeyFilter() &&
                SafeMemcpy(&flags, reinterpret_cast<const void*>(panel + 0x48), sizeof(flags));
            if (success)
            {
                if (!g_leaderPanelSaved)
                {
                    g_leaderPanelSaved = panel;
                    g_leaderPanelWasVisible = (flags & 1) != 0;
                }
                success = (flags & 1) != 0 || SetLeaderPanelVisible(panel, true);
                if (success && !IsLeaderPanelApplied())
                    Log("[+] LeaderPanel visibility enabled; IncomeFrame and ProductionFrame verified\n");
                InterlockedExchange(&g_leaderPanelApplied, success ? 1 : 0);
            }
        }
    }
    if (!success && !HasLeaderPanelError())
        Log("[!] LeaderPanel lookup or visibility update failed\n");
    InterlockedExchange(&g_leaderPanelFailed, success ? 0 : 1);
    if (!success)
        InterlockedExchange(&g_leaderPanelApplied, 0);
    ReleaseSRWLockExclusive(&g_leaderPanelLock);
}

bool CleanupLeaderPanelFeature()
{
    AcquireSRWLockExclusive(&g_leaderPanelLock);
    InterlockedExchange(&g_leaderPanelRequested, 0);
    const bool leaderRestored = RestoreLeaderPanel();
    const bool leaderHotkeysRestored = DisableLeaderHotkeyFilter();
    ReleaseSRWLockExclusive(&g_leaderPanelLock);
    return leaderRestored && leaderHotkeysRestored;
}