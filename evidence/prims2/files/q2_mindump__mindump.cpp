// mindump.cpp
// Write a full minidump of a target process, report file size, list captured modules.
// Build (MSVC):  cl /std:c++17 /EHsc /DSMOKE_TEST mindump.cpp /link dbghelp.lib
// Build (no entry): cl /std:c++17 /EHsc mindump.cpp /link dbghelp.lib

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// WriteProcMiniDump
//   pid      – target process ID
//   dumpPath – UTF-8 path for the output .dmp file
// Returns 0 on success, non-zero on failure.
// ---------------------------------------------------------------------------
static int WriteProcMiniDump(DWORD pid, const char* dumpPath)
{
    // Open the target process with the rights MiniDumpWriteDump requires.
    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
        FALSE, pid);
    if (!hProc) {
        fprintf(stderr, "[error] OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }

    // Create / overwrite the dump file.
    HANDLE hFile = CreateFileA(
        dumpPath,
        GENERIC_WRITE,
        0, nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[error] CreateFile('%s') failed: %lu\n", dumpPath, GetLastError());
        CloseHandle(hProc);
        return 1;
    }

    // MiniDumpWithFullMemory captures the entire virtual address space,
    // thread info, handle data, and module information.
    MINIDUMP_TYPE dumpType = MiniDumpWithFullMemory;

    BOOL ok = MiniDumpWriteDump(
        hProc,
        pid,
        hFile,
        dumpType,
        nullptr,   // no exception info
        nullptr,   // no user stream
        nullptr);  // no callback

    DWORD writeErr = GetLastError(); // capture before any other call

    if (!ok) {
        fprintf(stderr, "[error] MiniDumpWriteDump failed: %lu\n", writeErr);
        CloseHandle(hFile);
        CloseHandle(hProc);
        return 1;
    }

    // -----------------------------------------------------------------------
    // Report file size
    // -----------------------------------------------------------------------
    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(hFile, &fileSize)) {
        // Print size in both bytes and MB for convenience.
        double mb = static_cast<double>(fileSize.QuadPart) / (1024.0 * 1024.0);
        printf("[dump] File : %s\n", dumpPath);
        printf("[dump] Size : %lld bytes (%.2f MB)\n",
               static_cast<long long>(fileSize.QuadPart), mb);
    } else {
        fprintf(stderr, "[warn] GetFileSizeEx failed: %lu\n", GetLastError());
    }

    CloseHandle(hFile);

    // -----------------------------------------------------------------------
    // List module names captured in the dump.
    // We enumerate the modules from the live process because PSAPI is
    // simpler than parsing the dump, and we already have hProc open.
    // -----------------------------------------------------------------------
    DWORD needed = 0;
    EnumProcessModules(hProc, nullptr, 0, &needed);

    if (needed == 0) {
        fprintf(stderr, "[warn] EnumProcessModules returned 0 bytes needed\n");
        CloseHandle(hProc);
        return 0; // dump succeeded; module list is best-effort
    }

    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!EnumProcessModules(hProc, mods.data(),
                            static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                            &needed))
    {
        fprintf(stderr, "[warn] EnumProcessModules (sized) failed: %lu\n", GetLastError());
        CloseHandle(hProc);
        return 0;
    }

    DWORD modCount = needed / static_cast<DWORD>(sizeof(HMODULE));
    printf("[dump] Modules captured: %lu\n", modCount);
    for (DWORD i = 0; i < modCount; ++i) {
        char modName[MAX_PATH]{};
        if (GetModuleFileNameExA(hProc, mods[i], modName, MAX_PATH)) {
            // Print just the base name for readability; full path in parentheses.
            const char* base = strrchr(modName, '\\');
            base = base ? base + 1 : modName;
            printf("  [%4lu] %-40s  (%s)\n", i, base, modName);
        } else {
            printf("  [%4lu] <unknown> (GetModuleFileNameEx failed: %lu)\n",
                   i, GetLastError());
        }
    }

    CloseHandle(hProc);
    return 0;
}

// ---------------------------------------------------------------------------
// Smoke-test entry point
// ---------------------------------------------------------------------------
#ifdef SMOKE_TEST
int main(int argc, char* argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: mindump.exe <pid> <output.dmp>\n");
        return 1;
    }

    char* end = nullptr;
    unsigned long pid = strtoul(argv[1], &end, 10);
    if (!pid || (end && *end != '\0')) {
        fprintf(stderr, "[error] Invalid PID '%s'\n", argv[1]);
        return 1;
    }

    return WriteProcMiniDump(static_cast<DWORD>(pid), argv[2]);
}
#endif // SMOKE_TEST
