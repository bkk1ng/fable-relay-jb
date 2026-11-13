// main.cpp
// Entry point: reads entity list from target process via \\.\\Kmem device,
// projects heads to screen, picks nearest to centre within 240 px cone,
// drives smooth::State with a SendInput backend, and draws boxes via overlay.
//
// Build (MSVC x64):
//   cl /std:c++17 /O2 /W3 /DUNICODE /D_UNICODE ^
//      main.cpp proj.cpp smooth.cpp modules.cpp overlay.cpp ^
//      /link user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib
//
// Requires the Kmem driver to be loaded before launch.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>
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
#include "smooth.cpp"   // header-only; guarded by SMOOTH_CPP_INCLUDED

// modules.cpp
struct ModuleInfo {
    uintptr_t   base;
    std::size_t size;
    std::wstring image_path;
};
struct ResolvedModule { bool found; ModuleInfo info; };
ResolvedModule ResolveModule(DWORD pid, const wchar_t* name);

// overlay.cpp
namespace Overlay {
    bool Init(HINSTANCE hInstance);
    void Shutdown();
    void BeginFrame();
    void EndFrame();
    void SetTargetFps(float fps);
    void DrawRect(float x, float y, float w, float h,
                  float r, float g, float b, float a, bool filled);
    void DrawText(float x, float y, const wchar_t* text,
                  float r, float g, float b, float a);
    float Width();
    float Height();
}

// ---------------------------------------------------------------------------
// Kmem device contract
// ---------------------------------------------------------------------------

#define IOCTL_KMEM_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Fixed header sent with every IOCTL_KMEM_READ request.
#pragma pack(push, 1)
struct KmemRequest {
    ULONG_PTR ProcessId;
    ULONG_PTR RemoteAddress;
    SIZE_T    DataSize;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Game-specific layout (adjust offsets to match the target build)
// ---------------------------------------------------------------------------

// Base address of the game module within the target process.
static constexpr wchar_t kGameModule[] = L"game.exe";

// Offset from module base to the entity list pointer.
static constexpr uintptr_t kOffEntityListPtr  = 0x01D2F3A8;
// Number of entity slots.
static constexpr int        kMaxEntities       = 64;
// Stride between entity records.
static constexpr SIZE_T     kEntityStride      = 0x2C0;
// Offset within an entity record to the alive/valid byte.
static constexpr uintptr_t  kOffAlive          = 0x018;
// Offset to world-space feet position (3× float, XYZ).
static constexpr uintptr_t  kOffFeetPos        = 0x0A8;
// Head is feet + a fixed vertical offset in world units.
static constexpr float      kHeadHeightOffset  = 72.0f;

// Offset from module base to the view-matrix (16× float, row-major).
static constexpr uintptr_t  kOffViewMatrix     = 0x01D30000;

// PID of the target process (replace with dynamic lookup as needed).
static constexpr DWORD      kTargetPid         = 0;   // 0 = resolve at runtime

// ---------------------------------------------------------------------------
// Kmem helpers
// ---------------------------------------------------------------------------

static HANDLE g_hKmem = INVALID_HANDLE_VALUE;

static bool KmemOpen()
{
    g_hKmem = CreateFileW(L"\\\\.\\Kmem",
                          GENERIC_READ | GENERIC_WRITE,
                          0, nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    return g_hKmem != INVALID_HANDLE_VALUE;
}

static void KmemClose()
{
    if (g_hKmem != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hKmem);
        g_hKmem = INVALID_HANDLE_VALUE;
    }
}

// Read `size` bytes from `remoteAddr` in process `pid` into `outBuf`.
// Returns true on success.
static bool KmemRead(DWORD pid, uintptr_t remoteAddr, void* outBuf, SIZE_T size)
{
    if (g_hKmem == INVALID_HANDLE_VALUE || size == 0) return false;

    // Input buffer: request header followed by no extra data.
    KmemRequest req{};
    req.ProcessId     = static_cast<ULONG_PTR>(pid);
    req.RemoteAddress = static_cast<ULONG_PTR>(remoteAddr);
    req.DataSize      = size;

    // Output buffer receives the returned bytes.
    std::vector<BYTE> outBufVec(size);

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(g_hKmem,
                              IOCTL_KMEM_READ,
                              &req,       sizeof(req),
                              outBufVec.data(), static_cast<DWORD>(size),
                              &bytesReturned,
                              nullptr);
    if (!ok || bytesReturned != static_cast<DWORD>(size)) return false;

    std::memcpy(outBuf, outBufVec.data(), size);
    return true;
}

template<typename T>
static bool KmemReadT(DWORD pid, uintptr_t addr, T& out)
{
    return KmemRead(pid, addr, &out, sizeof(T));
}

// ---------------------------------------------------------------------------
// SendInput backend for smooth::State
// ---------------------------------------------------------------------------

static void SendInputBackend(float dx, float dy, void* /*ctx*/)
{
    INPUT in{};
    in.type           = INPUT_MOUSE;
    in.mi.dwFlags     = MOUSEEVENTF_MOVE;
    in.mi.dx          = static_cast<LONG>(dx);
    in.mi.dy          = static_cast<LONG>(dy);
    SendInput(1, &in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float Dist2D(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

// ---------------------------------------------------------------------------
// Entity record (screen-projected snapshot)
// ---------------------------------------------------------------------------

struct EntitySnap {
    float worldX, worldY, worldZ;   // feet position
    float scrX,   scrY;             // projected head (screen)
    float headWorldX, headWorldY, headWorldZ;
    float depth;
    bool  alive;
    bool  onScreen;
};

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // --- Open Kmem driver -----------------------------------------------
    if (!KmemOpen()) {
        MessageBoxW(nullptr,
                    L"Could not open \\\\.\\Kmem.\n"
                    L"Ensure the driver is loaded and retry.",
                    L"Driver missing", MB_ICONERROR | MB_OK);
        return 1;
    }

    // --- Resolve target PID ---------------------------------------------
    DWORD targetPid = kTargetPid;
    if (targetPid == 0) {
        // Walk the snapshot to find the game process by module name.
        // Simple approach: use CreateToolhelp32Snapshot if needed.
        // For brevity we rely on the caller having set kTargetPid.
        // Emit a debug message and fall through; KmemRead will fail
        // gracefully on each call when pid == 0.
    }

    // --- Resolve game module base ----------------------------------------
    uintptr_t gameBase = 0;
    if (targetPid != 0) {
        ResolvedModule rm = ResolveModule(targetPid, kGameModule);
        if (rm.found) gameBase = rm.info.base;
    }

    // --- Overlay init -----------------------------------------------------
    if (!Overlay::Init(hInstance)) {
        KmemClose();
        return 1;
    }
    Overlay::SetTargetFps(144.0f);

    // --- Smooth-move state -----------------------------------------------
    const float vpW = Overlay::Width();
    const float vpH = Overlay::Height();
    const float cxScreen = vpW * 0.5f;
    const float cyScreen = vpH * 0.5f;

    smooth::Config smoothCfg{};
    smoothCfg.max_step      = 18.0f;
    smoothCfg.settle_radius = 1.2f;
    smoothCfg.ease_exponent = 2.2f;
    smoothCfg.backend       = SendInputBackend;
    smoothCfg.user_ctx      = nullptr;

    smooth::State smoothState = smooth::make_state(cxScreen, cyScreen);

    // --- Main loop -------------------------------------------------------
    MSG msg{};
    while (true) {
        // Drain the Windows message queue.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // ---- Read view matrix -------------------------------------------
        float vp[16]{};
        bool  hasVP = false;
        if (gameBase != 0) {
            hasVP = KmemRead(targetPid,
                             gameBase + kOffViewMatrix,
                             vp, sizeof(vp));
        }

        // ---- Read entity list pointer -----------------------------------
        uintptr_t entityListBase = 0;
        if (hasVP && gameBase != 0) {
            uintptr_t ptrAddr = gameBase + kOffEntityListPtr;
            KmemReadT(targetPid, ptrAddr, entityListBase);
        }

        // ---- Project entities -------------------------------------------
        std::vector<EntitySnap> snaps;
        snaps.reserve(kMaxEntities);

        if (entityListBase != 0 && hasVP) {
            for (int i = 0; i < kMaxEntities; ++i) {
                uintptr_t entBase = entityListBase + static_cast<uintptr_t>(i) * kEntityStride;

                BYTE alive = 0;
                if (!KmemReadT(targetPid, entBase + kOffAlive, alive) || !alive)
                    continue;

                float feet[3]{};
                if (!KmemRead(targetPid, entBase + kOffFeetPos, feet, sizeof(feet)))
                    continue;

                EntitySnap s{};
                s.worldX = feet[0];
                s.worldY = feet[1];
                s.worldZ = feet[2];
                s.alive  = true;

                // Head is feet + vertical offset (assume Z-up; adjust axis if needed).
                s.headWorldX = feet[0];
                s.headWorldY = feet[1];
                s.headWorldZ = feet[2] + kHeadHeightOffset;

                float sx{}, sy{}, depth{};
                bool onScreen = Project(vp, vpW, vpH,
                                        s.headWorldX, s.headWorldY, s.headWorldZ,
                                        sx, sy, depth);

                // Reject entities behind the camera (depth <= 0 in D3D NDC).
                if (!onScreen || depth <= 0.0f) continue;

                s.scrX     = sx;
                s.scrY     = sy;
                s.depth    = depth;
                s.onScreen = true;

                snaps.push_back(s);
            }
        }

        // ---- Pick the target within the 240 px cone ---------------------
        constexpr float kConeRadius = 240.0f;

        const EntitySnap* best      = nullptr;
        float             bestDist  = kConeRadius + 1.0f;

        for (const auto& s : snaps) {
            float d = Dist2D(s.scrX, s.scrY, cxScreen, cyScreen);
            if (d < kConeRadius && d < bestDist) {
                bestDist = d;
                best     = &s;
            }
        }

        // ---- Feed target to smoother ------------------------------------
        if (best) {
            smooth::set_target(smoothState, best->scrX, best->scrY);
        }
        smooth::tick(smoothState, smoothCfg);

        // ---- Draw -------------------------------------------------------
        Overlay::BeginFrame();

        // Draw a bounding box for every live, on-screen entity.
        for (const auto& s : snaps) {
            // Project feet for the bottom of the box.
            float fsx{}, fsy{}, fdepth{};
            bool feetOnScreen = Project(vp, vpW, vpH,
                                        s.worldX, s.worldY, s.worldZ,
                                        fsx, fsy, fdepth);

            if (!feetOnScreen || fdepth <= 0.0f) continue;

            float boxTop    = s.scrY;    // head y
            float boxBottom = fsy;       // feet y
            if (boxTop > boxBottom) std::swap(boxTop, boxBottom);

            float boxH = boxBottom - boxTop;
            if (boxH < 2.0f) boxH = 2.0f;
            float boxW = boxH * 0.4f;   // approximate width from height ratio

            float boxX = s.scrX - boxW * 0.5f;

            // Colour: green for selected target, white otherwise.
            bool isTarget = (best && &s == best);
            float r = isTarget ? 0.0f : 1.0f;
            float g = 1.0f;
            float b = isTarget ? 0.0f : 1.0f;

            Overlay::DrawRect(boxX, boxTop, boxW, boxH, r, g, b, 1.0f, false);
        }

        // Zero-entity notice (debug; disable for release builds).
        if (snaps.empty()) {
            Overlay::DrawText(10.0f, 10.0f, L"No entities", 1.0f, 0.4f, 0.4f, 1.0f);
        }

        Overlay::EndFrame();
    }

done:
    smooth::reset(smoothState);
    Overlay::Shutdown();
    KmemClose();
    return static_cast<int>(msg.wParam);
}
