// dllmain.cpp : DX11 ImGui overlay — auto-scan all Present variants in dxgi.dll
#include "pch.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <dxgi1_2.h>
#include <MinHook.h>
#include <vector>
#include <set>

// ── forward declaration ──
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── Log via OutputDebugStringA ──
static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}

// ── globals ──
static HMODULE                  g_hModule           = nullptr;
static bool                     g_initialized       = false;
static bool                     g_showMenu          = true;

static ID3D11Device*            g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView*  g_pRenderTargetView = nullptr;

static HWND                     g_hWnd              = nullptr;
static WNDPROC                  g_origWndProc       = nullptr;

// ── Per-hook trampoline storage ──
using PresentFn  = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
struct PresentHook {
    uintptr_t   targetAddr;
    PresentFn   original;      // used for Present hooks
    bool        everCalled;
};
struct Present1Hook {
    uintptr_t    targetAddr;
    Present1Fn   original;
    bool         everCalled;
};
static std::vector<PresentHook>  g_presentHooks;
static std::vector<Present1Hook> g_present1Hooks;

// ── WndProc hook ──
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return TRUE;
    if (msg == WM_KEYDOWN && wParam == VK_SUBTRACT)
        g_showMenu = !g_showMenu;
    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// ── Initialize ImGui on first Present call ──
static bool InitImGui(IDXGISwapChain* pSwapChain)
{
    Log("[+] InitImGui: start\n");

    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_pd3dDevice))))
    {
        Log("[!] GetDevice failed\n");
        return false;
    }
    g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
    Log("[+] Got device=%p  context=%p\n", g_pd3dDevice, g_pd3dDeviceContext);

    ID3D11Texture2D* pBackBuffer = nullptr;
    if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer))))
    {
        Log("[!] GetBuffer failed\n");
        return false;
    }

    HRESULT hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr))
    {
        Log("[!] CreateRenderTargetView failed hr=0x%08X\n", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    pSwapChain->GetDesc(&desc);
    g_hWnd = desc.OutputWindow;
    Log("[+] HWND=%p  BufferCount=%u  Format=%u\n", g_hWnd, desc.BufferCount, desc.BufferDesc.Format);

    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    Log("[+] ImGui initialized OK\n");
    return true;
}

// ── Common render logic called from any Present hook ──
static void DoRender(IDXGISwapChain* pSwapChain)
{
    if (!g_initialized)
    {
        if (InitImGui(pSwapChain))
            g_initialized = true;
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_showMenu)
    {
        ImGui::Begin("SC2 Overlay", &g_showMenu, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Hello from ImGui!");
        ImGui::Text("Press NumPad- to toggle menu");
        ImGui::Text("Press END to unload");
        ImGui::End();
    }

    ImGui::Render();
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ── Generic Present hook dispatchers (one per slot, max 16) ──
// We need distinct function addresses for each hook, so we use templates.
template<int N>
static HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    auto& h = g_presentHooks[N];
    if (!h.everCalled)
    {
        h.everCalled = true;
        Log("[+] >>> HookedPresent[%d] FIRED! target=0x%llX swapchain=%p <<<\n",
            N, (unsigned long long)h.targetAddr, pSwapChain);
    }

    __try {
        DoRender(pSwapChain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[!] SEH exception 0x%08X in render (slot %d)\n", GetExceptionCode(), N);
    }

    return h.original(pSwapChain, SyncInterval, Flags);
}

// Instantiate 16 unique Present hook functions
using HookFnPtr = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
static HookFnPtr g_hookFns[] = {
    HookedPresent<0>,  HookedPresent<1>,  HookedPresent<2>,  HookedPresent<3>,
    HookedPresent<4>,  HookedPresent<5>,  HookedPresent<6>,  HookedPresent<7>,
    HookedPresent<8>,  HookedPresent<9>,  HookedPresent<10>, HookedPresent<11>,
    HookedPresent<12>, HookedPresent<13>, HookedPresent<14>, HookedPresent<15>,
};
static constexpr int MAX_PRESENT_HOOKS = 16;

// ── Present1 hook dispatchers ──
template<int N>
static HRESULT WINAPI HookedPresent1(IDXGISwapChain1* pSwapChain, UINT SyncInterval,
                                      UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pParams)
{
    auto& h = g_present1Hooks[N];
    if (!h.everCalled)
    {
        h.everCalled = true;
        Log("[+] >>> HookedPresent1[%d] FIRED! target=0x%llX swapchain=%p <<<\n",
            N, (unsigned long long)h.targetAddr, pSwapChain);
    }

    __try {
        DoRender(pSwapChain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[!] SEH exception 0x%08X in render1 (slot %d)\n", GetExceptionCode(), N);
    }

    return h.original(pSwapChain, SyncInterval, PresentFlags, pParams);
}

using Hook1FnPtr = HRESULT(WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static Hook1FnPtr g_hook1Fns[] = {
    HookedPresent1<0>,  HookedPresent1<1>,  HookedPresent1<2>,  HookedPresent1<3>,
    HookedPresent1<4>,  HookedPresent1<5>,  HookedPresent1<6>,  HookedPresent1<7>,
    HookedPresent1<8>,  HookedPresent1<9>,  HookedPresent1<10>, HookedPresent1<11>,
    HookedPresent1<12>, HookedPresent1<13>, HookedPresent1<14>, HookedPresent1<15>,
};
static constexpr int MAX_PRESENT1_HOOKS = 16;

// ── Scan results ──
struct ScanResults {
    std::vector<uintptr_t> present;   // vtable[8]
    std::vector<uintptr_t> present1;  // vtable[22]
};

// ── Scan dxgi.dll .rdata for COM vtables, extract unique Present/Present1 addresses ──
static ScanResults ScanDxgiForPresentAddresses()
{
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) return {};

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hDXGI);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(hDXGI) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    uintptr_t base = reinterpret_cast<uintptr_t>(hDXGI);
    uintptr_t textStart = 0, textEnd = 0;
    uintptr_t rdataStart = 0, rdataEnd = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (memcmp(sec[i].Name, ".text", 5) == 0)
        {
            textStart = base + sec[i].VirtualAddress;
            textEnd   = textStart + sec[i].Misc.VirtualSize;
        }
        if (memcmp(sec[i].Name, ".rdata", 6) == 0)
        {
            rdataStart = base + sec[i].VirtualAddress;
            rdataEnd   = rdataStart + sec[i].Misc.VirtualSize;
        }
    }

    Log("[+] dxgi.dll: base=%p .text=[%p-%p] .rdata=[%p-%p]\n",
        (void*)base, (void*)textStart, (void*)textEnd, (void*)rdataStart, (void*)rdataEnd);

    // Scan for vtable-like structures: sequences of 9+ pointers into .text
    std::set<uintptr_t> uniquePresent;
    std::set<uintptr_t> uniquePresent1;
    int vtableCount = 0;

    for (uintptr_t addr = rdataStart; addr + 9 * 8 <= rdataEnd; addr += 8)
    {
        auto* entries = reinterpret_cast<uintptr_t*>(addr);

        // Previous 8 bytes must NOT be a valid .text pointer (= vtable start boundary)
        if (addr > rdataStart)
        {
            uintptr_t prev = *reinterpret_cast<uintptr_t*>(addr - 8);
            if (prev >= textStart && prev < textEnd)
                continue; // not the start of a vtable
        }

        // Count consecutive .text pointers
        int valid = 0;
        for (int j = 0; j < 30 && (addr + (j + 1) * 8 <= rdataEnd); j++)
        {
            if (entries[j] >= textStart && entries[j] < textEnd)
                valid++;
            else
                break;
        }

        if (valid >= 9)
        {
            vtableCount++;
            // vtable[8] = Present
            uintptr_t presentCandidate = entries[8];
            if (uniquePresent.insert(presentCandidate).second)
            {
                Log("[+] vtable #%d at rdata+0x%X (%d entries): [8]=Present 0x%llX (dxgi+0x%X)\n",
                    vtableCount, (unsigned)(addr - base), valid,
                    (unsigned long long)presentCandidate,
                    (unsigned)(presentCandidate - base));
            }

            // vtable[22] = Present1 (only for IDXGISwapChain1+ vtables with >= 23 entries)
            if (valid >= 23)
            {
                uintptr_t present1Candidate = entries[22];
                if (uniquePresent1.insert(present1Candidate).second)
                {
                    Log("[+]   -> [22]=Present1 0x%llX (dxgi+0x%X)\n",
                        (unsigned long long)present1Candidate,
                        (unsigned)(present1Candidate - base));
                }
            }
        }
    }

    Log("[+] Found %d vtables, %zu unique Present, %zu unique Present1\n",
        vtableCount, uniquePresent.size(), uniquePresent1.size());

    ScanResults res;
    res.present  = std::vector<uintptr_t>(uniquePresent.begin(), uniquePresent.end());
    res.present1 = std::vector<uintptr_t>(uniquePresent1.begin(), uniquePresent1.end());
    return res;
}

// ── Cleanup ──
static void Cleanup()
{
    Log("[+] Cleanup: start\n");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_initialized)
    {
        if (g_origWndProc && g_hWnd)
            SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = nullptr; }
    }
    Log("[+] Cleanup: done\n");
}

// ── Setup ──
static bool DoSetup()
{
    // Wait for d3d11.dll or d3d12.dll (up to 120s) — ensures graphics subsystem is live
    Log("[+] Waiting for graphics DLL...\n");
    for (int i = 0; i < 1200; i++) {
        if (GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d12.dll")) break;
        Sleep(100);
    }
    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D11 && !hD3D12) {
        Log("[!] Neither d3d11.dll nor d3d12.dll found after 120s\n");
        return false;
    }
    Log("[+] d3d11.dll=%p  d3d12.dll=%p\n", hD3D11, hD3D12);

    if (!GetModuleHandleA("dxgi.dll")) {
        Log("[!] dxgi.dll not found\n");
        return false;
    }

    // Extra delay to let swapchain be created
    Sleep(3000);
    Log("[+] Starting scan...\n");

    // Scan dxgi.dll for all Present/Present1 candidates
    auto scan = ScanDxgiForPresentAddresses();
    if (scan.present.empty() && scan.present1.empty())
    {
        Log("[!] No Present candidates found\n");
        return false;
    }

    // Initialize MinHook
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK)
    {
        Log("[!] MH_Initialize failed: %s\n", MH_StatusToString(mhStatus));
        return false;
    }

    int totalHooks = 0;

    // Hook Present candidates (vtable[8])
    int hookCount = 0;
    for (auto addr : scan.present)
    {
        if (hookCount >= MAX_PRESENT_HOOKS) break;

        PresentHook ph{};
        ph.targetAddr = addr;
        ph.original   = nullptr;
        ph.everCalled = false;
        g_presentHooks.push_back(ph);

        int idx = hookCount;
        mhStatus = MH_CreateHook(
            reinterpret_cast<LPVOID>(addr),
            reinterpret_cast<LPVOID>(g_hookFns[idx]),
            reinterpret_cast<LPVOID*>(&g_presentHooks[idx].original));

        if (mhStatus == MH_OK)
        {
            Log("[+] Present Hook[%d] on 0x%llX\n", idx, (unsigned long long)addr);
            hookCount++;
            totalHooks++;
        }
        else
        {
            Log("[!] Present Hook[%d] on 0x%llX failed: %s\n", idx, (unsigned long long)addr, MH_StatusToString(mhStatus));
            g_presentHooks.pop_back();
        }
    }

    // Hook Present1 candidates (vtable[22])
    int hook1Count = 0;
    for (auto addr : scan.present1)
    {
        if (hook1Count >= MAX_PRESENT1_HOOKS) break;

        Present1Hook ph{};
        ph.targetAddr = addr;
        ph.original   = nullptr;
        ph.everCalled = false;
        g_present1Hooks.push_back(ph);

        int idx = hook1Count;
        mhStatus = MH_CreateHook(
            reinterpret_cast<LPVOID>(addr),
            reinterpret_cast<LPVOID>(g_hook1Fns[idx]),
            reinterpret_cast<LPVOID*>(&g_present1Hooks[idx].original));

        if (mhStatus == MH_OK)
        {
            Log("[+] Present1 Hook[%d] on 0x%llX\n", idx, (unsigned long long)addr);
            hook1Count++;
            totalHooks++;
        }
        else
        {
            Log("[!] Present1 Hook[%d] on 0x%llX failed: %s\n", idx, (unsigned long long)addr, MH_StatusToString(mhStatus));
            g_present1Hooks.pop_back();
        }
    }

    if (totalHooks == 0)
    {
        Log("[!] No hooks created\n");
        MH_Uninitialize();
        return false;
    }

    // Enable all hooks
    mhStatus = MH_EnableHook(MH_ALL_HOOKS);
    if (mhStatus != MH_OK)
    {
        Log("[!] MH_EnableHook failed: %s\n", MH_StatusToString(mhStatus));
        MH_Uninitialize();
        return false;
    }
    Log("[+] %d Present + %d Present1 = %d hooks enabled\n", hookCount, hook1Count, totalHooks);
    return true;
}

static bool DoSetupSEH()
{
    __try { return DoSetup(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[!] SEH EXCEPTION 0x%08X in setup!\n", GetExceptionCode());
        return false;
    }
}

// ── Main thread ──
static DWORD WINAPI MainThread(LPVOID)
{
    Log("[+] MainThread started, PID=%u\n", GetCurrentProcessId());

    if (!DoSetupSEH())
    {
        Log("[!] Setup failed\n");
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
        HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
