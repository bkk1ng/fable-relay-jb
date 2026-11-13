// inject.cpp — Windows DLL injector (C++17)
// Requires: Windows 7+, x64 or x86 (match target architecture)
// Build: cl /std:c++17 /W4 /O2 inject.cpp /link /OUT:inject.exe

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// -------------------------------------------------------------------------
// Privilege helpers
// -------------------------------------------------------------------------

static bool EnableDebugPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &hToken))
    {
        std::fprintf(stderr, "[!] OpenProcessToken failed: %lu\n", GetLastError());
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
    {
        std::fprintf(stderr, "[!] LookupPrivilegeValue failed: %lu\n", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr))
    {
        std::fprintf(stderr, "[!] AdjustTokenPrivileges failed: %lu\n", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    // AdjustTokenPrivileges succeeds even when the privilege isn't held;
    // ERROR_NOT_ALL_ASSIGNED is the signal that nothing changed.
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        std::fprintf(stderr, "[!] SeDebugPrivilege could not be assigned "
                             "(run as Administrator)\n");
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hToken);
    std::printf("[+] SeDebugPrivilege enabled\n");
    return true;
}

// -------------------------------------------------------------------------
// Module verification via Toolhelp32
// -------------------------------------------------------------------------

static bool ModuleLoaded(DWORD pid, const std::wstring& dllPath)
{
    // Extract just the filename for comparison — Toolhelp returns filenames,
    // not full paths, for modules in remote processes from a 32-bit snapshot.
    std::wstring name = dllPath;
    const auto   sep  = name.find_last_of(L"\\/");
    if (sep != std::wstring::npos)
        name = name.substr(sep + 1);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           pid);
    if (snap == INVALID_HANDLE_VALUE)
    {
        std::fprintf(stderr, "[!] CreateToolhelp32Snapshot failed: %lu\n",
                     GetLastError());
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    bool found = false;
    if (Module32FirstW(snap, &me))
    {
        do {
            if (_wcsicmp(me.szModule, name.c_str()) == 0)
            {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return found;
}

// -------------------------------------------------------------------------
// Core injection
// -------------------------------------------------------------------------

static bool Inject(DWORD pid, const wchar_t* dllPath)
{
    // ---- open target process ----
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD  |
                               PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION   |
                               PROCESS_VM_WRITE       |
                               PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc)
    {
        std::fprintf(stderr, "[!] OpenProcess(%lu) failed: %lu\n",
                     pid, GetLastError());
        return false;
    }
    std::printf("[+] Opened process %lu (handle %p)\n",
                pid, static_cast<void*>(hProc));

    // ---- allocate remote memory for the DLL path ----
    const SIZE_T pathBytes = (std::wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(hProc, nullptr, pathBytes,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!remote)
    {
        std::fprintf(stderr, "[!] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(hProc);
        return false;
    }
    std::printf("[+] Allocated %zu bytes at %p in target\n",
                pathBytes, remote);

    // ---- write the path ----
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remote, dllPath, pathBytes, &written) ||
        written != pathBytes)
    {
        std::fprintf(stderr, "[!] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    std::printf("[+] Wrote DLL path to target memory\n");

    // ---- resolve LoadLibraryW in our own process
    //      (safe: kernel32.dll is mapped at the same base in every process
    //       on the same OS session for the same architecture) ----
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32)
    {
        std::fprintf(stderr, "[!] GetModuleHandleW(kernel32) failed: %lu\n",
                     GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hK32, "LoadLibraryW"));
    if (!loadLib)
    {
        std::fprintf(stderr, "[!] GetProcAddress(LoadLibraryW) failed: %lu\n",
                     GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    std::printf("[+] LoadLibraryW resolved at %p\n",
                reinterpret_cast<void*>(loadLib));

    // ---- create remote thread ----
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                        loadLib, remote,
                                        0, nullptr);
    if (!hThread)
    {
        std::fprintf(stderr, "[!] CreateRemoteThread failed: %lu\n",
                     GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    std::printf("[+] Remote thread created (handle %p), waiting...\n",
                static_cast<void*>(hThread));

    // ---- wait with a 10-second timeout ----
    constexpr DWORD kTimeoutMs = 10'000;
    const DWORD waitResult = WaitForSingleObject(hThread, kTimeoutMs);
    if (waitResult == WAIT_TIMEOUT)
    {
        std::fprintf(stderr, "[!] Remote thread did not finish within %lu ms\n",
                     kTimeoutMs);
        CloseHandle(hThread);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    if (waitResult != WAIT_OBJECT_0)
    {
        std::fprintf(stderr, "[!] WaitForSingleObject failed: %lu\n",
                     GetLastError());
        CloseHandle(hThread);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    std::printf("[+] Remote thread exited with code 0x%08lX\n", exitCode);

    // ---- clean up remote allocations ----
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    // ---- verify module presence ----
    // Brief sleep to let the loader finish updating the module list.
    Sleep(200);

    const bool ok = ModuleLoaded(pid, dllPath);
    if (ok)
        std::printf("[+] Module verified in target process — injection successful\n");
    else
        std::fprintf(stderr, "[!] Module NOT found in snapshot — LoadLibrary may "
                             "have returned NULL (check DLL path and architecture)\n");

    return ok;
}

// -------------------------------------------------------------------------
// Entry point
// -------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::fprintf(stderr, "Usage: inject.exe <pid> <full\\path\\to\\dll>\n");
        return EXIT_FAILURE;
    }

    // Parse PID
    char*       endptr = nullptr;
    const DWORD pid    = static_cast<DWORD>(std::strtoul(argv[1], &endptr, 10));
    if (*endptr != '\0' || pid == 0)
    {
        std::fprintf(stderr, "[!] Invalid PID: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    // Convert DLL path to wide
    // MultiByteToWideChar handles paths with non-ASCII characters correctly.
    const int   wlen    = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, nullptr, 0);
    std::wstring dllPath(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, dllPath.data(), wlen);
    // Remove the null terminator that MultiByteToWideChar wrote into the string.
    if (!dllPath.empty() && dllPath.back() == L'\0')
        dllPath.pop_back();

    std::printf("[*] Target PID : %lu\n", pid);
    std::wprintf(L"[*] DLL path   : %s\n", dllPath.c_str());

    if (!EnableDebugPrivilege())
        std::fprintf(stderr, "[-] Continuing without SeDebugPrivilege "
                             "(may fail on protected processes)\n");

    return Inject(pid, dllPath.c_str()) ? EXIT_SUCCESS : EXIT_FAILURE;
}
