// modules.cpp
// Enumerate modules of a given PID, resolve module base by name,
// and read process memory through a driver handle via a single request struct.

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

// ---------------------------------------------------------------------------
// IOCTL / driver interface
// ---------------------------------------------------------------------------

#define DRIVER_NAME       L"\\\\.\\ModulesDriver"
#define IOCTL_READ_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Single request struct used for the driver read call
struct MemReadRequest {
    ULONG_PTR ProcessId;   // target PID
    ULONG_PTR Address;     // virtual address in target
    ULONG64   Size;        // bytes to read
    PVOID     OutBuffer;   // pointer to caller-allocated output buffer
    ULONG64   BytesRead;   // filled in by driver / IOCTL handler
};

// ---------------------------------------------------------------------------
// Module enumeration via Toolhelp32
// ---------------------------------------------------------------------------

struct ModuleEntry {
    wchar_t   Name[MAX_PATH];
    ULONG_PTR BaseAddress;
    DWORD     ModuleSize;
};

// Returns the number of entries written to `out` (up to `maxCount`).
// Returns -1 on error.
int EnumerateModules(DWORD pid, ModuleEntry* out, int maxCount)
{
    if (!out || maxCount <= 0)
        return -1;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    int count = 0;
    BOOL ok = Module32FirstW(snap, &me);
    while (ok && count < maxCount) {
        wcsncpy_s(out[count].Name, MAX_PATH, me.szModule, _TRUNCATE);
        out[count].BaseAddress = reinterpret_cast<ULONG_PTR>(me.modBaseAddr);
        out[count].ModuleSize  = me.modBaseSize;
        ++count;
        ok = Module32NextW(snap, &me);
    }

    CloseHandle(snap);
    return count;
}

// ---------------------------------------------------------------------------
// Resolve a module base address by name
// ---------------------------------------------------------------------------

// Name comparison is case-insensitive.
// Returns 0 if not found.
ULONG_PTR ResolveModuleBase(DWORD pid, const wchar_t* moduleName)
{
    ModuleEntry entries[1024];
    int count = EnumerateModules(pid, entries, 1024);
    if (count <= 0)
        return 0;

    for (int i = 0; i < count; ++i) {
        if (_wcsicmp(entries[i].Name, moduleName) == 0)
            return entries[i].BaseAddress;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Read process memory through a driver handle (single MemReadRequest)
// ---------------------------------------------------------------------------

// Opens the driver device, fires one IOCTL with the populated MemReadRequest,
// and writes the result into `buffer`.
// Returns TRUE on success, FALSE on failure.
// `bytesActuallyRead` receives the count reported by the driver.
BOOL ReadProcessMemoryViaDriver(
    DWORD     pid,
    ULONG_PTR address,
    PVOID     buffer,
    SIZE_T    size,
    SIZE_T*   bytesActuallyRead)
{
    if (!buffer || size == 0)
        return FALSE;

    HANDLE hDriver = CreateFileW(
        DRIVER_NAME,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hDriver == INVALID_HANDLE_VALUE)
        return FALSE;

    MemReadRequest req{};
    req.ProcessId = static_cast<ULONG_PTR>(pid);
    req.Address   = address;
    req.Size      = static_cast<ULONG64>(size);
    req.OutBuffer = buffer;
    req.BytesRead = 0;

    DWORD returned = 0;
    BOOL  result   = DeviceIoControl(
        hDriver,
        IOCTL_READ_MEMORY,
        &req, sizeof(req),   // input  buffer  → request
        &req, sizeof(req),   // output buffer  → driver fills BytesRead
        &returned,
        nullptr);

    if (bytesActuallyRead)
        *bytesActuallyRead = static_cast<SIZE_T>(req.BytesRead);

    CloseHandle(hDriver);
    return result;
}

// ---------------------------------------------------------------------------
// Standalone smoke / demo block
// ---------------------------------------------------------------------------

#ifdef MODULES_SMOKE_TEST

int main()
{
    // Use an explicit PID for testing; replace with a real target PID.
    DWORD testPid = GetCurrentProcessId();

    // --- Enumerate modules ---
    printf("Modules in PID %lu:\n", testPid);
    ModuleEntry entries[256];
    int count = EnumerateModules(testPid, entries, 256);
    if (count < 0) {
        printf("  EnumerateModules failed (err=%lu)\n", GetLastError());
    } else {
        for (int i = 0; i < count; ++i) {
            printf("  [%3d] 0x%016llX  %ls\n",
                i,
                static_cast<unsigned long long>(entries[i].BaseAddress),
                entries[i].Name);
        }
    }

    // --- Resolve a known module ---
    const wchar_t* target = L"ntdll.dll";
    ULONG_PTR base = ResolveModuleBase(testPid, target);
    if (base)
        printf("\nBase of %ls: 0x%016llX\n", target, static_cast<unsigned long long>(base));
    else
        printf("\nModule %ls not found.\n", target);

    // --- Driver read (requires driver to be loaded) ---
    if (base) {
        BYTE    header[64]  = {};
        SIZE_T  nRead       = 0;
        BOOL    ok = ReadProcessMemoryViaDriver(testPid, base, header, sizeof(header), &nRead);
        if (ok)
            printf("Driver read %zu bytes from 0x%016llX (first 2: %02X %02X)\n",
                nRead, static_cast<unsigned long long>(base),
                header[0], header[1]);
        else
            printf("Driver read failed (err=%lu) – driver may not be loaded.\n", GetLastError());
    }

    return 0;
}

// Build (MSVC):
//   cl /std:c++17 /DMODULES_SMOKE_TEST modules.cpp /link psapi.lib
//
// Build (MinGW):
//   g++ -std=c++17 -DMODULES_SMOKE_TEST modules.cpp -o modules -lpsapi

#endif // MODULES_SMOKE_TEST
