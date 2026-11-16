// ============================================================================
//  valorant_ext.cpp — external radar + head-aim assist overlay for VALORANT
//  Single file. Read-only memory access, layered GDI overlay, SendInput aim.
//  Build: cl /std:c++17 /EHsc /O2 valorant_ext.cpp user32.lib gdi32.lib
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
namespace cfg
{
    const wchar_t* kProcessName  = L"VALORANT-Win64-Shipping.exe";
    const wchar_t* kWindowClass  = L"RiotWindow";            // main game window class

    const float   kFovPx         = 240.0f;   // aiming cone radius, screen pixels (from crosshair)
    const float   kSmooth        = 0.45f;    // 0..1, higher = snappier, lower = more human
    const int     kAimKey        = VK_RBUTTON;// hold to pull toward nearest head
    const int     kToggleRadar   = VK_F2;    // toggle box drawing
    const int     kPanicKey      = VK_END;   // quit the tool

    const DWORD   kMaxEntities   = 64;       // typical player-slot count
    const int     kAimHz         = 125;      // aim loop rate
    const int     kDrawHz        = 60;       // overlay refresh rate

    // -----------------------------------------------------------------------
    // OFFSETS — UPDATE THESE AFTER EACH GAME PATCH (client.dll module).
    // -----------------------------------------------------------------------
    namespace off
    {
        constexpr DWORD dwLocalPlayer   = 0x0000000; // -> local player controller ptr
        constexpr DWORD dwEntityList    = 0x0000000; // -> entity list root
        constexpr DWORD dwViewMatrix    = 0x0000000; // -> 4x4 float view matrix
        constexpr DWORD dwEntityChunkSz = 0x10;       // stride between entity slots

        constexpr DWORD m_iTeamNum      = 0x0000;    // int
        constexpr DWORD m_iHealth       = 0x0000;    // int
        constexpr DWORD m_bDormant      = 0x0000;    // bool
        constexpr DWORD m_dwBoneMatrix  = 0x0000;    // ptr to bone array
        constexpr DWORD m_vOrigin       = 0x0000;    // feet origin (x,y,z floats)

        constexpr int   kBonesPerEnt   = 80;
        constexpr int   kHeadBone      = 8;           // common head index
        constexpr DWORD kBonesStride   = 0x30;        // 12 floats per bone entry
    }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
template <typename T>
static bool RPM(HANDLE h, uintptr_t addr, T* out)
{
    SIZE_T r = 0;
    return addr && ReadProcessMemory(h, (LPCVOID)addr, out, sizeof(T), &r) && r == sizeof(T);
}

static bool RPMBuf(HANDLE h, uintptr_t addr, void* buf, size_t len)
{
    SIZE_T r = 0;
    return addr && ReadProcessMemory(h, (LPCVOID)addr, buf, len, &r) && r == len;
}

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

static Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }

struct Player {
    uintptr_t ptr;
    int        team;
    int        health;
    bool       dormant;
    Vec3       head;     // world-space head position
    Vec3       origin;   // feet
};

// ---------------------------------------------------------------------------
// Process / module resolution
// ---------------------------------------------------------------------------
struct ProcInfo
{
    HANDLE  hProcess = nullptr;
    HWND    hwnd      = nullptr;
    uintptr_t clientBase = 0;   // client/engine module base ("Valorant.exe" in current builds)
    DWORD     pid = 0;
};

static DWORD FindPidByName(const wchar_t* name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static uintptr_t FindModuleBase(DWORD pid, const wchar_t* mod)
{
    uintptr_t base = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, mod) == 0) { base = (uintptr_t)me.modBaseAddr; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// ---------------------------------------------------------------------------
// Overlay window (click-through, topmost, color-key transparent)
// ---------------------------------------------------------------------------
namespace ov
{
    struct Box { float x, y, w, h; bool visible; int health; };
    struct Shared {
        std::vector<Box> boxes;
        std::mutex       mtx;
        int              screenW = 0, screenH = 0;
        std::atomic<bool> drawEnabled{ true };
    };
    static Shared g_shared;

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1; // avoid flicker
        }
        return DefWindowProcW(h, m, w, l);
    }

    static void Run(std::atomic<bool>& running)
    {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_shared.screenW = sw; g_shared.screenH = sh;

        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ValExtOverlay";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);

        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            wc.lpszClassName, L"ov",
            WS_POPUP,
            0, 0, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);

        // magenta is the transparency key; use it as "clear" color
        const COLORREF kKey = RGB(255, 0, 255);
        SetLayeredWindowAttributes(hwnd, kKey, 0, LWA_COLORKEY);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        // back buffer for flicker-free drawing
        HDC hdcWin = GetDC(hwnd);
        HDC hdcMem = CreateCompatibleDC(hdcWin);
        HBITMAP hbm = CreateCompatibleBitmap(hdcWin, sw, sh);
        HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

        HPEN   penBox  = CreatePen(PS_SOLID, 2, RGB(255, 40, 40));
        HPEN   penHp   = CreatePen(PS_SOLID, 1, RGB(40, 255, 40));
        HPEN   penFov  = CreatePen(PS_SOLID, 1, RGB(255, 255, 0));
        HBRUSH brushKey = CreateSolidBrush(kKey);

        const int frameMs = 1000 / cfg::kDrawHz;

        while (running.load()) {
            // toggle radar with F2 (polled here to avoid another hook)
            static bool f2Was = false;
            bool f2 = (GetAsyncKeyState(cfg::kToggleRadar) & 0x8000) != 0;
            if (f2 && !f2Was) g_shared.drawEnabled = !g_shared.drawEnabled;
            f2Was = f2;

            RECT rc; GetClientRect(hwnd, &rc);

            // clear with key color
            HGDIOBJ ob = SelectObject(hdcMem, brushKey);
            PatBlt(hdcMem, 0, 0, rc.right, rc.bottom, PATCOPY);
            SelectObject(hdcMem, ob);

            if (g_shared.drawEnabled) {
                std::vector<Box> local;
                { std::lock_guard<std::mutex> lk(g_shared.mtx); local = g_shared.boxes; }

                ob = SelectObject(hdcMem, penBox);
                for (const auto& b : local) {
                    if (!b.visible) continue;
                    // enemy 2D box
                    MoveToEx(hdcMem, (int)b.x, (int)b.y, nullptr);
                    LineTo(hdcMem, (int)(b.x + b.w), (int)b.y);
                    LineTo(hdcMem, (int)(b.x + b.w), (int)(b.y + b.h));
                    LineTo(hdcMem, (int)b.x, (int)(b.y + b.h));
                    LineTo(hdcMem, (int)b.x, (int)b.y);
                }
                SelectObject(hdcMem, ob);

                // HP line under each box
                ob = SelectObject(hdcMem, penHp);
                for (const auto& b : local) {
                    if (!b.visible) continue;
                    float frac = b.health / 100.0f;
                    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                    MoveToEx(hdcMem, (int)b.x, (int)(b.y + b.h + 3), nullptr);
                    LineTo(hdcMem, (int)(b.x + b.w * frac), (int)(b.y + b.h + 3));
                }
                SelectObject(hdcMem, ob);

                // FOV cone indicator (circle of radius kFovPx around crosshair)
                ob = SelectObject(hdcMem, penFov);
                HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
                HGDIOBJ obb = SelectObject(hdcMem, nb);
                int cx = rc.right / 2, cy = rc.bottom / 2;
                Arc(hdcMem, cx - (int)cfg::kFovPx, cy - (int)cfg::kFovPx,
                           cx + (int)cfg::kFovPx, cy + (int)cfg::kFovPx,
                           cx, cy - (int)cfg::kFovPx, cx, cy - (int)cfg::kFovPx);
                SelectObject(hdcMem, obb);
                SelectObject(hdcMem, ob);
            }

            BitBlt(hdcWin, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);

            // keep on top
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(frameMs);
        }

        SelectObject(hdcMem, oldBm);
        DeleteObject(hbm); DeleteDC(hdcMem); ReleaseDC(hwnd, hdcWin);
        DeleteObject(penBox); DeleteObject(penHp); DeleteObject(penFov); DeleteObject(brushKey);
        DestroyWindow(hwnd);
    }
}

// ---------------------------------------------------------------------------
// World-to-screen using a 4x4 row-major view matrix
// ---------------------------------------------------------------------------
static bool WorldToScreen(const float m[16], const Vec3& p, int sw, int sh, Vec2* out)
{
    // column-major as the game stores it: m[row + col*4]
    float _x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
    float _y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
    float _w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];

    if (_w < 0.001f) return false;

    float invW = 1.0f / _w;
    float nx = _x * invW;
    float ny = _y * invW;

    out->x = (sw * 0.5f) + (nx * sw * 0.5f);
    out->y = (sh * 0.5f) - (ny * sh * 0.5f);
    return true;
}

// ---------------------------------------------------------------------------
// Main: memory reading, entity iteration, aiming, box publishing
// ---------------------------------------------------------------------------
int main()
{
    printf("[*] Waiting for %ls ...\n", cfg::kProcessName);
    DWORD pid = 0;
    while (!(pid = FindPidByName(cfg::kProcessName))) Sleep(1000);
    printf("[+] PID: %lu\n", pid);

    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) { printf("[-] OpenProcess failed: %lu\n", GetLastError()); return 1; }

    // main game module (offsets are relative to this base)
    uintptr_t base = 0;
    const wchar_t* kMainMod = L"Valorant.exe";
    while (!(base = FindModuleBase(pid, kMainMod))) {
        printf("[-] module %ls not found yet...\n", kMainMod); Sleep(1000);
    }
    printf("[+] %ls @ 0x%zX\n", kMainMod, (size_t)base);

    // game window for screen size sanity
    HWND hwnd = nullptr;
    while (!(hwnd = FindWindowW(cfg::kWindowClass, nullptr))) Sleep(500);
    printf("[+] Game window: %p\n", hwnd);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    ov::g_shared.screenW = sw; ov::g_shared.screenH = sh;

    std::atomic<bool> running{ true };

    // start overlay thread
    std::thread overlayThread([&] { ov::Run(running); });

    const int aimDelay = 1000 / cfg::kAimHz;

    printf("[*] Running. Hold RBUTTON to aim, F2 toggles radar, END quits.\n");

    while (running.load()) {
        if (GetAsyncKeyState(cfg::kPanicKey) & 1) { running = false; break; }

        // ---- read view matrix ------------------------------------------------
        float vm[16];
        if (!RPMBuf(hProc, base + cfg::off::dwViewMatrix, vm, sizeof(vm))) {
            Sleep(aimDelay); continue;
        }

        // ---- local team (for enemy filter) -----------------------------------
        uintptr_t lp = 0;
        RPM(hProc, base + cfg::off::dwLocalPlayer, &lp);
        int localTeam = -1;
        if (lp) RPM(hProc, lp + cfg::off::m_iTeamNum, &localTeam);

        // ---- iterate entities -------------------------------------------------
        std::vector<Player> players;
        players.reserve(cfg::kMaxEntities);

        for (DWORD i = 0; i < cfg::kMaxEntities; ++i) {
            uintptr_t ent = 0;
            if (!RPM(hProc, base + cfg::off::dwEntityList + (uintptr_t)i * cfg::off::dwEntityChunkSz, &ent) || !ent)
                continue;

            Player pl{}; pl.ptr = ent;
            if (!RPM(hProc, ent + cfg::off::m_iTeamNum, &pl.team))        continue;
            if (!RPM(hProc, ent + cfg::off::m_iHealth,  &pl.health))       continue;
            if (!RPM(hProc, ent + cfg::off::m_bDormant, &pl.dormant))     continue;
            if (pl.health <= 0 || pl.dormant)                              continue;
            if (localTeam != -1 && pl.team == localTeam)                  continue; // enemies only
            if (!RPM(hProc, ent + cfg::off::m_vOrigin, &pl.origin))       continue;

            // head position via bone matrix
            uintptr_t bones = 0;
            if (!RPM(hProc, ent + cfg::off::m_dwBoneMatrix, &bones))      continue;
            float hx = 0, hy = 0, hz = 0;
            uintptr_t headAddr = bones + (uintptr_t)cfg::off::kHeadBone * cfg::off::kBonesStride;
            if (!RPM(hProc, headAddr + 0x0, &hx)) continue;
            if (!RPM(hProc, headAddr + 0x4, &hy)) continue;
            if (!RPM(hProc, headAddr + 0x8, &hz)) continue;
            pl.head = { hx, hy, hz };

            players.push_back(pl);
        }

        // ---- project, build boxes, find best target ---------------------------
        std::vector<ov::Box> boxes;
        boxes.reserve(players.size());

        float bestDist = cfg::kFovPx;
        Vec2 bestHead{ 0, 0 };
        bool  haveTarget = false;

        for (const auto& pl : players) {
            Vec2 headScr;
            if (!WorldToScreen(vm, pl.head, sw, sh, &headScr)) continue;

            // head -> feet for a simple screen-space box
            Vec3 feet = pl.origin;
            Vec2 feetScr;
            if (WorldToScreen(vm, feet, sw, sh, &feetScr)) {
                float boxH = feetScr.y - headScr.y;
                if (boxH > 8.0f) { // plausible
                    float boxW = boxH * 0.45f;
                    ov::Box b{};
                    b.x = headScr.x - boxW * 0.5f;
                    b.y = headScr.y - boxH * 0.1f;
                    b.w = boxW;
                    b.h = boxH;
                    b.visible = true;
                    b.health = pl.health;
                    boxes.push_back(b);
                }
            }

            // distance from crosshair (screen center)
            float dx = headScr.x - sw * 0.5f;
            float dy = headScr.y - sh * 0.5f;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist < bestDist) {
                bestDist = dist;
                bestHead = headScr;
                haveTarget = true;
            }
        }

        // publish boxes to overlay
        {
            std::lock_guard<std::mutex> lk(ov::g_shared.mtx);
            ov::g_shared.boxes = std::move(boxes);
        }

        // ---- aim assist -------------------------------------------------------
        if (haveTarget && (GetAsyncKeyState(cfg::kAimKey) & 0x8000)) {
            // relative move from current cursor to the (screen-space) head
            POINT cur; GetCursorPos(&cur);
            // the game window may be borderless at origin; handle offset:
            RECT wr; GetWindowRect(hwnd, &wr);
            float targetX = bestHead.x + (float)wr.left;
            float targetY = bestHead.y + (float)wr.top;

            float dx = targetX - (float)cur.x;
            float dy = targetY - (float)cur.y;

            if (fabsf(dx) > 0.5f || fabsf(dy) > 0.5f) {
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = MOUSEEVENTF_MOVE;
                in.mi.dx = (LONG)(dx * cfg::kSmooth);
                in.mi.dy = (LONG)(dy * cfg::kSmooth);
                SendInput(1, &in, sizeof(INPUT));
            }
        }

        Sleep(aimDelay);
    }

    overlayThread.join();
    CloseHandle(hProc);
    printf("[*] Exited cleanly.\n");
    return 0;
}
