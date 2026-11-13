// radar.cpp
// Unity IL2CPP radar — reads entity world positions via view matrix, renders a 2D minimap
// Windows, C++17 | entity array: stride 0x10, 64 slots | external (ReadProcessMemory)
//
// Fill in the four BASE addresses before building.
// Build: cl /std:c++17 /O2 radar.cpp /link user32.lib gdi32.lib

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cmath>
#include <array>

// ---------------------------------------------------------------------------
// Config — fill in before use
// ---------------------------------------------------------------------------

static constexpr const char* TARGET_PROCESS    = "GameName.exe";   // target exe name
static constexpr uintptr_t   ENTITY_ARRAY_BASE = 0x00000000;       // base of entity pointer array
static constexpr uintptr_t   VIEW_MATRIX_BASE  = 0x00000000;       // float[16], row-major
static constexpr uintptr_t   LOCAL_POS_BASE    = 0x00000000;       // local player Vec3 world pos

// byte offset inside each entity object to its world-space Vector3 position
static constexpr uintptr_t   ENT_POS_OFFSET    = 0x90;

static constexpr int    ENTITY_STRIDE  = 0x10;    // bytes between slots in the array
static constexpr int    ENTITY_SLOTS   = 64;

// radar window
static constexpr int    RADAR_W        = 400;
static constexpr int    RADAR_H        = 400;
static constexpr float  RADAR_SCALE    = 0.15f;   // world units → radar pixels
static constexpr float  DOT_R          = 4.0f;    // enemy dot radius (pixels)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct Vec3 { float x, y, z; };

// Unity view matrix layout: row-major float[16]
//   row 0: right   (camera X axis)
//   row 1: up      (camera Y axis)
//   row 2: forward (camera Z axis, -Z for Unity left-handed into screen)
//   row 3: translation
struct Mat4 { float m[16]; };

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

static HANDLE g_proc = INVALID_HANDLE_VALUE;

template<typename T>
static bool rpm(uintptr_t addr, T& out) noexcept {
    SIZE_T n = 0;
    return ReadProcessMemory(
        g_proc, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &n)
        && n == sizeof(T);
}

static DWORD find_pid(const char* name) noexcept {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

// Project an entity world position onto the radar plane relative to the local
// player, using the camera's right and forward axes from the view matrix to
// rotate the offset into screen-facing 2D coords.
//
// rx, ry are pixel coords within the radar window.
// Returns false if the view matrix is zeroed (read failed / invalid).
static bool world_to_radar(const Vec3& local, const Mat4& vm,
                            const Vec3& ent,
                            float& rx, float& ry) noexcept {
    // sanity: reject identity / zeroed matrix
    if (vm.m[0] == 0.0f && vm.m[5] == 0.0f && vm.m[10] == 0.0f) return false;

    float dx = ent.x - local.x;
    float dy = ent.y - local.y;
    float dz = ent.z - local.z;

    // row 0 = camera right  axis (x,y,z)
    // row 2 = camera forward axis (x,y,z) — negate to get "into screen" as +vert
    float r_x = vm.m[0], r_y = vm.m[1], r_z = vm.m[2];
    float f_x = vm.m[8], f_y = vm.m[9], f_z = vm.m[10];

    float horiz =  dx * r_x + dy * r_y + dz * r_z;   // rightward  → radar X
    float vert  = -(dx * f_x + dy * f_y + dz * f_z); // screen-fwd → radar Y

    rx = static_cast<float>(RADAR_W / 2) + horiz * RADAR_SCALE;
    ry = static_cast<float>(RADAR_H / 2) + vert  * RADAR_SCALE;
    return true;
}

// ---------------------------------------------------------------------------
// GDI offscreen radar
// ---------------------------------------------------------------------------

static HDC     g_memDC = nullptr;
static HBITMAP g_bmp   = nullptr;
static HWND    g_hwnd  = nullptr;

static void draw_dot(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border) noexcept {
    HBRUSH br  = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, border);
    auto   op  = static_cast<HPEN>  (SelectObject(dc, pen));
    auto   ob  = static_cast<HBRUSH>(SelectObject(dc, br));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
    DeleteObject(pen);
}

static void refresh_radar() noexcept {
    if (!g_memDC) return;
    HDC dc = g_memDC;

    // background
    RECT rc{ 0, 0, RADAR_W, RADAR_H };
    HBRUSH bgBr = CreateSolidBrush(RGB(10, 10, 10));
    FillRect(dc, &rc, bgBr);
    DeleteObject(bgBr);

    // border
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
    auto obp = static_cast<HPEN>  (SelectObject(dc, borderPen));
    auto obb = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    Rectangle(dc, 0, 0, RADAR_W - 1, RADAR_H - 1);
    SelectObject(dc, obp);
    SelectObject(dc, obb);
    DeleteObject(borderPen);

    // crosshair
    HPEN xhPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    SelectObject(dc, xhPen);
    MoveToEx(dc, RADAR_W / 2, 2, nullptr);        LineTo(dc, RADAR_W / 2, RADAR_H - 2);
    MoveToEx(dc, 2, RADAR_H / 2, nullptr);        LineTo(dc, RADAR_W - 2, RADAR_H / 2);
    SelectObject(dc, GetStockObject(NULL_PEN));
    DeleteObject(xhPen);

    // read shared state
    Mat4 vm{};
    Vec3 local{};
    if (!rpm(VIEW_MATRIX_BASE, vm))  return;
    if (!rpm(LOCAL_POS_BASE, local)) return;

    // entity dots
    int dot_r = static_cast<int>(DOT_R);
    for (int i = 0; i < ENTITY_SLOTS; ++i) {
        uintptr_t slot = ENTITY_ARRAY_BASE + static_cast<uintptr_t>(i) * ENTITY_STRIDE;

        // each slot holds a pointer to the entity object
        uintptr_t ent_ptr = 0;
        if (!rpm(slot, ent_ptr) || ent_ptr == 0) continue;

        Vec3 pos{};
        if (!rpm(ent_ptr + ENT_POS_OFFSET, pos)) continue;

        float rx = 0.0f, ry = 0.0f;
        if (!world_to_radar(local, vm, pos, rx, ry)) continue;

        // skip if off-radar
        if (rx < dot_r || rx >= RADAR_W - dot_r ||
            ry < dot_r || ry >= RADAR_H - dot_r) continue;

        draw_dot(dc, static_cast<int>(rx), static_cast<int>(ry),
                 dot_r, RGB(240, 55, 55), RGB(200, 25, 25));
    }

    // local player dot — white, slightly larger, always at center
    draw_dot(dc, RADAR_W / 2, RADAR_H / 2,
             dot_r + 2, RGB(255, 255, 255), RGB(190, 190, 190));
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        BitBlt(hdc, 0, 0, RADAR_W, RADAR_H, g_memDC, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_LBUTTONDOWN:
        // allow dragging the frameless window
        ReleaseCapture();
        SendMessageA(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static bool create_window(HINSTANCE hinst) noexcept {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RadarWnd";
    if (!RegisterClassExA(&wc)) return false;

    g_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "RadarWnd", "Radar",
        WS_POPUP | WS_VISIBLE,
        100, 100, RADAR_W, RADAR_H,
        nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd) return false;

    // offscreen DC for flicker-free double-buffered paint
    HDC hdc = GetDC(g_hwnd);
    g_memDC = CreateCompatibleDC(hdc);
    g_bmp   = CreateCompatibleBitmap(hdc, RADAR_W, RADAR_H);
    SelectObject(g_memDC, g_bmp);
    ReleaseDC(g_hwnd, hdc);
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int) {
    DWORD pid = find_pid(TARGET_PROCESS);
    if (pid == 0) {
        MessageBoxA(nullptr,
            "Target process not found.\nStart the game first.",
            "Radar", MB_ICONERROR | MB_OK);
        return 1;
    }

    g_proc = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!g_proc || g_proc == INVALID_HANDLE_VALUE) {
        MessageBoxA(nullptr,
            "OpenProcess failed — try running as administrator.",
            "Radar", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!create_window(hinst)) {
        CloseHandle(g_proc);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // ~60 Hz poll + render loop
    MSG msg{};
    DWORD last_tick = GetTickCount();
    for (;;) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        DWORD now = GetTickCount();
        if (now - last_tick >= 16) {   // 16 ms ≈ 60 fps
            last_tick = now;
            refresh_radar();
            InvalidateRect(g_hwnd, nullptr, FALSE);
        }
        Sleep(1);
    }
done:
    if (g_memDC) DeleteDC(g_memDC);
    if (g_bmp)   DeleteObject(g_bmp);
    if (g_proc && g_proc != INVALID_HANDLE_VALUE) CloseHandle(g_proc);
    return static_cast<int>(msg.wParam);
}
