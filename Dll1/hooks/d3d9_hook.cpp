#include "pch.h"
#include "hooks/d3d9_hook.h"
#include "render/overlay.h"
#include "core/globals.h"
#include "core/log.h"

#include <MinHook.h>

// IDirect3DSwapChain9::Present signature
using SwapChainPresentFn = HRESULT(WINAPI*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

static SwapChainPresentFn g_origSwapChainPresent = nullptr;

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

// ─── vtable address resolution ───

static uintptr_t GetSwapChainPresentAddress()
{
    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        Log("[!] Direct3DCreate9 failed\n");
        return 0;
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
        return 0;
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
        return 0;
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
        return 0;
    }

    void** vtbl = *reinterpret_cast<void***>(swapChain);
    uintptr_t presentAddr = reinterpret_cast<uintptr_t>(vtbl[3]);

    Log("[+] IDirect3DSwapChain9::Present = 0x%llX\n",
        static_cast<unsigned long long>(presentAddr));

    swapChain->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, wc.hInstance);

    return presentAddr;
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

    uintptr_t presentAddr = GetSwapChainPresentAddress();
    if (!presentAddr)
    {
        Log("[!] Failed to get SwapChain Present address\n");
        MH_Uninitialize();
        return false;
    }

    MH_STATUS st = MH_CreateHook(
        reinterpret_cast<LPVOID>(presentAddr),
        &HookedSwapChainPresent,
        reinterpret_cast<LPVOID*>(&g_origSwapChainPresent));
    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook failed: %s\n", MH_StatusToString(st));
        MH_Uninitialize();
        return false;
    }

    st = MH_EnableHook(reinterpret_cast<LPVOID>(presentAddr));
    if (st != MH_OK)
    {
        Log("[!] MH_EnableHook failed: %s\n", MH_StatusToString(st));
        MH_RemoveHook(reinterpret_cast<LPVOID>(presentAddr));
        MH_Uninitialize();
        return false;
    }

    Log("[+] D3D9 SwapChain::Present hooked @ 0x%llX\n",
        static_cast<unsigned long long>(presentAddr));
    return true;
}

void CleanupHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    ShutdownOverlay();
    Log("[+] Hooks cleanup done\n");
}
