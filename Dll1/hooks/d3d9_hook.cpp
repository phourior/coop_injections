#include "pch.h"
#include "hooks/d3d9_hook.h"
#include "hooks/game_hook.h"
#include "render/overlay.h"
#include "core/globals.h"
#include "core/log.h"

#include "ext/MinHook/include/MinHook.h"

// IDirect3DSwapChain9::Present signature
using SwapChainPresentFn = HRESULT(WINAPI*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

// IDirect3DDevice9::Reset signature (vtable[16])
using DeviceResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static SwapChainPresentFn g_origSwapChainPresent = nullptr;
static DeviceResetFn      g_origDeviceReset = nullptr;

// ─── Hooked SwapChain::Present ───

static HRESULT WINAPI HookedSwapChainPresent(
    IDirect3DSwapChain9* pSwapChain,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
    DWORD dwFlags)
{
    EnterHookCallback();

    if (!IsDllUnloading() && !IsOverlayReady())
    {
        __try
        {
            InitializeOverlay(pSwapChain);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] InitializeOverlay CRASHED: 0x%08X\n", GetExceptionCode());
        }
    }

    if (!IsDllUnloading() && IsOverlayReady())
    {
        __try
        {
            RenderOverlayFrame();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] RenderOverlayFrame CRASHED: 0x%08X\n", GetExceptionCode());
        }
    }

    HRESULT result = g_origSwapChainPresent(
        pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    LeaveHookCallback();
    return result;
}

// ─── Hooked Device::Reset ───

static HRESULT WINAPI HookedDeviceReset(
    IDirect3DDevice9* pDevice,
    D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    EnterHookCallback();
    Log("[*] Device::Reset called\n");

    if (!IsDllUnloading())
        OnDeviceLost();

    HRESULT hr = g_origDeviceReset(pDevice, pPresentationParameters);

    if (!IsDllUnloading() && SUCCEEDED(hr))
        OnDeviceReset();
    else if (!IsDllUnloading())
        Log("[!] Device::Reset failed: 0x%08X\n", hr);

    LeaveHookCallback();
    return hr;
}

// ─── vtable address resolution ───

struct HookAddresses
{
    uintptr_t swapChainPresent; // IDirect3DSwapChain9::Present  vtable[3]
    uintptr_t deviceReset;      // IDirect3DDevice9::Reset       vtable[16]
};

static constexpr const wchar_t* kD3D9AddressCache =
    L"COOP_INJECTIONS_D3D9_HOOK_ADDRESSES";

static bool IsExecutableAddress(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address),
                                  &memory, sizeof(memory)))
        return false;

    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return memory.State == MEM_COMMIT && (memory.Protect & executable) != 0 &&
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

static HookAddresses LoadCachedD3D9Addresses()
{
    wchar_t value[64]{};
    if (!GetEnvironmentVariableW(kD3D9AddressCache, value, _countof(value)))
        return {};

    unsigned long long present = 0;
    unsigned long long reset = 0;
    if (swscanf_s(value, L"%llx,%llx", &present, &reset) != 2)
        return {};

    HookAddresses addresses{
        static_cast<uintptr_t>(present),
        static_cast<uintptr_t>(reset),
    };
    if (!IsExecutableAddress(addresses.swapChainPresent) ||
        !IsExecutableAddress(addresses.deviceReset))
        return {};

    Log("[+] Reusing cached D3D9 hooks: Present=0x%llX Reset=0x%llX\n",
        present, reset);
    return addresses;
}

static void CacheD3D9Addresses(const HookAddresses& addresses)
{
    wchar_t value[64]{};
    swprintf_s(value, L"%llX,%llX",
        static_cast<unsigned long long>(addresses.swapChainPresent),
        static_cast<unsigned long long>(addresses.deviceReset));
    if (!SetEnvironmentVariableW(kD3D9AddressCache, value))
        Log("[!] Failed to cache D3D9 hook addresses: %u\n", GetLastError());
}

static HookAddresses GetD3D9Addresses()
{
    HookAddresses cached = LoadCachedD3D9Addresses();
    if (cached.swapChainPresent && cached.deviceReset)
        return cached;

    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        Log("[!] Direct3DCreate9 failed\n");
        return {};
    }

    const wchar_t* kClassName = L"SC2_DummyD3D9_Class";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"SC2_DummyD3D9",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        Log("[!] CreateWindowExW failed: %u\n", GetLastError());
        d3d9->Release();
        UnregisterClassW(kClassName, wc.hInstance);
        return {};
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = D3DERR_DEVICELOST;
    struct DeviceAttempt
    {
        D3DDEVTYPE type;
        const char* name;
    };
    const DeviceAttempt attempts[] = {
        { D3DDEVTYPE_HAL, "HAL" },
        { D3DDEVTYPE_REF, "REF" },
        { D3DDEVTYPE_NULLREF, "NULLREF" },
    };
    for (const DeviceAttempt& attempt : attempts)
    {
        hr = d3d9->CreateDevice(
            D3DADAPTER_DEFAULT, attempt.type, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
        if (device)
        {
            Log("[+] Created %s dummy D3D9 device\n", attempt.name);
            break;
        }
        Log("[!] CreateDevice(%s) failed: 0x%08X\n", attempt.name, hr);
    }

    if (FAILED(hr) || !device)
    {
        d3d9->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, wc.hInstance);
        return {};
    }

    IDirect3DSwapChain9* swapChain = nullptr;
    hr = device->GetSwapChain(0, &swapChain);
    if (FAILED(hr) || !swapChain)
    {
        Log("[!] GetSwapChain failed: 0x%08X\n", hr);
        device->Release();
        d3d9->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, wc.hInstance);
        return {};
    }

    void** scVtbl = *reinterpret_cast<void***>(swapChain);
    void** devVtbl = *reinterpret_cast<void***>(device);

    HookAddresses addrs{};
    addrs.swapChainPresent = reinterpret_cast<uintptr_t>(scVtbl[3]);
    addrs.deviceReset      = reinterpret_cast<uintptr_t>(devVtbl[16]);
    CacheD3D9Addresses(addrs);

    Log("[+] IDirect3DSwapChain9::Present = 0x%llX\n",
        static_cast<unsigned long long>(addrs.swapChainPresent));
    Log("[+] IDirect3DDevice9::Reset = 0x%llX\n",
        static_cast<unsigned long long>(addrs.deviceReset));

    swapChain->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, wc.hInstance);

    return addrs;
}

// ─── Public API ───

bool SetupHooks()
{
#ifndef _WIN64
    Log("[!] Build x64 for StarCraft II x64 process.\n");
    return false;
#endif

    if (MH_Initialize() != MH_OK)
    {
        Log("[!] MH_Initialize failed\n");
        return false;
    }

    HookAddresses addrs = GetD3D9Addresses();
    if (!addrs.swapChainPresent || !addrs.deviceReset)
    {
        Log("[!] Failed to get D3D9 vtable addresses\n");
        MH_Uninitialize();
        return false;
    }

    // Hook SwapChain::Present
    MH_STATUS st = MH_CreateHook(
        reinterpret_cast<LPVOID>(addrs.swapChainPresent),
        &HookedSwapChainPresent,
        reinterpret_cast<LPVOID*>(&g_origSwapChainPresent));
    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook Present failed: %s\n", MH_StatusToString(st));
        MH_Uninitialize();
        return false;
    }

    // Hook Device::Reset
    st = MH_CreateHook(
        reinterpret_cast<LPVOID>(addrs.deviceReset),
        &HookedDeviceReset,
        reinterpret_cast<LPVOID*>(&g_origDeviceReset));
    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook Reset failed: %s\n", MH_StatusToString(st));
        MH_Uninitialize();
        return false;
    }

    // Hook game functions (OnGameLobbyUpdate etc.)
    if (!SetupGameHooks())
        Log("[!] SetupGameHooks failed (non-fatal, continuing)\n");

    if (!SetupExperienceHooks())
        Log("[!] SetupExperienceHooks failed (non-fatal, continuing)\n");

    // Hook NNet UDP send/recv for custom map packet capture
    if (!SetupNNetHooks())
        Log("[!] SetupNNetHooks failed (non-fatal, continuing)\n");

    // Enable all hooks
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK)
    {
        Log("[!] MH_EnableHook failed: %s\n", MH_StatusToString(st));
        MH_Uninitialize();
        return false;
    }

    // NNet hook 目标在游戏热路径上，默认关闭以避免卡顿；
    // 用户在菜单勾选 "NNet 包捕获" 时再通过 EnableNNetCapture(true) 启用
    DisableNNetHooksAtStartup();

    Log("[+] D3D9 hooks installed: Present=0x%llX Reset=0x%llX\n",
        static_cast<unsigned long long>(addrs.swapChainPresent),
        static_cast<unsigned long long>(addrs.deviceReset));
    return true;
}

void CleanupHooks()
{
    BeginDllUnload();
    const MH_STATUS disableStatus = MH_DisableHook(MH_ALL_HOOKS);
    if (disableStatus != MH_OK && disableStatus != MH_ERROR_NOT_CREATED)
        Log("[!] MH_DisableHook cleanup failed: %s\n", MH_StatusToString(disableStatus));
    DetachOverlayWindowProc();
    WaitForHookCallbacks();

    CleanupGameFeatures();
    const MH_STATUS uninitializeStatus = MH_Uninitialize();
    if (uninitializeStatus != MH_OK && uninitializeStatus != MH_ERROR_NOT_INITIALIZED)
        Log("[!] MH_Uninitialize cleanup failed: %s\n", MH_StatusToString(uninitializeStatus));
    ShutdownOverlay();
    Log("[+] Hooks cleanup done\n");
}
