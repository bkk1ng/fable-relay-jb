// shim.cpp — POSIX implementations behind validation/inc/windows.h.
//
// The three functions that decide whether reader.cpp works are real:
//   CreateToolhelp32Snapshot / Process32* / Module32*  →  /proc enumeration
//   OpenProcess                                        →  /proc/<pid>/mem fd
//   ReadProcessMemory                                  →  pread on that fd
// Everything GDI is a recorded no-op so radar.cpp links and its geometry can be asserted.
#include "windows.h"
#include "tlhelp32.h"

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <sstream>

std::vector<ShimDraw> g_shim_draws;
static DWORD g_last_error = 0;

// ── snapshot ────────────────────────────────────────────────────────────────
struct Snap {
    bool is_module = false;
    DWORD pid = 0;
    size_t cursor = 0;
    std::vector<std::pair<DWORD, std::string>> procs;
    std::vector<std::pair<uintptr_t, std::string>> mods;
};

static std::string read_file(const std::string& p) {
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

static std::string basename_of(std::string s) {
    auto pos = s.find_last_of('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

static void scan_procs(Snap* s) {
    DIR* d = opendir("/proc");
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const DWORD pid = static_cast<DWORD>(std::atoi(e->d_name));
        std::string comm = read_file(std::string("/proc/") + e->d_name + "/comm");
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
        s->procs.emplace_back(pid, comm);
    }
    closedir(d);
    std::sort(s->procs.begin(), s->procs.end());
}

static void scan_modules(Snap* s, DWORD pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        // start-end perms offset dev inode path
        std::istringstream is(line);
        std::string range, perms, off, dev, inode, path;
        is >> range >> perms >> off >> dev >> inode;
        std::getline(is, path);
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        if (path.empty() || path[0] != '/') continue;
        const auto dash = range.find('-');
        const uintptr_t start = std::stoull(range.substr(0, dash), nullptr, 16);
        const std::string name = basename_of(path);
        auto it = std::find_if(s->mods.begin(), s->mods.end(),
                               [&](const auto& m) { return m.second == name; });
        if (it == s->mods.end()) s->mods.emplace_back(start, name);  // first mapping = base
    }
    std::sort(s->mods.begin(), s->mods.end());
}

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
    auto* s = new Snap();
    if (flags & TH32CS_SNAPPROCESS) {
        scan_procs(s);
    } else if (flags & (TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32)) {
        s->is_module = true;
        s->pid = pid;
        scan_modules(s, pid);
    }
    if (s->procs.empty() && s->mods.empty() && !s->is_module) {
        delete s; g_last_error = 6; return INVALID_HANDLE_VALUE;
    }
    return s;
}

BOOL Process32First(HANDLE snap, PROCESSENTRY32* pe) {
    auto* s = static_cast<Snap*>(snap);
    if (!s || s->procs.empty()) return FALSE;
    s->cursor = 0;
    const auto& [pid, name] = s->procs[0];
    pe->th32ProcessID = pid;
    std::snprintf(pe->szExeFile, sizeof(pe->szExeFile), "%s", name.c_str());
    return TRUE;
}

BOOL Process32Next(HANDLE snap, PROCESSENTRY32* pe) {
    auto* s = static_cast<Snap*>(snap);
    if (!s || ++s->cursor >= s->procs.size()) return FALSE;
    const auto& [pid, name] = s->procs[s->cursor];
    pe->th32ProcessID = pid;
    std::snprintf(pe->szExeFile, sizeof(pe->szExeFile), "%s", name.c_str());
    return TRUE;
}

BOOL Module32First(HANDLE snap, MODULEENTRY32* me) {
    auto* s = static_cast<Snap*>(snap);
    if (!s || s->mods.empty()) return FALSE;
    s->cursor = 0;
    const auto& [base, name] = s->mods[0];
    me->modBaseAddr = reinterpret_cast<BYTE*>(base);
    std::snprintf(me->szModule, sizeof(me->szModule), "%s", name.c_str());
    return TRUE;
}

BOOL Module32Next(HANDLE snap, MODULEENTRY32* me) {
    auto* s = static_cast<Snap*>(snap);
    if (!s || ++s->cursor >= s->mods.size()) return FALSE;
    const auto& [base, name] = s->mods[s->cursor];
    me->modBaseAddr = reinterpret_cast<BYTE*>(base);
    std::snprintf(me->szModule, sizeof(me->szModule), "%s", name.c_str());
    return TRUE;
}

// ── process memory ──────────────────────────────────────────────────────────
struct ProcHandle { DWORD pid; int memfd; };

HANDLE OpenProcess(DWORD, BOOL, DWORD pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/mem";
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { g_last_error = 5; return nullptr; }
    return new ProcHandle{pid, fd};
}

BOOL CloseHandle(HANDLE h) {
    if (auto* s = static_cast<Snap*>(h)) { delete s; return TRUE; }
    if (auto* p = static_cast<ProcHandle*>(h)) { ::close(p->memfd); delete p; return TRUE; }
    return TRUE;
}

BOOL ReadProcessMemory(HANDLE proc, LPCVOID addr, LPVOID buf, SIZE_T len, SIZE_T* read) {
    auto* p = static_cast<ProcHandle*>(proc);
    if (!p) { g_last_error = 6; return FALSE; }
    ssize_t n = ::pread(p->memfd, buf, len, static_cast<off_t>(reinterpret_cast<uintptr_t>(addr)));
    if (n < 0) { g_last_error = 299; if (read) *read = 0; return FALSE; }
    if (read) *read = static_cast<SIZE_T>(n);
    return n == static_cast<ssize_t>(len);
}

BOOL GetExitCodeProcess(HANDLE proc, DWORD* code) {
    auto* p = static_cast<ProcHandle*>(proc);
    if (!p || !code) return FALSE;
    // a live process still has a readable /proc/<pid>/stat; a dead one does not
    const std::string st = "/proc/" + std::to_string(p->pid) + "/stat";
    if (access(st.c_str(), F_OK) != 0) { *code = 1; return TRUE; }
    *code = STILL_ACTIVE;
    return TRUE;
}

DWORD GetLastError() { return g_last_error; }
void  Sleep(DWORD ms) { usleep(ms * 1000); }
int   _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }

// ── GDI / user32 no-ops (recorded) ──────────────────────────────────────────
static void rec(const char* op, int a = 0, int b = 0, int c = 0, int d = 0, DWORD col = 0) {
    ShimDraw s{};
    std::snprintf(s.op, sizeof(s.op), "%s", op);
    s.a = a; s.b = b; s.c = c; s.d = d; s.color = col;
    g_shim_draws.push_back(s);
}

HDC  GetDC(HWND) { return (HDC)1; }
int  ReleaseDC(HWND, HDC) { return 1; }
HDC  CreateCompatibleDC(HDC) { return (HDC)2; }
HBITMAP CreateCompatibleBitmap(HDC, int, int) { return (HBITMAP)3; }
HPEN CreatePen(int style, int w, DWORD col) { rec("CreatePen", style, w, 0, 0, col); return (HPEN)4; }
HBRUSH CreateSolidBrush(DWORD col) { rec("CreateSolidBrush", 0, 0, 0, 0, col); return (HBRUSH)5; }
HGDIOBJ SelectObject(HDC, HGDIOBJ o) { return o; }
BOOL DeleteObject(HGDIOBJ) { return 1; }
BOOL DeleteDC(HDC) { return 1; }
BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) { rec("BitBlt"); return 1; }
BOOL Ellipse(HDC, int l, int t, int r, int b) { rec("Ellipse", l, t, r, b); return 1; }
BOOL MoveToEx(HDC, int x, int y, LPVOID) { rec("MoveTo", x, y); return 1; }
BOOL LineTo(HDC, int x, int y) { rec("LineTo", x, y); return 1; }
int  FillRect(HDC, const RECT* rc, HBRUSH) {
    rec("FillRect", rc ? rc->left : 0, rc ? rc->top : 0, rc ? rc->right : 0, rc ? rc->bottom : 0);
    return 1;
}
HGDIOBJ GetStockObject(int obj) { return (HGDIOBJ)(intptr_t)obj; }
BOOL GetClientRect(HWND, RECT* rc) {
    if (rc) { rc->left = 0; rc->top = 0; rc->right = 400; rc->bottom = 400; }
    return 1;
}
HDC  BeginPaint(HWND, PAINTSTRUCT* ps) { if (ps) ps->hdc = (HDC)1; return (HDC)1; }
BOOL EndPaint(HWND, const PAINTSTRUCT*) { return 1; }
BOOL InvalidateRect(HWND, const RECT*, BOOL) { return 1; }
int  MessageBoxW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fprintf(stderr, "[shim] MessageBoxW: %ls\n", text ? text : L"");
    return 1;
}
HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, void*, HINSTANCE, LPVOID) {
    rec("CreateWindow");
    return (HWND)7;
}
BOOL ShowWindow(HWND, int) { return 1; }
BOOL UpdateWindow(HWND) { return 1; }
BOOL DestroyWindow(HWND) { return 1; }
HWND GetForegroundWindow() { return nullptr; }
HWND FindWindowW(LPCWSTR, LPCWSTR) { return nullptr; }
BOOL PeekMessageW(MSG*, HWND, UINT, UINT, UINT) { return 0; }
BOOL TranslateMessage(const MSG*) { return 1; }
LRESULT DispatchMessageW(const MSG*) { return 0; }
void PostQuitMessage(int) {}
ATOM RegisterClassExW(const WNDCLASSEXW*) { return 1; }
BOOL UnregisterClassW(LPCWSTR, HINSTANCE) { return 1; }
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
HINSTANCE GetModuleHandleW(LPCWSTR) { return (HINSTANCE)8; }
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR) { return (HCURSOR)9; }
