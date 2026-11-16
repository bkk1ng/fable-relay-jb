/*
 * modules.cpp
 *
 * - EnumerateModules(DWORD pid, ...)     : enumerate all modules for a given PID
 * - GetModuleBase(DWORD pid, ...)        : resolve a module base address by name
 * - ReadProcessMemoryViaDriver(...)      : read process memory through a driver handle
 *                                         using a single request struct
 *
 * Build (standalone smoke test):
 *   cl /std:c++17 /D MODULES_TEST /W4 /O2 modules.cpp /link
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

// ---------------------------------------------------------------------------
// Public data types
// ---------------------------------------------------------------------------

struct ModuleInfo {
    std::wstring name;        // base name, e.g. L"ntdll.dll"
    std::wstring fullPath;
    uintptr_t    base;
    DWORD        size;
};

// Request struct sent to the driver via DeviceIoControl.
// The driver interprets this as a single atomic read request.
struct DriverReadRequest {
    DWORD     targetPid;    // process to read from
    uintptr_t address;      // virtual address in target process
    ULONG     length;       // bytes to copy
    PVOID     outBuffer;    // kernel copies result here (user VA)
};

// ---------------------------------------------------------------------------
// EnumerateModules
// ---------------------------------------------------------------------------

std::vector<ModuleInfo> EnumerateModules(DWORD pid)
{
    std::vector<ModuleInfo> modules;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        throw std::runtime_error(
            std::string("CreateToolhelp32Snapshot failed, error=") + std::to_string(err));
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (Module32FirstW(snap, &me)) {
        do {
            ModuleInfo info{};
            info.name     = me.szModule;
            info.fullPath = me.szExePath;
            info.base     = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            info.size     = me.modBaseSize;
            modules.push_back(std::move(info));
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return modules;
}

// ---------------------------------------------------------------------------
// GetModuleBase
// ---------------------------------------------------------------------------

// Returns 0 if the module is not found.
uintptr_t GetModuleBase(DWORD pid, const std::wstring& moduleName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    uintptr_t result = 0;

    if (Module32FirstW(snap, &me)) {
        do {
            // Case-insensitive compare
            if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                result = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return result;
}

// ---------------------------------------------------------------------------
// ReadProcessMemoryViaDriver
//
// Opens the driver device at `devicePath` and sends a single DriverReadRequest
// via DeviceIoControl.  The IOCTL code must match whatever the driver exposes.
//
// Returns true on success; outBuffer is populated with `length` bytes.
// ---------------------------------------------------------------------------

// Adjust the IOCTL code to match the target driver's definition.
#define IOCTL_READ_PROCESS_MEMORY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

bool ReadProcessMemoryViaDriver(
    const wchar_t* devicePath,
    DWORD          targetPid,
    uintptr_t      address,
    ULONG          length,
    void*          outBuffer)
{
    if (!outBuffer || length == 0)
        return false;

    HANDLE hDevice = CreateFileW(
        devicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hDevice == INVALID_HANDLE_VALUE)
        return false;

    DriverReadRequest req{};
    req.targetPid = targetPid;
    req.address   = address;
    req.length    = length;
    req.outBuffer = outBuffer;

    DWORD bytesReturned = 0;
    BOOL  ok = DeviceIoControl(
        hDevice,
        IOCTL_READ_PROCESS_MEMORY,
        &req,  sizeof(req),
        &req,  sizeof(req),   // driver may write status back into the same struct
        &bytesReturned,
        nullptr);

    CloseHandle(hDevice);
    return ok != FALSE;
}

// ---------------------------------------------------------------------------
// Standalone smoke test
// ---------------------------------------------------------------------------

#ifdef MODULES_TEST

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[])
{
    // Default to our own PID so the test always has something to enumerate.
    DWORD pid = GetCurrentProcessId();
    if (argc >= 2)
        pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));

    wprintf(L"=== Modules for PID %lu ===\n", pid);
    try {
        auto mods = EnumerateModules(pid);
        for (auto& m : mods) {
            wprintf(L"  [%016llX] %8lu  %s\n",
                static_cast<unsigned long long>(m.base),
                m.size,
                m.name.c_str());
        }
        wprintf(L"Total: %zu module(s)\n\n", mods.size());
    } catch (const std::exception& ex) {
        fprintf(stderr, "EnumerateModules error: %s\n", ex.what());
        return 1;
    }

    // Resolve ntdll.dll base as a sanity check.
    uintptr_t ntdllBase = GetModuleBase(pid, L"ntdll.dll");
    if (ntdllBase)
        wprintf(L"ntdll.dll base: %016llX\n", static_cast<unsigned long long>(ntdllBase));
    else
        wprintf(L"ntdll.dll not found in PID %lu\n", pid);

    // Driver read demo (requires a real driver device; will fail gracefully here).
    if (ntdllBase) {
        BYTE buf[64]{};
        bool ok = ReadProcessMemoryViaDriver(
            L"\\\\.\\MyKmDriver",
            pid,
            ntdllBase,
            sizeof(buf),
            buf);
        wprintf(L"Driver read: %s\n", ok ? L"success" : L"failed (expected without driver)");
    }

    return 0;
}

#endif // MODULES_TEST
