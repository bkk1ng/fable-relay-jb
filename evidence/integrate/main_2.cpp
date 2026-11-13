// main.cpp
// Entry point: reads entity list via \\.\\Kmem driver, projects heads to screen,
// picks the nearest entity within a 240 px cone of screen-centre, feeds it to
// smooth::State (SendInput backend), and draws per-entity boxes via the overlay.
//
// Build (MSVC x64):
//   cl /std:c++17 /O2 /W3 /DUNICODE /D_UNICODE main.cpp proj.cpp smooth.cpp modules.cpp overlay.cpp
//       /link user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib psapi.lib
//
// The driver is optional: if CreateFileW fails the loop runs with zero entities.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations from companion translation units
// ---------------------------------------------------------------------------

// proj.cpp
bool Project(const float vp[16], float vpW, float vpH,
             float wx, float wy, float wz,
             float& sx, float& sy, float& depth);

struct CameraIntrinsics { float fovYDeg, aspect; };
void BuildViewProjection(const CameraIntrinsics& cam,
                         const float view[16], float out[16]);

// smooth.cpp
namespace smooth {
    struct Config {
        float  max_step;
        float  settle_radius;
        float  ease_exponent;
        void (*backend)(float dx, float dy, void*);
        void*  user_ctx;
    };
    struct State {
        float cur_x, cur_y;
        float tgt_x, tgt_y;
        bool  moving;
    };
    State make_state(float x, float y);
    void  set_target(State& s, float tx, float ty);
    bool  tick(State& s, const Config& cfg);
}

// modules.cpp
uintptr_t ResolveModule(DWORD pid, const wchar_t* name);

// overlay.cpp
namespace Overlay {
    bool Init(HINSTANCE hInstance);
    void Shutdown();
    void BeginFrame();
    void EndFrame();
    void SetTargetFps(float fps);
    // colour: 0xAARRGGBB
    void DrawRect(float x, float y, float w, float h,
                  uint32_t colour, bool filled);
    void DrawText(float x, float y, const wchar_t* text, uint32_t colour);
    float Width();
    float Height();
}

// ---------------------------------------------------------------------------
// Driver interface
// ---------------------------------------------------------------------------

// IOCTL code – must match the driver's definition.
#define IOCTL_KMEM_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
struct KmemReadRequest {
    ULONG_PTR ProcessId;
    ULONG_PTR RemoteAddress;
    SIZE_T    DataSize;
};
#pragma pack(pop)

// Reads DataSize bytes from RemoteAddress inside ProcessId.
// Returns true on success.
static bool KmemRead(HANDLE hDriver, DWORD pid,
                     uintptr_t remoteAddr, void* localBuf, SIZE_T size)
{
    if (hDriver == INVALID_HANDLE_VALUE || !localBuf || size == 0)
        return false;

    // Input buffer: request header followed by a placeholder for the returned bytes.
    // We allocate one flat buffer: [KmemReadRequest][data bytes].
    const SIZE_T inBufSize  = sizeof(KmemReadRequest) + size;
    const SIZE_T outBufSize = size;

    std::vector<BYTE> inBuf(inBufSize,  0);
    std::vector<BYTE> outBuf(outBufSize, 0);

    KmemReadRequest* req = reinterpret_cast<KmemReadRequest*>(inBuf.data());
    req->ProcessId    = static_cast<ULONG_PTR>(pid);
    req->RemoteAddress = static_cast<ULONG_PTR>(remoteAddr);
    req->DataSize     = size;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(hDriver,
                              IOCTL_KMEM_READ,
                              inBuf.data(),  static_cast<DWORD>(inBufSize),
                              outBuf.data(), static_cast<DWORD>(outBufSize),
                              &bytesReturned,
                              nullptr);
    if (!ok || bytesReturned < static_cast<DWORD>(size))
        return false;

    std::memcpy(localBuf, outBuf.data(), size);
    return true;
}

// ---------------------------------------------------------------------------
// Game data layout  (adjust offsets / structs to match the target)
// ---------------------------------------------------------------------------

// Position of an entity in world space + a liveness flag.
struct GameEntity {
    float    world[3];   // { x, y, z }  feet position
    float    headOffset; // height added to world[1] to reach the head
    bool     alive;
    uint8_t  _pad[3];
};

// Minimal camera block read from the game.
struct GameCamera {
    float viewMatrix[16]; // row-major 4×4
    float fovYDeg;
    float aspect;
};

// Fixed game-side addresses – replace with real offsets.
static constexpr uintptr_t kEntityListAddr  = 0x0;   // base + offset to entity array
static constexpr int        kMaxEntities     = 64;
static constexpr uintptr_t kCameraAddr       = 0x0;   // base + offset to camera block
static constexpr SIZE_T    kEntityStride     = sizeof(GameEntity);

// ---------------------------------------------------------------------------
// SendInput backend for smooth::
// ---------------------------------------------------------------------------

static void SendInputBackend(float dx, float dy, void* /*ctx*/)
{
    INPUT inp{};
    inp.type           = INPUT_MOUSE;
    inp.mi.dwFlags     = MOUSEEVENTF_MOVE;
    inp.mi.dx          = static_cast<LONG>(dx);
    inp.mi.dy          = static_cast<LONG>(dy);
    SendInput(1, &inp, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // --- Open driver (non-fatal) ---
    HANDLE hDriver = CreateFileW(L"\\\\.\\Kmem",
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    const bool driverOk = (hDriver != INVALID_HANDLE_VALUE);
    if (!driverOk) {
        OutputDebugStringW(L"[main] Kmem driver not found – running without memory reads.\n");
    }

    // --- Find target process (simple window-title approach) ---
    HWND hTarget = FindWindowW(nullptr, L"TargetGame");   // replace with real title
    DWORD targetPid = 0;
    if (hTarget)
        GetWindowThreadProcessId(hTarget, &targetPid);

    // Resolve game module base so we can build absolute addresses.
    uintptr_t gameBase = 0;
    if (driverOk && targetPid)
        gameBase = ResolveModule(targetPid, L"game.exe");  // replace with real module name

    // --- Overlay ---
    if (!Overlay::Init(hInstance)) {
        if (driverOk) CloseHandle(hDriver);
        return 1;
    }
    Overlay::SetTargetFps(144.0f);

    const float vpW = Overlay::Width();
    const float vpH = Overlay::Height();
    const float cxScreen = vpW * 0.5f;
    const float cyScreen = vpH * 0.5f;
    constexpr float kConePx = 240.0f;

    // --- Smooth-aim state ---
    smooth::Config aimCfg{};
    aimCfg.max_step      = 18.0f;
    aimCfg.settle_radius = 1.2f;
    aimCfg.ease_exponent = 2.0f;
    aimCfg.backend       = SendInputBackend;
    aimCfg.user_ctx      = nullptr;

    smooth::State aimState = smooth::make_state(cxScreen, cyScreen);

    // --- Main loop ---
    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // --- Read camera ---
        GameCamera cam{};
        bool camOk = false;
        if (driverOk && targetPid && gameBase) {
            camOk = KmemRead(hDriver, targetPid,
                             gameBase + kCameraAddr,
                             &cam, sizeof(cam));
        }

        // Build VP matrix (falls back to identity-ish if camera unreadable).
        float vp[16]{};
        if (camOk && cam.fovYDeg > 1.0f && cam.aspect > 0.0f) {
            CameraIntrinsics intrinsics{ cam.fovYDeg, cam.aspect };
            BuildViewProjection(intrinsics, cam.viewMatrix, vp);
        }

        // --- Read entity list ---
        GameEntity entities[kMaxEntities]{};
        int entityCount = 0;

        if (driverOk && targetPid && gameBase && camOk) {
            // Read whole array in one call for efficiency.
            KmemRead(hDriver, targetPid,
                     gameBase + kEntityListAddr,
                     entities, sizeof(entities));
            for (int i = 0; i < kMaxEntities; ++i)
                if (entities[i].alive) ++entityCount;
        }

        // --- Project + pick best target ---
        // bestIdx: index into entities[], -1 when no valid target.
        int   bestIdx       = -1;
        float bestDist2     = kConePx * kConePx;
        float bestSx = 0.f, bestSy = 0.f;

        // Per-entity screen data for drawing.
        struct EntityScreen {
            float fx, fy;        // feet screen pos
            float hx, hy;        // head screen pos
            float depth;
            bool  valid;
        };
        EntityScreen escreen[kMaxEntities]{};

        for (int i = 0; i < kMaxEntities; ++i) {
            if (!entities[i].alive) continue;

            float wx = entities[i].world[0];
            float wy = entities[i].world[1];
            float wz = entities[i].world[2];
            float headY = wy + entities[i].headOffset;

            float fsx, fsy, fdepth;
            float hsx, hsy, hdepth;

            bool feetOk = Project(vp, vpW, vpH, wx, wy,     wz, fsx, fsy, fdepth);
            bool headOk = Project(vp, vpW, vpH, wx, headY,  wz, hsx, hsy, hdepth);

            // Skip entities behind the camera (depth <= 0 in D3D NDC convention).
            if (!feetOk || !headOk || fdepth <= 0.0f || hdepth <= 0.0f)
                continue;

            escreen[i] = { fsx, fsy, hsx, hsy, hdepth, true };

            float d2 = dist2(hsx, hsy, cxScreen, cyScreen);
            if (d2 < bestDist2) {
                bestDist2 = d2;
                bestIdx   = i;
                bestSx    = hsx;
                bestSy    = hsy;
            }
        }

        // --- Feed smooth aim ---
        if (bestIdx >= 0) {
            smooth::set_target(aimState, bestSx, bestSy);
        }
        // Always tick so the cursor settles when no target is in cone.
        smooth::tick(aimState, aimCfg);

        // --- Draw ---
        Overlay::BeginFrame();

        for (int i = 0; i < kMaxEntities; ++i) {
            if (!escreen[i].valid) continue;

            const float hx = escreen[i].hx;
            const float hy = escreen[i].hy;
            const float fx = escreen[i].fx;
            const float fy = escreen[i].fy;

            // Derive a simple 2-D box from head and feet screen positions.
            float boxH   = fy - hy;
            if (boxH < 2.0f) boxH = 2.0f;
            float boxW   = boxH * 0.4f;
            float boxX   = hx - boxW * 0.5f;
            float boxY   = hy;

            bool isTarget = (i == bestIdx);
            uint32_t col = isTarget ? 0xFFFF3030u : 0xFF30FF30u;

            Overlay::DrawRect(boxX, boxY, boxW, boxH, col, false);

            if (isTarget) {
                Overlay::DrawText(boxX, boxY - 14.0f, L"TARGET", 0xFFFFFF30u);
            }
        }

        // Draw cone circle as a rough visual hint (using a hairline rect approximation).
        Overlay::DrawRect(cxScreen - kConePx, cyScreen - kConePx,
                          kConePx * 2.0f, kConePx * 2.0f,
                          0x33FFFFFF, false);

        if (entityCount == 0 && driverOk) {
            Overlay::DrawText(10.0f, 10.0f, L"No live entities", 0xFFAAAAAA);
        }
        if (!driverOk) {
            Overlay::DrawText(10.0f, 10.0f, L"Driver offline", 0xFFFF8040);
        }

        Overlay::EndFrame();
    }

    // --- Cleanup ---
    Overlay::Shutdown();
    if (driverOk) CloseHandle(hDriver);
    return 0;
}
