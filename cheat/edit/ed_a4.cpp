// valorant_aim_esp.cpp
// Compile with: cl /EHsc /std:c++17 /Fe:valorant_aim_esp.exe valorant_aim_esp.cpp /link user32.lib gdi32.lib d2d1.lib dwrite.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cmath>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <thread>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// ========== Configuration ==========
constexpr float AIM_CONE_RADIUS   = 240.0f;  // screen pixels from crosshair
constexpr float AIM_SMOOTHING     = 5.0f;    // max pixels moved per call
constexpr int   ESP_BOX_THICKNESS = 2;
constexpr float ESP_FONT_SIZE     = 14.0f;
const D2D1::ColorF ESP_BOX_COLOR  = D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f);

// VALORANT specific offsets (need updating per patch)
constexpr uintptr_t GAME_OFFSET_ENTITY_LIST  = 0x1F3D860;
constexpr uintptr_t GAME_OFFSET_VIEW_MATRIX  = 0x1F3D8C0;
constexpr uintptr_t GAME_OFFSET_LOCAL_PLAYER = 0x21A1E28;
constexpr uintptr_t PLAYER_OFFSET_TEAM       = 0x3D0;
constexpr uintptr_t PLAYER_OFFSET_HEALTH     = 0x340;
constexpr uintptr_t PLAYER_OFFSET_BONE_ARRAY = 0x5F0;
constexpr uintptr_t PLAYER_OFFSET_POSITION   = 0x110;
constexpr int       BONE_INDEX_HEAD          = 8;

// ========== Memory Read Placeholder ==========
namespace driver {
    HANDLE   process_handle = nullptr;
    uintptr_t base_address  = 0;

    bool init(const wchar_t* /*process_name*/) {
        return true;
    }

    template<typename T>
    T read(uintptr_t /*address*/) {
        return T{};
    }

    void read_buffer(uintptr_t /*address*/, void* /*buffer*/, size_t /*size*/) {}
}

// ========== Math Types ==========
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct ViewMatrix { float matrix[4][4]; };

Vec3  operator+(const Vec3& a, const Vec3& b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
Vec3  operator-(const Vec3& a, const Vec3& b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
Vec3  operator*(const Vec3& v, float s)       { return { v.x*s,   v.y*s,   v.z*s   }; }
float length(const Vec3& v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

// ========== Overlay Globals ==========
HWND                   overlay_hwnd   = nullptr;
ID2D1Factory*          d2d_factory    = nullptr;
ID2D1HwndRenderTarget* render_target  = nullptr;
ID2D1SolidColorBrush*  brush          = nullptr;
IDWriteFactory*        write_factory  = nullptr;
IDWriteTextFormat*     text_format    = nullptr;

int screen_width  = 1920;
int screen_height = 1080;

// ========== Overlay Setup ==========
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_PAINT:   ValidateRect(hwnd, nullptr); return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

bool init_overlay(HINSTANCE hinstance) {
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hinstance;
    wc.lpszClassName = L"ValorantOverlay";
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassExW(&wc);

    overlay_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, screen_width, screen_height,
        nullptr, nullptr, hinstance, nullptr);
    if (!overlay_hwnd) return false;

    SetLayeredWindowAttributes(overlay_hwnd, RGB(0,0,0), 0, LWA_COLORKEY);
    ShowWindow(overlay_hwnd, SW_SHOW);
    UpdateWindow(overlay_hwnd);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
    RECT rc;
    GetClientRect(overlay_hwnd, &rc);
    d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(overlay_hwnd,
            D2D1::SizeU(rc.right, rc.bottom)),
        &render_target);
    render_target->CreateSolidColorBrush(ESP_BOX_COLOR, &brush);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), (IUnknown**)&write_factory);
    write_factory->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, ESP_FONT_SIZE, L"en-us", &text_format);

    return true;
}

void cleanup_overlay() {
    if (brush)         brush->Release();
    if (render_target) render_target->Release();
    if (d2d_factory)   d2d_factory->Release();
    if (write_factory) write_factory->Release();
    if (text_format)   text_format->Release();
    if (overlay_hwnd)  DestroyWindow(overlay_hwnd);
}

// ========== Game Data Fetching ==========
struct Player {
    uintptr_t address;
    int       team;
    int       health;
    Vec3      head_pos;
    Vec3      feet_pos;
    bool      valid;
};

ViewMatrix view_matrix;
int        local_team;

bool get_view_matrix() {
    // Real: driver::read_buffer(base + GAME_OFFSET_VIEW_MATRIX,
    //                           &view_matrix, sizeof(view_matrix));
    return true;
}

Vec3 get_bone_position(uintptr_t bone_array_base, int bone_index) {
    // Real: return driver::read<Vec3>(bone_array_base + bone_index * 0x20);
    return Vec3{};
}

std::vector<Player> get_players() {
    std::vector<Player> players;
    // for (int i = 0; i < 64; ++i) { ... }
    return players;
}

// ========== World To Screen ==========
bool world_to_screen(const Vec3& world, Vec2& screen) {
    float w = view_matrix.matrix[3][0] * world.x +
              view_matrix.matrix[3][1] * world.y +
              view_matrix.matrix[3][2] * world.z +
              view_matrix.matrix[3][3];
    if (w < 0.01f) return false;

    float inv_w = 1.0f / w;
    float x = (view_matrix.matrix[0][0] * world.x +
               view_matrix.matrix[0][1] * world.y +
               view_matrix.matrix[0][2] * world.z +
               view_matrix.matrix[0][3]) * inv_w;
    float y = (view_matrix.matrix[1][0] * world.x +
               view_matrix.matrix[1][1] * world.y +
               view_matrix.matrix[1][2] * world.z +
               view_matrix.matrix[1][3]) * inv_w;

    screen.x = (x + 1.0f) * 0.5f * screen_width;
    screen.y = (1.0f - y) * 0.5f * screen_height;
    return true;
}

// ========== Aim Assist ==========
//
// FIX (overshoot): previously this function sent MOUSEEVENTF_ABSOLUTE with
// per-step pixel deltas treated as absolute 0-65535 coordinates, which caused
// the cursor to jump near the screen origin.  It also fired all steps in a
// single call, so a fast flick moved the full distance in one frame.
//
// Replacement: one MOUSEEVENTF_MOVE (relative) per call, magnitude clamped to
// AIM_SMOOTHING pixels.  The caller drives pacing via the main loop rate.
void move_mouse_to(int x, int y) {
    POINT current;
    GetCursorPos(&current);
    float dx   = (float)(x - current.x);
    float dy   = (float)(y - current.y);
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.0f) return;

    // Scale down to at most AIM_SMOOTHING pixels; on slow approaches (dist <=
    // AIM_SMOOTHING) scale == 1.0 so the cursor steps exactly to the target.
    float scale = std::min(1.0f, AIM_SMOOTHING / dist);

    INPUT input      = {};
    input.type       = INPUT_MOUSE;
    input.mi.dx      = (LONG)(dx * scale);
    input.mi.dy      = (LONG)(dy * scale);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;   // relative, no ABSOLUTE flag
    SendInput(1, &input, sizeof(INPUT));
}

//
// FIX (staleness): previously the view matrix used for cone selection was
// whatever get_view_matrix() last wrote to the global — potentially a full
// loop iteration old.  The move destination was derived from the same stale
// batch-read player positions.
//
// Replacement:
//   1. Refresh the view matrix at function entry so all world_to_screen calls
//      below use the same consistent, fresh snapshot.
//   2. After picking the best entity by cone angle, re-read its head bone from
//      memory immediately before projecting the move destination.  This means
//      a player who moved between the batch read and the aimbot decision is
//      aimed at their actual current position rather than where they were.
void run_aimbot(const std::vector<Player>& players) {
    if (players.empty()) return;

    // Refresh matrix once here; all world_to_screen calls below use this snap.
    get_view_matrix();

    Vec2  crosshair    = { screen_width / 2.0f, screen_height / 2.0f };
    float best_dist_sq = AIM_CONE_RADIUS * AIM_CONE_RADIUS;
    uintptr_t best_entity = 0;

    for (const auto& p : players) {
        if (p.health <= 0) continue;
        Vec2 head_screen;
        if (!world_to_screen(p.head_pos, head_screen)) continue;

        float dx      = head_screen.x - crosshair.x;
        float dy      = head_screen.y - crosshair.y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq < best_dist_sq) {
            best_dist_sq  = dist_sq;
            best_entity   = p.address;
        }
    }

    if (best_entity != 0) {
        // Re-read head position fresh so the move uses current world coords,
        // not the positions that were snapshotted during get_players().
        uintptr_t bone_array = driver::read<uintptr_t>(
            best_entity + PLAYER_OFFSET_BONE_ARRAY);
        Vec3 fresh_head = get_bone_position(bone_array, BONE_INDEX_HEAD);

        Vec2 target;
        if (world_to_screen(fresh_head, target)) {
            move_mouse_to((int)target.x, (int)target.y);
        }
    }
}

// ========== ESP Drawing ==========
void draw_box(ID2D1HwndRenderTarget* rt,
              float x1, float y1, float x2, float y2) {
    D2D1_RECT_F rect = D2D1::RectF(x1, y1, x2, y2);
    rt->DrawRectangle(rect, brush, ESP_BOX_THICKNESS);
}

void draw_health(ID2D1HwndRenderTarget* rt,
                 float x, float y, int health) {
    float bar_height = 30.0f * (health / 150.0f);
    D2D1_RECT_F bg   = D2D1::RectF(x - 6.0f, y, x - 2.0f, y + 30.0f);
    rt->FillRectangle(bg, brush);
    (void)bar_height; // health bar color brush left to caller
}

void draw_esp(const std::vector<Player>& players) {
    if (!render_target) return;
    render_target->BeginDraw();
    render_target->Clear(D2D1::ColorF(0, 0, 0, 0));

    for (const auto& p : players) {
        if (p.health <= 0) continue;
        Vec2 head, feet;
        if (!world_to_screen(p.head_pos,  head)) continue;
        if (!world_to_screen(p.feet_pos,  feet)) continue;

        float height = feet.y - head.y;
        float width  = height * 0.65f;
        float left   = head.x - width  / 2.0f;
        float right  = head.x + width  / 2.0f;

        draw_box(render_target, left, head.y, right, feet.y);
    }

    render_target->EndDraw();
}

// ========== Main Loop ==========
int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE, LPSTR, int) {
    if (!driver::init(L"VALORANT-Win64-Shipping.exe")) return 1;

    uintptr_t local_player =
        driver::read<uintptr_t>(driver::base_address + GAME_OFFSET_LOCAL_PLAYER);
    local_team = driver::read<int>(local_player + PLAYER_OFFSET_TEAM);

    if (!init_overlay(hinstance)) return 2;

    MSG  msg;
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Fetch view matrix and player list as close together as possible to
        // minimise the window in which positions can go stale between reads.
        get_view_matrix();
        auto players = get_players();

        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            run_aimbot(players);
        }

        draw_esp(players);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    cleanup_overlay();
    return 0;
}
