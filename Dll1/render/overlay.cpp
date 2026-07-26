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

static void UpdateScreenSize(HWND hWnd)
{
    // 审查修复 #10：窗口化切换和 D3D Reset 后重新读取客户区，避免小地图
    // 坐标继续使用初始化时的旧分辨率。
    RECT clientRect{};
    if (!hWnd || !GetClientRect(hWnd, &clientRect))
        return;

    const LONG width = clientRect.right - clientRect.left;
    const LONG height = clientRect.bottom - clientRect.top;
    if (width > 0 && height > 0)
    {
        g_screenWidth = static_cast<float>(width);
        g_screenHeight = static_cast<float>(height);
    }
}

// ─── WndProc hook ───

static bool IsMouseInputMessage(UINT msg)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

static bool IsKeyboardInputMessage(UINT msg)
{
    switch (msg)
    {
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR:
        return true;
    default:
        return false;
    }
}

static bool IsImeCommitMessage(UINT msg)
{
    return msg == WM_IME_COMPOSITION || msg == WM_IME_CHAR;
}

static bool IsKeyTransitionMessage(UINT msg)
{
    return msg == WM_KEYDOWN || msg == WM_KEYUP ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    EnterHookCallback();
    WNDPROC originalWndProc = g_origWndProc;

    if (IsDllUnloading())
    {
        LRESULT result = CallWindowProcW(originalWndProc, hWnd, msg, wParam, lParam);
        LeaveHookCallback();
        return result;
    }

    const bool isMenuHotkey = wParam == VK_F12 || wParam == VK_SUBTRACT;
    if (isMenuHotkey && IsKeyTransitionMessage(msg))
    {
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
            g_showMenu = !g_showMenu;
        LeaveHookCallback();
        return 0;
    }

    // GUI 开启时，END 仍由卸载线程处理，但按键消息不再传给游戏。
    if (g_showMenu && IsKeyTransitionMessage(msg) && wParam == VK_END)
    {
        LeaveHookCallback();
        return 0;
    }

    if (msg == WM_SIZE && wParam != SIZE_MINIMIZED)
        UpdateScreenSize(hWnd);

    if (g_showMenu)
    {
        // ImGui's Win32 backend calls DefWindowProcW for WM_IME_COMPOSITION.
        // Forwarding that same message to the game's WndProc commits IME text twice.
        // This overlay has no IME text field, so leave IME commits exclusively to the game.
        if (!IsImeCommitMessage(msg))
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        const ImGuiIO& io = ImGui::GetIO();
        // 审查修复 #2：只吞掉 ImGui 明确捕获的输入；其余消息继续交给游戏。
        // 左上调试窗使用 NoMouseInputs，因此只有右侧交互菜单会触发鼠标捕获。
        if ((io.WantCaptureMouse && IsMouseInputMessage(msg)) ||
            (io.WantCaptureKeyboard && IsKeyboardInputMessage(msg)))
        {
            LeaveHookCallback();
            return 0;
        }
    }

    LRESULT result = CallWindowProcW(originalWndProc, hWnd, msg, wParam, lParam);
    LeaveHookCallback();
    return result;
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

    g_screenWidth  = static_cast<float>(w);
    g_screenHeight = static_cast<float>(h);

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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
        | ImGuiConfigFlags_NoMouseCursorChange;

    // ─── 加载中文字体（微软雅黑） ───
    {
        ImFontConfig fontCfg;
        fontCfg.OversampleH = 1;
        fontCfg.OversampleV = 1;
        fontCfg.PixelSnapH  = true;

        // 地图名可能包含繁体汉字、日文假名及其他 CJK 字符，必须保留完整范围。
        static const ImWchar ranges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x2000, 0x206F, // General Punctuation
            0x3000, 0x30FF, // CJK Symbols, Hiragana, Katakana
            0x31F0, 0x31FF, // Katakana Phonetic Extensions
            0xFF00, 0xFFEF, // Halfwidth and Fullwidth Forms
            0x4E00, 0x9FFF, // CJK Unified Ideographs
            0,
        };

        ImFont* font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyh.ttc", 32.0f, &fontCfg, ranges);

        if (font)
            Log("[+] Chinese font loaded (msyh.ttc)\n");
        else
            Log("[!] Failed to load msyh.ttc, falling back to default\n");
    }

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

void DetachOverlayWindowProc()
{
    if (g_origWndProc && g_hWnd)
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
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

    DetachOverlayWindowProc();
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

    // 调试信息跟随菜单显示/隐藏
    if (g_showMenu)
    {
        DrawDebugOverlay();
        DrawMenu();
    }

    // 小地图神器点始终渲染
    DrawMinimapOverlay();

    ImGui::EndFrame();
    ImGui::Render();

    // 审查修复 #6：DX9 后端内部已保存并恢复完整设备状态，不再在外层
    // 创建第二个 D3DSBT_ALL StateBlock。
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void OnDeviceLost()
{
    if (g_imguiInitialized)
        ImGui_ImplDX9_InvalidateDeviceObjects();
}

void OnDeviceReset()
{
    UpdateScreenSize(g_hWnd);
    if (g_imguiInitialized)
        ImGui_ImplDX9_CreateDeviceObjects();
}
