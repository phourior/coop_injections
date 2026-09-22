#include "pch.h"
#include "hooks/game_hook.h"
#include "hooks/game_hook_internal.h"
#include "core/globals.h"
#include "core/memory.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

#include <intrin.h>

using UnitVisibilityFn = uint8_t(__fastcall*)(uintptr_t, uint8_t, uint8_t);
using ObserverPlayersFn = uint32_t(__fastcall*)();
using PlayerSetFn = uint32_t(__fastcall*)(uint32_t);
using ViewPlayerFn = uint8_t(__fastcall*)();
using FogViewFn = uintptr_t(__fastcall*)(uint32_t, uint32_t, uint8_t);
using FogBufferFn = uintptr_t(__fastcall*)(uint8_t, uintptr_t, uintptr_t);
using FogTextureFn = uintptr_t(__fastcall*)(uintptr_t, uint8_t);

static UnitVisibilityFn g_origUnitVisibility = nullptr;
static uintptr_t g_enhancedVisionEntry = 0;
static uintptr_t g_enhancedVisionAux = 0;
static uintptr_t g_enhancedVisionPlayer = 0;
static uintptr_t g_observerDisplaySelection = 0;
static bool g_observerDisplayPatched = false;
static ObserverPlayersFn g_originalObserverPlayers = nullptr;
static uintptr_t g_observerPlayersEntry = 0;
static bool g_observerPlayersEnabled = false;
static PlayerSetFn g_playerSet = nullptr;
static ViewPlayerFn g_originalViewPlayer = nullptr;
static uintptr_t g_observerViewPlayerEntry = 0;
static bool g_observerViewPlayerEnabled = false;
static volatile LONG g_observerViewPlayerLogged = 0;
static FogViewFn g_originalFogView = nullptr;
static uintptr_t g_observerFogEntry = 0;
static bool g_observerFogEnabled = false;
static volatile LONG g_observerFogInvocationLogged = 0;
static FogBufferFn g_originalFogBuffer = nullptr;
static FogTextureFn g_originalFogTexture = nullptr;
static uintptr_t g_observerFogBufferEntry = 0;
static uintptr_t g_observerFogTextureEntry = 0;
static bool g_observerFogReadersEnabled = false;
static volatile LONG g_observerFogReaderLogged = 0;
static SRWLOCK g_observerFogLock = SRWLOCK_INIT;
static uintptr_t g_observerFogState = 0;
static uint32_t g_observerFogFirst = 0;
static uint32_t g_observerFogSecond = 0;
static thread_local bool g_observerVisibilityQuery = false;
static volatile LONG g_observerPlayersQueryLogged = 0;
static volatile LONG g_observerVisibilityResultLogged = 0;
static volatile LONG g_observerOwnerResultLogged[17] = {};
static bool g_enhancedVisionHookEnabled = false;
static bool g_enhancedVisionAuxPatched = false;
static volatile LONG g_enhancedVisionActive = 0;
static uintptr_t g_enhancedVisionFogState = 0;
static uint32_t g_enhancedVisionFogMask = 0;
static uint8_t g_enhancedVisionFogAlpha = 0;
static uint8_t g_enhancedVisionFogPlayer = 0;
static SRWLOCK g_observerCallbackLock = SRWLOCK_INIT;
static thread_local unsigned int g_observerCallbackDepth = 0;

static constexpr uint8_t OBSERVER_DISPLAY_ORIGINAL[] = {
    0x0F, 0xB6, 0x84, 0x39, 0xD8, 0x08, 0x00, 0x00
};
static constexpr uint8_t OBSERVER_DISPLAY_PATCH[] = {
    0x0F, 0xB6, 0x87, 0xE8, 0x08, 0x00, 0x00, 0x90
};
static constexpr uint8_t OBSERVER_FOG_ORIGINAL[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
    0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00
};
static constexpr uint8_t OBSERVER_PLAYERS_ORIGINAL[] = {
    0x48, 0x83, 0xEC, 0x28, 0xE8, 0xD7, 0x63, 0x7C, 0xFF,
    0x8B, 0x0D, 0x21, 0x6C, 0x55, 0x03
};
static constexpr uint8_t ENHANCED_VISION_AUX_ORIGINAL[] = {0x8B, 0x4B, 0xFC};
static constexpr uint8_t ENHANCED_VISION_AUX_PATCH[] = {0xEB, 0x06, 0x90};
static constexpr uint8_t VISION_ENTRY_ORIGINAL[] = {
    0x44, 0x88, 0x44, 0x24, 0x18, 0x88, 0x54, 0x24, 0x10,
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x56,
};
static constexpr uint8_t ENHANCED_VISION_MASK_ORIGINAL[] = {
    0x48, 0x8D, 0x0D, 0x90, 0x0E, 0x37, 0x02, 0x8B, 0x34, 0x81,
};

static constexpr uint8_t EnhancedVisionResult(uint8_t original, uint64_t flags)
{
    return original >= 8 ? original :
        static_cast<uint8_t>(8 + (((flags >> 30) & 1) && !((flags >> 29) & 1)));
}

static constexpr bool PreserveExploredVisibility(uint8_t original, uint8_t cached)
{
    return original == 4 || original == 5 ||
        ((cached == 4 || cached == 5) && original <= 5);
}

static void EnterObserverCallback()
{
    EnterHookCallback();
    if (g_observerCallbackDepth++ == 0)
        AcquireSRWLockShared(&g_observerCallbackLock);
}

static void LeaveObserverCallback()
{
    if (--g_observerCallbackDepth == 0)
        ReleaseSRWLockShared(&g_observerCallbackLock);
    LeaveHookCallback();
}

class ObserverCallbackRundown
{
public:
    ObserverCallbackRundown()
    {
        AcquireSRWLockExclusive(&g_observerCallbackLock);
    }

    ~ObserverCallbackRundown()
    {
        ReleaseSRWLockExclusive(&g_observerCallbackLock);
    }

    ObserverCallbackRundown(const ObserverCallbackRundown&) = delete;
    ObserverCallbackRundown& operator=(const ObserverCallbackRundown&) = delete;
};

static bool IsEnhancedVisionArtifact(uint64_t token)
{
    const uintptr_t definition = static_cast<uintptr_t>((token & 0x00FFFFFFFFFFFFFFULL) << 5);
    if (!definition)
        return false;
    const uintptr_t nameObject = ReadMemory<uintptr_t>(definition + 0xC8);
    const uintptr_t name = nameObject ? ReadMemory<uintptr_t>(nameObject + 8) : 0;
    if (!name)
        return false;
    static constexpr const char* names[] = {
        "ZeratulArtifactPickup1", "ZeratulArtifactPickup2",
        "ZeratulArtifactPickup3", "ZeratulArtifactPickupUnlimited",
    };
    for (const char* expected : names)
    {
        char actual[40] = {};
        const size_t length = strlen(expected) + 1;
        if (SafeMemcpy(actual, reinterpret_cast<const void*>(name), length) &&
            memcmp(actual, expected, length) == 0)
            return true;
    }
    return false;
}

static uint32_t __fastcall HookObserverPlayers()
{
    if (g_observerVisibilityQuery)
    {
        const uint32_t players = g_playerSet(1) | g_playerSet(2);
        if (InterlockedCompareExchange(&g_observerPlayersQueryLogged, 1, 0) == 0)
            Log("[*] ObserverVision: unit query uses player set 0x%08X\n", players);
        return players;
    }
    EnterObserverCallback();
    const uint32_t result = g_originalObserverPlayers();
    LeaveObserverCallback();
    return result;
}

static uint8_t QueryObserverVisibility(uintptr_t object, uint8_t flags)
{
    const bool previous = g_observerVisibilityQuery;
    uint8_t result = 0;
    g_observerVisibilityQuery = true;
    __try
    {
        result = g_origUnitVisibility(object, 16, flags);
    }
    __finally
    {
        g_observerVisibilityQuery = previous;
    }
    if (InterlockedCompareExchange(&g_observerVisibilityResultLogged, 1, 0) == 0)
        Log("[*] ObserverVision: first player-16 unit visibility result=%u flags=%u\n",
            static_cast<unsigned>(result), static_cast<unsigned>(flags));
    return result;
}

static uint8_t __fastcall HookObserverViewPlayer()
{
    EnterObserverCallback();
    uint8_t player = g_originalViewPlayer();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t callerRva = base && caller >= base ? caller - base : 0;
    const bool observerMode = !IsDllUnloading() &&
        InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) == 2;
    const bool observerRenderQuery = callerRva == 0xA035CA ||
        callerRva == 0x1CE7E9D || callerRva == 0x1CE8AA2 ||
        callerRva == 0x1CE8FD4 || callerRva == 0x1CE90F6;
    if (observerMode && observerRenderQuery)
    {
        player = 16;
        if (InterlockedCompareExchange(&g_observerViewPlayerLogged, 1, 0) == 0)
            Log("[*] ObserverVision: render view-player queries use observer player\n");
    }
    LeaveObserverCallback();
    return player;
}

static uint8_t __fastcall HookUnitVisibility(uintptr_t object, uint8_t player, uint8_t flags)
{
    EnterObserverCallback();
    uint8_t localPlayer = 0xFF;
    const LONG mode = InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0);
    const uint8_t original = g_origUnitVisibility(object, player, flags);
    if (!IsDllUnloading() && (mode == 1 || mode == 2) && object && player < 16 &&
        SafeMemcpy(&localPlayer, reinterpret_cast<const void*>(g_enhancedVisionPlayer), 1) &&
        player == localPlayer)
    {
        uint8_t cached = 0;
        if (!SafeMemcpy(&cached, reinterpret_cast<const void*>(object + 0x8D8 + player), 1) ||
            PreserveExploredVisibility(original, cached))
        {
            LeaveObserverCallback();
            return original;
        }
    }
    if (!IsDllUnloading() && mode == 2 && object && player == 16)
    {
        const uint8_t result = QueryObserverVisibility(object, flags);
        uintptr_t unit = 0;
        uint8_t owner = 0xFF;
        uint8_t current = 0;
        uint8_t observerCache = 0;
        SafeMemcpy(&unit, reinterpret_cast<const void*>(object + 0x7A0), sizeof(unit));
        if (unit)
            SafeMemcpy(&owner, reinterpret_cast<const void*>(unit + 0x40), 1);
        SafeMemcpy(&current, reinterpret_cast<const void*>(object + 0x8E9), 1);
        SafeMemcpy(&observerCache, reinterpret_cast<const void*>(object + 0x8E8), 1);
        if (owner <= 16 &&
            InterlockedCompareExchange(&g_observerOwnerResultLogged[owner], 1, 0) == 0)
        {
            Log("[*] ObserverVision: owner=%u native16=%u union=%u current=%u cache16=%u flags=%u\n",
                static_cast<unsigned>(owner), static_cast<unsigned>(original),
                static_cast<unsigned>(result), static_cast<unsigned>(current),
                static_cast<unsigned>(observerCache), static_cast<unsigned>(flags));
        }
        LeaveObserverCallback();
        return result;
    }

    uintptr_t unit = 0;
    int8_t excluded = 0;
    uint8_t special = 0;
    uint64_t state = 0;
    uint64_t token = 0;
    const bool enhance = !IsDllUnloading() &&
        mode == 1 &&
        SafeMemcpy(&localPlayer, reinterpret_cast<const void*>(g_enhancedVisionPlayer), 1) &&
        player == localPlayer && object &&
        SafeMemcpy(&unit, reinterpret_cast<const void*>(object + 0x7A0), sizeof(unit)) && unit &&
        SafeMemcpy(&excluded, reinterpret_cast<const void*>(unit + 0x41), 1) && excluded <= 0 &&
        SafeMemcpy(&special, reinterpret_cast<const void*>(unit + 0x118), 1) &&
        SafeMemcpy(&state, reinterpret_cast<const void*>(unit + 0x24), sizeof(state)) &&
        SafeMemcpy(&token, reinterpret_cast<const void*>(unit + 8), sizeof(token));
    uint8_t result = original;
    if (enhance && (special & 0x40))
        result = IsEnhancedVisionArtifact(token) ? 12 : 1;
    else if (enhance)
        result = EnhancedVisionResult(result, state);
    LeaveObserverCallback();
    return result;
}

static bool ResolveEnhancedVision()
{
    if (g_origUnitVisibility)
        return true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    if (!base)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != 0x6A57F729 || nt->OptionalHeader.SizeOfImage != 0x8204000)
    {
        Log("[!] EnhancedVision: unsupported executable fingerprint\n");
        return false;
    }
    const uintptr_t entry = base + 0x1CD7A30;
    const uintptr_t auxiliary = base + 0x235E193;
    static constexpr uint8_t auxiliaryContext[] = {
        0x8B, 0x4B, 0xFC, 0xE8, 0xA5, 0x00, 0x0D, 0x00, 0xBE, 0x02, 0x00, 0x00, 0x00,
    };
    if (!VisionBytesEqual(entry, VISION_ENTRY_ORIGINAL, sizeof(VISION_ENTRY_ORIGINAL)) ||
        !VisionBytesEqual(auxiliary, auxiliaryContext, sizeof(auxiliaryContext)) ||
        !VisionBytesEqual(base + 0x1CD7A79, ENHANCED_VISION_MASK_ORIGINAL, sizeof(ENHANCED_VISION_MASK_ORIGINAL)))
    {
        Log("[!] EnhancedVision: code mismatch or another trainer owns the patch\n");
        return false;
    }
    const MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(entry),
        reinterpret_cast<void*>(&HookUnitVisibility), reinterpret_cast<void**>(&g_origUnitVisibility));
    if (status != MH_OK)
    {
        Log("[!] EnhancedVision: create hook failed: %s\n", MH_StatusToString(status));
        return false;
    }
    g_enhancedVisionEntry = entry;
    g_enhancedVisionAux = auxiliary;
    g_enhancedVisionPlayer = base + 0x3BB8BD0;
    return true;
}

static uintptr_t ResolveEnhancedVisionFogState()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    uint32_t lowKey = 0;
    uint32_t lowValue = 0;
    uint32_t highKey = 0;
    uint32_t highValue = 0;
    if (!base ||
        !SafeMemcpy(&lowKey, reinterpret_cast<const void*>(base + 0x3F94B88), sizeof(lowKey)) ||
        !SafeMemcpy(&lowValue, reinterpret_cast<const void*>(base + 0x3EAF0B4), sizeof(lowValue)) ||
        !SafeMemcpy(&highKey, reinterpret_cast<const void*>(base + 0x3EAD420), sizeof(highKey)) ||
        !SafeMemcpy(&highValue, reinterpret_cast<const void*>(base + 0x3F94B8C), sizeof(highValue)))
        return 0;
    const uint32_t low = ~lowKey + lowValue;
    const uint32_t high = highKey + highValue;
    return static_cast<uintptr_t>((uint64_t{high} << 32) | low);
}

bool IsObserverVisionRuntimeReady()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    const uintptr_t state = ResolveEnhancedVisionFogState();
    uint32_t bounds[4] = {};
    uint8_t localPlayer = 0xFF;
    return base && state &&
        SafeMemcpy(bounds, reinterpret_cast<const void*>(base + 0x4470DD0), sizeof(bounds)) &&
        bounds[0] == 0 && bounds[1] == 0 &&
        bounds[2] >= 2 && bounds[2] <= 256 && bounds[3] >= 2 && bounds[3] <= 256 &&
        SafeMemcpy(&localPlayer, reinterpret_cast<const void*>(base + 0x3BB8BD0), 1) &&
        localPlayer < 16;
}

static uintptr_t __fastcall HookObserverFogView(uint32_t first, uint32_t second, uint8_t allPlayers)
{
    EnterObserverCallback();
    AcquireSRWLockExclusive(&g_observerFogLock);
    uint8_t localPlayer = 0xFF;
    const uintptr_t state = ResolveEnhancedVisionFogState();
    const bool playerReady = SafeMemcpy(&localPlayer,
        reinterpret_cast<const void*>(g_enhancedVisionPlayer), sizeof(localPlayer));
    if (InterlockedCompareExchange(&g_observerFogInvocationLogged, 1, 0) == 0)
        Log("[*] ObserverVision fog entry: player=%u allPlayers=%u state=0x%llX mode=%ld\n",
            static_cast<unsigned>(localPlayer), static_cast<unsigned>(allPlayers),
            static_cast<unsigned long long>(state),
            InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0));
    const bool overrideView = !IsDllUnloading() && state && !allPlayers &&
        InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) == 2 &&
        playerReady && localPlayer < 16;
    uintptr_t result = 0;
    __try
    {
        result = g_originalFogView(first, second, overrideView ? 1 : allPlayers);
        if (overrideView)
        {
            if (g_observerFogState != state)
                Log("[*] ObserverVision: native combined fog; opacity unchanged\n");
            g_observerFogFirst = first;
            g_observerFogSecond = second;
            g_observerFogState = state;
        }
        else
        {
            g_observerFogState = 0;
        }
    }
    __finally
    {
        ReleaseSRWLockExclusive(&g_observerFogLock);
        LeaveObserverCallback();
    }
    return result;
}

static uint8_t ObserverFogReadPlayer(uint8_t player, uintptr_t caller, uintptr_t expectedCallerRva)
{
    if (IsDllUnloading() || player >= 16 ||
        InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) != 2)
        return player;
    const uintptr_t base = g_enhancedVisionPlayer - 0x3BB8BD0;
    uint8_t localPlayer = 0xFF;
    uint8_t renderPlayer = 0xFF;
    if (caller != base + expectedCallerRva ||
        !SafeMemcpy(&localPlayer, reinterpret_cast<const void*>(g_enhancedVisionPlayer), 1) ||
        player != localPlayer ||
        !SafeMemcpy(&renderPlayer, reinterpret_cast<const void*>(base + 0x446A950), 1) ||
        renderPlayer != 16)
        return player;
    if (InterlockedCompareExchange(&g_observerFogReaderLogged, 1, 0) == 0)
        Log("[*] ObserverVision: fog renderer reads combined player buffer; local=%u\n",
            static_cast<unsigned>(localPlayer));
    return 16;
}

static uintptr_t __fastcall HookObserverFogBuffer(uint8_t player, uintptr_t rectangle, uintptr_t output)
{
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    EnterObserverCallback();
    const uintptr_t result = g_originalFogBuffer(
        ObserverFogReadPlayer(player, caller, 0xB78C1B), rectangle, output);
    LeaveObserverCallback();
    return result;
}

static uintptr_t __fastcall HookObserverFogTexture(uintptr_t output, uint8_t player)
{
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    EnterObserverCallback();
    const uintptr_t result = g_originalFogTexture(output,
        ObserverFogReadPlayer(player, caller, 0xB4214F));
    LeaveObserverCallback();
    return result;
}

static bool PrepareObserverFogReaders()
{
    static constexpr uint8_t bufferEntry[] = {
        0x8B, 0x05, 0x7A, 0x02, 0xA8, 0x03, 0x4C, 0x8B, 0xD2,
        0x33, 0x05, 0xC9, 0xD7, 0x4A, 0x03
    };
    static constexpr uint8_t textureEntry[] = {
        0x0F, 0xB6, 0x05, 0xE9, 0x9C, 0xA6, 0x03, 0x80, 0xFA, 0x10,
        0x44, 0x0F, 0xB6, 0xC2
    };
    struct ReaderHook
    {
        uintptr_t rva;
        void* callback;
        void** original;
        const uint8_t* bytes;
        size_t size;
    };
    const ReaderHook readers[] = {
        {0xA00B80, reinterpret_cast<void*>(&HookObserverFogBuffer),
         reinterpret_cast<void**>(&g_originalFogBuffer), bufferEntry, sizeof(bufferEntry)},
        {0xA00C60, reinterpret_cast<void*>(&HookObserverFogTexture),
         reinterpret_cast<void**>(&g_originalFogTexture), textureEntry, sizeof(textureEntry)},
    };
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    if (!base)
        return false;
    for (const auto& reader : readers)
    {
        const uintptr_t entry = base + reader.rva;
        if (!VisionBytesEqual(entry, reader.bytes, reader.size) ||
            (!*reader.original && MH_CreateHook(reinterpret_cast<void*>(entry),
                reader.callback, reader.original) != MH_OK))
            return false;
    }
    g_observerFogBufferEntry = base + readers[0].rva;
    g_observerFogTextureEntry = base + readers[1].rva;
    return true;
}

static bool ApplyObserverHookState(bool enable)
{
    const uintptr_t targets[] = {
        g_observerPlayersEntry,
        g_observerFogEntry,
        g_observerViewPlayerEntry,
        g_observerFogBufferEntry,
        g_observerFogTextureEntry,
        g_enhancedVisionEntry,
    };
    for (const uintptr_t target : targets)
    {
        if (!target)
            continue;
        const MH_STATUS status = enable
            ? MH_QueueEnableHook(reinterpret_cast<void*>(target))
            : MH_QueueDisableHook(reinterpret_cast<void*>(target));
        if (status != MH_OK)
        {
            Log("[!] ObserverVision: queue %s failed at 0x%llX: %s\n",
                enable ? "enable" : "disable",
                static_cast<unsigned long long>(target), MH_StatusToString(status));
            return false;
        }
    }
    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK)
    {
        Log("[!] ObserverVision: applying queued hook state failed: %s\n",
            MH_StatusToString(applied));
        return false;
    }
    g_observerPlayersEnabled = enable && g_observerPlayersEntry;
    g_observerFogEnabled = enable && g_observerFogEntry;
    g_observerViewPlayerEnabled = enable && g_observerViewPlayerEntry;
    g_observerFogReadersEnabled = enable &&
        g_observerFogBufferEntry && g_observerFogTextureEntry;
    g_enhancedVisionHookEnabled = enable && g_enhancedVisionEntry;
    if (enable)
        InterlockedExchange(&g_observerFogReaderLogged, 0);
    return true;
}

static bool RestoreObserverFogView()
{
    bool restored = true;
    AcquireSRWLockExclusive(&g_observerFogLock);
    if (g_observerFogState)
    {
        const uintptr_t state = ResolveEnhancedVisionFogState();
        if (!state)
            restored = false;
        else if (state == g_observerFogState)
        {
            __try
            {
                g_originalFogView(g_observerFogFirst, g_observerFogSecond, 0);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                restored = false;
            }
        }
        if (restored)
        {
            g_observerFogState = 0;
            Log("[-] ObserverVision: native fog selection restored\n");
        }
    }
    ReleaseSRWLockExclusive(&g_observerFogLock);
    return restored;
}

static bool RestoreEnhancedVisionFog();

static bool InitializeObserverFogView()
{
    const uintptr_t base = g_enhancedVisionPlayer - 0x3BB8BD0;
    const uintptr_t state = ResolveEnhancedVisionFogState();
    uint32_t bounds[4] = {};
    uint8_t localPlayer = 0xFF;
    uint8_t originalAlpha = 0;
    if (!state || !g_originalFogView || !g_playerSet ||
        !SafeMemcpy(bounds, reinterpret_cast<const void*>(base + 0x4470DD0), sizeof(bounds)) ||
        bounds[0] != 0 || bounds[1] != 0 ||
        bounds[2] < 2 || bounds[2] > 256 || bounds[3] < 2 || bounds[3] > 256 ||
        !SafeMemcpy(&localPlayer, reinterpret_cast<const void*>(g_enhancedVisionPlayer), 1) ||
        localPlayer >= 16 ||
        !SafeMemcpy(&originalAlpha, reinterpret_cast<const void*>(state + 0x20 + localPlayer), 1))
    {
        Log("[!] ObserverVision: current fog dimensions/state could not be verified\n");
        return false;
    }

    bool applied = false;
    AcquireSRWLockExclusive(&g_observerFogLock);
    __try
    {
        __try
        {
            g_observerFogFirst = bounds[2];
            g_observerFogSecond = bounds[3];
            g_observerFogState = state;
            g_originalFogView(bounds[2], bounds[3], 1);
            uint32_t encodedMask = 0;
            uint32_t maskKey = 0;
            uint8_t renderPlayer = 0xFF;
            uint8_t currentAlpha = 0;
            const uint32_t players = g_playerSet(1) | g_playerSet(2);
            applied = players != 0 &&
                SafeMemcpy(&encodedMask, reinterpret_cast<const void*>(base + 0x4480E28), 4) &&
                SafeMemcpy(&maskKey, reinterpret_cast<const void*>(base + 0x3EAFFD8), 4) &&
                SafeMemcpy(&renderPlayer, reinterpret_cast<const void*>(base + 0x446A950), 1) &&
                SafeMemcpy(&currentAlpha, reinterpret_cast<const void*>(state + 0x20 + localPlayer), 1) &&
                ((encodedMask - maskKey - 0x33A7EAB5u) & players) == players &&
                (renderPlayer == 16 || (players & (players - 1)) == 0) &&
                currentAlpha == originalAlpha;
            Log("[*] ObserverVision fog initialized: size=%ux%u mask=0x%08X player=%u alpha=%u verified=%d\n",
                bounds[2], bounds[3], encodedMask - maskKey - 0x33A7EAB5u,
                static_cast<unsigned>(renderPlayer), static_cast<unsigned>(currentAlpha), applied);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] ObserverVision: native fog initialization raised an exception\n");
        }
    }
    __finally
    {
        ReleaseSRWLockExclusive(&g_observerFogLock);
    }
    return applied;
}

static bool ApplyEnhancedVisionFog()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    static constexpr uint8_t terrainEntry[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xDA, 0x85, 0xC9,
    };
    static constexpr uint8_t alphaEntry[] = {
        0x40, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00, 0x8B, 0xD9,
    };
    static constexpr uint8_t refreshEntry[] = {
        0x48, 0x83, 0xEC, 0x28, 0x8B, 0x05, 0x5E, 0xE2, 0x4A, 0x03,
    };
    if (!base ||
        !VisionBytesEqual(base + 0x242CFD0, terrainEntry, sizeof(terrainEntry)) ||
        !VisionBytesEqual(base + 0x242E240, alphaEntry, sizeof(alphaEntry)) ||
        !VisionBytesEqual(base + 0xA00B20, refreshEntry, sizeof(refreshEntry)))
        return false;
    const uintptr_t state = ResolveEnhancedVisionFogState();
    uint8_t player = 0xFF;
    uint32_t currentMask = 0;
    uint8_t currentAlpha = 0;
    if (!state ||
        !SafeMemcpy(&player, reinterpret_cast<const void*>(g_enhancedVisionPlayer), sizeof(player)) ||
        player >= 16 ||
        !SafeMemcpy(&currentMask, reinterpret_cast<const void*>(state + 0x10), sizeof(currentMask)) ||
        !SafeMemcpy(&currentAlpha, reinterpret_cast<const void*>(state + 0x20 + player), sizeof(currentAlpha)))
        return false;
    if (state == g_enhancedVisionFogState && player != g_enhancedVisionFogPlayer)
        return RestoreEnhancedVisionFog() && ApplyEnhancedVisionFog();
    const bool sameState = state == g_enhancedVisionFogState;
    const uint8_t targetAlpha = 127;
    if (sameState && currentMask == UINT32_MAX && currentAlpha == targetAlpha)
        return true;
    if (!sameState || currentMask != UINT32_MAX)
        g_enhancedVisionFogMask = currentMask;
    if (!sameState || currentAlpha != 127)
        g_enhancedVisionFogAlpha = currentAlpha;
    g_enhancedVisionFogPlayer = player;
    g_enhancedVisionFogState = state;
    __try
    {
        reinterpret_cast<void(__fastcall*)(uint32_t, uint8_t)>(base + 0x242CFD0)(0, 0);
        reinterpret_cast<void(__fastcall*)(uint32_t, int32_t)>(base + 0x242E240)(player, 204800);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    const uint32_t mask = UINT32_MAX;
    const uint8_t alpha = targetAlpha;
    const bool applied = ResolveEnhancedVisionFogState() == state &&
        VisionBytesEqual(state + 0x10, reinterpret_cast<const uint8_t*>(&mask), sizeof(mask)) &&
        VisionBytesEqual(state + 0x20 + player, &alpha, sizeof(alpha));
    if (applied)
        Log("[*] EnhancedVision fog: applied=%d player=%u savedMask=0x%08X savedAlpha=%u\n",
            applied, static_cast<unsigned>(player), g_enhancedVisionFogMask,
            static_cast<unsigned>(g_enhancedVisionFogAlpha));
    return applied;
}

static bool RestoreEnhancedVisionFog()
{
    if (!g_enhancedVisionFogState)
        return true;
    const uintptr_t current = ResolveEnhancedVisionFogState();
    if (!current)
        return false;
    if (current != g_enhancedVisionFogState)
    {
        Log("[*] EnhancedVision fog: state replaced; not writing the previous session address\n");
        g_enhancedVisionFogState = 0;
        return true;
    }
    uint32_t currentMask = 0;
    uint8_t currentAlpha = 0;
    if (!SafeMemcpy(&currentMask, reinterpret_cast<const void*>(current + 0x10), sizeof(currentMask)) ||
        !SafeMemcpy(&currentAlpha, reinterpret_cast<const void*>(current + 0x20 + g_enhancedVisionFogPlayer), sizeof(currentAlpha)))
        return false;
    if (currentMask != UINT32_MAX)
        g_enhancedVisionFogMask = currentMask;
    if (currentAlpha != 127)
        g_enhancedVisionFogAlpha = currentAlpha;
    if (!SafeMemcpy(reinterpret_cast<void*>(current + 0x10), &g_enhancedVisionFogMask,
        sizeof(g_enhancedVisionFogMask)) ||
        !SafeMemcpy(reinterpret_cast<void*>(current + 0x20 + g_enhancedVisionFogPlayer),
            &g_enhancedVisionFogAlpha, sizeof(g_enhancedVisionFogAlpha)))
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    __try
    {
        reinterpret_cast<void(__fastcall*)()>(base + 0xA00B20)();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    if (!VisionBytesEqual(current + 0x10, reinterpret_cast<const uint8_t*>(&g_enhancedVisionFogMask), sizeof(g_enhancedVisionFogMask)) ||
        !VisionBytesEqual(current + 0x20 + g_enhancedVisionFogPlayer, &g_enhancedVisionFogAlpha, sizeof(g_enhancedVisionFogAlpha)))
        return false;
    g_enhancedVisionFogState = 0;
    Log("[-] EnhancedVision fog restored\n");
    return true;
}

bool RestoreEnhancedVisionMode()
{
    InterlockedExchange(&g_enhancedVisionActive, 0);
    if (!ApplyObserverHookState(false))
        return false;
    ObserverCallbackRundown callbackRundown;
    if (g_observerDisplayPatched)
    {
        if (!VisionBytesEqual(g_observerDisplaySelection, OBSERVER_DISPLAY_PATCH, sizeof(OBSERVER_DISPLAY_PATCH)) ||
            !WriteCodeBytes(g_observerDisplaySelection, OBSERVER_DISPLAY_ORIGINAL, sizeof(OBSERVER_DISPLAY_ORIGINAL)) ||
            !VisionBytesEqual(g_observerDisplaySelection, OBSERVER_DISPLAY_ORIGINAL, sizeof(OBSERVER_DISPLAY_ORIGINAL)))
            return false;
        g_observerDisplayPatched = false;
    }
    if (g_observerFogEntry &&
        !VisionBytesEqual(g_observerFogEntry, OBSERVER_FOG_ORIGINAL, sizeof(OBSERVER_FOG_ORIGINAL)))
        return false;
    if (!RestoreObserverFogView())
        return false;
    if (g_observerPlayersEntry &&
        !VisionBytesEqual(g_observerPlayersEntry, OBSERVER_PLAYERS_ORIGINAL, sizeof(OBSERVER_PLAYERS_ORIGINAL)))
    {
        Log("[!] ObserverVision: players entry restoration could not be verified\n");
        return false;
    }
    if (g_enhancedVisionEntry &&
        !VisionBytesEqual(g_enhancedVisionEntry, VISION_ENTRY_ORIGINAL, sizeof(VISION_ENTRY_ORIGINAL)))
    {
        Log("[!] EnhancedVision: entry restoration could not be verified\n");
        return false;
    }
    if (!RestoreEnhancedVisionFog())
    {
        Log("[!] EnhancedVision: fog restoration failed\n");
        return false;
    }
    if (g_enhancedVisionAuxPatched)
    {
        if (!VisionBytesEqual(g_enhancedVisionAux, ENHANCED_VISION_AUX_PATCH, sizeof(ENHANCED_VISION_AUX_PATCH)) ||
            !WriteCodeBytes(g_enhancedVisionAux, ENHANCED_VISION_AUX_ORIGINAL, sizeof(ENHANCED_VISION_AUX_ORIGINAL)) ||
            !VisionBytesEqual(g_enhancedVisionAux, ENHANCED_VISION_AUX_ORIGINAL, sizeof(ENHANCED_VISION_AUX_ORIGINAL)))
        {
            Log("[!] EnhancedVision: auxiliary restore failed\n");
            return false;
        }
        g_enhancedVisionAuxPatched = false;
    }
    return true;
}

static bool PrepareObserverPlayersHook()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    static constexpr uint8_t playerSetEntry[] = {
        0x48, 0x83, 0xEC, 0x28, 0x8B, 0xC1, 0x4C, 0x8D, 0x05, 0xA3, 0x73, 0xA9, 0xFF
    };
    if (!base || !VisionBytesEqual(base + 0x568C50, playerSetEntry, sizeof(playerSetEntry)))
        return false;
    g_playerSet = reinterpret_cast<PlayerSetFn>(base + 0x568C50);
    if (!g_observerPlayersEntry)
    {
        const uintptr_t entry = base + 0x9569D0;
        if (!base || !VisionBytesEqual(entry, OBSERVER_PLAYERS_ORIGINAL, sizeof(OBSERVER_PLAYERS_ORIGINAL)))
            return false;
        const MH_STATUS created = MH_CreateHook(reinterpret_cast<void*>(entry),
            reinterpret_cast<void*>(&HookObserverPlayers), reinterpret_cast<void**>(&g_originalObserverPlayers));
        if (created != MH_OK)
            return false;
        g_observerPlayersEntry = entry;
    }
    if (!VisionBytesEqual(g_observerPlayersEntry, OBSERVER_PLAYERS_ORIGINAL, sizeof(OBSERVER_PLAYERS_ORIGINAL)))
        return false;
    return true;
}

static bool PrepareObserverFogHook()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    const uintptr_t entry = base + 0xA02A20;
    if (!base || !VisionBytesEqual(entry, OBSERVER_FOG_ORIGINAL, sizeof(OBSERVER_FOG_ORIGINAL)))
        return false;
    if (!g_originalFogView)
    {
        if (MH_CreateHook(reinterpret_cast<void*>(entry), reinterpret_cast<void*>(&HookObserverFogView),
            reinterpret_cast<void**>(&g_originalFogView)) != MH_OK)
            return false;
        g_observerFogEntry = entry;
    }
    InterlockedExchange(&g_observerFogInvocationLogged, 0);
    return true;
}

static bool PrepareObserverViewPlayerHook()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
    const uintptr_t entry = base + 0x92BDE0;
    static constexpr uint8_t expected[] = {
        0x48, 0x83, 0xEC, 0x28, 0xE8, 0xC7, 0x0F, 0x7F, 0xFF,
    };
    if (!base || !VisionBytesEqual(entry, expected, sizeof(expected)))
        return false;
    if (!g_originalViewPlayer)
    {
        if (MH_CreateHook(reinterpret_cast<void*>(entry), reinterpret_cast<void*>(&HookObserverViewPlayer), reinterpret_cast<void**>(&g_originalViewPlayer)) != MH_OK)
            return false;
        g_observerViewPlayerEntry = entry;
    }
    InterlockedExchange(&g_observerViewPlayerLogged, 0);
    return true;
}

bool ApplyEnhancedVisionMode(bool observerSelected)
{
    if (InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) != 0)
        return true;
    if (observerSelected && !IsObserverVisionRuntimeReady())
        return false;
    if (!RestoreEnhancedVisionMode() || !ResolveEnhancedVision())
        return false;
    if (!VisionBytesEqual(g_enhancedVisionEntry, VISION_ENTRY_ORIGINAL, sizeof(VISION_ENTRY_ORIGINAL)) ||
        !VisionBytesEqual(g_enhancedVisionEntry + 0x49, ENHANCED_VISION_MASK_ORIGINAL, sizeof(ENHANCED_VISION_MASK_ORIGINAL)) ||
        !VisionBytesEqual(g_enhancedVisionAux, ENHANCED_VISION_AUX_ORIGINAL, sizeof(ENHANCED_VISION_AUX_ORIGINAL)))
        return false;
    if (observerSelected)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("SC2_x64.exe"));
        g_observerDisplaySelection = base + 0x1CE80B7;
        if (!VisionBytesEqual(g_observerDisplaySelection, OBSERVER_DISPLAY_ORIGINAL, sizeof(OBSERVER_DISPLAY_ORIGINAL)))
            return false;
        if (!PrepareObserverPlayersHook() || !PrepareObserverFogHook() ||
            !PrepareObserverViewPlayerHook() || !PrepareObserverFogReaders())
        {
            RestoreEnhancedVisionMode();
            return false;
        }
        if (!ApplyObserverHookState(true))
        {
            RestoreEnhancedVisionMode();
            return false;
        }
        if (!WriteCodeBytes(g_observerDisplaySelection, OBSERVER_DISPLAY_PATCH, sizeof(OBSERVER_DISPLAY_PATCH)))
        {
            RestoreEnhancedVisionMode();
            return false;
        }
        g_observerDisplayPatched = true;
        InterlockedExchange(&g_observerPlayersQueryLogged, 0);
        InterlockedExchange(&g_observerVisibilityResultLogged, 0);
        for (volatile LONG& logged : g_observerOwnerResultLogged)
            InterlockedExchange(&logged, 0);
        InterlockedExchange(&g_enhancedVisionActive, 2);
        if (!InitializeObserverFogView())
        {
            RestoreEnhancedVisionMode();
            return false;
        }
        Log("[+] ObserverVision enabled; native player union, fog opacity unchanged\n");
        return true;
    }
    if (!WriteCodeBytes(g_enhancedVisionAux, ENHANCED_VISION_AUX_PATCH, sizeof(ENHANCED_VISION_AUX_PATCH)))
        return false;
    g_enhancedVisionAuxPatched = true;
    const MH_STATUS status = MH_EnableHook(reinterpret_cast<void*>(g_enhancedVisionEntry));
    if (status != MH_OK)
    {
        Log("[!] EnhancedVision: enable failed: %s\n", MH_StatusToString(status));
        RestoreEnhancedVisionMode();
        return false;
    }
    g_enhancedVisionHookEnabled = true;
    if (!ApplyEnhancedVisionFog())
    {
        Log("[!] EnhancedVision: fog initialization failed\n");
        RestoreEnhancedVisionMode();
        return false;
    }
    InterlockedExchange(&g_enhancedVisionActive, 1);
    Log("[+] EnhancedVision enabled\n");
    return true;
}

bool IsEnhancedVisionModeApplied(bool observerSelected)
{
    return (g_enhancedVisionAuxPatched && g_enhancedVisionHookEnabled &&
            InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) == 1) ||
        (observerSelected && g_observerPlayersEnabled && g_observerFogEnabled &&
            g_observerFogReadersEnabled && g_observerViewPlayerEnabled &&
            g_observerDisplayPatched && g_enhancedVisionHookEnabled &&
            InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) == 2);
}

bool RefreshEnhancedVisionRuntime(bool enhancedSelected, bool fullMapVisionEnabled, bool mapAllowed)
{
    if (!enhancedSelected || !fullMapVisionEnabled || !mapAllowed ||
        g_enhancedVisionHookEnabled == false || g_enhancedVisionAuxPatched == false ||
        InterlockedCompareExchange(&g_enhancedVisionActive, 0, 0) != 1)
        return true;
    return ApplyEnhancedVisionFog();
}