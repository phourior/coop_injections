#include "pch.h"
#include "hooks/game_hook.h"
#include "hooks/game_hook_internal.h"
#include "core/log.h"
#include "core/memory.h"

static constexpr const char* MASTERY_PATTERN =
    "66 89 97 68 01 00 00 66 89 8F 6A 01 00 00";

static uintptr_t g_masteryPatchAddr = 0;
static uint8_t g_masteryOrigBytes[14] = {};
static void* g_masteryShellcode = nullptr;
static bool g_masteryEnabled = false;

bool EnableMasteryMax()
{
    if (g_masteryEnabled)
        return true;

    if (!g_masteryPatchAddr)
    {
        g_masteryPatchAddr = PatternScan("SC2_x64.exe", MASTERY_PATTERN);
        if (!g_masteryPatchAddr)
        {
            Log("[!] EnableMasteryMax: pattern not found\n");
            return false;
        }
        Log("[*] Mastery patch addr: 0x%llX\n", static_cast<unsigned long long>(g_masteryPatchAddr));
    }

    if (!SafeMemcpy(g_masteryOrigBytes, reinterpret_cast<const void*>(g_masteryPatchAddr), 14))
    {
        Log("[!] EnableMasteryMax: backup failed\n");
        return false;
    }

    if (!g_masteryShellcode)
    {
        g_masteryShellcode = VirtualAlloc(nullptr, 32,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_masteryShellcode)
        {
            Log("[!] EnableMasteryMax: VirtualAlloc failed\n");
            return false;
        }

        const uintptr_t returnAddr = g_masteryPatchAddr + 14;
        uint8_t sc[32] = {
            0x66, 0xC7, 0x87, 0x68, 0x01, 0x00, 0x00, 0xFF, 0x7F,
            0x66, 0xC7, 0x87, 0x6A, 0x01, 0x00, 0x00, 0xFF, 0x7F,
            0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
            static_cast<uint8_t>(returnAddr),
            static_cast<uint8_t>(returnAddr >> 8),
            static_cast<uint8_t>(returnAddr >> 16),
            static_cast<uint8_t>(returnAddr >> 24),
            static_cast<uint8_t>(returnAddr >> 32),
            static_cast<uint8_t>(returnAddr >> 40),
            static_cast<uint8_t>(returnAddr >> 48),
            static_cast<uint8_t>(returnAddr >> 56),
        };
        memcpy(g_masteryShellcode, sc, 32);
        FlushInstructionCache(GetCurrentProcess(), g_masteryShellcode, 32);
    }

    uint8_t jmp14[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    const uintptr_t shellcodeAddress = reinterpret_cast<uintptr_t>(g_masteryShellcode);
    memcpy(jmp14 + 6, &shellcodeAddress, 8);

    if (!WriteCodeBytes(g_masteryPatchAddr, jmp14, sizeof(jmp14)))
    {
        Log("[!] EnableMasteryMax: patch failed\n");
        return false;
    }

    g_masteryEnabled = true;
    Log("[+] Mastery MAX enabled (pattern-based)\n");
    return true;
}

void DisableMasteryMax()
{
    if (!g_masteryEnabled || !g_masteryPatchAddr)
        return;

    if (!WriteCodeBytes(g_masteryPatchAddr, g_masteryOrigBytes, sizeof(g_masteryOrigBytes)))
    {
        Log("[!] DisableMasteryMax: restore failed\n");
        return;
    }
    g_masteryEnabled = false;
    Log("[-] Mastery MAX disabled\n");
}

bool IsMasteryMaxEnabled()
{
    return g_masteryEnabled;
}