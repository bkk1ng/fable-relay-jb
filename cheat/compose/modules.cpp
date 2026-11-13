// modules.cpp
// Process module enumeration, base-resolution by name, and driver-backed
// memory reads for a running process identified by PID.
//
// Build: C++17, user-mode, link with ntdll.lib / psapi.lib.
//   cl /std:c++17 /EHsc /D_MODULES_SMOKE modules.cpp /link /SUBSYSTEM:CONSOLE
//
// All functions referenced from the smoke block are defined in this file.

#include <windows.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Public data types
// ---------------------------------------------------------------------------

// Information about a single loaded module in the target process.
struct ModuleInfo {
    uintptr_t base;          // base load address of the module
    std::size_t size;        // size of the image in bytes
    std::wstring image_path; // fully qualified image path (empty if unknown)
};

// Result of a module base/size/path lookup.
struct ResolvedModule {
    bool        found;       // true when the named module was located
    ModuleInfo  info;       // valid only when found == true
};

// Result of a driver-backed memory read.
struct ReadBuffer {
    bool            success;     // true when the read completed without error
    std::size_t     bytes_read;  // number of bytes actually copied
    std::vector<std::uint8_t> data; // buffer of capacity 'size', bytes_read valid
};

// ---------------------------------------------------------------------------
// IOCTL contract expected from the read-capable driver handle.
// ---------------------------------------------------------------------------
//
// The driver is expected to accept a single method-buffered or method-in-direct
// IOCTL with the control code below. The input buffer carries the target PID,
// the remote address to read from, and the number of bytes requested. The
// output buffer receives the copied bytes and a count.
//
// Driver developers usually pick their own IOCTL code; this file does not
// assume a specific value. Callers must supply the correct code through
// set_driver_ioctl(). If a driver exposes read requests through a different
// transport (e.g. a dedicated read IOCTL per call), the read path can be
// re-pointed by editing ReadProcessMemoryDriver() alone.

#ifndef IOCTL_READ_PROCESS_MEMORY
#define IOCTL_READ_PROCESS_MEMORY 0x222008  // placeholder; override at runtime
#endif

struct DriverReadRequest {
    DWORD      pid;        // target process id
    uintptr_t  address;    // remote address to read from
    std::size_t size;       // number of bytes to read
};

struct DriverReadResponse {
    std::size_t bytes_read; // number of bytes actually copied
    // Followed by 'size' bytes of payload in the same output buffer.
};

// ---------------------------------------------------------------------------
// Module enumeration
// ---------------------------------------------------------------------------

// Enumerate all loaded modules of the process identified by 'pid'.
//
// Returns a vector of ModuleInfo entries. On failure (invalid PID, unable to
// open the process, EnumProcessModulesEx failure) the returned vector is
// empty. A return value of true means 'out' was populated; false means the
// caller should inspect GetLastError() for the cause.
bool EnumerateProcessModules(DWORD pid, std::vector<ModuleInfo>& out) {
    out.clear();

    // PROCESS_QUERY_INFORMATION | PROCESS_VM_READ: the minimum rights needed
    // for EnumProcessModulesEx to walk the remote module list.
    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        pid);
    if (process == nullptr) {
        return false;
    }

    // First call learns the count; second call fills the array. EnumProcessModulesEx
    // with LIST_MODULES_ALL covers 32, 64, and mixed-mode lists on x64.
    HMODULE handles[1024];
    DWORD  needed = 0;

    bool ok = EnumProcessModulesEx(
        process,
        handles,
        sizeof(handles),
        &needed,
        LIST_MODULES_ALL) != 0;

    // The first call may fail because the buffer is too small even though it
    // returns the count. When 'needed' exceeds our static array we fall through
    // to a heap allocation below.
    if (!ok && needed == 0) {
        CloseHandle(process);
        return false;
    }

    DWORD module_count = needed / sizeof(HMODULE);
    std::vector<HMODULE> dyn_handles;

    // Static buffer was big enough and the call succeeded.
    if (ok && module_count <= (sizeof(handles) / sizeof(HMODULE))) {
        for (DWORD i = 0; i < module_count; ++i) {
            ModuleInfo mi{};
            mi.base = reinterpret_cast<uintptr_t>(handles[i]);

            MODULEINFO mod_info{};
            if (GetModuleInformation(process, handles[i], &mod_info, sizeof(mod_info))) {
                mi.size = mod_info.SizeOfImage;
            }

            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameExW(process, handles[i], path, MAX_PATH) > 0) {
                mi.image_path = path;
            }

            out.push_back(std::move(mi));
        }
        CloseHandle(process);
        return true;
    }

    // Static buffer was too small: allocate exactly what is needed and retry.
    dyn_handles.resize(module_count);
    if (!EnumProcessModulesEx(
            process,
            dyn_handles.data(),
            static_cast<DWORD>(dyn_handles.size() * sizeof(HMODULE)),
            &needed,
            LIST_MODULES_ALL)) {
        CloseHandle(process);
        return false;
    }

    module_count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < module_count; ++i) {
        ModuleInfo mi{};
        mi.base = reinterpret_cast<uintptr_t>(dyn_handles[i]);

        MODULEINFO mod_info{};
        if (GetModuleInformation(process, dyn_handles[i], &mod_info, sizeof(mod_info))) {
            mi.size = mod_info.SizeOfImage;
        }

        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameExW(process, dyn_handles[i], path, MAX_PATH) > 0) {
            mi.image_path = path;
        }

        out.push_back(std::move(mi));
    }

    CloseHandle(process);
    return true;
}

// ---------------------------------------------------------------------------
// Module base resolution by name
// ---------------------------------------------------------------------------

// Case-insensitive wide-string comparison that treats backslashes and forward
// slashes as equivalent for path matching.
static bool IEqualsPath(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca == L'/') ca = L'\\';
        if (cb == L'/') cb = L'\\';
        // towlower keeps ASCII fast and handles the common locale cases.
        if (towlower(ca) != towlower(cb)) {
            return false;
        }
    }
    return true;
}

// Extract the trailing file name from a full path.
static std::wstring BaseName(const std::wstring& path) {
    std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

// Resolve the base address, size, and image path of the module whose name
// matches 'module_name'. The match is case-insensitive and compares either
// the full image path or just the base file name, so callers may pass either
// "ntdll.dll" or "C:\Windows\System32\ntdll.dll".
//
// Returns a ResolvedModule. When 'found' is false the rest of the struct is
// zero-initialized.
ResolvedModule ResolveModuleByName(DWORD pid, const std::wstring& module_name) {
    ResolvedModule result{};
    result.found = false;
    result.info.base = 0;
    result.info.size = 0;

    std::vector<ModuleInfo> modules;
    if (!EnumerateProcessModules(pid, modules)) {
        return result;
    }

    for (const auto& m : modules) {
        const std::wstring& full = m.image_path;
        std::wstring base = BaseName(full);

        if (IEqualsPath(full, module_name) || IEqualsPath(base, module_name)) {
            result.found = true;
            result.info.base = m.base;
            result.info.size = m.size;
            result.info.image_path = full;
            return result;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Driver-backed memory read
// ---------------------------------------------------------------------------

// Configuration carried across calls so the read routine does not need a
// global. Callers set this once after opening their driver handle.
struct DriverConfig {
    HANDLE driver;    // handle to a read-capable driver
    DWORD  ioctl;     // IOCTL control code for read requests
};

static DriverConfig g_driver = { nullptr, IOCTL_READ_PROCESS_MEMORY };

// Point the read path at a live driver handle and IOCTL code.
void set_driver_handle(HANDLE driver, DWORD ioctl_code) {
    g_driver.driver = driver;
    g_driver.ioctl  = ioctl_code;
}

// Read 'size' bytes from 'address' in the process 'pid' through the configured
// driver handle. The driver is expected to accept an IOCTL whose input is a
// DriverReadRequest and whose output is a DriverReadResponse header followed
// by the copied bytes.
//
// On success returns a ReadBuffer with success == true, bytes_read populated,
// and 'data' holding the returned bytes (only the first bytes_read are valid).
// On failure success == false and bytes_read is 0.
ReadBuffer ReadProcessMemoryDriver(DWORD pid, uintptr_t address, std::size_t size) {
    ReadBuffer rb{};
    rb.success = false;
    rb.bytes_read = 0;
    rb.data.assign(size, 0);

    if (g_driver.driver == nullptr || g_driver.driver == INVALID_HANDLE_VALUE) {
        return rb;
    }
    if (size == 0) {
        rb.success = true;
        return rb;
    }

    DriverReadRequest req{};
    req.pid     = pid;
    req.address = address;
    req.size    = size;

    // Output layout: [DriverReadResponse header][payload bytes].
    std::vector<std::uint8_t> out_buf(sizeof(DriverReadResponse) + size, 0);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(
        g_driver.driver,
        g_driver.ioctl,
        &req,
        sizeof(req),
        out_buf.data(),
        static_cast<DWORD>(out_buf.size()),
        &returned,
        nullptr);

    if (!ok) {
        return rb;
    }

    if (returned < sizeof(DriverReadResponse)) {
        return rb;
    }

    DriverReadResponse resp{};
    std::memcpy(&resp, out_buf.data(), sizeof(resp));

    if (resp.bytes_read > size) {
        // Driver reported more than we asked for; clamp to the requested size.
        resp.bytes_read = size;
    }

    std::memcpy(rb.data.data(),
                out_buf.data() + sizeof(DriverReadResponse),
                resp.bytes_read);
    rb.bytes_read = resp.bytes_read;
    rb.success = true;
    return rb;
}

// ---------------------------------------------------------------------------
// Smoke entry point
// ---------------------------------------------------------------------------

#ifdef MODULES_SMOKE

#include <cstdio>

int main() {
    DWORD pid = GetCurrentProcessId();

    std::vector<ModuleInfo> mods;
    if (EnumerateProcessModules(pid, mods)) {
        std::printf("Enumerated %zu modules in pid %lu\n", mods.size(), pid);
        for (std::size_t i = 0; i < mods.size() && i < 5; ++i) {
            std::printf("  [%2zu] base=0x%016llx size=%-8llu %ls\n",
                        i,
                        static_cast<unsigned long long>(mods[i].base),
                        static_cast<unsigned long long>(mods[i].size),
                        mods[i].image_path.c_str());
        }
    } else {
        std::printf("EnumerateProcessModules failed: %lu\n", GetLastError());
    }

    ResolvedModule rm = ResolveModuleByName(pid, L"ntdll.dll");
    if (rm.found) {
        std::printf("Resolved ntdll.dll: base=0x%016llx size=%llu path=%ls\n",
                    static_cast<unsigned long long>(rm.info.base),
                    static_cast<unsigned long long>(rm.info.size),
                    rm.info.image_path.c_str());
    } else {
        std::printf("ResolveModuleByName(ntdll.dll) failed\n");
    }

    return 0;
}

#endif // MODULES_SMOKE
