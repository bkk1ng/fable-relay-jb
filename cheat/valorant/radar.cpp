// radar.cpp
// Plots Valorant entity world positions onto a 2D minimap using the view matrix
// and an entity array (stride 0x10, 64 slots).
//
// Build (standalone smoke test):
//   cl /std:c++17 /DRADAR_STANDALONE /W4 /O2 radar.cpp /link user32.lib gdi32.lib
//
// Exports:
//   RadarInit      – create the radar window and GDI resources
//   RadarUpdate    – read entity array + view matrix, redraw minimap
//   RadarShutdown  – destroy window and free resources

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <array>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int   RADAR_WND_SIZE   = 400;        // square window in pixels
static constexpr int   ENTITY_SLOTS     = 64;
static constexpr DWORD ENTITY_STRIDE    = 0x10;        // bytes between entity records

// World-space bounds for normalisation – tune to the map you're reading.
// These are symmetric; the radar renders [-WORLD_HALF, +WORLD_HALF] in each axis.
static constexpr float WORLD_HALF       = 15000.0f;

// ---------------------------------------------------------------------------
// Data layout
// Each entity slot starts at base + slot * ENTITY_STRIDE and contains:
//   +0x00  float x  (world)
//   +0x04  float y  (world)
//   +0x08  float z  (world)
//   +0x0C  uint8_t flags  (bit 0 = alive, bit 1 = enemy)
// ---------------------------------------------------------------------------

struct EntitySlot {
    float   x;
    float   y;
    float   z;
    uint8_t flags;
    uint8_t _pad[3];
};
static_assert(sizeof(EntitySlot) == ENTITY_STRIDE, "EntitySlot must equal ENTITY_STRIDE");

// View matrix – column-major 4×4, same layout as D3D/OpenGL (row-major storage).
struct ViewMatrix {
    float m[4][4]; // m[row][col]
};

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

namespace {

struct RadarState {
    HWND    hwnd        = nullptr;
    HDC     hdcMem      = nullptr;      // back-buffer DC
    HBITMAP hbmMem      = nullptr;      // back-buffer bitmap
    HBRUSH  hbrBg       = nullptr;
    HBRUSH  hbrAlly     = nullptr;
    HBRUSH  hbrEnemy    = nullptr;
    HPEN    hpenNull    = nullptr;
    int     wndSize     = RADAR_WND_SIZE;
    bool    running     = false;
};

static RadarState g_state;

// Forward declarations
LRESULT CALLBACK RadarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

} // namespace

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Project world XY → radar pixel (Z is ignored for top-down minimap).
// Returns false when the entity is not visible at all.
static bool WorldToRadar(float wx, float wy,
                         int wndSize, int& outPx, int& outPy)
{
    // Normalise to [0,1] then scale to window.
    float nx = (wx + WORLD_HALF) / (2.0f * WORLD_HALF);
    float ny = 1.0f - (wy + WORLD_HALF) / (2.0f * WORLD_HALF); // Y flipped

    outPx = static_cast<int>(nx * static_cast<float>(wndSize));
    outPy = static_cast<int>(ny * static_cast<float>(wndSize));

    return (outPx >= 0 && outPx < wndSize && outPy >= 0 && outPy < wndSize);
}

static void DrawDot(HDC hdc, HBRUSH hbr, int cx, int cy, int r = 4)
{
    HBRUSH hbrOld = static_cast<HBRUSH>(SelectObject(hdc, hbr));
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, hbrOld);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// RadarInit – create the radar overlay window and allocate GDI resources.
/// @param hInstance  HINSTANCE of the calling module.
/// @return TRUE on success, FALSE on failure.
BOOL RadarInit(HINSTANCE hInstance)
{
    if (g_state.running) return TRUE;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = RadarWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ValorantRadarClass";
    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return FALSE;
    }

    int size = RADAR_WND_SIZE;
    g_state.wndSize = size;

    g_state.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"ValorantRadarClass",
        L"Radar",
        WS_POPUP | WS_VISIBLE,
        10, 10, size, size,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_state.hwnd) return FALSE;

    // Build back-buffer
    HDC hdcScr   = GetDC(g_state.hwnd);
    g_state.hdcMem  = CreateCompatibleDC(hdcScr);
    g_state.hbmMem  = CreateCompatibleBitmap(hdcScr, size, size);
    ReleaseDC(g_state.hwnd, hdcScr);

    SelectObject(g_state.hdcMem, g_state.hbmMem);

    // GDI resources
    g_state.hbrBg    = CreateSolidBrush(RGB(20, 20, 20));
    g_state.hbrAlly  = CreateSolidBrush(RGB(60, 180, 90));   // green
    g_state.hbrEnemy = CreateSolidBrush(RGB(220, 60, 60));   // red
    g_state.hpenNull = static_cast<HPEN>(
        GetStockObject(NULL_PEN));

    g_state.running = true;
    ShowWindow(g_state.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_state.hwnd);
    return TRUE;
}

/// RadarUpdate – read the entity array, project positions, and redraw.
/// @param pEntityBase  Pointer to the first byte of the entity array.
/// @param pViewMatrix  Pointer to the current 4×4 view matrix (float[4][4], row-major).
void RadarUpdate(const void* pEntityBase, const ViewMatrix* pViewMatrix)
{
    if (!g_state.running || !pEntityBase) return;

    int size    = g_state.wndSize;
    HDC hdcMem  = g_state.hdcMem;
    HPEN hpOld  = static_cast<HPEN>(SelectObject(hdcMem, g_state.hpenNull));

    // Clear background
    RECT rc = { 0, 0, size, size };
    FillRect(hdcMem, &rc, g_state.hbrBg);

    // Draw a faint grid
    HPEN hpenGrid = CreatePen(PS_SOLID, 1, RGB(45, 45, 45));
    HPEN hpGrid   = static_cast<HPEN>(SelectObject(hdcMem, hpenGrid));
    int step = size / 4;
    for (int i = 1; i < 4; ++i) {
        MoveToEx(hdcMem, i * step, 0, nullptr);
        LineTo  (hdcMem, i * step, size);
        MoveToEx(hdcMem, 0, i * step, nullptr);
        LineTo  (hdcMem, size, i * step);
    }
    SelectObject(hdcMem, hpGrid);
    DeleteObject(hpenGrid);

    SelectObject(hdcMem, g_state.hpenNull);

    // Plot entities
    const uint8_t* base = static_cast<const uint8_t*>(pEntityBase);
    for (int i = 0; i < ENTITY_SLOTS; ++i) {
        const EntitySlot* ent = reinterpret_cast<const EntitySlot*>(
            base + static_cast<ptrdiff_t>(i) * ENTITY_STRIDE);

        // Bit 0 = alive
        if (!(ent->flags & 0x01)) continue;

        int px, py;
        if (!WorldToRadar(ent->x, ent->y, size, px, py)) continue;

        // Bit 1 = enemy
        HBRUSH hbr = (ent->flags & 0x02) ? g_state.hbrEnemy : g_state.hbrAlly;
        DrawDot(hdcMem, hbr, px, py);
    }

    // Optionally draw local player indicator at view-matrix origin.
    // The camera position can be extracted from the inverse view matrix:
    // pos = -R^T * t  (columns 0-2 row 3 are -dot(right,pos) etc.)
    if (pViewMatrix) {
        // Reconstruct world-space camera position from view matrix.
        // Standard view matrix: M[3][0..2] = -dot(axis, eye)
        const float* m = &pViewMatrix->m[0][0]; // row-major: m[r*4+c]
        // Row vectors (right, up, forward) are m[0..2][0..2].
        // Eye = -(R^T * t) where t = (m[0][3], m[1][3], m[2][3])
        // With row-major storage accessed as m[row][col]:
        float tx = pViewMatrix->m[0][3];
        float ty = pViewMatrix->m[1][3];
        float tz = pViewMatrix->m[2][3];

        // Inverse-rotate t back to world space: eye = -R^T * t
        float r00 = pViewMatrix->m[0][0], r01 = pViewMatrix->m[0][1], r02 = pViewMatrix->m[0][2];
        float r10 = pViewMatrix->m[1][0], r11 = pViewMatrix->m[1][1], r12 = pViewMatrix->m[1][2];
        float r20 = pViewMatrix->m[2][0], r21 = pViewMatrix->m[2][1], r22 = pViewMatrix->m[2][2];

        float camX = -(r00 * tx + r10 * ty + r20 * tz);
        float camY = -(r01 * tx + r11 * ty + r21 * tz);
        // float camZ = -(r02 * tx + r12 * ty + r22 * tz); // unused for top-down

        int px, py;
        if (WorldToRadar(camX, camY, size, px, py)) {
            // White cross for local player
            HPEN hpSelf = CreatePen(PS_SOLID, 2, RGB(255, 255, 80));
            HPEN hpPrev = static_cast<HPEN>(SelectObject(hdcMem, hpSelf));
            MoveToEx(hdcMem, px - 6, py,     nullptr); LineTo(hdcMem, px + 6, py);
            MoveToEx(hdcMem, px,     py - 6, nullptr); LineTo(hdcMem, px,     py + 6);
            SelectObject(hdcMem, hpPrev);
            DeleteObject(hpSelf);
        }
    }

    SelectObject(hdcMem, hpOld);

    // Blit back-buffer → window
    HDC hdcWnd = GetDC(g_state.hwnd);
    BitBlt(hdcWnd, 0, 0, size, size, hdcMem, 0, 0, SRCCOPY);
    ReleaseDC(g_state.hwnd, hdcWnd);

    // Pump messages so the window stays responsive
    MSG msg;
    while (PeekMessageW(&msg, g_state.hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/// RadarShutdown – destroy the window and free all GDI resources.
void RadarShutdown()
{
    if (!g_state.running) return;
    g_state.running = false;

    if (g_state.hbrBg)    { DeleteObject(g_state.hbrBg);    g_state.hbrBg    = nullptr; }
    if (g_state.hbrAlly)  { DeleteObject(g_state.hbrAlly);  g_state.hbrAlly  = nullptr; }
    if (g_state.hbrEnemy) { DeleteObject(g_state.hbrEnemy); g_state.hbrEnemy = nullptr; }
    if (g_state.hbmMem)   { DeleteObject(g_state.hbmMem);   g_state.hbmMem   = nullptr; }
    if (g_state.hdcMem)   { DeleteDC(g_state.hdcMem);       g_state.hdcMem   = nullptr; }
    if (g_state.hwnd)     { DestroyWindow(g_state.hwnd);    g_state.hwnd     = nullptr; }

    UnregisterClassW(L"ValorantRadarClass", GetModuleHandleW(nullptr));
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

namespace {

LRESULT CALLBACK RadarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        g_state.running = false;
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_state.hdcMem) {
            BitBlt(hdc, 0, 0, g_state.wndSize, g_state.wndSize,
                   g_state.hdcMem, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // prevent flicker; we paint the whole surface
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
// Standalone smoke test
// ---------------------------------------------------------------------------

#ifdef RADAR_STANDALONE

#include <cstdio>
#include <cstdlib>

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    if (!RadarInit(hInst)) {
        MessageBoxW(nullptr, L"RadarInit failed", L"Error", MB_ICONERROR);
        return 1;
    }

    // Build a synthetic entity array: 64 slots, every other one alive.
    std::array<EntitySlot, ENTITY_SLOTS> entities{};
    for (int i = 0; i < ENTITY_SLOTS; ++i) {
        // Scatter entities randomly across the world bounds.
        float t = static_cast<float>(i) / static_cast<float>(ENTITY_SLOTS);
        entities[i].x     = (t * 2.0f - 1.0f) * WORLD_HALF;
        entities[i].y     = (std::sin(t * 6.28318f)) * WORLD_HALF * 0.8f;
        entities[i].z     = 0.0f;
        entities[i].flags = (i % 2 == 0) ? 0x01u         // alive, ally
                                           : 0x03u;        // alive, enemy
    }

    // Identity view matrix (camera at origin looking down -Z).
    ViewMatrix vm{};
    vm.m[0][0] = vm.m[1][1] = vm.m[2][2] = vm.m[3][3] = 1.0f;

    // Animate for ~10 seconds (100 frames at ~100 ms each).
    for (int frame = 0; frame < 100 && g_state.running; ++frame) {
        // Drift entities slightly to show animation.
        for (int i = 0; i < ENTITY_SLOTS; ++i) {
            entities[i].x += (i % 3 - 1) * 50.0f;
            entities[i].y += (i % 5 - 2) * 30.0f;
        }
        RadarUpdate(entities.data(), &vm);
        Sleep(100);
    }

    RadarShutdown();
    return 0;
}

#endif // RADAR_STANDALONE
