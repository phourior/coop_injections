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
    if (!IsOverlayReady())
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

    if (IsOverlayReady())
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

    return g_origSwapChainPresent(
        pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

// ─── Hooked Device::Reset ───

static HRESULT WINAPI HookedDeviceReset(
    IDirect3DDevice9* pDevice,
    D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    Log("[*] Device::Reset called\n");

    OnDeviceLost();

    HRESULT hr = g_origDeviceReset(pDevice, pPresentationParameters);

    if (SUCCEEDED(hr))
        OnDeviceReset();
    else
        Log("[!] Device::Reset failed: 0x%08X\n", hr);

    return hr;
}

// ─── vtable address resolution ───

struct HookAddresses
{
    uintptr_t swapChainPresent; // IDirect3DSwapChain9::Present  vtable[3]
    uintptr_t deviceReset;      // IDirect3DDevice9::Reset       vtable[16]
};

static HookAddresses GetD3D9Addresses()
{
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
    HRESULT hr = d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);

    if (FAILED(hr) || !device)
    {
        Log("[!] CreateDevice failed: 0x%08X\n", hr);
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
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    ShutdownOverlay();
    Log("[+] Hooks cleanup done\n");
}
