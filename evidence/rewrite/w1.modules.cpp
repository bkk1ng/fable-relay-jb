// modules.cpp
// Enumerate modules of a given PID, resolve module base by name,
// and read process memory through a driver handle via a single request struct.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// IOCTL / driver interface definitions
// ---------------------------------------------------------------------------

// Adjust these to match your driver's actual IOCTL codes and device name.
#define DRIVER_DEVICE_NAME      L"\\\\.\\MyKernelDriver"
#define IOCTL_READ_PROCESS_MEM  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
struct ReadProcessMemoryRequest
{
    ULONG_PTR ProcessId;        // Target PID
    ULONG_PTR RemoteAddress;    // Address to read from in the target process
    ULONG_PTR LocalBuffer;      // Output buffer address (user-mode VA)
    SIZE_T    Size;             // Bytes to read
    NTSTATUS  Status;           // Filled by driver on return
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Module enumeration
// ---------------------------------------------------------------------------

struct ModuleEntry
{
    std::wstring name;          // Module base name (lower-case)
    uintptr_t    base;
    DWORD        size;
};

// Returns false and leaves 'modules' empty on error.
bool EnumerateModules(DWORD pid, std::vector<ModuleEntry>& modules)
{
    modules.clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (!Module32FirstW(snap, &me))
    {
        CloseHandle(snap);
        return false;
    }

    do
    {
        ModuleEntry entry{};
        // Store lower-case name for case-insensitive lookup.
        wchar_t lower[MAX_MODULE_NAME32 + 1]{};
        wcsncpy_s(lower, me.szModule, _TRUNCATE);
        for (wchar_t* p = lower; *p; ++p)
            *p = static_cast<wchar_t>(towlower(*p));

        entry.name = lower;
        entry.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        entry.size = me.modBaseSize;
        modules.push_back(entry);
    } while (Module32NextW(snap, &me));

    CloseHandle(snap);
    return true;
}

// ---------------------------------------------------------------------------
// Resolve module base address by name (case-insensitive)
// Returns 0 if not found.
// ---------------------------------------------------------------------------

uintptr_t ResolveModuleBase(const std::vector<ModuleEntry>& modules,
                             const wchar_t* moduleName)
{
    if (!moduleName) return 0;

    // Build lower-case search key.
    wchar_t key[MAX_MODULE_NAME32 + 1]{};
    wcsncpy_s(key, moduleName, _TRUNCATE);
    for (wchar_t* p = key; *p; ++p)
        *p = static_cast<wchar_t>(towlower(*p));

    for (const auto& m : modules)
    {
        if (m.name == key)
            return m.base;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Read process memory through the driver
// ---------------------------------------------------------------------------

// driverHandle   – open handle to the kernel driver device
// pid            – target process ID
// remoteAddress  – address in the target process to read from
// outBuf         – local buffer to receive the data
// size           – number of bytes to read
// Returns true on success.
bool DriverReadProcessMemory(HANDLE        driverHandle,
                              ULONG_PTR     pid,
                              ULONG_PTR     remoteAddress,
                              void*         outBuf,
                              SIZE_T        size)
{
    if (driverHandle == INVALID_HANDLE_VALUE || !outBuf || size == 0)
        return false;

    ReadProcessMemoryRequest req{};
    req.ProcessId     = pid;
    req.RemoteAddress = remoteAddress;
    req.LocalBuffer   = reinterpret_cast<ULONG_PTR>(outBuf);
    req.Size          = size;
    req.Status        = static_cast<NTSTATUS>(0xC0000001L); // STATUS_UNSUCCESSFUL

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        driverHandle,
        IOCTL_READ_PROCESS_MEM,
        &req, sizeof(req),
        &req, sizeof(req),
        &bytesReturned,
        nullptr
    );

    return ok && req.Status == 0 /* STATUS_SUCCESS */;
}

// ---------------------------------------------------------------------------
// Open a handle to the kernel driver device.
// Returns INVALID_HANDLE_VALUE on failure.
// ---------------------------------------------------------------------------

HANDLE OpenDriverHandle()
{
    HANDLE h = CreateFileW(
        DRIVER_DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    return h; // caller checks for INVALID_HANDLE_VALUE
}

// ---------------------------------------------------------------------------
// Smoke-test entry point (compiled only when MODULES_SMOKE_TEST is defined)
// ---------------------------------------------------------------------------

#ifdef MODULES_SMOKE_TEST

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: modules_test <pid> [module_name]\n");
        return 1;
    }

    DWORD pid = static_cast<DWORD>(strtoul(argv[1], nullptr, 10));

    // --- Enumerate modules ---
    std::vector<ModuleEntry> modules;
    if (!EnumerateModules(pid, modules))
    {
        fprintf(stderr, "EnumerateModules failed for PID %lu (error %lu)\n",
                pid, GetLastError());
        return 1;
    }

    printf("Modules in PID %lu:\n", pid);
    for (const auto& m : modules)
        wprintf(L"  %s  base=0x%llx  size=0x%lx\n",
                m.name.c_str(),
                static_cast<unsigned long long>(m.base),
                m.size);

    // --- Resolve base by name (optional second arg) ---
    if (argc >= 3)
    {
        wchar_t wname[256]{};
        mbstowcs_s(nullptr, wname, argv[2], _TRUNCATE);

        uintptr_t base = ResolveModuleBase(modules, wname);
        if (base)
            wprintf(L"ResolveModuleBase(%s) = 0x%llx\n", wname,
                    static_cast<unsigned long long>(base));
        else
            wprintf(L"ResolveModuleBase(%s) => not found\n", wname);

        // --- Read 16 bytes from module base via driver ---
        HANDLE drv = OpenDriverHandle();
        if (drv == INVALID_HANDLE_VALUE)
        {
            fprintf(stderr, "OpenDriverHandle failed (error %lu) – skipping read\n",
                    GetLastError());
        }
        else if (base)
        {
            unsigned char buf[16]{};
            bool ok = DriverReadProcessMemory(drv,
                                              static_cast<ULONG_PTR>(pid),
                                              static_cast<ULONG_PTR>(base),
                                              buf, sizeof(buf));
            if (ok)
            {
                printf("First %zu bytes at base: ", sizeof(buf));
                for (size_t i = 0; i < sizeof(buf); ++i)
                    printf("%02x ", buf[i]);
                printf("\n");
            }
            else
            {
                fprintf(stderr, "DriverReadProcessMemory failed\n");
            }
            CloseHandle(drv);
        }
    }

    return 0;
}

#endif // MODULES_SMOKE_TEST
