#include "pch.h"
#include "render/overlay.h"
#include "render/menu.h"
#include "core/globals.h"
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ─── Internal state ───

static volatile bool s_initInProgress = false;

// ─── Window helpers ───

static HWND FindMainWindowForCurrentProcess()
{
    struct FindData { DWORD pid; HWND hwnd; } data{ GetCurrentProcessId(), nullptr };

    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
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

// ─── Public API ───

bool IsOverlayReady()
{
    return g_imguiInitialized;
}

bool InitializeOverlay(IDirect3DSwapChain9* pSwapChain)
{
    if (g_imguiInitialized)
        return true;
    if (s_initInProgress)
        return false;
    s_initInProgress = true;

    Log("[*] InitOverlay step 1: GetDevice\n");

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = pSwapChain->GetDevice(&device);
    if (FAILED(hr) || !device)
    {
        Log("[!] GetDevice failed: 0x%08X\n", hr);
        s_initInProgress = false;
        return false;
    }

    Log("[*] InitOverlay step 2: GetPresentParameters\n");

    D3DPRESENT_PARAMETERS pp{};
    hr = pSwapChain->GetPresentParameters(&pp);
    if (FAILED(hr))
    {
        Log("[!] GetPresentParameters failed: 0x%08X\n", hr);
        device->Release();
        s_initInProgress = false;
        return false;
    }

    Log("[*] InitOverlay step 3: FindWindow\n");

    HWND hwnd = pp.hDeviceWindow;
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
        hwnd = FindMainWindowForCurrentProcess();

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
    {
        Log("[!] No valid window found\n");
        device->Release();
        s_initInProgress = false;
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 640 || h < 360)
    {
        Log("[!] Window too small: %ldx%ld\n", w, h);
        device->Release();
        s_initInProgress = false;
        return false;
    }

    Log("[*] InitOverlay step 4: Subclass window hwnd=%p %ldx%ld\n", hwnd, w, h);

    g_device = device;
    g_swapChain = pSwapChain;
    g_hWnd = hwnd;

    SetLastError(0);
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    if (!g_origWndProc && GetLastError() != 0)
    {
        Log("[!] SetWindowLongPtrW failed: %u\n", GetLastError());
        g_device->Release();
        g_device = nullptr;
        s_initInProgress = false;
        return false;
    }

    Log("[*] InitOverlay step 5: ImGui::CreateContext\n");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    Log("[*] InitOverlay step 6: ImGui backend init\n");

    if (!ImGui_ImplWin32_Init(g_hWnd) || !ImGui_ImplDX9_Init(g_device))
    {
        Log("[!] ImGui backend init failed\n");
        if (g_origWndProc && g_hWnd)
            SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
        ImGui::DestroyContext();
        g_device->Release();
        g_device = nullptr;
        s_initInProgress = false;
        return false;
    }

    g_imguiInitialized = true;
    s_initInProgress = false;
    Log("[+] Overlay init OK. hwnd=%p %ldx%ld\n", g_hWnd, w, h);
    return true;
}

void ShutdownOverlay()
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

void RenderOverlayFrame()
{
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_showMenu)
        DrawMenu();

    ImGui::EndFrame();
    ImGui::Render();

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
