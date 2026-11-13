// main.cpp
// Entry point: reads entity list via \\.\\Kmem driver, projects heads to screen,
// picks the nearest entity within a 240 px cone from screen centre, feeds it to
// smooth::State with a SendInput backend, and draws per-entity boxes with the overlay.
//
// Build (MSVC x64):
//   cl /std:c++17 /O2 /W3 /DUNICODE /D_UNICODE main.cpp proj.cpp smooth.cpp modules.cpp overlay.cpp ^
//      /link user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib
//
// All four companion TUs are #included as implementation units so this file links standalone.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Forward-declare the APIs exported by the companion translation units.
// (The real declarations live in those files; we replicate the minimum here so
// main.cpp is self-contained when the TUs are compiled separately.)
// ---------------------------------------------------------------------------

// --- real API of the four primitives (see evidence/prims/files) ---
bool Project(const float vp[16], float vpW, float vpH,
             float wx, float wy, float wz,
             float& sx, float& sy, float& depth);

struct CameraIntrinsics { float fovYDeg, aspect; };
void BuildViewProjection(const CameraIntrinsics& cam, const float view[16], float out[16],
                         float zNear = 0.1f, float zFar = 1000.0f);

namespace smooth {
    using DeltaFn = void(*)(float dx, float dy, void* ctx);
    struct Config {
        float   max_step      = 16.0f;
        float   settle_radius = 1.0f;
        float   ease_exponent = 2.0f;
        DeltaFn backend       = nullptr;
        void*   user_ctx      = nullptr;
    };
    struct State { float cur_x, cur_y, tgt_x, tgt_y, sub_x, sub_y; bool has_target; };
    State make_state(float x, float y);
    void  set_target(State& s, float tx, float ty);
    void  reset(State& s);
    bool  tick(State& s, const Config& cfg);
}

struct Color {
    float r, g, b, a;
    constexpr Color(float R, float G, float B, float A = 1.f) : r(R), g(G), b(B), a(A) {}
};

struct ResolvedModule { uintptr_t base; size_t size; };
ResolvedModule ResolveModuleByName(unsigned long pid, const std::wstring& name);

namespace Overlay {
    bool  Init(HINSTANCE hInstance, float targetFps);
    void  Shutdown();
    void  BeginFrame();
    void  EndFrame();
    void  SetTargetFps(float fps);
    void  DrawRect(float x, float y, float w, float h, Color c, bool filled);
    void  DrawLine(float x0, float y0, float x1, float y1, Color c, float thickness);
    void  DrawTextA(float x, float y, const wchar_t* text, Color c, const wchar_t* font, int size);
    float ScreenWidth();
    float ScreenHeight();
}
#define ResolveModule(p, n) (ResolveModuleByName((p), std::wstring(n)).base)

// ---------------------------------------------------------------------------
// Kmem driver contract
// ---------------------------------------------------------------------------

static const wchar_t* KMEM_DEVICE_PATH = L"\\\\.\\Kmem";

// CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS) is a common
// convention; adjust if your driver uses a different code.
#define IOCTL_KMEM_READ  CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
struct KmemReadRequest {
    ULONG_PTR ProcessId;
    ULONG_PTR RemoteAddress;
    SIZE_T    DataSize;
};
#pragma pack(pop)

// Reads DataSize bytes from RemoteAddress in process ProcessId through the
// Kmem device.  Returns true on success.
static bool KmemRead(HANDLE hDevice, ULONG_PTR pid, ULONG_PTR remoteAddr,
                     void* outBuf, SIZE_T size)
{
    // Input  = header struct
    // Output = header struct followed by the returned bytes
    const SIZE_T inputSize  = sizeof(KmemReadRequest);
    const SIZE_T outputSize = sizeof(KmemReadRequest) + size;

    std::vector<BYTE> outVec(outputSize, 0);
    KmemReadRequest req{};
    req.ProcessId     = pid;
    req.RemoteAddress = remoteAddr;
    req.DataSize      = size;

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_KMEM_READ,
                              &req, static_cast<DWORD>(inputSize),
                              outVec.data(), static_cast<DWORD>(outputSize),
                              &returned, nullptr);
    if (!ok || returned < static_cast<DWORD>(sizeof(KmemReadRequest) + size))
        return false;

    std::memcpy(outBuf, outVec.data() + sizeof(KmemReadRequest), size);
    return true;
}

// ---------------------------------------------------------------------------
// Game-specific layout (edit to match the actual game build)
// ---------------------------------------------------------------------------

// Adjust these offsets to match the target game's data layout.
namespace GameLayout {
    // Offset from module base to the pointer that holds the entity list address.
    static constexpr uintptr_t ENTITY_LIST_PTR = 0x01234567;

    // Maximum slots in the entity list.
    static constexpr int MAX_ENTITIES = 256;

    // Stride between entity entries (bytes).
    static constexpr SIZE_T ENTITY_STRIDE = 0x1000;

    // Offsets within a single entity entry.
    static constexpr uintptr_t OFF_HEALTH     = 0x100;  // int32
    static constexpr uintptr_t OFF_POS_X      = 0x120;  // float
    static constexpr uintptr_t OFF_POS_Y      = 0x124;  // float
    static constexpr uintptr_t OFF_POS_Z      = 0x128;  // float
    static constexpr uintptr_t OFF_BOX_HEIGHT = 0x140;  // float — approximate model height
    static constexpr uintptr_t OFF_TEAM       = 0x104;  // int32  — 0=enemy, 1=friendly

    // View matrix (row-major 4×4 floats) and viewport dimensions.
    static constexpr uintptr_t VIEW_MATRIX_PTR = 0x02345678;
    static constexpr uintptr_t VIEWPORT_W_PTR  = 0x023456A0; // float
    static constexpr uintptr_t VIEWPORT_H_PTR  = 0x023456A4; // float

    // Local player team id pointer.
    static constexpr uintptr_t LOCAL_TEAM_PTR  = 0x023456B0; // int32

    // Name of the game module.
    static const wchar_t* MODULE_NAME = L"game.exe";

    // Target process name (used for PID lookup).
    static const wchar_t* PROCESS_NAME = L"game.exe";
}

// ---------------------------------------------------------------------------
// PID lookup by process name
// ---------------------------------------------------------------------------
#include <tlhelp32.h>
static DWORD FindProcessByName(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// ---------------------------------------------------------------------------
// SendInput backend for smooth::
// ---------------------------------------------------------------------------
static void SendInputBackend(float dx, float dy, void* /*ctx*/)
{
    if (dx == 0.0f && dy == 0.0f) return;

    INPUT inp{};
    inp.type           = INPUT_MOUSE;
    inp.mi.dwFlags     = MOUSEEVENTF_MOVE;
    inp.mi.dx          = static_cast<LONG>(dx);
    inp.mi.dy          = static_cast<LONG>(dy);
    SendInput(1, &inp, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Entity snapshot
// ---------------------------------------------------------------------------
struct EntitySnapshot {
    float wx, wy, wz;        // world feet position
    float height;            // approximate model height in world units
    float headWx, headWy, headWz; // derived head position
    float sx, sy;            // screen feet
    float headSx, headSy;   // screen head
    float depth;
    bool  valid;             // passed all culling tests
    int   team;
};

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // --- Open Kmem device ---
    HANDLE hKmem = CreateFileW(KMEM_DEVICE_PATH,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hKmem == INVALID_HANDLE_VALUE) {
        MessageBoxW(nullptr,
                    L"Could not open \\Device\\Kmem.\n"
                    L"Make sure the driver is loaded and you are running as Administrator.",
                    L"Driver missing", MB_ICONERROR | MB_OK);
        return 1;
    }

    // --- Find target process ---
    DWORD targetPid = FindProcessByName(GameLayout::PROCESS_NAME);
    if (targetPid == 0) {
        MessageBoxW(nullptr, L"Target process not found.", L"Error", MB_ICONERROR | MB_OK);
        CloseHandle(hKmem);
        return 1;
    }

    // --- Resolve game module base ---
    uintptr_t moduleBase = ResolveModule(targetPid, GameLayout::MODULE_NAME);
    if (moduleBase == 0) {
        MessageBoxW(nullptr, L"Could not resolve game module.", L"Error", MB_ICONERROR | MB_OK);
        CloseHandle(hKmem);
        return 1;
    }

    // --- Overlay ---
    if (!Overlay::Init(hInstance, 240.0f)) {
        MessageBoxW(nullptr, L"Overlay initialisation failed.", L"Error", MB_ICONERROR | MB_OK);
        CloseHandle(hKmem);
        return 1;
    }
    Overlay::SetTargetFps(60.0f);

    const float scrW = Overlay::ScreenWidth();
    const float scrH = Overlay::ScreenHeight();
    const float cxScreen = scrW * 0.5f;
    const float cyScreen = scrH * 0.5f;

    // --- Smooth state ---
    smooth::State smoothState = smooth::make_state(cxScreen, cyScreen);
    smooth::Config smoothCfg{};
    smoothCfg.max_step      = 20.0f;
    smoothCfg.settle_radius = 1.5f;
    smoothCfg.ease_exponent = 2.0f;
    smoothCfg.backend       = SendInputBackend;
    smoothCfg.user_ctx      = nullptr;

    // Cone radius in pixels (half-angle).
    static constexpr float CONE_RADIUS = 240.0f;

    // --- Main loop ---
    MSG msg{};
    while (true) {
        // Drain the Windows message queue.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // ---- Read view matrix ----
        uintptr_t viewMatAddr = 0;
        if (!KmemRead(hKmem, targetPid,
                      moduleBase + GameLayout::VIEW_MATRIX_PTR,
                      &viewMatAddr, sizeof(viewMatAddr)))
            continue;

        float vp[16]{};
        if (!KmemRead(hKmem, targetPid, viewMatAddr, vp, sizeof(vp)))
            continue;

        float vpW = scrW, vpH = scrH;
        KmemRead(hKmem, targetPid,
                 moduleBase + GameLayout::VIEWPORT_W_PTR, &vpW, sizeof(float));
        KmemRead(hKmem, targetPid,
                 moduleBase + GameLayout::VIEWPORT_H_PTR, &vpH, sizeof(float));
        if (vpW < 1.0f) vpW = scrW;
        if (vpH < 1.0f) vpH = scrH;

        // ---- Read local player team ----
        int localTeam = -1;
        KmemRead(hKmem, targetPid,
                 moduleBase + GameLayout::LOCAL_TEAM_PTR, &localTeam, sizeof(int));

        // ---- Read entity list pointer ----
        uintptr_t entityListBase = 0;
        if (!KmemRead(hKmem, targetPid,
                      moduleBase + GameLayout::ENTITY_LIST_PTR,
                      &entityListBase, sizeof(entityListBase)))
            continue;

        // ---- Enumerate entities ----
        std::vector<EntitySnapshot> entities;
        entities.reserve(GameLayout::MAX_ENTITIES);

        int aliveCount = 0;
        for (int i = 0; i < GameLayout::MAX_ENTITIES; ++i) {
            uintptr_t base = entityListBase + static_cast<uintptr_t>(i) * GameLayout::ENTITY_STRIDE;

            int32_t health = 0;
            if (!KmemRead(hKmem, targetPid, base + GameLayout::OFF_HEALTH,
                          &health, sizeof(health)))
                continue;
            if (health <= 0) continue;

            int32_t team = 0;
            KmemRead(hKmem, targetPid, base + GameLayout::OFF_TEAM, &team, sizeof(team));
            if (team == localTeam) continue; // skip teammates

            float px = 0, py = 0, pz = 0, bh = 1.8f;
            KmemRead(hKmem, targetPid, base + GameLayout::OFF_POS_X,      &px, sizeof(float));
            KmemRead(hKmem, targetPid, base + GameLayout::OFF_POS_Y,      &py, sizeof(float));
            KmemRead(hKmem, targetPid, base + GameLayout::OFF_POS_Z,      &pz, sizeof(float));
            KmemRead(hKmem, targetPid, base + GameLayout::OFF_BOX_HEIGHT, &bh, sizeof(float));
            if (bh < 0.1f) bh = 1.8f;

            // Feet screen position
            EntitySnapshot e{};
            e.wx = px; e.wy = py; e.wz = pz;
            e.height = bh;
            e.team   = team;

            float d1 = 0.0f;
            bool feetVis = Project(vp, vpW, vpH, px, py, pz, e.sx, e.sy, d1);

            // Head world position (assume Y is up; adjust if Z-up)
            e.headWx = px;
            e.headWy = py + bh;
            e.headWz = pz;
            float d2 = 0.0f;
            bool headVis = Project(vp, vpW, vpH, e.headWx, e.headWy, e.headWz,
                                   e.headSx, e.headSy, d2);

            // Both projections must be in front of the camera
            if (!feetVis || !headVis || d1 <= 0.0f || d2 <= 0.0f) {
                e.valid = false;
            } else {
                e.depth = d1;
                e.valid = true;
                ++aliveCount;
            }
            entities.push_back(e);
        }

        // ---- Aim selection: nearest head to screen centre within 240 px cone ----
        int bestIdx = -1;
        float bestDist = FLT_MAX;

        if (aliveCount > 0) {
            for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                const auto& e = entities[i];
                if (!e.valid) continue;

                float dxS = e.headSx - cxScreen;
                float dyS = e.headSy - cyScreen;
                float dist = std::sqrt(dxS * dxS + dyS * dyS);
                if (dist < CONE_RADIUS && dist < bestDist) {
                    bestDist = dist;
                    bestIdx  = i;
                }
            }
        }

        // ---- Update smooth state ----
        if (bestIdx >= 0) {
            const auto& tgt = entities[bestIdx];
            smooth::set_target(smoothState, tgt.headSx, tgt.headSy);
        }
        // Always tick so the cursor decelerates smoothly when target is lost
        smooth::tick(smoothState, smoothCfg);

        // ---- Overlay drawing ----
        Overlay::BeginFrame();

        for (const auto& e : entities) {
            if (!e.valid) continue;

            // Bounding box from head screen pos to feet screen pos
            float boxTop    = e.headSy;
            float boxBottom = e.sy;
            float boxH      = boxBottom - boxTop;
            if (boxH < 2.0f) boxH = 2.0f;
            float boxW      = boxH * 0.4f; // rough aspect
            float boxLeft   = e.headSx - boxW * 0.5f;

            // Colour: red for selected target, white for others
            float r = 1.0f, g = 1.0f, b = 1.0f, a = 0.85f;
            if (static_cast<int>(&e - entities.data()) == bestIdx) {
                r = 1.0f; g = 0.2f; b = 0.2f;
            }

            Overlay::DrawRect(boxLeft, boxTop, boxW, boxH, Color(r, g, b, a), false);
        }

        // Zero-entity notice (debug overlay, invisible in release if desired)
        if (aliveCount == 0) {
            Overlay::DrawTextA(10.0f, 10.0f, L"No entities", Color(1.0f, 1.0f, 0.0f, 0.7f), L"Consolas", 16);
        }

        Overlay::EndFrame();
    }

cleanup:
    Overlay::Shutdown();
    CloseHandle(hKmem);
    return 0;
}
