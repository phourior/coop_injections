#include "pch.h"
#include "render/overlay.h"
#include "render/menu.h"
#include "core/globals.h"
#include "core/log.h"
#include "hooks/game_hook.h"

#include <imgui.h>
#include <imgui_internal.h>
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
    const bool usedPresentWindow = hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd);
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
        hwnd = FindMainWindowForCurrentProcess();

    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
    {
        Log("[!] No valid window found\n");
        device->Release();
        s_initInProgress = false;
        return false;
    }

    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
    {
        Log("[!] GetClientRect failed: %u\n", GetLastError());
        device->Release();
        s_initInProgress = false;
        return false;
    }
    LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 640 || h < 360)
    {
        Log("[!] Window too small: %ldx%ld\n", w, h);
        device->Release();
        s_initInProgress = false;
        return false;
    }

    char windowClass[128]{};
    wchar_t windowTitle[256]{};
    GetClassNameA(hwnd, windowClass, static_cast<int>(_countof(windowClass)));
    GetWindowTextW(hwnd, windowTitle, static_cast<int>(_countof(windowTitle)));
    Log("[*] SwapChain parameters: windowed=%d backBuffer=%ux%u format=%u "
        "swapEffect=%u deviceWindow=%p selectedWindow=%p source=%s\n",
        pp.Windowed, pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
        pp.SwapEffect, pp.hDeviceWindow, hwnd,
        usedPresentWindow ? "present-parameters" : "process-window-fallback");
    Log("[*] Selected window: class='%s' title='%ls' client=%ldx%ld visible=%d\n",
        windowClass, windowTitle, w, h, IsWindowVisible(hwnd));

    D3DDEVICE_CREATION_PARAMETERS creation{};
    D3DCAPS9 caps{};
    IDirect3D9* direct3D = nullptr;
    D3DADAPTER_IDENTIFIER9 adapter{};
    const HRESULT creationHr = device->GetCreationParameters(&creation);
    const HRESULT capsHr = device->GetDeviceCaps(&caps);
    const HRESULT direct3DHr = device->GetDirect3D(&direct3D);
    HRESULT adapterHr = E_FAIL;
    if (SUCCEEDED(direct3DHr) && direct3D)
        adapterHr = direct3D->GetAdapterIdentifier(creation.AdapterOrdinal, 0, &adapter);
    Log("[*] D3D9 device: creationHr=0x%08X adapter=%u type=%u behavior=0x%08lX "
        "focusWindow=%p capsHr=0x%08X devCaps=0x%08lX presentationIntervals=0x%08lX\n",
        creationHr, creation.AdapterOrdinal, creation.DeviceType, creation.BehaviorFlags,
        creation.hFocusWindow, capsHr, caps.DevCaps, caps.PresentationIntervals);
    Log("[*] D3D9 adapter: queryHr=0x%08X description='%s' device='%s' "
        "vendor=0x%04lX deviceId=0x%04lX subsystem=0x%08lX revision=%lu driver='%s'\n",
        adapterHr, adapter.Description, adapter.DeviceName, adapter.VendorId,
        adapter.DeviceId, adapter.SubSysId, adapter.Revision, adapter.Driver);
    if (direct3D)
        direct3D->Release();

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const UINT dpi = GetDpiForWindow(hwnd);
    Log("[*] Window environment: style=0x%llX exStyle=0x%llX dpi=%u "
        "thread=%lu process=%lu desktop=%p cloakedCheck=%d\n",
        static_cast<unsigned long long>(style), static_cast<unsigned long long>(exStyle),
        dpi, GetWindowThreadProcessId(hwnd, nullptr), GetCurrentProcessId(),
        GetThreadDesktop(GetCurrentThreadId()), IsWindowEnabled(hwnd));

    Log("[*] InitOverlay step 4: Subclass window hwnd=%p %ldx%ld\n", hwnd, w, h);

    g_screenWidth  = static_cast<float>(w);
    g_screenHeight = static_cast<float>(h);

    g_device = device;
    g_swapChain = pSwapChain;
    g_hWnd = hwnd;

    SetLastError(0);
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
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
    // 鼠标指针修复：禁止 ImGui 根据悬停控件修改 Win32 系统指针样式，
    // 避免它与游戏自身的指针更新互相覆盖而造成指针反复横跳。
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
    Log("[+] Overlay init OK. hwnd=%p %ldx%ld imgui=%s backendPlatform='%s' "
        "backendRenderer='%s' fontCount=%d ini=%s\n",
        g_hWnd, w, h, ImGui::GetVersion(),
        io.BackendPlatformName ? io.BackendPlatformName : "null",
        io.BackendRendererName ? io.BackendRendererName : "null",
        io.Fonts->Fonts.Size, io.IniFilename ? io.IniFilename : "disabled");
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
    static unsigned long long frameCount = 0;
    static ULONGLONG lastDiagnosticAt = 0;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 神器坐标和地图边界在内部按 150 ms 刷新，所有绘制区域共享该快照。
    RefreshOverlayGameData();

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

    ++frameCount;
    const ULONGLONG now = GetTickCount64();
    const bool logDiagnostic = frameCount == 1 || now - lastDiagnosticAt >= 5000;
    if (logDiagnostic)
    {
        ImDrawData* drawData = ImGui::GetDrawData();
        IDirect3DSurface9* renderTarget = nullptr;
        D3DSURFACE_DESC renderTargetDesc{};
        const HRESULT renderTargetHr = g_device->GetRenderTarget(0, &renderTarget);
        if (SUCCEEDED(renderTargetHr) && renderTarget)
        {
            renderTarget->GetDesc(&renderTargetDesc);
            renderTarget->Release();
        }

        const HRESULT cooperativeHr = g_device->TestCooperativeLevel();
        D3DVIEWPORT9 viewport{};
        const HRESULT viewportHr = g_device->GetViewport(&viewport);
        const ImGuiIO& io = ImGui::GetIO();
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const ImGuiWindow* currentWindow = ImGui::FindWindowByName("合作外挂");
        const ImGuiWindow* debugWindow = ImGui::FindWindowByName("##DebugOverlay");
        Log("[*] Overlay frame %llu: menu=%d display=%.0fx%.0f lists=%d "
            "vertices=%d indices=%d rtHr=0x%08X rt=%ux%u format=%u "
            "cooperative=0x%08X viewportHr=0x%08X viewport=%lu,%lu %lux%lu "
            "delta=%.4f frameRate=%.1f mouse=%.0f,%.0f hwnd=%p foreground=%p\n",
            frameCount, g_showMenu,
            drawData ? drawData->DisplaySize.x : 0.0f,
            drawData ? drawData->DisplaySize.y : 0.0f,
            drawData ? drawData->CmdListsCount : 0,
            drawData ? drawData->TotalVtxCount : 0,
            drawData ? drawData->TotalIdxCount : 0,
            renderTargetHr, renderTargetDesc.Width, renderTargetDesc.Height,
            renderTargetDesc.Format, cooperativeHr, viewportHr,
            viewport.X, viewport.Y, viewport.Width, viewport.Height,
            io.DeltaTime, io.Framerate, io.MousePos.x, io.MousePos.y,
            g_hWnd, GetForegroundWindow());
        Log("[*] ImGui state frame %llu: frame=%d activeWindows=%d mainViewport="
            "pos=%.0f,%.0f size=%.0fx%.0f menuWindow=%p pos=%.0f,%.0f "
            "size=%.0fx%.0f hidden=%d collapsed=%d debugWindow=%p "
            "pos=%.0f,%.0f size=%.0fx%.0f hidden=%d fontTex=%llu textures=%d\n",
            frameCount, ImGui::GetFrameCount(), ImGui::GetCurrentContext()->WindowsActiveCount,
            mainViewport->Pos.x, mainViewport->Pos.y,
            mainViewport->Size.x, mainViewport->Size.y,
            currentWindow,
            currentWindow ? currentWindow->Pos.x : 0.0f,
            currentWindow ? currentWindow->Pos.y : 0.0f,
            currentWindow ? currentWindow->Size.x : 0.0f,
            currentWindow ? currentWindow->Size.y : 0.0f,
            currentWindow ? currentWindow->Hidden : -1,
            currentWindow ? currentWindow->Collapsed : -1,
            debugWindow,
            debugWindow ? debugWindow->Pos.x : 0.0f,
            debugWindow ? debugWindow->Pos.y : 0.0f,
            debugWindow ? debugWindow->Size.x : 0.0f,
            debugWindow ? debugWindow->Size.y : 0.0f,
            debugWindow ? debugWindow->Hidden : -1,
            static_cast<unsigned long long>(io.Fonts->TexRef.GetTexID()),
            ImGui::GetPlatformIO().Textures.Size);
        lastDiagnosticAt = now;
    }

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
