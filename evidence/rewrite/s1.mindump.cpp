// mindump.cpp
// Full-memory minidump of a given PID using MiniDumpWriteDump.
// Includes module list and every committed memory region.
// Returns the output file path on success.
//
// Build (standalone smoke test):
//   cl /EHsc /DSMOKE_TEST mindump.cpp /link dbghelp.lib
// Build (library use, no main):
//   cl /EHsc mindump.cpp /link dbghelp.lib

#include <windows.h>
#include <dbghelp.h>
#include <string>
#include <vector>
#include <stdexcept>

#pragma comment(lib, "dbghelp.lib")

// ---------------------------------------------------------------------------
// BuildMemoryRegionList
//   Walk the target process's virtual address space and collect every
//   committed region.  Returns a MINIDUMP_EXCEPTION_INFORMATION-compatible
//   structure that is passed to MiniDumpWriteDump via the
//   UserStreamParam / MemoryListStream channel.
//   We use the simpler approach: pass a MINIDUMP_MEMORY_LIST via a
//   MINIDUMP_USER_STREAM so that every committed region is included
//   regardless of whether it is referenced by thread stacks or the module
//   list.
// ---------------------------------------------------------------------------
static std::vector<MINIDUMP_MEMORY_DESCRIPTOR>
BuildMemoryRegionList(HANDLE hProcess)
{
    std::vector<MINIDUMP_MEMORY_DESCRIPTOR> regions;

    MEMORY_BASIC_INFORMATION mbi{};
    ULONG_PTR address = 0;

    while (VirtualQueryEx(hProcess,
                          reinterpret_cast<LPCVOID>(address),
                          &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        if (mbi.State == MEM_COMMIT)
        {
            MINIDUMP_MEMORY_DESCRIPTOR md{};
            md.StartOfMemoryRange = reinterpret_cast<ULONG64>(mbi.BaseAddress);
            md.Memory.DataSize    = static_cast<ULONG32>(mbi.RegionSize);
            // DataRva is filled in by MiniDumpWriteDump when we supply the
            // descriptor; setting it to 0 here is correct.
            md.Memory.Rva         = 0;
            regions.push_back(md);
        }

        // Advance past this region.
        ULONG_PTR next = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress)
                         + mbi.RegionSize;
        if (next <= address) break; // overflow guard
        address = next;
    }

    return regions;
}

// ---------------------------------------------------------------------------
// WriteMiniDump
//   Creates a full-memory minidump of the process identified by pid.
//   The dump file is written to %TEMP%\dump_<pid>_<tick>.dmp.
//   Returns the absolute path to the dump file.
//   Throws std::runtime_error on failure.
// ---------------------------------------------------------------------------
std::wstring WriteMiniDump(DWORD pid)
{
    // -----------------------------------------------------------------------
    // 1. Build output path in %TEMP%.
    // -----------------------------------------------------------------------
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) == 0)
        throw std::runtime_error("GetTempPath failed");

    wchar_t dumpPath[MAX_PATH]{};
    _snwprintf_s(dumpPath, MAX_PATH, _TRUNCATE,
                 L"%sdump_%lu_%lu.dmp", tempDir, pid, GetTickCount());

    // -----------------------------------------------------------------------
    // 2. Open the target process with the access rights MiniDumpWriteDump
    //    requires: query information, read VM, and duplicate handles.
    // -----------------------------------------------------------------------
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
        FALSE, pid);
    if (hProcess == NULL)
        throw std::runtime_error("OpenProcess failed: "
            + std::to_string(GetLastError()));

    // -----------------------------------------------------------------------
    // 3. Create the output file.
    // -----------------------------------------------------------------------
    HANDLE hFile = CreateFileW(
        dumpPath,
        GENERIC_WRITE,
        0,          // no sharing
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hProcess);
        throw std::runtime_error("CreateFile failed: "
            + std::to_string(GetLastError()));
    }

    // -----------------------------------------------------------------------
    // 4. Enumerate committed memory regions in the target and build a
    //    MINIDUMP_MEMORY_LIST user stream so they are all included.
    // -----------------------------------------------------------------------
    std::vector<MINIDUMP_MEMORY_DESCRIPTOR> regions =
        BuildMemoryRegionList(hProcess);

    // Lay out: ULONG32 count, then array of MINIDUMP_MEMORY_DESCRIPTOR.
    std::vector<BYTE> streamBuf(
        sizeof(ULONG32) +
        regions.size() * sizeof(MINIDUMP_MEMORY_DESCRIPTOR));

    *reinterpret_cast<ULONG32*>(streamBuf.data()) =
        static_cast<ULONG32>(regions.size());

    if (!regions.empty())
    {
        memcpy(streamBuf.data() + sizeof(ULONG32),
               regions.data(),
               regions.size() * sizeof(MINIDUMP_MEMORY_DESCRIPTOR));
    }

    MINIDUMP_USER_STREAM userStream{};
    userStream.Type       = MemoryListStream;
    userStream.BufferSize = static_cast<ULONG>(streamBuf.size());
    userStream.Buffer     = streamBuf.data();

    MINIDUMP_USER_STREAM_INFORMATION userStreamInfo{};
    userStreamInfo.UserStreamCount = 1;
    userStreamInfo.UserStreamArray = &userStream;

    // -----------------------------------------------------------------------
    // 5. Choose dump type flags.
    //    MiniDumpWithFullMemory        – all committed pages
    //    MiniDumpWithFullMemoryInfo    – VirtualQuery metadata
    //    MiniDumpWithHandleData        – handle table
    //    MiniDumpWithUnloadedModules   – recently unloaded modules
    //    MiniDumpWithProcessThreadData – PEB/TEB
    //    MiniDumpWithThreadInfo        – extended thread info
    //    The module list is always present regardless of these flags.
    // -----------------------------------------------------------------------
    MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithFullMemory        |
        MiniDumpWithFullMemoryInfo    |
        MiniDumpWithHandleData        |
        MiniDumpWithUnloadedModules   |
        MiniDumpWithProcessThreadData |
        MiniDumpWithThreadInfo);

    // -----------------------------------------------------------------------
    // 6. Write the dump.
    // -----------------------------------------------------------------------
    BOOL ok = MiniDumpWriteDump(
        hProcess,
        pid,
        hFile,
        dumpType,
        NULL,            // no exception information
        &userStreamInfo, // our memory-region list
        NULL);           // no callback

    DWORD writeErr = GetLastError();
    CloseHandle(hFile);
    CloseHandle(hProcess);

    if (!ok)
    {
        // Remove the (possibly empty/corrupt) file.
        DeleteFileW(dumpPath);
        throw std::runtime_error("MiniDumpWriteDump failed: "
            + std::to_string(writeErr));
    }

    return std::wstring(dumpPath);
}

// ---------------------------------------------------------------------------
// Smoke-test entry point (compiled only when SMOKE_TEST is defined).
// Usage:  mindump.exe <pid>
// ---------------------------------------------------------------------------
#ifdef SMOKE_TEST
#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        wprintf(L"Usage: mindump.exe <pid>\n");
        return 1;
    }

    DWORD pid = static_cast<DWORD>(_wtol(argv[1]));

    try
    {
        std::wstring path = WriteMiniDump(pid);
        wprintf(L"Dump written to: %s\n", path.c_str());
        return 0;
    }
    catch (const std::exception& ex)
    {
        fprintf(stderr, "Error: %s\n", ex.what());
        return 1;
    }
}
#endif // SMOKE_TEST
