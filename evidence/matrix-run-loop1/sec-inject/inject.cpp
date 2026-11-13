// inject.cpp — Windows DLL injector (C++17)
//
// Build (MSVC):  cl /std:c++17 /EHsc inject.cpp /link advapi32.lib
// Build (MinGW): g++ -std=c++17 -o inject.exe inject.cpp -ladvapi32
//
// Usage: inject.exe <pid> <path\to\dll.dll>
//   pid    Target process ID (decimal)
//   path   Path to the DLL to inject (relative or absolute)
//
// Run elevated for cross-session / cross-process injection.

#include <windows.h>
#include <tlhelp32.h>
#include <cwctype>
#include <string>
#include <string_view>
#include <iostream>
#include <system_error>
#include <memory>
#include <type_traits>

namespace {

// ---------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------

struct HandleDeleter {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

// Frees VirtualAllocEx memory on scope exit — prevents leaks if a later
// step in the injection sequence throws.
struct RemoteMemGuard {
    HANDLE proc = nullptr;
    LPVOID ptr  = nullptr;
    ~RemoteMemGuard() {
        if (proc && ptr)
            VirtualFreeEx(proc, ptr, 0, MEM_RELEASE);
    }
};

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

[[noreturn]] void throw_last_error(std::string_view msg) {
    throw std::system_error(
        static_cast<int>(GetLastError()),
        std::system_category(),
        std::string(msg));
}

// ---------------------------------------------------------------------------
// String conversion
// ---------------------------------------------------------------------------

// Convert a UTF-8 narrow string to a UTF-16 wide string.
std::wstring widen(const char* utf8) {
    if (!utf8 || !*utf8)
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len == 0)
        throw_last_error("MultiByteToWideChar (sizing)");
    std::wstring out(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), len) == 0)
        throw_last_error("MultiByteToWideChar (conversion)");
    out.resize(std::wcslen(out.c_str()));   // trim the embedded null terminator
    return out;
}

// ---------------------------------------------------------------------------
// Privilege escalation
// ---------------------------------------------------------------------------

// Enable SeDebugPrivilege in the current process token so we can open
// processes running under other sessions or service accounts.
void enable_debug_privilege() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken))
        throw_last_error("OpenProcessToken");
    UniqueHandle token(rawToken);

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
        throw_last_error("LookupPrivilegeValueW");

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        DWORD ec = GetLastError();
        // ERROR_NOT_ALL_ASSIGNED means the privilege wasn't in the token (non-admin).
        if (ec == ERROR_NOT_ALL_ASSIGNED)
            throw std::system_error(static_cast<int>(ec), std::system_category(),
                "SeDebugPrivilege not present — run as administrator");
        throw_last_error("AdjustTokenPrivileges");
    }
}

// ---------------------------------------------------------------------------
// Module verification
// ---------------------------------------------------------------------------

// Case-insensitive wide-string comparison.
bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(static_cast<wint_t>(a[i])) !=
            std::towlower(static_cast<wint_t>(b[i])))
            return false;
    return true;
}

// Extract the file-name component from a full path.
std::wstring base_name(std::wstring_view path) {
    size_t slash = path.find_last_of(L"\\/");
    return (slash != std::wstring_view::npos)
        ? std::wstring(path.substr(slash + 1))
        : std::wstring(path);
}

// Walk a Toolhelp32 module snapshot for the given PID and report whether
// a module matching the supplied DLL's base name is present.
// Uses TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32 to enumerate both 32-bit and
// 64-bit module lists in the target.
bool module_loaded(DWORD pid, std::wstring_view dllPath) {
    std::wstring target = base_name(dllPath);

    HANDLE rawSnap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (rawSnap == INVALID_HANDLE_VALUE)
        throw_last_error("CreateToolhelp32Snapshot");
    UniqueHandle snap(rawSnap);

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (!Module32FirstW(snap.get(), &me))
        return false;   // no modules at all — injection surely failed

    do {
        // me.szModule is the base file name without the directory path.
        if (iequals(me.szModule, target))
            return true;
    } while (Module32NextW(snap.get(), &me));

    return false;
}

// ---------------------------------------------------------------------------
// Core injection
// ---------------------------------------------------------------------------

// Inject a DLL into a target process by:
//   1. Opening the process with VM + thread-creation rights
//   2. Allocating remote memory for the wide DLL path
//   3. Writing the DLL path into the target
//   4. Resolving LoadLibraryW from kernel32.dll
//   5. Spawning a remote thread whose entry point is LoadLibraryW
//   6. Waiting (with timeout) for the thread to return
//
// Returns the exit code of LoadLibraryW (the HMODULE as a DWORD), or 0 on failure.
DWORD inject_dll(DWORD pid, const std::wstring& dllPath) {
    // --- Open the target process with the rights we need ---
    // PROCESS_CREATE_THREAD                — CreateRemoteThread
    // PROCESS_VM_OPERATION                 — VirtualAllocEx / VirtualFreeEx
    // PROCESS_VM_WRITE                      — WriteProcessMemory
    // PROCESS_VM_READ                        — optional, for diagnostics
    // PROCESS_QUERY_LIMITED_INFORMATION      — needed for verification paths
    HANDLE rawProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!rawProc)
        throw_last_error("OpenProcess");
    UniqueHandle proc(rawProc);

    // --- Allocate remote memory for the wide DLL path (null-terminated) ---
    SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(proc.get(), nullptr, pathBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
        throw_last_error("VirtualAllocEx");

    // Guard: auto-frees remote memory if any subsequent step throws.
    RemoteMemGuard memGuard{ proc.get(), remotePath };

    // --- Write the DLL path into the target's address space ---
    SIZE_T written = 0;
    if (!WriteProcessMemory(proc.get(), remotePath,
            dllPath.data(), pathBytes, &written))
        throw_last_error("WriteProcessMemory");
    if (written != pathBytes)
        throw std::runtime_error("WriteProcessMemory: partial write");

    // --- Resolve LoadLibraryW ---
    // kernel32.dll is mapped at the same address in every process on a
    // given boot session, so the address we resolve locally is valid in
    // the remote process.
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32)
        throw_last_error("GetModuleHandleW(kernel32.dll)");

    FARPROC loadLib = GetProcAddress(hK32, "LoadLibraryW");
    if (!loadLib)
        throw_last_error("GetProcAddress(LoadLibraryW)");

    // LoadLibraryW has signature HMODULE WINAPI (*)(LPCWSTR), which differs
    // from LPTHREAD_START_ROUTINE only in return type. The calling convention
    // (WINAPI/__stdcall on x86, default on x64) matches, so a reinterpret_cast
    // is safe here — CreateRemoteThread will call it correctly.
    auto pfnLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(loadLib));

    // --- Create the remote thread ---
    // The thread's lpParameter is the pointer to the remote DLL path string.
    // When the thread starts it calls LoadLibraryW(remotePath), loading the DLL.
    HANDLE rawThread = CreateRemoteThread(
        proc.get(), nullptr, 0,
        pfnLoadLib, remotePath, 0, nullptr);
    if (!rawThread)
        throw_last_error("CreateRemoteThread");
    UniqueHandle thread(rawThread);

    // --- Wait for the thread to finish, with a timeout ---
    constexpr DWORD TIMEOUT_MS = 10000;  // 10 seconds

    DWORD waitResult = WaitForSingleObject(thread.get(), TIMEOUT_MS);

    if (waitResult == WAIT_TIMEOUT) {
        // The DLL is still loading (maybe it's doing heavy work in DllMain,
        // or the loader hit a deadlock). Kill the thread so we don't leave
        // a dangling remote thread, then report the failure.
        TerminateThread(thread.get(), 1);
        throw std::runtime_error(
            "Remote LoadLibraryW timed out after "
            + std::to_string(TIMEOUT_MS) + " ms");
    }
    if (waitResult == WAIT_FAILED)
        throw_last_error("WaitForSingleObject");

    // --- Retrieve the thread exit code ---
    // This is the HMODULE returned by LoadLibraryW (non-zero on success),
    // or 0 (NULL) if the load failed.
    DWORD exitCode = 0;
    if (!GetExitCodeThread(thread.get(), &exitCode))
        throw_last_error("GetExitCodeThread");

    // --- Free the remote string memory ---
    // The thread has already consumed it by this point.
    VirtualFreeEx(proc.get(), remotePath, 0, MEM_RELEASE);
    memGuard.ptr = nullptr;   // prevent double-free in the guard destructor

    return exitCode;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void print_usage() {
    std::cerr <<
        "Usage: inject.exe <pid> <path\\to\\module.dll>\n"
        "  pid    Target process ID (decimal)\n"
        "  path   Full or relative path to the DLL to inject\n"
        "\n"
        "Run elevated for cross-session / cross-process injection.\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc != 3) {
        print_usage();
        return 1;
    }

    try {
        // --- Parse PID ---
        unsigned long pid = 0;
        try {
            size_t pos = 0;
            pid = std::stoul(argv[1], &pos, 10);
            if (pos != std::string(argv[1]).size())
                throw std::invalid_argument("trailing characters");
        } catch (const std::exception&) {
            std::cerr << "Error: '" << argv[1]
                      << "' is not a valid process ID.\n\n";
            print_usage();
            return 1;
        }

        // --- Resolve the DLL path to absolute form ---
        // An absolute path avoids ambiguity in the target process's working
        // directory and makes verification cleaner.
        std::wstring dllPath = widen(argv[2]);

        DWORD absLen = GetFullPathNameW(dllPath.c_str(), 0, nullptr, nullptr);
        if (absLen == 0)
            throw_last_error("GetFullPathNameW (sizing)");
        std::wstring absPath(static_cast<size_t>(absLen), L'\0');
        if (GetFullPathNameW(dllPath.c_str(), absLen, absPath.data(), nullptr) == 0)
            throw_last_error("GetFullPathNameW (resolve)");
        absPath.resize(std::wcslen(absPath.c_str()));
        dllPath = absPath;

        // --- Verify the DLL file exists before touching the target ---
        DWORD attr = GetFileAttributesW(dllPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            std::cerr << "Error: DLL not found: " << argv[2] << "\n";
            return 1;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            std::cerr << "Error: Path is a directory, not a file: " << argv[2] << "\n";
            return 1;
        }

        // --- Enable SeDebugPrivilege ---
        std::wcout << L"[+] Enabling SeDebugPrivilege...\n";
        enable_debug_privilege();

        // --- Inject ---
        std::wcout << L"[+] Target PID: " << pid << L"\n";
        std::wcout << L"[+] DLL path:   " << dllPath << L"\n";
        std::wcout << L"[+] Injecting...\n";

        DWORD exitCode = inject_dll(static_cast<DWORD>(pid), dllPath);

        if (exitCode == 0) {
            std::cerr << "LoadLibraryW returned NULL — the DLL failed to load.\n"
                         "Check architecture match (x64 vs x86) and DLL dependencies.\n";
            return 1;
        }

        std::wcout << L"[+] LoadLibraryW returned 0x"
                   << std::hex << exitCode << std::dec << L"\n";

        // --- Verify the module is actually loaded via Toolhelp32 snapshot ---
        std::wcout << L"[+] Verifying module presence...\n";
        if (module_loaded(static_cast<DWORD>(pid), dllPath)) {
            std::wcout << L"[+] Module confirmed in target process.\n";
            return 0;
        }

        std::cerr << "[!] Module not found in snapshot — it may have been "
                     "unloaded immediately (DllMain returned FALSE) or the "
                     "load failed silently.\n";
        return 1;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
