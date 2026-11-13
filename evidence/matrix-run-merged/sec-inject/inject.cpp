// inject.cpp — DLL injector (C++17, Windows x64)
// Enables SeDebugPrivilege, injects a DLL into a target process by PID,
// then verifies the module is loaded via a Toolhelp32 snapshot.

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <stdexcept>
#include <iostream>
#include <filesystem>

// ─── privilege helpers ────────────────────────────────────────────────────────

static void EnableDebugPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &hToken))
    {
        throw std::runtime_error("OpenProcessToken failed: " +
                                 std::to_string(GetLastError()));
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
    {
        CloseHandle(hToken);
        throw std::runtime_error("LookupPrivilegeValue failed: " +
                                 std::to_string(GetLastError()));
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr) ||
        GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        CloseHandle(hToken);
        throw std::runtime_error("AdjustTokenPrivileges failed: " +
                                 std::to_string(GetLastError()));
    }

    CloseHandle(hToken);
}

// ─── verification helper ─────────────────────────────────────────────────────

// Returns true if `moduleName` (filename only, case-insensitive) is loaded in
// the process identified by `pid`.
static bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName)
{
    // Extract filename component so callers can pass a full path.
    std::wstring name = std::filesystem::path(moduleName).filename().wstring();

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    bool found = false;
    if (Module32FirstW(hSnap, &me))
    {
        do
        {
            // Case-insensitive compare on filename only.
            std::wstring loadedName = std::filesystem::path(me.szModule).filename().wstring();
            if (_wcsicmp(loadedName.c_str(), name.c_str()) == 0)
            {
                found = true;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }

    CloseHandle(hSnap);
    return found;
}

// ─── core injector ───────────────────────────────────────────────────────────

static void InjectDll(DWORD pid, const std::wstring& dllPath)
{
    // Resolve the absolute path so the remote process can locate the file.
    std::wstring absPath = std::filesystem::absolute(dllPath).wstring();
    const SIZE_T pathBytes = (absPath.size() + 1) * sizeof(wchar_t);

    // ── 1. Open the target process ──────────────────────────────────────────
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE |
        PROCESS_VM_READ,
        FALSE,
        pid);

    if (!hProcess)
        throw std::runtime_error("OpenProcess failed: " +
                                 std::to_string(GetLastError()));

    // RAII wrapper — frees the remote buffer and process handle on any exit.
    LPVOID remoteBuf = nullptr;
    auto cleanup = [&]() noexcept {
        if (remoteBuf)
            VirtualFreeEx(hProcess, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
    };

    // ── 2. Allocate remote memory for the DLL path ──────────────────────────
    remoteBuf = VirtualAllocEx(hProcess, nullptr, pathBytes,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf)
    {
        cleanup();
        throw std::runtime_error("VirtualAllocEx failed: " +
                                 std::to_string(GetLastError()));
    }

    // ── 3. Write the DLL path into the remote buffer ────────────────────────
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, remoteBuf,
                            absPath.c_str(), pathBytes, &written) ||
        written != pathBytes)
    {
        cleanup();
        throw std::runtime_error("WriteProcessMemory failed: " +
                                 std::to_string(GetLastError()));
    }

    // ── 4. Resolve LoadLibraryW in the local process ────────────────────────
    // kernel32.dll is mapped at the same base address in every process on the
    // same OS session, so the local function pointer is valid remotely.
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32)
    {
        cleanup();
        throw std::runtime_error("GetModuleHandle(kernel32) failed: " +
                                 std::to_string(GetLastError()));
    }

    auto pLoadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hKernel32, "LoadLibraryW"));

    if (!pLoadLibraryW)
    {
        cleanup();
        throw std::runtime_error("GetProcAddress(LoadLibraryW) failed: " +
                                 std::to_string(GetLastError()));
    }

    // ── 5. Spawn the remote thread ───────────────────────────────────────────
    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,       // default security
        0,             // default stack size
        pLoadLibraryW,
        remoteBuf,     // argument = pointer to DLL path in remote memory
        0,
        nullptr);

    if (!hThread)
    {
        cleanup();
        throw std::runtime_error("CreateRemoteThread failed: " +
                                 std::to_string(GetLastError()));
    }

    // ── 6. Wait for LoadLibraryW to return (5-second timeout) ───────────────
    constexpr DWORD kTimeoutMs = 5000;
    DWORD waitResult = WaitForSingleObject(hThread, kTimeoutMs);

    if (waitResult == WAIT_TIMEOUT)
    {
        // Don't terminate — leave the thread running but report the issue.
        CloseHandle(hThread);
        cleanup();
        throw std::runtime_error("Remote thread timed out after " +
                                 std::to_string(kTimeoutMs) + " ms");
    }

    if (waitResult != WAIT_OBJECT_0)
    {
        CloseHandle(hThread);
        cleanup();
        throw std::runtime_error("WaitForSingleObject failed: " +
                                 std::to_string(GetLastError()));
    }

    // Check the thread exit code — LoadLibraryW returns NULL on failure.
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    if (exitCode == 0)
    {
        cleanup();
        throw std::runtime_error("LoadLibraryW in remote process returned NULL "
                                 "(DLL not found or init failed)");
    }

    cleanup();

    // ── 7. Verify the module appears in a Toolhelp32 snapshot ───────────────
    // Give the loader a moment to fully register the module entry.
    Sleep(100);

    if (!IsModuleLoaded(pid, absPath))
        throw std::runtime_error("Injection appeared to succeed but the module "
                                 "was not found in the process snapshot");

    std::wcout << L"[+] Injected and verified: " << absPath
               << L" -> PID " << pid << L'\n';
}

// ─── entry point ─────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        std::wcerr << L"Usage: inject.exe <PID> <path\\to\\payload.dll>\n";
        return 1;
    }

    DWORD pid = 0;
    try
    {
        pid = static_cast<DWORD>(std::stoul(argv[1]));
    }
    catch (...)
    {
        std::wcerr << L"[-] Invalid PID: " << argv[1] << L'\n';
        return 1;
    }

    const std::wstring dllPath = argv[2];

    try
    {
        EnableDebugPrivilege();
        std::wcout << L"[+] SeDebugPrivilege enabled\n";

        InjectDll(pid, dllPath);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[-] " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
