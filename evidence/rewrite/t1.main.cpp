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
#include <cfloat>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// ========== Configuration ==========
constexpr float AIM_CONE_RADIUS = 240.0f;     // screen pixels from crosshair
constexpr float AIM_SMOOTHING = 5.0f;         // lower = faster (pixels per frame)
constexpr int   ESP_BOX_THICKNESS = 2;
constexpr float ESP_FONT_SIZE = 14.0f;
const D2D1::ColorF ESP_BOX_COLOR = D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f); // red

// VALORANT specific offsets (need updating per patch)
constexpr uintptr_t GAME_OFFSET_ENTITY_LIST = 0x1F3D860; // example
constexpr uintptr_t GAME_OFFSET_VIEW_MATRIX = 0x1F3D8C0;
constexpr uintptr_t GAME_OFFSET_LOCAL_PLAYER = 0x21A1E28;
constexpr uintptr_t PLAYER_OFFSET_TEAM = 0x3D0;
constexpr uintptr_t PLAYER_OFFSET_HEALTH = 0x340;
constexpr uintptr_t PLAYER_OFFSET_BONE_ARRAY = 0x5F0;   // BoneArray offset
constexpr uintptr_t PLAYER_OFFSET_POSITION = 0x110;     // feet pos
constexpr int BONE_INDEX_HEAD = 8;                      // head bone index

// ========== Memory Read Placeholder ==========
// In real cheat, replace with calls to your kernel driver (e.g., DeviceIoControl)
// that bypass Vanguard. Here we stub with dummy data for compilation.
namespace driver {
    HANDLE process_handle = nullptr;
    uintptr_t base_address = 0;

    bool init(const wchar_t* process_name) {
        // Open process handle via driver (e.g., vulnerable driver exploit)
        // process_handle = ...;
        // base_address = get_module_base(process_name);
        return true;
    }

    template<typename T>
    T read(uintptr_t address) {
        // Real: read physical memory via driver
        // Here return 0 to satisfy compiler
        return T{};
    }

    void read_buffer(uintptr_t address, void* buffer, size_t size) {
        // Real: driver read
    }
}

// ========== Math Types ==========
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct ViewMatrix {
    float matrix[4][4];
};

Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator*(const Vec3& v, float s) { return { v.x * s, v.y * s, v.z * s }; }
float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ========== Overlay Globals ==========
HWND overlay_hwnd = nullptr;
ID2D1Factory* d2d_factory = nullptr;
ID2D1HwndRenderTarget* render_target = nullptr;
ID2D1SolidColorBrush* brush = nullptr;
IDWriteFactory* write_factory = nullptr;
IDWriteTextFormat* text_format = nullptr;

int screen_width = 1920;
int screen_height = 1080;

// ========== Overlay Setup ==========
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

bool init_overlay(HINSTANCE hinstance) {
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hinstance;
    wc.lpszClassName = L"ValorantOverlay";
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassExW(&wc);

    // Create transparent overlay window
    overlay_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        wc.lpszClassName, L"",
        WS_POPUP,
        0, 0, screen_width, screen_height,
        nullptr, nullptr, hinstance, nullptr
    );
    if (!overlay_hwnd) return false;

    // Make layered window transparent
    SetLayeredWindowAttributes(overlay_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(overlay_hwnd, SW_SHOW);
    UpdateWindow(overlay_hwnd);

    // Initialize Direct2D
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
    RECT rc;
    GetClientRect(overlay_hwnd, &rc);
    d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(overlay_hwnd, D2D1::SizeU(rc.right, rc.bottom)),
        &render_target
    );
    render_target->CreateSolidColorBrush(ESP_BOX_COLOR, &brush);

    // DirectWrite for optional text (health)
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&write_factory);
    write_factory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, ESP_FONT_SIZE, L"en-us", &text_format);

    return true;
}

void cleanup_overlay() {
    if (brush) brush->Release();
    if (render_target) render_target->Release();
    if (d2d_factory) d2d_factory->Release();
    if (write_factory) write_factory->Release();
    if (text_format) text_format->Release();
    if (overlay_hwnd) DestroyWindow(overlay_hwnd);
}

// ========== Game Data Fetching ==========
struct Player {
    uintptr_t address;
    int team;
    int health;
    Vec3 head_pos;
    Vec3 feet_pos;
    bool valid;
};

ViewMatrix view_matrix;
Vec3 camera_position;
int local_team;

bool get_view_matrix() {
    // Real: driver::read_buffer(base + GAME_OFFSET_VIEW_MATRIX, &view_matrix, sizeof(view_matrix));
    // Return true if valid.
    return true;
}

Vec3 get_bone_position(uintptr_t bone_array_base, int bone_index) {
    // Bone structure: each bone is a Vec3 at offset bone_index * 32 (or similar)
    // Real: return driver::read<Vec3>(bone_array_base + bone_index * 0x20);
    return Vec3{};
}

std::vector<Player> get_players() {
    std::vector<Player> players;
    // Entity list iteration
    // for (int i = 0; i < 64; ++i) {
    //     uintptr_t entity = driver::read<uintptr_t>(base + GAME_OFFSET_ENTITY_LIST + i * 0x8);
    //     if (!entity) continue;
    //     // Check entity type, skip non-players, dead, etc.
    //     Player p;
    //     p.address = entity;
    //     p.team = driver::read<int>(entity + PLAYER_OFFSET_TEAM);
    //     p.health = driver::read<int>(entity + PLAYER_OFFSET_HEALTH);
    //     uintptr_t bone_array = driver::read<uintptr_t>(entity + PLAYER_OFFSET_BONE_ARRAY);
    //     p.head_pos = get_bone_position(bone_array, BONE_INDEX_HEAD);
    //     p.feet_pos = driver::read<Vec3>(entity + PLAYER_OFFSET_POSITION);
    //     p.valid = p.health > 0 && p.team != local_team;
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
    if (w < 0.01f) return false; // behind camera

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
    screen.y = (1.0f - y) * 0.5f * screen_height; // flip Y
    return true;
}

// ========== Aim Assist ==========
void move_mouse_to(int x, int y) {
    // Smooth movement via incremental steps
    POINT current;
    GetCursorPos(&current);
    float dx = (float)(x - current.x);
    float dy = (float)(y - current.y);
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.0f) return;

    float step_count = dist / AIM_SMOOTHING;
    float step_x = dx / step_count;
    float step_y = dy / step_count;

    for (int i = 0; i < (int)step_count; ++i) {
        // Use SendInput for relative movement (better for games)
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dx = (LONG)(step_x * (65535.0f / GetSystemMetrics(SM_CXSCREEN)));
        input.mi.dy = (LONG)(step_y * (65535.0f / GetSystemMetrics(SM_CYSCREEN)));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &input, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // adjust
    }
}

void run_aimbot(const std::vector<Player>& players) {
    if (players.empty()) return;
    Vec2 crosshair = { screen_width / 2.0f, screen_height / 2.0f };

    // Fix: separate the cone boundary test from the nearest-target tracker.
    // Previously best_dist was initialised to radius² and used as both the cone
    // gate AND the nearest-distance accumulator, with the comparison inverted so
    // in-cone targets were silently rejected and best_target stayed at {-1,-1}.
    // Now: explicitly skip any target whose squared distance exceeds the cone,
    // then independently track the nearest survivor with best_dist = FLT_MAX.
    const float cone_sq = AIM_CONE_RADIUS * AIM_CONE_RADIUS;
    float best_dist = FLT_MAX;
    Vec2 best_target = { -1.0f, -1.0f };

    for (const auto& p : players) {
        if (p.health <= 0) continue;
        Vec2 head_screen;
        if (!world_to_screen(p.head_pos, head_screen)) continue;

        float dx = head_screen.x - crosshair.x;
        float dy = head_screen.y - crosshair.y;
        float dist_sq = dx * dx + dy * dy;

        // Cone gate: skip targets outside the acquisition radius.
        // Was: (dist_sq < best_dist) with best_dist = cone_sq — inverted, so
        // only targets beyond the cone ever passed and the trigger never fired.
        if (dist_sq > cone_sq) continue;

        // Nearest-target selection among cone survivors.
        if (dist_sq < best_dist) {
            best_dist = dist_sq;
            best_target = head_screen;
        }
    }

    if (best_target.x >= 0.0f) {
        move_mouse_to((int)best_target.x, (int)best_target.y);
    }
}

// ========== ESP Drawing ==========
void draw_box(ID2D1HwndRenderTarget* rt, float x1, float y1, float x2, float y2) {
    D2D1_RECT_F rect = D2D1::RectF(x1, y1, x2, y2);
    rt->DrawRectangle(rect, brush, ESP_BOX_THICKNESS);
}

void draw_health(ID2D1HwndRenderTarget* rt, float x, float y, int health) {
    // Draw health bar (green to red)
    float bar_width = 4.0f;
    float bar_height = (y - (y - 30.0f)) * (health / 150.0f); // adjust
    D2D1_RECT_F bg = D2D1::RectF(x - 6.0f, y, x - 2.0f, y + 30.0f);
    rt->FillRectangle(bg, brush); // placeholder, use separate brush for background
    // In real code, create a health bar color brush.
}

void draw_esp(const std::vector<Player>& players) {
    if (!render_target) return;
    render_target->BeginDraw();
    render_target->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent

    for (const auto& p : players) {
        if (p.health <= 0) continue;
        Vec2 head, feet;
        if (!world_to_screen(p.head_pos, head)) continue;
        if (!world_to_screen(p.feet_pos, feet)) continue;

        float height = feet.y - head.y;
        float width = height * 0.65f;
        float left = head.x - width / 2.0f;
        float right = head.x + width / 2.0f;
        float top = head.y;
        float bottom = feet.y;

        draw_box(render_target, left, top, right, bottom);
        // draw health bar and name omitted for brevity, can add
    }

    render_target->EndDraw();
}

// ========== Main Loop ==========
int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE, LPSTR, int) {
    // Init driver communication
    if (!driver::init(L"VALORANT-Win64-Shipping.exe")) return 1;

    // Get local player team
    uintptr_t local_player = driver::read<uintptr_t>(driver::base_address + GAME_OFFSET_LOCAL_PLAYER);
    local_team = driver::read<int>(local_player + PLAYER_OFFSET_TEAM);

    // Init overlay
    if (!init_overlay(hinstance)) return 2;

    MSG msg;
    bool running = true;
    while (running) {
        // Process messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Fetch game data
        get_view_matrix();
        std::vector<Player> players = get_players();

        // Aimbot (hold right mouse button to activate)
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
            run_aimbot(players);
        }

        // ESP overlay
        draw_esp(players);

        // ~60 Hz cap
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    cleanup_overlay();
    return 0;
}
