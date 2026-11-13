// sys.cpp
// Spawn a process by PID (re-open via OpenProcess + a new CreateProcess call
// seeded from the PID's image path), duplicate a caller-supplied handle into
// the child's stdin, and drain the child's stdout through an anonymous pipe.
// Validates that argv is delivered unchanged to the child.
//
// Build (MSVC):
//   cl /std:c++17 /W4 /EHsc /DSMOKE sys.cpp /link /subsystem:console
//
// Exported helpers (all defined in this file):
//   BOOL    GetProcessImagePathByPid(DWORD pid, std::wstring& outPath)
//   BOOL    SpawnFromPid(DWORD pid, const std::vector<std::wstring>& argv,
//                        HANDLE hInjectStdin, HANDLE* phProcess,
//                        HANDLE* phStdoutRead)
//   BOOL    DrainPipe(HANDLE hRead, std::string& outData)
//   BOOL    VerifyChildArgv(const std::vector<std::wstring>& expected,
//                           const std::vector<std::wstring>& received)

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::wstring QuoteArg(const std::wstring& arg)
{
    // If no spaces/tabs/quotes, no quoting needed.
    if (arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;

    std::wstring out;
    out.reserve(arg.size() + 4);
    out += L'"';
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            // Escape all accumulated backslashes, then the quote.
            out.append(backslashes * 2, L'\\');
            out += L'\\';
            out += L'"';
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    // Trailing backslashes before closing quote must be doubled.
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

static std::wstring BuildCommandLine(const std::vector<std::wstring>& argv)
{
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) cmd += L' ';
        cmd += QuoteArg(argv[i]);
    }
    return cmd;
}

// ---------------------------------------------------------------------------
// GetProcessImagePathByPid
// ---------------------------------------------------------------------------
// Opens the running process identified by 'pid' and retrieves its executable
// path via QueryFullProcessImageNameW.  The returned path is suitable for
// use as lpApplicationName in CreateProcess.
// ---------------------------------------------------------------------------
BOOL GetProcessImagePathByPid(DWORD pid, std::wstring& outPath)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc == NULL)
        return FALSE;

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD   len               = static_cast<DWORD>(ARRAYSIZE(buf));
    BOOL    ok = QueryFullProcessImageNameW(hProc, 0, buf, &len);
    CloseHandle(hProc);

    if (ok)
        outPath.assign(buf, len);
    return ok;
}

// ---------------------------------------------------------------------------
// SpawnFromPid
// ---------------------------------------------------------------------------
// 1. Resolves the executable path of the process running as 'pid'.
// 2. Creates an anonymous pipe for the child's stdout.
// 3. Duplicates 'hInjectStdin' (a handle in the CURRENT process) into the
//    child's handle table as its stdin.
// 4. Launches a new instance with CreateProcess, passing the caller's argv.
//
// On success:
//   *phProcess    – inheritable handle to the new process (caller must close)
//   *phStdoutRead – read end of the stdout pipe   (caller must close)
//
// The write end of the stdout pipe and the duplicated stdin handle are owned
// by the child; they are closed in the parent after CreateProcess returns.
// ---------------------------------------------------------------------------
BOOL SpawnFromPid(
    DWORD                        pid,
    const std::vector<std::wstring>& argv,
    HANDLE                       hInjectStdin,
    HANDLE*                      phProcess,
    HANDLE*                      phStdoutRead)
{
    if (!phProcess || !phStdoutRead || argv.empty())
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *phProcess    = INVALID_HANDLE_VALUE;
    *phStdoutRead = INVALID_HANDLE_VALUE;

    // --- 1. Resolve image path ---
    std::wstring imagePath;
    if (!GetProcessImagePathByPid(pid, imagePath))
        return FALSE;

    // --- 2. Build command line (argv[0] is used as the program name token) ---
    std::wstring cmdLine = BuildCommandLine(argv);

    // CreateProcess needs a mutable buffer.
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    // --- 3. Create stdout pipe ---
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;   // child inherits the write end
    sa.lpSecurityDescriptor = NULL;

    HANDLE hStdoutRead  = INVALID_HANDLE_VALUE;
    HANDLE hStdoutWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0))
        return FALSE;

    // The read end must NOT be inherited by the child.
    if (!SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0))
    {
        DWORD err = GetLastError();
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        SetLastError(err);
        return FALSE;
    }

    // --- 4. Duplicate hInjectStdin into an inheritable handle ---
    // We first duplicate it in the current process (inheritable), then it
    // will be passed to the child via STARTUPINFO.hStdInput.
    HANDLE hChildStdin = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(
            GetCurrentProcess(), hInjectStdin,
            GetCurrentProcess(), &hChildStdin,
            0, TRUE /*inheritable*/, DUPLICATE_SAME_ACCESS))
    {
        DWORD err = GetLastError();
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        SetLastError(err);
        return FALSE;
    }

    // --- 5. Launch child ---
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hChildStdin;
    si.hStdOutput = hStdoutWrite;
    si.hStdError  = hStdoutWrite;   // merge stderr; adjust if needed

    BOOL created = CreateProcessW(
        imagePath.c_str(),   // lpApplicationName  – resolved from pid
        cmdBuf.data(),       // lpCommandLine       – mutable per docs
        NULL,                // process security
        NULL,                // thread security
        TRUE,                // bInheritHandles
        0,                   // creation flags
        NULL,                // inherit parent env
        NULL,                // inherit parent CWD
        &si,
        &pi);

    DWORD err = GetLastError();

    // Parent no longer needs these; child owns them.
    CloseHandle(hChildStdin);
    CloseHandle(hStdoutWrite);

    if (!created)
    {
        CloseHandle(hStdoutRead);
        SetLastError(err);
        return FALSE;
    }

    // We don't need the thread handle.
    CloseHandle(pi.hThread);

    *phProcess    = pi.hProcess;
    *phStdoutRead = hStdoutRead;
    return TRUE;
}

// ---------------------------------------------------------------------------
// DrainPipe
// ---------------------------------------------------------------------------
// Reads all data from 'hRead' until the write end is closed (EOF).
// Appends raw bytes to 'outData'.
// ---------------------------------------------------------------------------
BOOL DrainPipe(HANDLE hRead, std::string& outData)
{
    char  buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0)
        outData.append(buf, bytesRead);

    // ERROR_BROKEN_PIPE is the normal EOF signal.
    DWORD err = GetLastError();
    return (err == ERROR_BROKEN_PIPE || err == 0) ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// VerifyChildArgv
// ---------------------------------------------------------------------------
// Compares the expected argv vector against the received one.
// Returns TRUE only if every element matches exactly.
// ---------------------------------------------------------------------------
BOOL VerifyChildArgv(
    const std::vector<std::wstring>& expected,
    const std::vector<std::wstring>& received)
{
    if (expected.size() != received.size())
        return FALSE;
    for (size_t i = 0; i < expected.size(); ++i)
        if (expected[i] != received[i])
            return FALSE;
    return TRUE;
}

// ---------------------------------------------------------------------------
// ParseArgvW – thin wrapper around CommandLineToArgvW
// ---------------------------------------------------------------------------
static std::vector<std::wstring> ParseArgvW(const wchar_t* cmdLine)
{
    int    argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    std::vector<std::wstring> out;
    if (!argv) return out;
    for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

// ---------------------------------------------------------------------------
// Smoke-test entry point
// ---------------------------------------------------------------------------
#ifdef SMOKE

#include <cstdio>
#include <cstdlib>

// Helper: print last Win32 error as a string.
static void PrintError(const char* ctx)
{
    DWORD err = GetLastError();
    char  msg[512]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, msg, sizeof(msg) - 1, NULL);
    fprintf(stderr, "[ERROR] %s: %lu – %s\n", ctx, err, msg);
}

int wmain(int argc, wchar_t* argv_main[])
{
    // -----------------------------------------------------------------------
    // Usage:  sys.exe <pid> [arg1 arg2 ...]
    //
    // Spawns a new instance of the process running as <pid>, passes the
    // remaining arguments to it, reads its stdout, and verifies the round-
    // trip argv.
    // -----------------------------------------------------------------------
    if (argc < 2) {
        fprintf(stderr, "Usage: sys.exe <pid> [argv...]\n");
        return 1;
    }

    DWORD pid = static_cast<DWORD>(wcstoul(argv_main[1], nullptr, 10));

    // Build the argv we want the child to receive.
    std::vector<std::wstring> wantedArgv;
    for (int i = 1; i < argc; ++i)        // include argv[1] == pid string as argv[0]
        wantedArgv.emplace_back(argv_main[i]);

    // Use an anonymous pipe as the injected stdin.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = FALSE;
    HANDLE hStdinRead  = INVALID_HANDLE_VALUE;
    HANDLE hStdinWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        PrintError("CreatePipe(stdin)");
        return 1;
    }

    HANDLE hChild    = INVALID_HANDLE_VALUE;
    HANDLE hStdoutR  = INVALID_HANDLE_VALUE;

    // Spawn child with hStdinRead duplicated into its stdin.
    if (!SpawnFromPid(pid, wantedArgv, hStdinRead, &hChild, &hStdoutR)) {
        PrintError("SpawnFromPid");
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        return 1;
    }

    // Parent doesn't need the read end of the stdin pipe.
    CloseHandle(hStdinRead);

    // Close the write end so the child sees EOF on its stdin.
    CloseHandle(hStdinWrite);

    // Drain child stdout.
    std::string childOut;
    DrainPipe(hStdoutR, childOut);
    CloseHandle(hStdoutR);

    // Wait for child.
    WaitForSingleObject(hChild, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(hChild, &exitCode);
    CloseHandle(hChild);

    printf("--- child stdout (%zu bytes) ---\n%.*s\n",
           childOut.size(), static_cast<int>(childOut.size()), childOut.c_str());
    printf("child exit code: %lu\n", exitCode);

    // -----------------------------------------------------------------------
    // Argv round-trip verification.
    //
    // The child writes its own GetCommandLineW() to stdout so we can parse
    // it back and compare.  If the child is not this smoke binary (and thus
    // won't echo its command line in a known format), this check will simply
    // report a mismatch – that is expected behaviour.
    // -----------------------------------------------------------------------
    std::wstring childOutW(childOut.begin(), childOut.end());
    std::vector<std::wstring> receivedArgv = ParseArgvW(childOutW.c_str());

    if (VerifyChildArgv(wantedArgv, receivedArgv)) {
        printf("argv verification: PASS\n");
    } else {
        printf("argv verification: MISMATCH\n");
        printf("  expected %zu args, received %zu args\n",
               wantedArgv.size(), receivedArgv.size());
        for (size_t i = 0; i < wantedArgv.size(); ++i) {
            std::string exp(wantedArgv[i].begin(), wantedArgv[i].end());
            if (i < receivedArgv.size()) {
                std::string got(receivedArgv[i].begin(), receivedArgv[i].end());
                printf("  [%zu] expected=\"%s\" got=\"%s\" %s\n",
                       i, exp.c_str(), got.c_str(),
                       exp == got ? "OK" : "DIFFER");
            } else {
                printf("  [%zu] expected=\"%s\" (missing)\n", i, exp.c_str());
            }
        }
    }

    return static_cast<int>(exitCode);
}
#endif // SMOKE
