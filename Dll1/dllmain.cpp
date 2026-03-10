// dllmain.cpp : D3D9 ImGui overlay hook for SC2
// Hooks IDirect3DSwapChain9::Present (vtable[3]) via MinHook
#include "pch.h"

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <MinHook.h>

#include <cstdarg>
#include <cstdio>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}

// IDirect3DSwapChain9::Present signature
// HRESULT Present(const RECT*, const RECT*, HWND, const RGNDATA*, DWORD)
using SwapChainPresentFn = HRESULT(WINAPI*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

static HMODULE              g_hModule = nullptr;
static volatile bool        g_initInProgress = false;
static volatile bool        g_imguiInitialized = false;
static bool                 g_showMenu = true;

static IDirect3DDevice9*    g_device = nullptr;
static IDirect3DSwapChain9* g_swapChain = nullptr;
static HWND                 g_hWnd = nullptr;
static WNDPROC              g_origWndProc = nullptr;

static SwapChainPresentFn   g_origSwapChainPresent = nullptr;

// ─── Window helpers ───

static HWND FindMainWindowForCurrentProcess()
{
    struct FindData
    {
        DWORD pid;
        HWND hwnd;
    } data{ GetCurrentProcessId(), nullptr };

    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL
    {
        auto* p = reinterpret_cast<FindData*>(lParam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid != p->pid || !IsWindowVisible(hWnd))
            return TRUE;
        if (GetWindow(hWnd, GW_OWNER) != nullptr)
            return TRUE;
        p->hwnd = hWnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&data));

    return data.hwnd;
}

// ─── WndProc hook ───

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYUP && wParam == VK_SUBTRACT)
    {
        g_showMenu = !g_showMenu;
        return 0;
    }

    if (g_showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return TRUE;

    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// ─── ImGui init / shutdown / render ───

static bool InitializeImGui(IDirect3DSwapChain9* pSwapChain)
{
    if (g_imguiInitialized)
        return true;
    if (g_initInProgress)
        return false;
    g_initInProgress = true;

    Log("[*] InitImGui step 1: GetDevice\n");

    // Get device from swap chain
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = pSwapChain->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        Log("[!] GetDevice from SwapChain failed: 0x%08X\n", hr);
        g_initInProgress = false;
        return false;
    }

    Log("[*] InitImGui step 2: GetPresentParameters. device=%p\n", device);

    // Get the present parameters to find the HWND
    D3DPRESENT_PARAMETERS pp{};
    hr = pSwapChain->GetPresentParameters(&pp);
    if (FAILED(hr))
    {
        Log("[!] GetPresentParameters failed: 0x%08X\n", hr);
        device->Release();
        g_initInProgress = false;
        return false;
    }

    Log("[*] InitImGui step 3: FindWindow. pp.hDeviceWindow=%p\n", pp.hDeviceWindow);

    HWND hwnd = pp.hDeviceWindow;
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
        hwnd = FindMainWindowForCurrentProcess();

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
    {
        Log("[!] No valid window found for ImGui init\n");
        device->Release();
        g_initInProgress = false;
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    LONG width = rc.right - rc.left;
    LONG height = rc.bottom - rc.top;
    if (width < 640 || height < 360)
    {
        Log("[!] Window too small: %ldx%ld\n", width, height);
        device->Release();
        g_initInProgress = false;
        return false;
    }

    Log("[*] InitImGui step 4: SetWindowLongPtr. hwnd=%p %ldx%ld\n", hwnd, width, height);

    g_device = device;
    g_swapChain = pSwapChain;
    g_hWnd = hwnd;

    // Subclass window
    SetLastError(0);
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    if (!g_origWndProc && GetLastError() != 0)
    {
        Log("[!] SetWindowLongPtrW failed: %u\n", GetLastError());
        g_device->Release();
        g_device = nullptr;
        g_initInProgress = false;
        return false;
    }

    Log("[*] InitImGui step 5: ImGui::CreateContext\n");

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    Log("[*] InitImGui step 6: ImGui_ImplWin32_Init + ImGui_ImplDX9_Init\n");

    if (!ImGui_ImplWin32_Init(g_hWnd) || !ImGui_ImplDX9_Init(g_device))
    {
        Log("[!] ImGui backend init failed\n");
        if (g_origWndProc && g_hWnd)
            SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
        ImGui::DestroyContext();
        g_device->Release();
        g_device = nullptr;
        g_initInProgress = false;
        return false;
    }

    g_imguiInitialized = true;
    g_initInProgress = false;
    wchar_t title[256]{};
    GetWindowTextW(g_hWnd, title, static_cast<int>(std::size(title)));
    Log("[+] ImGui D3D9 init OK. hwnd=%p size=%ldx%ld title=%ws\n", g_hWnd, width, height, title);
    return true;
}

static void ShutdownImGuiState()
{
    if (g_imguiInitialized)
    {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }

    if (g_origWndProc && g_hWnd)
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    g_origWndProc = nullptr;
    g_hWnd = nullptr;

    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_swapChain = nullptr;
}

static void RenderImGui()
{
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_showMenu)
    {
        ImGui::Begin("SC2 Overlay", &g_showMenu, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("ImGui attached (D3D9)");
        ImGui::Text("NumPad-: Toggle menu");
        ImGui::Text("END: Unload DLL");
        ImGui::End();
    }

    ImGui::EndFrame();
    ImGui::Render();

    // Save and restore D3D9 state
    IDirect3DStateBlock9* stateBlock = nullptr;
    g_device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
    if (stateBlock)
        stateBlock->Capture();

    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    if (stateBlock)
    {
        stateBlock->Apply();
        stateBlock->Release();
    }
}

// ─── Hooked SwapChain::Present ───

static bool g_initFailed = false;
static UINT g_frameCount = 0;

static HRESULT WINAPI HookedSwapChainPresent(
    IDirect3DSwapChain9* pSwapChain,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
    DWORD dwFlags)
{
    g_frameCount++;

    if (g_frameCount <= 3)
    {
        Log("[+] Frame %u: pSwapChain=%p origFn=%p\n", g_frameCount, pSwapChain, g_origSwapChainPresent);
    }

    // Phase 1: First 60 frames, just pass through to validate hook stability
    if (g_frameCount <= 60)
        return g_origSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);

    // Phase 2: Validate pSwapChain pointer before using it
    if (!g_imguiInitialized && !g_initFailed && g_frameCount == 61)
    {
        Log("[*] Phase 2: Validating pSwapChain=%p\n", pSwapChain);

        __try
        {
            // Test if we can read the vtable pointer
            void** vtbl = *reinterpret_cast<void***>(pSwapChain);
            Log("[*] vtable ptr = %p\n", vtbl);

            // Test if we can read vtable[3] (Present)
            void* presentFn = vtbl[3];
            Log("[*] vtable[3] = %p\n", presentFn);

            // Test GetDevice (vtable[8])
            void* getDeviceFn = vtbl[8];
            Log("[*] vtable[8] (GetDevice) = %p\n", getDeviceFn);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] pSwapChain validation FAILED: 0x%08X\n", GetExceptionCode());
            g_initFailed = true;
        }
    }

    // Phase 3: Attempt init on frame 62
    if (!g_imguiInitialized && !g_initFailed && g_frameCount == 62)
    {
        Log("[*] Phase 3: Calling GetDevice on pSwapChain=%p\n", pSwapChain);

        __try
        {
            IDirect3DDevice9* device = nullptr;
            HRESULT hr = pSwapChain->GetDevice(&device);
            Log("[*] GetDevice result: hr=0x%08X device=%p\n", hr, device);
            if (device)
                device->Release();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] GetDevice CRASHED: 0x%08X\n", GetExceptionCode());
            g_initFailed = true;
        }
    }

    // Phase 4: Full init on frame 63
    if (!g_imguiInitialized && !g_initFailed && g_frameCount == 63)
    {
        Log("[*] Phase 4: Full InitializeImGui\n");

        __try
        {
            if (!InitializeImGui(pSwapChain))
                g_initFailed = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] InitializeImGui CRASHED: 0x%08X\n", GetExceptionCode());
            g_initFailed = true;
        }
    }

    // Render if initialized
    if (g_imguiInitialized)
    {
        __try
        {
            RenderImGui();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[!] RenderImGui CRASHED: 0x%08X\n", GetExceptionCode());
        }
    }

    return g_origSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

// ─── vtable hook setup ───

static uintptr_t GetSwapChainPresentAddress()
{
    // Create a temporary D3D9 device + swap chain to read the vtable
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
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &pp,
        &device);

    if (FAILED(hr) || !device)
    {
        Log("[!] CreateDevice failed: 0x%08X\n", hr);
        d3d9->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, wc.hInstance);
        return 0;
    }

    // Get the implicit swap chain
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

    // Read vtable[3] = Present
    void** vtbl = *reinterpret_cast<void***>(swapChain);
    uintptr_t presentAddr = reinterpret_cast<uintptr_t>(vtbl[3]);

    Log("[+] IDirect3DSwapChain9 vtable[3] (Present) = 0x%llX\n",
        static_cast<unsigned long long>(presentAddr));

    swapChain->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, wc.hInstance);

    return presentAddr;
}

static bool SetupHooks()
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
        Log("[!] MH_CreateHook Present failed: %s\n", MH_StatusToString(st));
        MH_Uninitialize();
        return false;
    }

    st = MH_EnableHook(reinterpret_cast<LPVOID>(presentAddr));
    if (st != MH_OK)
    {
        Log("[!] MH_EnableHook Present failed: %s\n", MH_StatusToString(st));
        MH_RemoveHook(reinterpret_cast<LPVOID>(presentAddr));
        MH_Uninitialize();
        return false;
    }

    Log("[+] D3D9 SwapChain::Present hooked @ 0x%llX\n",
        static_cast<unsigned long long>(presentAddr));
    return true;
}

static void Cleanup()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    ShutdownImGuiState();
    Log("[+] Cleanup done\n");
}

static DWORD WINAPI MainThread(LPVOID)
{
    Log("[+] MainThread started. PID=%u\n", GetCurrentProcessId());

    if (!SetupHooks())
    {
        Log("[!] SetupHooks failed\n");
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    while (!(GetAsyncKeyState(VK_END) & 0x8000))
        Sleep(100);

    Cleanup();
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;

        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
