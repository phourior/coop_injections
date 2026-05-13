#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <conio.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

struct Options {
    std::wstring class_name = L"FakeAnalysisHostWindow";
    std::wstring window_title = L"Fake Analysis Host";
    std::wstring dll_to_load;
    fs::path dump_dir = L"dumps";
    int interval_seconds = 0;
    bool all_readable = false;
    bool hide_window = false;
    bool no_dump_after_load = false;
};

struct RegionInfo {
    std::uintptr_t base = 0;
    std::uintptr_t allocation_base = 0;
    std::size_t size = 0;
    DWORD protect = 0;
    DWORD state = 0;
    DWORD type = 0;
};

static std::wstring widen(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

static std::string narrow(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static void usage() {
    std::wcout
        << L"FakeHost usage:\n"
        << L"  FakeHost.exe [--class <window_class>] [--title <window_title>]\n"
        << L"               [--load <dll_path>] [--dump-dir <dir>]\n"
        << L"               [--interval <seconds>] [--all] [--hide-window]\n"
        << L"               [--no-dump-after-load]\n\n"
        << L"Keys while running:\n"
        << L"  d  dump memory now\n"
        << L"  m  write module list\n"
        << L"  q  quit\n";
}

static Options parse_args(int argc, wchar_t** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto need_value = [&](const wchar_t* name) -> std::wstring {
            if (i + 1 >= argc) {
                std::wcerr << L"missing value for " << name << L"\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == L"--help" || arg == L"-h") {
            usage();
            std::exit(0);
        } else if (arg == L"--class") {
            opt.class_name = need_value(L"--class");
        } else if (arg == L"--title") {
            opt.window_title = need_value(L"--title");
        } else if (arg == L"--load") {
            opt.dll_to_load = need_value(L"--load");
        } else if (arg == L"--dump-dir") {
            opt.dump_dir = need_value(L"--dump-dir");
        } else if (arg == L"--interval") {
            opt.interval_seconds = std::max(0, std::stoi(need_value(L"--interval")));
        } else if (arg == L"--all") {
            opt.all_readable = true;
        } else if (arg == L"--hide-window") {
            opt.hide_window = true;
        } else if (arg == L"--no-dump-after-load") {
            opt.no_dump_after_load = true;
        } else {
            std::wcerr << L"unknown argument: " << arg << L"\n";
            usage();
            std::exit(2);
        }
    }
    return opt;
}

static bool has_any_protection(DWORD protect, std::initializer_list<DWORD> wanted) {
    protect &= 0xff;
    for (const DWORD item : wanted) {
        if (protect == item) {
            return true;
        }
    }
    return false;
}

static bool is_readable(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    return has_any_protection(protect, {
        PAGE_READONLY,
        PAGE_READWRITE,
        PAGE_WRITECOPY,
        PAGE_EXECUTE_READ,
        PAGE_EXECUTE_READWRITE,
        PAGE_EXECUTE_WRITECOPY,
    });
}

static bool is_executable(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    return has_any_protection(protect, {
        PAGE_EXECUTE,
        PAGE_EXECUTE_READ,
        PAGE_EXECUTE_READWRITE,
        PAGE_EXECUTE_WRITECOPY,
    });
}

static const char* protect_name(DWORD protect) {
    switch (protect & 0xff) {
    case PAGE_EXECUTE: return "EXECUTE";
    case PAGE_EXECUTE_READ: return "EXECUTE_READ";
    case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
    case PAGE_EXECUTE_WRITECOPY: return "EXECUTE_WRITECOPY";
    case PAGE_NOACCESS: return "NOACCESS";
    case PAGE_READONLY: return "READONLY";
    case PAGE_READWRITE: return "READWRITE";
    case PAGE_WRITECOPY: return "WRITECOPY";
    default: return "UNKNOWN";
    }
}

static const char* type_name(DWORD type) {
    switch (type) {
    case MEM_IMAGE: return "IMAGE";
    case MEM_MAPPED: return "MAPPED";
    case MEM_PRIVATE: return "PRIVATE";
    default: return "UNKNOWN";
    }
}

static std::string hex_u64(std::uint64_t value, int width = 0) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    if (width > 0) {
        oss << std::setw(width);
    }
    oss << value;
    return oss.str();
}

static std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

static std::vector<RegionInfo> collect_regions(bool all_readable) {
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);

    std::vector<RegionInfo> regions;
    auto current = reinterpret_cast<std::uintptr_t>(sys.lpMinimumApplicationAddress);
    const auto max_addr = reinterpret_cast<std::uintptr_t>(sys.lpMaximumApplicationAddress);

    while (current < max_addr) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi));
        if (got == 0) {
            current += 0x10000;
            continue;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto next = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && is_readable(mbi.Protect) && (all_readable || is_executable(mbi.Protect))) {
            regions.push_back({
                base,
                reinterpret_cast<std::uintptr_t>(mbi.AllocationBase),
                static_cast<std::size_t>(mbi.RegionSize),
                mbi.Protect,
                mbi.State,
                mbi.Type,
            });
        }
        current = next > current ? next : current + 0x10000;
    }

    return regions;
}

static void write_modules(const fs::path& dir) {
    fs::create_directories(dir);
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateToolhelp32Snapshot failed: " << GetLastError() << L"\n";
        return;
    }

    std::ofstream out(dir / "modules.tsv", std::ios::binary);
    out << "base\tsize\tmodule\tpath\n";

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snap, &entry)) {
        do {
            out << "0x" << hex_u64(reinterpret_cast<std::uint64_t>(entry.modBaseAddr), 16)
                << "\t0x" << hex_u64(entry.modBaseSize)
                << "\t" << narrow(entry.szModule)
                << "\t" << narrow(entry.szExePath)
                << "\n";
        } while (Module32NextW(snap, &entry));
    }

    CloseHandle(snap);
    std::wcout << L"wrote " << (dir / "modules.tsv").wstring() << L"\n";
}

static bool read_size_of_image(std::uintptr_t base, DWORD& size_of_image) {
    __try {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return false;
        }
        size_of_image = nt->OptionalHeader.SizeOfImage;
        return size_of_image != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static fs::path dump_target_allocation(HMODULE module, const fs::path& root_dir, std::wstring_view label) {
    const std::uintptr_t module_base = reinterpret_cast<std::uintptr_t>(module);
    const fs::path run_dir = root_dir / widen(timestamp() + "_target");
    fs::create_directories(run_dir);

    MEMORY_BASIC_INFORMATION first{};
    const SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(module_base), &first, sizeof(first));

    std::ofstream status(run_dir / "target_status.tsv", std::ios::binary);
    status << "field\tvalue\n";
    status << "label\t" << narrow(std::wstring(label)) << "\n";
    status << "module_base\t0x" << hex_u64(module_base, 16) << "\n";
    status << "virtual_query_ok\t" << (got != 0 ? "1" : "0") << "\n";

    wchar_t module_path[MAX_PATH]{};
    if (GetModuleFileNameW(module, module_path, MAX_PATH) != 0) {
        status << "module_path\t" << narrow(module_path) << "\n";
    } else {
        status << "module_path_error\t" << GetLastError() << "\n";
    }

    if (got == 0) {
        status << "virtual_query_error\t" << GetLastError() << "\n";
        std::wcout << L"target allocation is not queryable after LoadLibrary: 0x"
                   << std::hex << module_base << std::dec << L"\n";
        return run_dir;
    }

    const auto allocation_base = reinterpret_cast<std::uintptr_t>(first.AllocationBase);
    DWORD size_of_image = 0;
    const bool has_pe_size = read_size_of_image(module_base, size_of_image);
    std::uintptr_t scan_end = allocation_base + (has_pe_size ? size_of_image : 0x2000000);

    status << "allocation_base\t0x" << hex_u64(allocation_base, 16) << "\n";
    status << "initial_region_base\t0x" << hex_u64(reinterpret_cast<std::uintptr_t>(first.BaseAddress), 16) << "\n";
    status << "initial_region_size\t0x" << hex_u64(first.RegionSize) << "\n";
    status << "initial_region_state\t0x" << hex_u64(first.State) << "\n";
    status << "initial_region_protect\t" << protect_name(first.Protect) << "\n";
    status << "initial_region_type\t" << type_name(first.Type) << "\n";
    status << "pe_size_of_image\t" << (has_pe_size ? ("0x" + hex_u64(size_of_image)) : "unreadable") << "\n";

    std::ofstream manifest(run_dir / "target_regions.tsv", std::ios::binary);
    manifest << "file\tbase\tallocation_base\tsize\tprotect\ttype\n";

    std::vector<std::byte> buffer;
    int index = 0;
    for (std::uintptr_t current = allocation_base; current < scan_end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi)) == 0) {
            current += 0x1000;
            continue;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto next = base + mbi.RegionSize;
        if (reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != allocation_base && base > module_base) {
            break;
        }

        if (mbi.State == MEM_COMMIT && is_readable(mbi.Protect)) {
            buffer.resize(mbi.RegionSize);
            SIZE_T read = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base), buffer.data(), mbi.RegionSize, &read) && read != 0) {
                std::ostringstream name;
                name << "target_" << std::setw(4) << std::setfill('0') << index++
                     << "_0x" << hex_u64(base, 16)
                     << "_0x" << hex_u64(static_cast<std::uint64_t>(read))
                     << "_" << type_name(mbi.Type)
                     << "_" << protect_name(mbi.Protect)
                     << ".bin";

                const fs::path file = run_dir / widen(name.str());
                std::ofstream out(file, std::ios::binary);
                out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read));

                manifest << name.str()
                         << "\t0x" << hex_u64(base, 16)
                         << "\t0x" << hex_u64(allocation_base, 16)
                         << "\t0x" << hex_u64(static_cast<std::uint64_t>(read))
                         << "\t" << protect_name(mbi.Protect)
                         << "\t" << type_name(mbi.Type)
                         << "\n";
            }
        }

        current = next > current ? next : current + 0x1000;
    }

    std::wcout << L"target dump wrote " << index << L" regions to " << run_dir.wstring() << L"\n";
    return run_dir;
}

static fs::path dump_memory(const Options& opt) {
    const fs::path run_dir = opt.dump_dir / widen(timestamp());
    fs::create_directories(run_dir);

    write_modules(run_dir);

    std::ofstream manifest(run_dir / "regions.tsv", std::ios::binary);
    manifest << "file\tbase\tallocation_base\tsize\tprotect\ttype\n";

    const auto regions = collect_regions(opt.all_readable);
    std::vector<std::byte> buffer;
    int index = 0;
    for (const auto& region : regions) {
        buffer.resize(region.size);
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(region.base), buffer.data(), region.size, &read) || read == 0) {
            continue;
        }

        std::ostringstream name;
        name << "region_" << std::setw(4) << std::setfill('0') << index++
             << "_0x" << hex_u64(region.base, 16)
             << "_0x" << hex_u64(static_cast<std::uint64_t>(read))
             << "_" << type_name(region.type)
             << "_" << protect_name(region.protect)
             << ".bin";

        const fs::path file = run_dir / widen(name.str());
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read));

        manifest << name.str()
                 << "\t0x" << hex_u64(region.base, 16)
                 << "\t0x" << hex_u64(region.allocation_base, 16)
                 << "\t0x" << hex_u64(static_cast<std::uint64_t>(read))
                 << "\t" << protect_name(region.protect)
                 << "\t" << type_name(region.type)
                 << "\n";
    }

    std::wcout << L"dumped " << index << L" regions to " << run_dir.wstring() << L"\n";
    return run_dir;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND create_host_window(const Options& opt) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = opt.class_name.c_str();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const DWORD style = opt.hide_window ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowExW(
        0,
        opt.class_name.c_str(),
        opt.window_title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        540,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (hwnd != nullptr && !opt.hide_window) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

int wmain(int argc, wchar_t** argv) {
    const Options opt = parse_args(argc, argv);
    std::wcout << L"FakeHost pid=" << GetCurrentProcessId() << L"\n";
    std::wcout << L"class=\"" << opt.class_name << L"\" title=\"" << opt.window_title << L"\"\n";
    std::wcout << L"dump mode: " << (opt.all_readable ? L"all readable regions" : L"executable readable regions") << L"\n";

    HWND hwnd = create_host_window(opt);
    if (hwnd == nullptr) {
        std::wcerr << L"CreateWindowEx failed: " << GetLastError() << L"\n";
    } else {
        std::wcout << L"window handle=0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec << L"\n";
    }

    HMODULE loaded = nullptr;
    if (!opt.dll_to_load.empty()) {
        std::wcout << L"loading dll: " << opt.dll_to_load << L"\n";
        loaded = LoadLibraryW(opt.dll_to_load.c_str());
        if (loaded == nullptr) {
            std::wcerr << L"LoadLibrary failed: " << GetLastError() << L"\n";
        } else {
            std::wcout << L"loaded at 0x" << std::hex << reinterpret_cast<std::uintptr_t>(loaded) << std::dec << L"\n";
            if (!opt.no_dump_after_load) {
                dump_target_allocation(loaded, opt.dump_dir, L"after_LoadLibrary");
            }
        }
    }

    write_modules(opt.dump_dir);

    auto last_dump = std::chrono::steady_clock::now();
    bool running = true;
    while (running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (_kbhit()) {
            const wchar_t ch = static_cast<wchar_t>(_getwch());
            if (ch == L'q' || ch == L'Q') {
                running = false;
            } else if (ch == L'd' || ch == L'D') {
                dump_memory(opt);
                last_dump = std::chrono::steady_clock::now();
            } else if (ch == L'm' || ch == L'M') {
                write_modules(opt.dump_dir);
            }
        }

        if (opt.interval_seconds > 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_dump).count();
            if (elapsed >= opt.interval_seconds) {
                dump_memory(opt);
                last_dump = now;
            }
        }

        Sleep(50);
    }

    if (loaded != nullptr) {
        FreeLibrary(loaded);
    }
    return 0;
}
