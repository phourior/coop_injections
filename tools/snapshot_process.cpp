#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static bool IsReadable(DWORD protect)
{
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS))
        return false;

    switch (protect & 0xff)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

static const char* ProtectName(DWORD protect)
{
    switch (protect & 0xff)
    {
    case PAGE_READONLY: return "READONLY";
    case PAGE_READWRITE: return "READWRITE";
    case PAGE_WRITECOPY: return "WRITECOPY";
    case PAGE_EXECUTE: return "EXECUTE";
    case PAGE_EXECUTE_READ: return "EXECUTE_READ";
    case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
    case PAGE_EXECUTE_WRITECOPY: return "EXECUTE_WRITECOPY";
    default: return "UNKNOWN";
    }
}

static const char* TypeName(DWORD type)
{
    switch (type)
    {
    case MEM_IMAGE: return "IMAGE";
    case MEM_MAPPED: return "MAPPED";
    case MEM_PRIVATE: return "PRIVATE";
    default: return "UNKNOWN";
    }
}

struct ModuleRange
{
    uintptr_t base;
    size_t size;
    std::string name;
};

static std::vector<ModuleRange> WriteModules(DWORD pid, const fs::path& output)
{
    std::vector<ModuleRange> modules;
    std::ofstream stream(output / "modules.tsv", std::ios::binary);
    stream << "base\tsize\tmodule\tpath\n";

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            stream << "0x" << std::hex << std::setw(16) << std::setfill('0')
                   << reinterpret_cast<uintptr_t>(entry.modBaseAddr)
                   << "\t0x" << entry.modBaseSize << "\t";
            stream << fs::path(entry.szModule).string() << "\t"
                   << fs::path(entry.szExePath).string() << "\n";
            modules.push_back({ reinterpret_cast<uintptr_t>(entry.modBaseAddr),
                                entry.modBaseSize, fs::path(entry.szModule).string() });
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

static bool IsTargetImage(uintptr_t allocationBase, const std::vector<ModuleRange>& modules)
{
    for (const ModuleRange& module : modules)
    {
        if (module.base != allocationBase)
            continue;
        return _stricmp(module.name.c_str(), "SC2_x64.exe") == 0 ||
               _stricmp(module.name.c_str(), "dll1.dll") == 0;
    }
    return false;
}

static size_t DumpRegion(HANDLE process, uintptr_t base, size_t size, const fs::path& output)
{
    std::ofstream stream(output, std::ios::binary);
    std::vector<char> buffer(0x10000);
    size_t totalRead = 0;

    for (size_t offset = 0; offset < size; offset += buffer.size())
    {
        const SIZE_T wanted = (std::min)(buffer.size(), size - offset);
        SIZE_T read = 0;
        std::fill(buffer.begin(), buffer.end(), char{});
        ReadProcessMemory(process, reinterpret_cast<const void*>(base + offset),
                          buffer.data(), wanted, &read);
        stream.write(buffer.data(), static_cast<std::streamsize>(wanted));
        totalRead += read;
    }
    return totalRead;
}

static size_t IndexScalars(HANDLE process, uintptr_t base, size_t size, std::ofstream& output)
{
    std::vector<unsigned char> buffer(size);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(base),
                           buffer.data(), size, &bytesRead))
        return 0;

    size_t found = 0;
    for (size_t offset = 0; offset + sizeof(uint32_t) <= bytesRead; offset += sizeof(uint32_t))
    {
        uint32_t bits = 0;
        std::memcpy(&bits, buffer.data() + offset, sizeof(bits));
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        const bool integerCandidate = (bits >= 1 && bits <= 3) || bits == 5 || bits == 10;
        const bool floatCandidate = value == 1.0f || value == 2.0f || value == 3.0f ||
                                    value == 5.0f || value == 10.0f;
        if (!integerCandidate && !floatCandidate)
            continue;

        output << "0x" << std::hex << std::setw(16) << std::setfill('0') << base + offset
               << "\t0x" << std::setw(8) << bits << "\n";
        ++found;
    }
    return found;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        std::wcerr << L"usage: snapshot_process.exe <pid> <output-dir>\n";
        return 2;
    }

    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    const fs::path output = argv[2];
    fs::create_directories(output);

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
    {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return 1;
    }

    const std::vector<ModuleRange> modules = WriteModules(pid, output);
    std::ofstream manifest(output / "regions.tsv", std::ios::binary);
    std::ofstream scalars(output / "scalars.tsv", std::ios::binary);
    manifest << "file\tbase\tallocation_base\tsize\tbytes_read\tprotect\ttype\n";
    scalars << "address\tvalue\n";

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    uintptr_t address = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    size_t index = 0;
    size_t total = 0;

    while (address < maxAddress)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQueryEx(process, reinterpret_cast<const void*>(address), &memory, sizeof(memory)))
        {
            address += 0x10000;
            continue;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        const uintptr_t next = base + memory.RegionSize;
        const bool targetImage = memory.Type == MEM_IMAGE &&
            IsTargetImage(reinterpret_cast<uintptr_t>(memory.AllocationBase), modules);
        const bool usefulPrivate = memory.Type == MEM_PRIVATE && memory.RegionSize <= 0x1000000 &&
            (memory.Protect & 0xff) == PAGE_READWRITE;
        if (memory.State == MEM_COMMIT && IsReadable(memory.Protect) && targetImage)
        {
            wchar_t fileName[192]{};
            swprintf_s(fileName, L"region_%04zu_0x%016llx_0x%llx_%S_%S.bin", index,
                       static_cast<unsigned long long>(base),
                       static_cast<unsigned long long>(memory.RegionSize),
                       TypeName(memory.Type), ProtectName(memory.Protect));
            const size_t bytesRead = DumpRegion(process, base, memory.RegionSize, output / fileName);
            manifest << fs::path(fileName).string() << "\t0x" << std::hex << std::setw(16)
                     << std::setfill('0') << base << "\t0x"
                     << reinterpret_cast<uintptr_t>(memory.AllocationBase) << "\t0x"
                     << memory.RegionSize << "\t0x" << bytesRead << "\t"
                     << ProtectName(memory.Protect) << "\t" << TypeName(memory.Type) << "\n";
            total += memory.RegionSize;
            ++index;
        }
        else if (memory.State == MEM_COMMIT && usefulPrivate)
        {
            IndexScalars(process, base, memory.RegionSize, scalars);
        }
        address = next > address ? next : address + 0x10000;
    }

    CloseHandle(process);
    std::cout << "regions=" << index << " bytes=" << total << " output=" << output.string() << "\n";
    return 0;
}