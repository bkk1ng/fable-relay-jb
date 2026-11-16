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
#include <cwchar>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// ========== Configuration ==========
constexpr float AIM_CONE_RADIUS   = 240.0f;
constexpr float AIM_SMOOTHING     = 5.0f;
constexpr float ESP_BOX_THICKNESS = 2.0f;   // 2px outline
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

// VALORANT uses centimetres internally; divide by this to get metres.
constexpr float UNITS_PER_METRE = 100.0f;

// ========== Memory Read Placeholder ==========
namespace driver {
    HANDLE    process_handle = nullptr;
    uintptr_t base_address   = 0;

    bool init(const wchar_t* /*process_name*/) { return true; }

    template<typename T>
    T read(uintptr_t /*address*/) { return T{}; }

    void read_buffer(uintptr_t /*address*/, void* /*buffer*/, size_t /*size*/) {}
}

// ========== Math Types ==========
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct ViewMatrix { float matrix[4][4]; };

Vec3  operator+(const Vec3& a, const Vec3& b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
Vec3  operator-(const Vec3& a, const Vec3& b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
Vec3  operator*(const Vec3& v, float s)        { return { v.x*s,   v.y*s,   v.z*s   }; }
float length(const Vec3& v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

// ========== Overlay Globals ==========
HWND                   overlay_hwnd  = nullptr;
ID2D1Factory*          d2d_factory   = nullptr;
ID2D1HwndRenderTarget* render_target = nullptr;
ID2D1SolidColorBrush*  brush         = nullptr;  // reused; colour set before each call
IDWriteFactory*        write_factory = nullptr;
IDWriteTextFormat*     text_format   = nullptr;

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
        nullptr, nullptr, hinstance, nullptr
    );
    if (!overlay_hwnd) return false;

    SetLayeredWindowAttributes(overlay_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(overlay_hwnd, SW_SHOW);
    UpdateWindow(overlay_hwnd);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
    RECT rc;
    GetClientRect(overlay_hwnd, &rc);
    d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(overlay_hwnd, D2D1::SizeU(rc.right, rc.bottom)),
        &render_target
    );

    // Single reusable brush; colour is set via SetColor() before each draw call.
    render_target->CreateSolidColorBrush(ESP_BOX_COLOR, &brush);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), (IUnknown**)&write_factory);
    write_factory->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, ESP_FONT_SIZE, L"en-us", &text_format
    );
    if (text_format)
        text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

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
    int   team;
    int   health;
    Vec3  head_pos;
    Vec3  feet_pos;
    bool  valid;
};

ViewMatrix view_matrix;
Vec3       camera_position;  // set from local player position each frame
int        local_team;

bool get_view_matrix() {
    // Real: driver::read_buffer(base + GAME_OFFSET_VIEW_MATRIX, &view_matrix, sizeof(view_matrix));
    return true;
}

Vec3 get_bone_position(uintptr_t bone_array_base, int bone_index) {
    // Real: return driver::read<Vec3>(bone_array_base + bone_index * 0x20);
    return Vec3{};
}

std::vector<Player> get_players() {
    std::vector<Player> players;
    // for (int i = 0; i < 64; ++i) {
    //     uintptr_t entity = driver::read<uintptr_t>(base + GAME_OFFSET_ENTITY_LIST + i * 0x8);
    //     if (!entity) continue;
    //     Player p;
    //     p.address  = entity;
    //     p.team     = driver::read<int>(entity + PLAYER_OFFSET_TEAM);
    //     p.health   = driver::read<int>(entity + PLAYER_OFFSET_HEALTH);
    //     uintptr_t bone_array = driver::read<uintptr_t>(entity + PLAYER_OFFSET_BONE_ARRAY);
    //     p.head_pos = get_bone_position(bone_array, BONE_INDEX_HEAD);
    //     p.feet_pos = driver::read<Vec3>(entity + PLAYER_OFFSET_POSITION);
    //     p.valid    = p.health > 0 && p.team != local_team;
    //     if (p.valid) players.push_back(p);
    // }
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
void move_mouse_to(int x, int y) {
    POINT current;
    GetCursorPos(&current);
    float dx   = (float)(x - current.x);
    float dy   = (float)(y - current.y);
    float dist = std::sqrt(dx*dx + dy*dy);
    if (dist < 1.0f) return;

    float step_count = dist / AIM_SMOOTHING;
    float step_x     = dx / step_count;
    float step_y     = dy / step_count;

    for (int i = 0; i < (int)step_count; ++i) {
        INPUT input      = {};
        input.type       = INPUT_MOUSE;
        input.mi.dx      = (LONG)(step_x * (65535.0f / GetSystemMetrics(SM_CXSCREEN)));
        input.mi.dy      = (LONG)(step_y * (65535.0f / GetSystemMetrics(SM_CYSCREEN)));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &input, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void run_aimbot(const std::vector<Player>& players) {
    if (players.empty()) return;
    Vec2  crosshair   = { screen_width / 2.0f, screen_height / 2.0f };
    float best_dist   = AIM_CONE_RADIUS * AIM_CONE_RADIUS;
    Vec2  best_target = { -1.0f, -1.0f };

    for (const auto& p : players) {
        if (p.health <= 0) continue;
        Vec2 head_screen;
        if (!world_to_screen(p.head_pos, head_screen)) continue;

        float dx      = head_screen.x - crosshair.x;
        float dy      = head_screen.y - crosshair.y;
        float dist_sq = dx*dx + dy*dy;
        if (dist_sq < best_dist) {
            best_dist   = dist_sq;
            best_target = head_screen;
        }
    }

    if (best_target.x >= 0.0f)
        move_mouse_to((int)best_target.x, (int)best_target.y);
}

// ========== Colour helpers ==========
// Returns green at full health, red at zero health.
// health_ratio clamped to [0, 1].
static D2D1::ColorF health_colour(float health_ratio) {
    float r = 1.0f - health_ratio;
    float g = health_ratio;
    return D2D1::ColorF(r, g, 0.0f, 1.0f);
}

// ========== ESP Drawing ==========

// 2px outlined bounding box.
static void draw_box(float x1, float y1, float x2, float y2) {
    brush->SetColor(ESP_BOX_COLOR);
    render_target->DrawRectangle(D2D1::RectF(x1, y1, x2, y2), brush, ESP_BOX_THICKNESS);
}

// Vertical health bar left of the box; colour ramps red (0 hp) -> green (full hp).
static void draw_health_bar(float left, float top, float bottom,
                             int health, int max_health = 100) {
    constexpr float BAR_WIDTH = 4.0f;
    constexpr float BAR_GAP   = 3.0f;

    float bar_right  = left - BAR_GAP;
    float bar_left   = bar_right - BAR_WIDTH;
    float bar_height = bottom - top;

    // Dark background
    brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f));
    render_target->FillRectangle(D2D1::RectF(bar_left, top, bar_right, bottom), brush);

    // Filled portion, colour-ramped
    float ratio      = std::clamp((float)health / (float)max_health, 0.0f, 1.0f);
    float filled_top = top + bar_height * (1.0f - ratio);

    brush->SetColor(health_colour(ratio));
    render_target->FillRectangle(
        D2D1::RectF(bar_left, filled_top, bar_right, bottom), brush);
}

// Distance label in metres, centred above the box.
static void draw_distance_label(float box_left, float box_right,
                                 float box_top,  float distance_m) {
    wchar_t buf[16];
    swprintf_s(buf, L"%.0fm", distance_m);

    constexpr float LABEL_H = ESP_FONT_SIZE + 2.0f;
    D2D1_RECT_F layout = D2D1::RectF(
        box_left,
        box_top - LABEL_H - 2.0f,
        box_right,
        box_top - 2.0f
    );

    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));  // white
    render_target->DrawText(
        buf, (UINT32)wcslen(buf),
        text_format, layout, brush
    );
}

void draw_esp(const std::vector<Player>& players) {
    if (!render_target) return;
    render_target->BeginDraw();
    render_target->Clear(D2D1::ColorF(0, 0, 0, 0));

    for (const auto& p : players) {
        if (p.health <= 0) continue;

        Vec2 head, feet;
        if (!world_to_screen(p.head_pos, head)) continue;
        if (!world_to_screen(p.feet_pos, feet)) continue;

        float height = feet.y - head.y;
        if (height < 1.0f) continue;

        float width  = height * 0.65f;
        float left   = head.x - width * 0.5f;
        float right  = head.x + width * 0.5f;
        float top    = head.y;
        float bottom = feet.y;

        // 3D distance from camera to player feet, converted to metres
        float dist_m = length(p.feet_pos - camera_position) / UNITS_PER_METRE;

        draw_box(left, top, right, bottom);
        draw_health_bar(left, top, bottom, p.health);
        draw_distance_label(left, right, top, dist_m);
    }

    render_target->EndDraw();
}

// ========== Main Loop ==========
int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE, LPSTR, int) {
    if (!driver::init(L"VALORANT-Win64-Shipping.exe")) return 1;

    uintptr_t local_player = driver::read<uintptr_t>(
        driver::base_address + GAME_OFFSET_LOCAL_PLAYER);
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

        get_view_matrix();

        // Real: camera_position = driver::read<Vec3>(local_player + PLAYER_OFFSET_POSITION);

        auto players = get_players();
        run_aimbot(players);
        draw_esp(players);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    cleanup_overlay();
    return 0;
}
