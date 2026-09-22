#include "pch.h"
#include "hooks/game_hook.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

#include <limits>

// ════════════════════════════════════════════════════════════════
//  经验倍率 Hook
// ════════════════════════════════════════════════════════════════

using ExperienceFn = __int64(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static ExperienceFn g_origExperience[8] = {};
static uintptr_t g_experienceTargets[8] = {};
static size_t g_experienceHookCount = 0;
static volatile LONG g_experienceEnabled = 0;
static volatile LONG g_experienceMultiplier = 30;

static void ScaleExperiencePayload(uintptr_t payload)
{
    if (!payload || InterlockedCompareExchange(&g_experienceEnabled, 0, 0) == 0)
        return;

    uintptr_t entries = 0;
    uint64_t count = 0;
    if (!SafeMemcpy(&entries, reinterpret_cast<const void*>(payload + 0x18), sizeof(entries)) ||
        !SafeMemcpy(&count, reinterpret_cast<const void*>(payload + 0x20), sizeof(count)) ||
        !entries || count == 0 || count > 16)
        return;

    const uint64_t multiplier = static_cast<uint64_t>(
        InterlockedCompareExchange(&g_experienceMultiplier, 0, 0));

    for (uint64_t index = 0; index < count; ++index)
    {
        const uintptr_t entry = entries + index * 0x80;
        uint8_t active = 0;
        uint64_t value = 0;
        if (!SafeMemcpy(&active, reinterpret_cast<const void*>(entry + 0x08), sizeof(active)) ||
            !active ||
            !SafeMemcpy(&value, reinterpret_cast<const void*>(entry + 0x09), sizeof(value)) ||
            value == 0)
            continue;

        constexpr uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
        const uint64_t scaled = value > maximum / multiplier ? maximum : value * multiplier;
        if (scaled != value)
            SafeMemcpy(reinterpret_cast<void*>(entry + 0x09), &scaled, sizeof(scaled));
    }
}

static __int64 CallExperienceOriginal(size_t index, uintptr_t a1, uintptr_t a2,
    uintptr_t payload, uintptr_t a4)
{
    EnterHookCallback();
    if (!IsDllUnloading())
        ScaleExperiencePayload(payload);
    ExperienceFn original = g_origExperience[index];
    const __int64 result = original ? original(a1, a2, payload, a4) : 0;
    LeaveHookCallback();
    return result;
}

#define DEFINE_EXPERIENCE_DETOUR(index) \
    static __int64 __fastcall ExperienceDetour##index( \
        uintptr_t a1, uintptr_t a2, uintptr_t payload, uintptr_t a4) \
    { \
        return CallExperienceOriginal(index, a1, a2, payload, a4); \
    }

DEFINE_EXPERIENCE_DETOUR(0)
DEFINE_EXPERIENCE_DETOUR(1)
DEFINE_EXPERIENCE_DETOUR(2)
DEFINE_EXPERIENCE_DETOUR(3)
DEFINE_EXPERIENCE_DETOUR(4)
DEFINE_EXPERIENCE_DETOUR(5)
DEFINE_EXPERIENCE_DETOUR(6)
DEFINE_EXPERIENCE_DETOUR(7)

#undef DEFINE_EXPERIENCE_DETOUR

static LPVOID const EXPERIENCE_DETOURS[8] = {
    reinterpret_cast<LPVOID>(&ExperienceDetour0),
    reinterpret_cast<LPVOID>(&ExperienceDetour1),
    reinterpret_cast<LPVOID>(&ExperienceDetour2),
    reinterpret_cast<LPVOID>(&ExperienceDetour3),
    reinterpret_cast<LPVOID>(&ExperienceDetour4),
    reinterpret_cast<LPVOID>(&ExperienceDetour5),
    reinterpret_cast<LPVOID>(&ExperienceDetour6),
    reinterpret_cast<LPVOID>(&ExperienceDetour7),
};

static size_t FindExperienceTargets(uintptr_t* targets, size_t capacity)
{
    HMODULE module = GetModuleBase("SC2_x64.exe");
    if (!module || !targets || capacity == 0)
        return 0;

    auto* base = reinterpret_cast<const uint8_t*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    static constexpr uint8_t prefix[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
        0x56, 0x57, 0x41, 0x56, 0xB8, 0x80, 0x44, 0x00, 0x00,
    };
    static constexpr uint8_t suffix[] = {
        0x48, 0x2B, 0xE0, 0x49, 0x8B, 0xE8, 0x0F, 0xB6, 0xFA,
        0x48, 0x8B, 0xF1, 0x48, 0x81, 0xC1, 0x90, 0x01,
    };
    static constexpr uint8_t experienceEventType[] = { 0x41, 0xB0, 0x0E };
    constexpr size_t experienceEventTypeOffset = 0x2B;

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    constexpr size_t requiredSize = experienceEventTypeOffset + sizeof(experienceEventType);
    size_t found = 0;
    for (size_t offset = 0; offset + requiredSize <= imageSize && found < capacity; ++offset)
    {
        const uint8_t* candidate = base + offset;
        if (memcmp(candidate, prefix, sizeof(prefix)) != 0 ||
            candidate[sizeof(prefix)] != 0xE8 ||
            memcmp(candidate + sizeof(prefix) + 5, suffix, sizeof(suffix)) != 0 ||
            memcmp(candidate + experienceEventTypeOffset, experienceEventType,
                sizeof(experienceEventType)) != 0)
            continue;

        targets[found++] = reinterpret_cast<uintptr_t>(candidate);
        offset += requiredSize - 1;
    }
    return found;
}

bool SetupExperienceHooks()
{
    uintptr_t targets[8] = {};
    const size_t targetCount = FindExperienceTargets(targets, _countof(targets));
    if (targetCount == 0)
    {
        Log("[!] Experience: no hook targets found\n");
        return false;
    }

    Log("[*] Experience: found %zu candidate hook target(s)\n", targetCount);

    for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
    {
        const size_t slot = g_experienceHookCount;
        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(targets[targetIndex]),
            EXPERIENCE_DETOURS[slot],
            reinterpret_cast<LPVOID*>(&g_origExperience[slot]));
        if (status != MH_OK)
        {
            Log("[!] Experience hook[%zu] failed at 0x%llX: %s\n",
                targetIndex, static_cast<unsigned long long>(targets[targetIndex]),
                MH_StatusToString(status));
            continue;
        }

        g_experienceTargets[slot] = targets[targetIndex];
        ++g_experienceHookCount;
        Log("[+] Experience hook[%zu] created at 0x%llX\n", slot,
            static_cast<unsigned long long>(targets[targetIndex]));
    }

    return g_experienceHookCount != 0;
}

void EnableExperienceMultiplier(bool on)
{
    if (on && g_experienceHookCount == 0)
    {
        Log("[!] Experience multiplier unavailable: no hooks installed\n");
        return;
    }

    if (on)
    {
        for (size_t index = 0; index < g_experienceHookCount; ++index)
        {
            MH_STATUS status = MH_EnableHook(reinterpret_cast<LPVOID>(g_experienceTargets[index]));
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                Log("[!] Experience hook[%zu] enable failed at 0x%llX: %s\n",
                    index,
                    static_cast<unsigned long long>(g_experienceTargets[index]),
                    MH_StatusToString(status));
            }
        }
    }

    InterlockedExchange(&g_experienceEnabled, on ? 1 : 0);
    Log("[%c] Experience multiplier %s (%ldx)\n", on ? '+' : '-',
        on ? "enabled" : "disabled",
        InterlockedCompareExchange(&g_experienceMultiplier, 0, 0));
}

bool IsExperienceMultiplierEnabled()
{
    return InterlockedCompareExchange(&g_experienceEnabled, 0, 0) != 0;
}

void SetExperienceMultiplier(float multiplier)
{
    LONG value = static_cast<LONG>(multiplier + 0.5f);
    if (value < 1)
        value = 1;
    if (value > 30)
        value = 30;
    InterlockedExchange(&g_experienceMultiplier, value);
}

float GetExperienceMultiplier()
{
    return static_cast<float>(InterlockedCompareExchange(&g_experienceMultiplier, 0, 0));
}