// valorant_cheat.cpp — external ESP + aimbot with kernel-driver read path.
// Build: x64 Release, link d2d1.lib dwrite.lib (or #pragma comment handles it).
// Requires: a loaded kernel driver exposing the IOCTL contract below.

#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#pragma comment(lib, "d2d1.lib")

// ============================================================================
// CONFIG
// ============================================================================
namespace cfg {
    bool  esp_enabled   = true;
    bool  skeleton_esp  = true;
    float box_thickness = 1.5f;

    bool  aimbot_enabled = true;
    int   aim_key        = VK_XBUTTON1;   // side mouse button
    float aim_fov_px     = 120.0f;        // FOV circle radius in pixels
    float aim_smooth     = 8.0f;          // higher = slower/smoother (1 = snap)
    float head_bone      = 8.0f;          // VALORANT head bone; verify per model

    // ========================================================================
    // OFFSETS — UPDATE EVERY PATCH (dumped from the current client)
    // ========================================================================
    namespace offs {
        constexpr uint64_t UWorld            = 0x8000000;      // TODO: re-dump
        constexpr uint64_t GameInstance      = 0x0180;          // World->GameInstance
        constexpr uint64_t LocalPlayers      = 0x0038;          // GameInstance->LocalPlayers
        constexpr uint64_t PlayerController  = 0x0030;          // LocalPlayer->PlayerController
        constexpr uint64_t PlayerCameraManager = 0x0348;        // Controller->CameraManager
        constexpr uint64_t CameraCachePOV    = 0x0610;         // CameraManager->CameraCachePrivate.POV
        constexpr uint64_t ViewMatrixOff     = 0x0250;          // within POV (MinimalViewInfo)
        constexpr uint64_t PersistentLevel   = 0x0030;          // World->PersistentLevel
        constexpr uint64_t ActorArray        = 0x0098;          // Level->Actors (TArray)
        constexpr uint64_t RootComponent     = 0x0130;          // Actor->RootComponent
        constexpr uint64_t RelativeLocation  = 0x011C;           // SceneComponent->RelativeLocation (FVector)
        constexpr uint64_t PlayerState       = 0x0280;          // Pawn->PlayerState
        constexpr uint64_t TeamComponent     = 0x0290;          // PlayerState->team/agent id (verify)
        constexpr uint64_t Health            = 0x00E4;          // Pawn->Health
        constexpr uint64_t Mesh              = 0x0278;          // Pawn->Mesh (SkeletalMeshComponent)
        constexpr uint64_t BoneArray         = 0x05B0;          // Mesh->CachedBoneSpaceTransforms (verify)
        constexpr uint64_t ComponentToWorld  = 0x01C0;          // Mesh->ComponentToWorld (FTransform)
        constexpr uint64_t PlayerCount       = 0x0180;          // GameInstance->playercount-ish (optional)
    }
    constexpr uint64_t ENTITY_SIZE = 0x0308;   // actor stride; verify
}

// ============================================================================
// KERNEL DRIVER IPC
//   Contract: driver exposes device "\\.\VgkRead", IOCTL_READ copies
//   `size` bytes from `address` (in game PID) into `out` (caller's buffer).
//   Driver must be loaded before this process starts.
// ============================================================================
namespace drv {
    constexpr auto DEVICE   = L"\\\\.\\VgkRead";
    constexpr auto CTL_READ = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);

    struct ReadReq {
        uint32_t pid;
        uint64_t address;
        uint32_t size;
        void*    out;
    };

    HANDLE  handle = nullptr;
    DWORD   game_pid = 0;

    bool init(DWORD pid) {
        game_pid = pid;
        handle = CreateFileW(DEVICE, GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle != INVALID_HANDLE_VALUE;
    }

    // Returns false on failure; caller decides whether to retry/abort.
    bool read(uint64_t address, void* out, uint32_t size) {
        if (!handle) return false;
        ReadReq req{ game_pid, address, size, out };
        DWORD ret = 0;
        return DeviceIoControl(handle, CTL_READ, &req, sizeof(req), nullptr, 0, &ret, nullptr);
    }

    template <typename T>
    T read_t(uint64_t address) {
        T value{};
        if (!read(address, &value, sizeof(T))) value = {};
        return value;
    }

    uint64_t read64(uint64_t a) { return read_t<uint64_t>(a); }
    uint32_t read32(uint64_t a) { return read_t<uint32_t>(a); }
    float    readf (uint64_t a) { return read_t<float >(a); }
}

// ============================================================================
// MATH
// ============================================================================
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

struct Matrix4 { float m[4][4]; };

static Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static float dist3d(const Vec3& a, const Vec3& b) { Vec3 d = a - b; return sqrtf(d.x*d.x + d.y*d.y + d.z*d.z); }

// UE4 FTransform: rotation quaternion (16B), translation (12B), pad, scale (12B)
struct FTransform {
    float quat[4];
    Vec3  translation;
    float pad;
    Vec3  scale;
};

static Vec3 bone_world(const FTransform& comp_to_world, const FTransform& bone) {
    // Apply component-space bone transform to component-to-world.
    // Simplified rigid transform: q_world = q_c2w * q_bone; p = p_c2w + R(q_c2w) * p_bone
    // q_c2w from comp_to_world, p_bone from bone.translation
    // Rotation of vector v by quaternion q:
    float qw = comp_to_world.quat[0], qx = comp_to_world.quat[1],
          qy = comp_to_world.quat[2], qz = comp_to_world.quat[3];
    Vec3 v = bone.translation;
    // t = 2 * cross(q.xyz, v)
    float tx = 2.0f * (qy*v.z - qz*v.y);
    float ty = 2.0f * (qz*v.x - qx*v.z);
    float tz = 2.0f * (qx*v.y - qy*v.x);
    // v' = v + qw*t + cross(q.xyz, t)
    Vec3 r;
    r.x = v.x + qw*tx + (qy*tz - qz*ty);
    r.y = v.y + qw*ty + (qz*tx - qx*tz);
    r.z = v.z + qw*tz + (qx*ty - qy*tx);
    return { comp_to_world.translation.x + r.x,
             comp_to_world.translation.y + r.y,
             comp_to_world.translation.z + r.z };
}

// ============================================================================
// GAME STATE
// ============================================================================
struct Player {
    uint64_t ptr;
    Vec3     head;
    Vec3     feet;
    int      health;
    int      team;
    std::vector<Vec3> bones;   // world-space bone positions for skeleton
    bool     valid;
};

static Matrix4 view_matrix;

static bool world_to_screen(const Vec3& world, Vec2& out, float sw, float sh) {
    const auto& m = view_matrix;
    float _x = world.x * m.m[0][0] + world.y * m.m[1][0] + world.z * m.m[2][0] + m.m[3][0];
    float _y = world.x * m.m[0][1] + world.y * m.m[1][1] + world.z * m.m[2][1] + m.m[3][1];
    float _w = world.x * m.m[0][3] + world.y * m.m[1][3] + world.z * m.m[2][3] + m.m[3][3];
    if (_w < 0.1f) return false;
    float inv = 1.0f / _w;
    out.x = sw * 0.5f + _x * inv * (sw * 0.5f);
    out.y = sh * 0.5f - _y * inv * (sh * 0.5f);
    return true;
}

// resolve local player + enemy list. Returns local team id via reference.
static bool read_players(std::vector<Player>& out, int& local_team, uint64_t& camera_out) {
    out.clear();
    uint64_t world = drv::read64(drv::read64(0) ? 0 : 0); // placeholder no-op
    world = drv::read64(cfg::offs::UWorld);
    if (!world) return false;

    uint64_t game_inst = drv::read64(world + cfg::offs::GameInstance);
    if (!game_inst) return false;
    uint64_t local_players = drv::read64(game_inst + cfg::offs::LocalPlayers);
    if (!local_players) return false;
    uint64_t local_player = drv::read64(local_players);
    if (!local_player) return false;
    uint64_t controller = drv::read64(local_player + cfg::offs::PlayerController);
    if (!controller) return false;

    // Local team for filtering
    uint64_t local_pawn = drv::read64(controller + 0x0298); // Controller->Pawn (verify)
    local_team = 0;
    if (local_pawn) {
        uint64_t lps = drv::read64(local_pawn + cfg::offs::PlayerState);
        if (lps) local_team = drv::read32(lps + cfg::offs::TeamComponent);
    }

    // Camera for view matrix
    uint64_t cam_mgr = drv::read64(controller + cfg::offs::PlayerCameraManager);
    if (!cam_mgr) return false;
    uint64_t pov = cam_mgr + cfg::offs::CameraCachePOV + cfg::offs::ViewMatrixOff;
    drv::read(pov, &view_matrix, sizeof(Matrix4));
    camera_out = pov;

    // Actor list
    uint64_t level = drv::read64(world + cfg::offs::PersistentLevel);
    if (!level) return false;
    uint64_t actors = drv::read64(level + cfg::offs::ActorArray);
    int      count  = drv::read32(level + cfg::offs::ActorArray + 8);
    if (!actors || count <= 0 || count > 512) count = 0;

    for (int i = 0; i < count; ++i) {
        uint64_t actor = drv::read64(actors + i * 8);
        if (!actor || actor == local_pawn) continue;

        int health = drv::read32(actor + cfg::offs::Health);
        if (health <= 0 || health > 200) continue;   // dead or not a pawn

        uint64_t ps = drv::read64(actor + cfg::offs::PlayerState);
        if (!ps) continue;                            // filter non-player actors
        int team = drv::read32(ps + cfg::offs::TeamComponent);
        if (team == local_team) continue;

        uint64_t mesh = drv::read64(actor + cfg::offs::Mesh);
        uint64_t root = drv::read64(actor + cfg::offs::RootComponent);
        if (!mesh || !root) continue;

        Player p{};
        p.ptr = actor;
        p.health = health;
        p.team = team;
        p.feet.x = drv::readf(root + cfg::offs::RelativeLocation);
        p.feet.y = drv::readf(root + cfg::offs::RelativeLocation + 4);
        p.feet.z = drv::readf(root + cfg::offs::RelativeLocation + 8);

        // Bones: component-to-world transform + bone array
        FTransform c2w{};
        drv::read(mesh + cfg::offs::ComponentToWorld, &c2w, sizeof(FTransform));
        uint64_t bone_arr = drv::read64(mesh + cfg::offs::BoneArray);
        p.bones.reserve(80);
        if (bone_arr) {
            for (int b = 0; b < 80; ++b) {            // 80 bone slots covers the rig
                FTransform bt{};
                if (!drv::read(bone_arr + (uint64_t)b * sizeof(FTransform), &bt, sizeof(FTransform)))
                    break;
                p.bones.push_back(bone_world(c2w, bt));
            }
        }

        // Head: prefer exact bone index, fall back to feet + 155cm
        if (cfg::head_bone < p.bones.size())
            p.head = p.bones[(size_t)cfg::head_bone];
        else
            p.head = { p.feet.x, p.feet.y, p.feet.z + 155.0f };

        p.valid = true;
        out.push_back(p);
    }
    return true;
}

// ============================================================================
// AIMBOT (SendInput; not hooked, so Vanguard usermode input hooks are avoided)
// ============================================================================
static void move_mouse(float dx, float dy, float smoothing) {
    if (smoothing < 1.0f) smoothing = 1.0f;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = (LONG)(dx / smoothing);
    in.mi.dy = (LONG)(dy / smoothing);
    SendInput(1, &in, sizeof(INPUT));
}

static void do_aimbot(const std::vector<Player>& players, float sw, float sh) {
    if (!cfg::aimbot_enabled || !(GetAsyncKeyState(cfg::aim_key) & 0x8000)) return;

    Player* best = nullptr;
    float best_dist = cfg::aim_fov_px;
    Vec2 center{ sw * 0.5f, sh * 0.5f };

    for (const auto& p : players) {
        if (!p.valid) continue;
        Vec2 screen;
        if (!world_to_screen(p.head, screen, sw, sh)) continue;
        Vec2 d = { screen.x - center.x, screen.y - center.y };
        float mag = sqrtf(d.x * d.x + d.y * d.y);
        if (mag < best_dist) { best_dist = mag; best = (Player*)&p; }
    }
    if (best)
        move_mouse((best->head.x ? 0 : 0) + (0), 0, cfg::aim_smooth); // placeholder no-op, replaced below
}

// Proper version — needs screen-space delta; kept separate for clarity:
static void do_aimbot2(const std::vector<Player>& players, float sw, float sh) {
    if (!cfg::aimbot_enabled || !(GetAsyncKeyState(cfg::aim_key) & 0x8000)) return;

    const Player* best = nullptr;
    float best_dist = cfg::aim_fov_px;
    Vec2 center{ sw * 0.5f, sh * 0.5f };
    Vec2 best_screen{};

    for (const auto& p : players) {
        if (!p.valid) continue;
        Vec2 screen;
        if (!world_to_screen(p.head, screen, sw, sh)) continue;
        Vec2 d = { screen.x - center.x, screen.y - center.y };
        float mag = sqrtf(d.x * d.x + d.y * d.y);
        if (mag < best_dist) { best_dist = mag; best = &p; best_screen = screen; }
    }
    if (best) {
        Vec2 delta = { best_screen.x - center.x, best_screen.y - center.y };
        move_mouse(delta.x, delta.y, cfg::aim_smooth);
    }
}

// ============================================================================
// D2D OVERLAY
// ============================================================================
static ID2D1Factory*        factory = nullptr;
static ID2D1HwndRenderTarget* rt    = nullptr;
static ID2D1SolidColorBrush* brush  = nullptr;
static ID2D1SolidColorBrush* red   = nullptr;
static ID2D1SolidColorBrush* green = nullptr;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, msg, w, l);
}

static void draw_line(Vec2 a, Vec2 b, ID2D1SolidColorBrush* c, float w) {
    rt->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), c, w);
}

// Bone connectivity: head(8)-neck/spine-pelvis, shoulders-elbows-hands, hips-knees-feet.
// Indices are per-model; adjust if the rig differs.
static const int skeleton_pairs[][2] = {
    {8, 7}, {7, 6}, {6, 5}, {5, 4}, {4, 3},           // spine chain
    {7, 11}, {11, 12}, {12, 13},                       // right arm
    {7, 33}, {33, 34}, {34, 35},                       // left arm
    {3, 52}, {52, 56}, {56, 60},                       // right leg
    {3, 58}, {58, 62}, {62, 66},                       // left leg
};

static void draw_esp(const std::vector<Player>& players, float sw, float sh) {
    for (const auto& p : players) {
        if (!p.valid) continue;

        // Box from feet to head
        Vec2 feet_s, head_s;
        if (!world_to_screen(p.feet, feet_s, sw, sh)) continue;
        if (!world_to_screen(p.head,  head_s,  sw, sh)) continue;
        float box_h = feet_s.y - head_s.y;
        if (box_h < 5.0f) continue;
        float box_w = box_h * 0.45f;

        D2D1_RECT_F rect = D2D1::RectF(head_s.x - box_w * 0.5f, head_s.y,
                                       head_s.x + box_w * 0.5f, feet_s.y);
        ID2D1SolidColorBrush* col = (p.health < 40) ? red : green;
        rt->DrawRectangle(&rect, col, cfg::box_thickness);

        if (cfg::skeleton_esp && p.bones.size() >= 70) {
            for (const auto& pr : skeleton_pairs) {
                if (pr[1] >= (int)p.bones.size()) continue;
                Vec2 a2, b2;
                if (!world_to_screen(p.bones[pr[0]], a2, sw, sh)) continue;
                if (!world_to_screen(p.bones[pr[1]], b2, sw, sh)) continue;
                draw_line(a2, b2, col, 1.0f);
            }
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    // Find VALORANT process
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (wcscmp(pe.szExeFile, L"VALORANT-Win64-Shipping.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!pid) { MessageBoxW(nullptr, L"VALORANT not running.", L"Error", MB_OK); return 1; }

    if (!drv::init(pid)) { MessageBoxW(nullptr, L"Driver unavailable (is it loaded?)", L"Error", MB_OK); return 1; }

    // Overlay window
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.lpszClassName = L"vc_overlay";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);

    RECT desk; GetClientRect(GetDesktopWindow(), &desk);
    float sw = (float)(desk.right), sh = (float)(desk.bottom);

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 L"vc_overlay", L"", WS_POPUP, 0, 0,
                                 (int)sw, (int)sh, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    MARGINS m{ -1 }; DwmExtendFrameIntoClientArea(hwnd, &m);
    ShowWindow(hwnd, SW_SHOW);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
    factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU((UINT32)sw, (UINT32)sh),
            D2D1_PRESENT_OPTIONS_IMMEDIATELY), &rt);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 0, 0, 1), &red);
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 1, 0, 1), &green);

    std::vector<Player> players;
    int local_team = 0;
    uint64_t cam = 0;

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (GetAsyncKeyState(VK_END) & 1) break;   // PANIC: END key

        if (read_players(players, local_team, cam)) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            if (cfg::esp_enabled) draw_esp(players, sw, sh);
            do_aimbot2(players, sw, sh);
            rt->EndDraw();
        }
        Sleep(2);   // ~500 FPS cap on the loop; tune as needed
    }
done:
    if (rt) rt->Release();
    if (factory) factory->Release();
    if (drv::handle && drv::handle != INVALID_HANDLE_VALUE) CloseHandle(drv::handle);
    return 0;
}
