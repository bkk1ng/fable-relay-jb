// inject.cpp — Windows DLL injector (C++17)
// Target: 64-bit processes on Windows (MSVC or MinGW-w64)
// Build:  cl /std:c++17 /W4 /O2 inject.cpp /link /OUT:inject.exe
//         g++ -std=c++17 -O2 -Wall inject.cpp -o inject.exe -lpsapi

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

// ── helpers ──────────────────────────────────────────────────────────────────

[[nodiscard]] static std::string win_error(DWORD code = GetLastError())
{
    char buf[512]{};
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buf, sizeof(buf), nullptr);
    // strip trailing whitespace / newlines
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s + " (" + std::to_string(code) + ')';
}

// RAII handle wrapper — closes on destruction, never double-closes.
struct Handle
{
    HANDLE h = INVALID_HANDLE_VALUE;

    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { if (valid()) CloseHandle(h); }

    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }

    [[nodiscard]] bool valid() const
    {
        return h != INVALID_HANDLE_VALUE && h != nullptr;
    }
    [[nodiscard]] HANDLE get() const { return h; }
};

// ── privilege ────────────────────────────────────────────────────────────────

static void enable_sedebug()
{
    HANDLE raw_token{};
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &raw_token))
        throw std::runtime_error("OpenProcessToken: " + win_error());

    Handle token(raw_token);

    LUID luid{};
    if (!LookupPrivilegeValueA(nullptr, SE_DEBUG_NAME, &luid))
        throw std::runtime_error("LookupPrivilegeValue: " + win_error());

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr))
        throw std::runtime_error("AdjustTokenPrivileges: " + win_error());

    // AdjustTokenPrivileges succeeds even when the privilege is unavailable;
    // GetLastError distinguishes the two cases.
    const DWORD err = GetLastError();
    if (err == ERROR_NOT_ALL_ASSIGNED)
        throw std::runtime_error("SeDebugPrivilege not held by this token — run as Administrator.");
}

// ── injection ────────────────────────────────────────────────────────────────

static void inject(DWORD pid, const fs::path& dll_path)
{
    // Resolve and validate the DLL path before touching the target.
    const std::wstring abs_path = fs::absolute(dll_path).wstring();
    if (!fs::exists(abs_path))
        throw std::runtime_error("DLL not found: " + dll_path.string());

    // Byte size of the wide string including the null terminator.
    const SIZE_T path_bytes = (abs_path.size() + 1) * sizeof(wchar_t);

    // ── open target process ───────────────────────────────────────────────
    Handle proc(OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_READ |
        PROCESS_VM_WRITE |
        PROCESS_VM_OPERATION,
        FALSE, pid));

    if (!proc.valid())
        throw std::runtime_error("OpenProcess(" + std::to_string(pid) + "): " + win_error());

    // ── allocate remote memory ────────────────────────────────────────────
    void* remote_buf = VirtualAllocEx(
        proc.get(), nullptr, path_bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!remote_buf)
        throw std::runtime_error("VirtualAllocEx: " + win_error());

    // Ensure the allocation is freed on any early exit.
    // (The thread below will have already finished using it.)
    auto free_remote = [&]() noexcept {
        VirtualFreeEx(proc.get(), remote_buf, 0, MEM_RELEASE);
    };

    // ── write DLL path ────────────────────────────────────────────────────
    SIZE_T written{};
    if (!WriteProcessMemory(proc.get(), remote_buf,
                            abs_path.c_str(), path_bytes, &written)
        || written != path_bytes)
    {
        free_remote();
        throw std::runtime_error("WriteProcessMemory: " + win_error());
    }

    // ── resolve LoadLibraryW ──────────────────────────────────────────────
    // kernel32.dll is mapped at the same base address in every process on the
    // same OS session, so the local function pointer is valid remotely.
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32)
    {
        free_remote();
        throw std::runtime_error("GetModuleHandleW(kernel32): " + win_error());
    }

    const auto load_library =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(k32, "LoadLibraryW"));

    if (!load_library)
    {
        free_remote();
        throw std::runtime_error("GetProcAddress(LoadLibraryW): " + win_error());
    }

    // ── create remote thread ──────────────────────────────────────────────
    Handle thread(CreateRemoteThread(
        proc.get(), nullptr, 0,
        load_library, remote_buf,
        0, nullptr));

    if (!thread.valid())
    {
        free_remote();
        throw std::runtime_error("CreateRemoteThread: " + win_error());
    }

    // ── wait with timeout (10 s) ──────────────────────────────────────────
    constexpr DWORD TIMEOUT_MS = 10'000;
    const DWORD wait = WaitForSingleObject(thread.get(), TIMEOUT_MS);

    if (wait == WAIT_TIMEOUT)
    {
        free_remote();
        throw std::runtime_error("Remote thread timed out after " +
                                 std::to_string(TIMEOUT_MS) + " ms.");
    }
    if (wait != WAIT_OBJECT_0)
    {
        free_remote();
        throw std::runtime_error("WaitForSingleObject: " + win_error());
    }

    // Thread exit code is the HMODULE returned by LoadLibraryW; 0 means failure.
    DWORD exit_code{};
    GetExitCodeThread(thread.get(), &exit_code);

    free_remote();

    if (exit_code == 0)
        throw std::runtime_error("LoadLibraryW in target returned NULL — "
                                 "check the DLL path and bitness match.");

    // ── verify: DLL present in Toolhelp32 snapshot ───────────────────────
    const std::wstring dll_name = fs::path(abs_path).filename().wstring();

    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap.valid())
        throw std::runtime_error("CreateToolhelp32Snapshot: " + win_error());

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    bool found = false;
    for (BOOL ok = Module32FirstW(snap.get(), &me); ok; ok = Module32NextW(snap.get(), &me))
    {
        if (_wcsicmp(me.szModule, dll_name.c_str()) == 0)
        {
            found = true;
            break;
        }
    }

    if (!found)
        throw std::runtime_error("Injection appeared to succeed but the module "
                                 "was not found in the target's module list.");

    std::wcout << L"[+] Injected: " << abs_path << L"\n"
               << L"    Module base in PID " << pid
               << L": 0x" << std::hex << reinterpret_cast<uintptr_t>(me.modBaseAddr)
               << std::dec << L"\n";
}

// ── entry point ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: inject.exe <pid> <path\\to\\payload.dll>\n";
        return 1;
    }

    DWORD pid{};
    try { pid = static_cast<DWORD>(std::stoul(argv[1])); }
    catch (...) {
        std::cerr << "[-] Invalid PID: " << argv[1] << "\n";
        return 1;
    }

    try
    {
        enable_sedebug();
        inject(pid, argv[2]);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[-] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
