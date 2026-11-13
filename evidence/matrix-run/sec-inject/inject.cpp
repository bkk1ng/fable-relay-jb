// inject.cpp — DLL injector (C++17, Windows)
// Requires linking against: kernel32.lib, tlhelp32.lib
// Build: cl /std:c++17 /W4 /EHsc inject.cpp /link kernel32.lib tlhelp32.lib

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

// ----------------------------------------------------------------------------
// Privilege helpers
// ----------------------------------------------------------------------------

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

    // AdjustTokenPrivileges succeeds even when the privilege was not assigned;
    // GetLastError distinguishes the two cases.
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        std::fprintf(stderr, "[!] SeDebugPrivilege not assigned to this token.\n");
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hToken);
    std::printf("[+] SeDebugPrivilege enabled.\n");
    return true;
}

// ----------------------------------------------------------------------------
// Module presence check via Toolhelp32 snapshot
// ----------------------------------------------------------------------------

static bool IsModuleLoaded(DWORD pid, const std::wstring& dllPath)
{
    // Extract just the filename for comparison — Toolhelp returns filenames, not full paths.
    std::wstring name = dllPath;
    const auto   sep  = name.find_last_of(L"\\/");
    if (sep != std::wstring::npos)
        name = name.substr(sep + 1);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE)
    {
        std::fprintf(stderr, "[!] CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    bool found = false;
    if (Module32FirstW(hSnap, &me))
    {
        do {
            if (_wcsicmp(me.szModule, name.c_str()) == 0)
            {
                found = true;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }

    CloseHandle(hSnap);
    return found;
}

// ----------------------------------------------------------------------------
// Core injection
// ----------------------------------------------------------------------------

static bool InjectDll(DWORD pid, const std::wstring& dllPath)
{
    // --- 1. Open the target process ----------------------------------------
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD  |
        PROCESS_VM_OPERATION   |
        PROCESS_VM_WRITE       |
        PROCESS_VM_READ        |
        PROCESS_QUERY_INFORMATION,
        FALSE,
        pid);

    if (!hProcess)
    {
        std::fprintf(stderr, "[!] OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return false;
    }
    std::printf("[+] Opened process %lu.\n", pid);

    // --- 2. Allocate memory for the DLL path --------------------------------
    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID remoteBuffer = VirtualAllocEx(
        hProcess,
        nullptr,
        pathBytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (!remoteBuffer)
    {
        std::fprintf(stderr, "[!] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(hProcess);
        return false;
    }
    std::printf("[+] Allocated %zu bytes at %p in target.\n", pathBytes, remoteBuffer);

    // --- 3. Write the DLL path ----------------------------------------------
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, remoteBuffer,
                            dllPath.c_str(), pathBytes, &written)
        || written != pathBytes)
    {
        std::fprintf(stderr, "[!] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    std::printf("[+] DLL path written to target.\n");

    // --- 4. Resolve LoadLibraryW in the injector's address space ------------
    // kernel32.dll is mapped to the same VA in every process on Windows, so the
    // address we read here is valid in the target.
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32)
    {
        std::fprintf(stderr, "[!] GetModuleHandleW(kernel32) failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    auto pLoadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hKernel32, "LoadLibraryW"));

    if (!pLoadLibraryW)
    {
        std::fprintf(stderr, "[!] GetProcAddress(LoadLibraryW) failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    std::printf("[+] LoadLibraryW resolved at %p.\n",
                reinterpret_cast<void*>(pLoadLibraryW));

    // --- 5. Spawn the remote thread -----------------------------------------
    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,          // default security
        0,                // default stack
        pLoadLibraryW,
        remoteBuffer,     // argument: pointer to DLL path in target
        0,
        nullptr);

    if (!hThread)
    {
        std::fprintf(stderr, "[!] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    std::printf("[+] Remote thread created (handle %p).\n", hThread);

    // --- 6. Wait with a 10-second timeout -----------------------------------
    constexpr DWORD kTimeoutMs = 10'000;
    DWORD waitResult = WaitForSingleObject(hThread, kTimeoutMs);

    bool success = false;
    switch (waitResult)
    {
    case WAIT_OBJECT_0:
    {
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        // LoadLibraryW returns the HMODULE; 0 means failure.
        if (exitCode != 0)
        {
            std::printf("[+] Remote thread exited (LoadLibraryW returned %#lx).\n",
                        exitCode);
            success = true;
        }
        else
        {
            std::fprintf(stderr, "[!] Remote thread exited but LoadLibraryW returned NULL.\n");
        }
        break;
    }
    case WAIT_TIMEOUT:
        std::fprintf(stderr, "[!] Remote thread timed out after %lu ms.\n", kTimeoutMs);
        break;
    default:
        std::fprintf(stderr, "[!] WaitForSingleObject failed: %lu\n", GetLastError());
        break;
    }

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteBuffer, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    // --- 7. Verify via Toolhelp32 snapshot ----------------------------------
    if (success)
    {
        if (IsModuleLoaded(pid, dllPath))
            std::printf("[+] Module confirmed present in target process.\n");
        else
            std::fprintf(stderr,
                "[!] Thread returned success but module not found in snapshot — "
                "may still be loading or was unloaded immediately.\n");
    }

    return success;
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::fprintf(stderr, "Usage: inject.exe <pid> <full\\path\\to\\payload.dll>\n");
        return 1;
    }

    DWORD pid = static_cast<DWORD>(std::stoul(argv[1]));

    // Convert the DLL path to wide string.
    const std::string  pathNarrow(argv[2]);
    const std::wstring dllPath(pathNarrow.cbegin(), pathNarrow.cend());

    std::printf("[*] Target PID  : %lu\n", pid);
    std::wprintf(L"[*] DLL path    : %s\n", dllPath.c_str());

    if (!EnableDebugPrivilege())
        std::fprintf(stderr, "[-] Continuing without SeDebugPrivilege (may fail on protected processes).\n");

    if (!InjectDll(pid, dllPath))
    {
        std::fprintf(stderr, "[-] Injection failed.\n");
        return 1;
    }

    std::printf("[+] Injection complete.\n");
    return 0;
}
