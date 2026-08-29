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

// IDirect3DDevice9::Present signature (vtable[17])
using DevicePresentFn = HRESULT(WINAPI*)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

// IDirect3DDevice9Ex::PresentEx signature (vtable[121])
using DevicePresentExFn = HRESULT(WINAPI*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

// IDirect3DDevice9::EndScene signature (vtable[42])
using DeviceEndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);

// IDirect3DDevice9::Reset signature (vtable[16])
using DeviceResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static SwapChainPresentFn g_origSwapChainPresent = nullptr;
static DevicePresentFn    g_origDevicePresent = nullptr;
static DevicePresentExFn  g_origDevicePresentEx = nullptr;
static DeviceEndSceneFn   g_origDeviceEndScene = nullptr;
static DeviceResetFn      g_origDeviceReset = nullptr;
static volatile LONG      g_swapChainPresentCalls = 0;
static volatile LONG      g_devicePresentCalls = 0;
static volatile LONG      g_devicePresentExCalls = 0;
static volatile LONG      g_deviceEndSceneCalls = 0;
static volatile LONG      g_presentResultLogged = 0;

static void RenderFromSwapChain(IDirect3DSwapChain9* swapChain)
{
    if (!IsDllUnloading() && !IsOverlayReady())
    {
        __try
        {
            InitializeOverlay(swapChain);
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
}

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

    const LONG presentCall = InterlockedIncrement(&g_swapChainPresentCalls);
    if (presentCall == 1)
    {
        Log("[+] First D3D9 SwapChain::Present callback: swapChain=%p "
            "destWindow=%p flags=0x%08lX\n",
            pSwapChain, hDestWindowOverride, dwFlags);
    }

    if (InterlockedCompareExchange(&g_deviceEndSceneCalls, 0, 0) == 0)
        RenderFromSwapChain(pSwapChain);

    HRESULT result = g_origSwapChainPresent(
        pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    if (InterlockedCompareExchange(&g_presentResultLogged, 1, 0) == 0)
        Log("[*] First original D3D9 SwapChain::Present result: 0x%08X\n", result);
    else if (FAILED(result) && result != D3DERR_WASSTILLDRAWING)
        Log("[!] D3D9 SwapChain::Present failed: 0x%08X\n", result);
    LeaveHookCallback();
    return result;
}

// ─── Hooked Device::Present ───

static HRESULT WINAPI HookedDevicePresent(
    IDirect3DDevice9* pDevice,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion)
{
    EnterHookCallback();

    const LONG presentCall = InterlockedIncrement(&g_devicePresentCalls);
    if (presentCall == 1)
        Log("[+] First D3D9 Device::Present callback: device=%p destWindow=%p\n",
            pDevice, hDestWindowOverride);

    IDirect3DSwapChain9* swapChain = nullptr;
    const HRESULT swapChainHr = pDevice->GetSwapChain(0, &swapChain);
    if (SUCCEEDED(swapChainHr) && swapChain)
    {
        if (InterlockedCompareExchange(&g_deviceEndSceneCalls, 0, 0) == 0)
            RenderFromSwapChain(swapChain);
        swapChain->Release();
    }
    else if (presentCall == 1)
    {
        Log("[!] Device::Present GetSwapChain failed: 0x%08X\n", swapChainHr);
    }

    const HRESULT result = g_origDevicePresent(
        pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    if (FAILED(result) && result != D3DERR_WASSTILLDRAWING)
        Log("[!] D3D9 Device::Present failed: 0x%08X\n", result);

    LeaveHookCallback();
    return result;
}

// ─── Hooked Device9Ex::PresentEx ───

static HRESULT WINAPI HookedDevicePresentEx(
    IDirect3DDevice9Ex* pDevice,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
    DWORD dwFlags)
{
    EnterHookCallback();

    const LONG presentCall = InterlockedIncrement(&g_devicePresentExCalls);
    if (presentCall == 1)
        Log("[+] First D3D9Ex Device::PresentEx callback: device=%p "
            "destWindow=%p flags=0x%08lX\n",
            pDevice, hDestWindowOverride, dwFlags);

    IDirect3DSwapChain9* swapChain = nullptr;
    const HRESULT swapChainHr = pDevice->GetSwapChain(0, &swapChain);
    if (SUCCEEDED(swapChainHr) && swapChain)
    {
        if (InterlockedCompareExchange(&g_deviceEndSceneCalls, 0, 0) == 0)
            RenderFromSwapChain(swapChain);
        swapChain->Release();
    }
    else if (presentCall == 1)
    {
        Log("[!] Device::PresentEx GetSwapChain failed: 0x%08X\n", swapChainHr);
    }

    const HRESULT result = g_origDevicePresentEx(
        pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    if (FAILED(result) && result != D3DERR_WASSTILLDRAWING)
        Log("[!] D3D9Ex Device::PresentEx failed: 0x%08X\n", result);

    LeaveHookCallback();
    return result;
}

// ─── Hooked Device::EndScene ───

static HRESULT WINAPI HookedDeviceEndScene(IDirect3DDevice9* pDevice)
{
    EnterHookCallback();

    const LONG endSceneCall = InterlockedIncrement(&g_deviceEndSceneCalls);
    if (endSceneCall == 1)
        Log("[+] First D3D9 Device::EndScene callback: device=%p\n", pDevice);

    IDirect3DSwapChain9* swapChain = nullptr;
    const HRESULT swapChainHr = pDevice->GetSwapChain(0, &swapChain);
    if (SUCCEEDED(swapChainHr) && swapChain)
    {
        RenderFromSwapChain(swapChain);
        swapChain->Release();
    }
    else if (endSceneCall == 1)
    {
        Log("[!] Device::EndScene GetSwapChain failed: 0x%08X\n", swapChainHr);
    }

    const HRESULT result = g_origDeviceEndScene(pDevice);
    if (FAILED(result))
        Log("[!] D3D9 Device::EndScene failed: 0x%08X\n", result);

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
    {
        Log("[+] Device::Reset succeeded: windowed=%d backBuffer=%ux%u format=%u hwnd=%p\n",
            pPresentationParameters ? pPresentationParameters->Windowed : 0,
            pPresentationParameters ? pPresentationParameters->BackBufferWidth : 0,
            pPresentationParameters ? pPresentationParameters->BackBufferHeight : 0,
            pPresentationParameters ? pPresentationParameters->BackBufferFormat : 0,
            pPresentationParameters ? pPresentationParameters->hDeviceWindow : nullptr);
        OnDeviceReset();
    }
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
    uintptr_t devicePresent;    // IDirect3DDevice9::Present     vtable[17]
    uintptr_t devicePresentEx;  // IDirect3DDevice9Ex::PresentEx vtable[121]
    uintptr_t deviceEndScene;   // IDirect3DDevice9::EndScene    vtable[42]
};

// 重复加载修复：缓存保存在 SC2 进程环境中，因此 DLL 卸载后仍然存在，
// SC2 进程退出时又会自动失效，不会把一个进程的绝对地址带到下一个进程。
static constexpr const wchar_t* kD3D9AddressCache =
    L"COOP_INJECTIONS_D3D9_HOOK_ADDRESSES";

// 复用缓存前确认地址仍指向已提交的可执行页面，避免损坏或过期的环境变量
// 被直接交给 MinHook。
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

    // 第二次及后续加载优先复用第一次解析出的虚表地址，避免在已有游戏设备
    // 运行时创建另一个 HAL 设备并持续收到 D3DERR_DEVICELOST。
static HookAddresses LoadCachedD3D9Addresses()
{
    wchar_t value[128]{};
    if (!GetEnvironmentVariableW(kD3D9AddressCache, value, _countof(value)))
        return {};

    unsigned long long present = 0;
    unsigned long long reset = 0;
    unsigned long long devicePresent = 0;
    unsigned long long devicePresentEx = 0;
    unsigned long long deviceEndScene = 0;
    if (swscanf_s(value, L"%llx,%llx,%llx,%llx,%llx",
        &present, &reset, &devicePresent, &devicePresentEx, &deviceEndScene) != 5)
        return {};

    HookAddresses addresses{
        static_cast<uintptr_t>(present),
        static_cast<uintptr_t>(reset),
        static_cast<uintptr_t>(devicePresent),
        static_cast<uintptr_t>(devicePresentEx),
        static_cast<uintptr_t>(deviceEndScene),
    };
    if (!IsExecutableAddress(addresses.swapChainPresent) ||
        !IsExecutableAddress(addresses.deviceReset) ||
        !IsExecutableAddress(addresses.devicePresent) ||
        !IsExecutableAddress(addresses.deviceEndScene) ||
        (addresses.devicePresentEx && !IsExecutableAddress(addresses.devicePresentEx)))
        return {};

    Log("[+] Reusing cached D3D9 hooks: SwapChainPresent=0x%llX "
        "Reset=0x%llX DevicePresent=0x%llX PresentEx=0x%llX EndScene=0x%llX\n",
        present, reset, devicePresent, devicePresentEx, deviceEndScene);
    return addresses;
}

// SetEnvironmentVariableW 修改的是当前 SC2 进程环境；它不依赖 DLL 自身的
// 静态存储，所以 FreeLibrary 后仍可供下一次注入读取。
static void CacheD3D9Addresses(const HookAddresses& addresses)
{
    wchar_t value[128]{};
    swprintf_s(value, L"%llX,%llX,%llX,%llX,%llX",
        static_cast<unsigned long long>(addresses.swapChainPresent),
        static_cast<unsigned long long>(addresses.deviceReset),
        static_cast<unsigned long long>(addresses.devicePresent),
        static_cast<unsigned long long>(addresses.devicePresentEx),
        static_cast<unsigned long long>(addresses.deviceEndScene));
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
    // 首次解析仍需临时设备。优先使用与游戏一致的 HAL；若 HAL 被当前显示
    // 状态占用，则依次尝试不占用硬件显示设备的 REF 和 NULLREF。
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
    addrs.devicePresent    = reinterpret_cast<uintptr_t>(devVtbl[17]);
    addrs.deviceEndScene   = reinterpret_cast<uintptr_t>(devVtbl[42]);

    IDirect3D9Ex* d3d9Ex = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    const HRESULT create9ExHr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
    if (SUCCEEDED(create9ExHr) && d3d9Ex)
    {
        D3DPRESENT_PARAMETERS ppEx = pp;
        const HRESULT createDeviceExHr = d3d9Ex->CreateDeviceEx(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &ppEx, nullptr, &deviceEx);
        if (SUCCEEDED(createDeviceExHr) && deviceEx)
        {
            void** devExVtbl = *reinterpret_cast<void***>(deviceEx);
            addrs.devicePresentEx = reinterpret_cast<uintptr_t>(devExVtbl[121]);
            Log("[+] IDirect3DDevice9Ex::PresentEx = 0x%llX\n",
                static_cast<unsigned long long>(addrs.devicePresentEx));
        }
        else
        {
            Log("[!] CreateDeviceEx(HAL) failed: 0x%08X; PresentEx hook unavailable\n",
                createDeviceExHr);
        }
    }
    else
    {
        Log("[!] Direct3DCreate9Ex failed: 0x%08X; PresentEx hook unavailable\n",
            create9ExHr);
    }
    // 必须在释放临时设备前保存函数入口，使同一 SC2 进程后续重载不再依赖
    // 临时设备能否创建成功。
    CacheD3D9Addresses(addrs);

    Log("[+] IDirect3DSwapChain9::Present = 0x%llX\n",
        static_cast<unsigned long long>(addrs.swapChainPresent));
    Log("[+] IDirect3DDevice9::Reset = 0x%llX\n",
        static_cast<unsigned long long>(addrs.deviceReset));
    Log("[+] IDirect3DDevice9::Present = 0x%llX\n",
        static_cast<unsigned long long>(addrs.devicePresent));
    Log("[+] IDirect3DDevice9::EndScene = 0x%llX\n",
        static_cast<unsigned long long>(addrs.deviceEndScene));

    swapChain->Release();
    if (deviceEx) deviceEx->Release();
    if (d3d9Ex) d3d9Ex->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, wc.hInstance);

    return addrs;
}

// ─── Public API ───

bool HasD3D9PresentHookFired()
{
    return InterlockedCompareExchange(&g_swapChainPresentCalls, 0, 0) != 0 ||
        InterlockedCompareExchange(&g_devicePresentCalls, 0, 0) != 0 ||
        InterlockedCompareExchange(&g_devicePresentExCalls, 0, 0) != 0 ||
        InterlockedCompareExchange(&g_deviceEndSceneCalls, 0, 0) != 0;
}

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
    if (!addrs.swapChainPresent || !addrs.deviceReset ||
        !addrs.devicePresent || !addrs.deviceEndScene)
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

    if (addrs.devicePresentEx)
    {
        st = MH_CreateHook(
            reinterpret_cast<LPVOID>(addrs.devicePresentEx),
            &HookedDevicePresentEx,
            reinterpret_cast<LPVOID*>(&g_origDevicePresentEx));
        if (st != MH_OK)
        {
            Log("[!] MH_CreateHook PresentEx failed: %s\n", MH_StatusToString(st));
            MH_Uninitialize();
            return false;
        }
    }

    st = MH_CreateHook(
        reinterpret_cast<LPVOID>(addrs.deviceEndScene),
        &HookedDeviceEndScene,
        reinterpret_cast<LPVOID*>(&g_origDeviceEndScene));
    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook EndScene failed: %s\n", MH_StatusToString(st));
        MH_Uninitialize();
        return false;
    }

    // Some D3D9 clients render continuously through IDirect3DDevice9::Present
    // and call IDirect3DSwapChain9::Present only during transitions.
    st = MH_CreateHook(
        reinterpret_cast<LPVOID>(addrs.devicePresent),
        &HookedDevicePresent,
        reinterpret_cast<LPVOID*>(&g_origDevicePresent));
    if (st != MH_OK)
    {
        Log("[!] MH_CreateHook Device Present failed: %s\n", MH_StatusToString(st));
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

    Log("[+] D3D9 hooks installed: SwapChainPresent=0x%llX "
        "DevicePresent=0x%llX PresentEx=0x%llX EndScene=0x%llX Reset=0x%llX\n",
        static_cast<unsigned long long>(addrs.swapChainPresent),
        static_cast<unsigned long long>(addrs.devicePresent),
        static_cast<unsigned long long>(addrs.devicePresentEx),
        static_cast<unsigned long long>(addrs.deviceEndScene),
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
