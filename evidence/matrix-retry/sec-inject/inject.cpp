// inject.cpp — Windows DLL injector (C++17)
// Requires: Windows SDK, link against Kernel32.lib, Psapi.lib, Advapi32.lib
// Build: cl /std:c++17 /W4 inject.cpp /link Kernel32.lib Psapi.lib Advapi32.lib
// Run:   inject.exe <pid> <absolute_path_to_dll>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <string>
#include <stdexcept>
#include <filesystem>

// ─── RAII handle wrapper ─────────────────────────────────────────────────────

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE h) : h(h) {}
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    operator HANDLE() const { return h; }
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};

// ─── Error helpers ────────────────────────────────────────────────────────────

[[nodiscard]] static std::string lastErrorMsg(const char* context) {
    DWORD err = GetLastError();
    char buf[512]{};
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, buf, static_cast<DWORD>(sizeof(buf)), nullptr);
    return std::string(context) + " (0x" +
           [&]{ char h[16]{}; sprintf_s(h, "%08X", err); return std::string(h); }() +
           "): " + buf;
}

static void require(bool cond, const char* ctx) {
    if (!cond) throw std::runtime_error(lastErrorMsg(ctx));
}

// ─── SeDebugPrivilege ─────────────────────────────────────────────────────────

static void enableSeDebugPrivilege() {
    HANDLE rawToken{};
    require(
        OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken),
        "OpenProcessToken");
    HandleGuard token(rawToken);

    LUID luid{};
    require(LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &luid),
            "LookupPrivilegeValue(SeDebugPrivilege)");

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    require(AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr),
            "AdjustTokenPrivileges");
    // AdjustTokenPrivileges succeeds even on partial success; check last error.
    require(GetLastError() == ERROR_SUCCESS, "AdjustTokenPrivileges (not all assigned)");
}

// ─── Module presence check via Toolhelp32 snapshot ───────────────────────────

static bool isModuleLoaded(DWORD pid, const std::wstring& dllName) {
    // Extract the filename component for comparison; Module32First only gives
    // the basename, not the full path.
    std::wstring target = std::filesystem::path(dllName).filename().wstring();
    // Lower-case both sides for a case-insensitive match.
    auto toLower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };
    target = toLower(target);

    HandleGuard snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap.valid()) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (!Module32FirstW(snap, &me)) return false;

    do {
        if (toLower(me.szModule) == target) return true;
    } while (Module32NextW(snap, &me));

    return false;
}

// ─── Core injection ───────────────────────────────────────────────────────────

static void inject(DWORD pid, const std::wstring& dllPath) {
    // Validate the DLL exists before touching the target process.
    if (!std::filesystem::exists(dllPath))
        throw std::runtime_error("DLL not found: " +
                                 std::filesystem::path(dllPath).string());

    // Open the target with the rights we need.
    HandleGuard proc(OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE |
        PROCESS_VM_READ,
        FALSE, pid));
    require(proc.valid(), "OpenProcess");

    // Allocate enough space for the wide path including the null terminator.
    const std::size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, nullptr, pathBytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    require(remote != nullptr, "VirtualAllocEx");

    // Write the DLL path into the remote allocation.
    SIZE_T written{};
    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), pathBytes, &written) ||
        written != pathBytes)
    {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        throw std::runtime_error(lastErrorMsg("WriteProcessMemory"));
    }

    // Resolve LoadLibraryW in the injector process; the address is stable
    // across processes for the same OS/bitness combination.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    require(k32 != nullptr, "GetModuleHandleW(kernel32.dll)");

    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    require(loadLib != nullptr, "GetProcAddress(LoadLibraryW)");

    // Spawn the remote thread that calls LoadLibraryW(dllPath).
    HandleGuard thread(CreateRemoteThread(proc, nullptr, 0,
                                          loadLib, remote, 0, nullptr));
    if (!thread.valid()) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        throw std::runtime_error(lastErrorMsg("CreateRemoteThread"));
    }

    // Wait up to 10 seconds for the loader to finish.
    constexpr DWORD kTimeoutMs = 10'000;
    DWORD waitResult = WaitForSingleObject(thread, kTimeoutMs);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE); // free regardless of outcome

    if (waitResult == WAIT_TIMEOUT)
        throw std::runtime_error("Remote thread timed out after 10 s");
    if (waitResult != WAIT_OBJECT_0)
        throw std::runtime_error(lastErrorMsg("WaitForSingleObject"));

    // Check the thread exit code; LoadLibraryW returns the HMODULE on success.
    DWORD exitCode{};
    require(GetExitCodeThread(thread, &exitCode), "GetExitCodeThread");
    if (exitCode == 0)
        throw std::runtime_error("LoadLibraryW in target returned NULL — "
                                 "check DLL dependencies and bitness match");

    // Confirm via snapshot that the module actually appears in the target.
    if (!isModuleLoaded(pid, dllPath))
        throw std::runtime_error("Module not found in snapshot after injection");

    std::wcout << L"[+] Injected successfully: " << dllPath << L'\n';
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: inject.exe <pid> <absolute_dll_path>\n";
        return EXIT_FAILURE;
    }

    DWORD pid{};
    try {
        pid = static_cast<DWORD>(std::stoul(argv[1]));
    } catch (...) {
        std::cerr << "[-] Invalid PID: " << argv[1] << '\n';
        return EXIT_FAILURE;
    }

    // Convert the path argument to a wide string.
    int wLen = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, nullptr, 0);
    if (wLen <= 0) {
        std::cerr << "[-] Path encoding error\n";
        return EXIT_FAILURE;
    }
    std::wstring dllPath(static_cast<std::size_t>(wLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, dllPath.data(), wLen);
    dllPath.resize(dllPath.size() - 1); // drop the null terminator std::wstring added

    try {
        enableSeDebugPrivilege();
        std::cout << "[*] SeDebugPrivilege enabled\n";

        inject(pid, dllPath);
    } catch (const std::exception& ex) {
        std::cerr << "[-] " << ex.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
