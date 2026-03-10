// dllmain.cpp : DX11 ImGui overlay via PolyHook2
#include "pch.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <polyhook2/Detour/NatDetour.hpp>

// ── forward declarations ──
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── types ──
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

// ── globals ──
static HMODULE              g_hModule       = nullptr;
static bool                 g_initialized   = false;
static bool                 g_showMenu      = true;

static ID3D11Device*        g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*  g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

static HWND                 g_hWnd          = nullptr;
static WNDPROC              g_origWndProc   = nullptr;

static uint64_t             g_presentTrampoline = 0;
static std::unique_ptr<PLH::NatDetour> g_presentDetour;

// ── WndProc hook ──
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return TRUE;

    // Toggle menu with INSERT key
    if (msg == WM_KEYDOWN && wParam == VK_INSERT)
        g_showMenu = !g_showMenu;

    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// ── Initialize ImGui on the first Present call ──
static bool InitImGui(IDXGISwapChain* pSwapChain)
{
    // Get device & context from swapchain
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_pd3dDevice))))
        return false;
    g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);

    // Get back buffer and create RTV
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer))))
        return false;

    HRESULT hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr))
        return false;

    // Get window handle from swap chain desc
    DXGI_SWAP_CHAIN_DESC desc{};
    pSwapChain->GetDesc(&desc);
    g_hWnd = desc.OutputWindow;

    // Hook the window procedure
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    return true;
}

// ── Hooked Present ──
static HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    if (!g_initialized)
    {
        if (InitImGui(pSwapChain))
            g_initialized = true;
    }

    if (g_initialized)
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_showMenu)
        {
            ImGui::Begin("SC2 Overlay", &g_showMenu, ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Hello from ImGui!");
            ImGui::Text("Press INSERT to toggle menu");
            ImGui::End();
        }

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    auto trampoline = reinterpret_cast<PresentFn>(g_presentTrampoline);
    return trampoline(pSwapChain, SyncInterval, Flags);
}

// ── Retrieve the vftable address of IDXGISwapChain::Present via a dummy device ──
static uintptr_t GetPresentAddress()
{
    // Create a temporary hidden window
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.lpszClassName  = L"DummyDX11Wnd";
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hWnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device*   pDevice    = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &featureLevel, 1, D3D11_SDK_VERSION,
        &sd, &pSwapChain, &pDevice, nullptr, &pContext);

    uintptr_t presentAddr = 0;
    if (SUCCEEDED(hr))
    {
        // IDXGISwapChain vtable index 8 = Present
        auto* vtable = *reinterpret_cast<uintptr_t**>(pSwapChain);
        presentAddr = vtable[8];
    }

    if (pContext)   pContext->Release();
    if (pDevice)    pDevice->Release();
    if (pSwapChain) pSwapChain->Release();
    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return presentAddr;
}

// ── Cleanup ──
static void Cleanup()
{
    // Unhook Present
    if (g_presentDetour)
        g_presentDetour->unHook();

    if (g_initialized)
    {
        // Restore original WndProc
        if (g_origWndProc && g_hWnd)
            SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = nullptr; }
    }
}

// ── Main worker thread ──
static DWORD WINAPI MainThread(LPVOID)
{
    // Get the real Present address via dummy device
    uintptr_t presentAddr = GetPresentAddress();
    if (!presentAddr)
    {
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    // Hook Present with PolyHook2 NatDetour
    g_presentDetour = std::make_unique<PLH::NatDetour>(
        presentAddr,
        reinterpret_cast<uintptr_t>(&HookedPresent),
        &g_presentTrampoline);

    if (!g_presentDetour->hook())
    {
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
    }

    // Wait for END key to unload
    while (!(GetAsyncKeyState(VK_END) & 0x8000))
        Sleep(100);

    Cleanup();
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (hThread)
            CloseHandle(hThread);
    }
    return TRUE;
}

