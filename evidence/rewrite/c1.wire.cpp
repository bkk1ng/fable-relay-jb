// wire.cpp – entity-read / projection / smoothing wiring
// Ties together the three modules and drives the cursor toward the nearest
// projected entity inside a cone.  Build standalone:
//   cl /EHsc /std:c++17 /DSMOKE_TEST wire.cpp /link user32.lib

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

// ---------------------------------------------------------------------------
// Shared primitive types
// ---------------------------------------------------------------------------

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

// ---------------------------------------------------------------------------
// Module: entity reader
// ---------------------------------------------------------------------------
// Reads a list of world-space entity positions through an opaque device handle.
// In production the device handle wraps whatever memory-access primitive is in
// use; here it is typed void* so the wiring layer stays decoupled.

struct EntityList {
    std::vector<Vec3> positions;   // world-space
    std::vector<std::uint64_t> ids;
};

// ReadEntityPositions
//   device     – opaque handle passed through from the caller
//   baseAddr   – module base or relevant base address for offset arithmetic
//   maxEnts    – upper bound on how many entities to read in one pass
//   out        – filled on success; unchanged on failure
// Returns true if at least one entity was read.
bool ReadEntityPositions(void* device, std::uintptr_t baseAddr,
                         std::size_t maxEnts, EntityList& out)
{
    out.positions.clear();
    out.ids.clear();
    if (!device || maxEnts == 0) return false;

    // -----------------------------------------------------------------------
    // Offset layout (adjust to match the target's memory map):
    //   baseAddr + 0x00000 → entity array pointer
    //   each slot           → 0x00  entity id   (uint64)
    //                       → 0x08  x (float)
    //                       → 0x0C  y (float)
    //                       → 0x10  z (float)
    // -----------------------------------------------------------------------
    constexpr std::uintptr_t kEntityArrayOff = 0x00000;
    constexpr std::size_t    kSlotStride     = 0x20;
    constexpr std::uintptr_t kIdOff          = 0x00;
    constexpr std::uintptr_t kPosXOff        = 0x08;

    const auto* h = static_cast<const std::uint8_t*>(device);

    std::uintptr_t arrayPtr = 0;
    std::memcpy(&arrayPtr, h + baseAddr + kEntityArrayOff, sizeof(arrayPtr));
    if (!arrayPtr) return false;

    out.positions.reserve(maxEnts);
    out.ids.reserve(maxEnts);

    for (std::size_t i = 0; i < maxEnts; ++i) {
        const auto* slot = h + arrayPtr + i * kSlotStride;

        std::uint64_t id = 0;
        std::memcpy(&id, slot + kIdOff, sizeof(id));
        if (id == 0) continue;   // empty slot

        Vec3 pos{};
        std::memcpy(&pos.x, slot + kPosXOff,       sizeof(float));
        std::memcpy(&pos.y, slot + kPosXOff + 0x04, sizeof(float));
        std::memcpy(&pos.z, slot + kPosXOff + 0x08, sizeof(float));

        out.positions.push_back(pos);
        out.ids.push_back(id);
    }
    return !out.positions.empty();
}

// ---------------------------------------------------------------------------
// Module: world-to-screen projection
// ---------------------------------------------------------------------------
// ViewMatrix is column-major (D3D convention): vM[col][row].
// Returns false when the point is behind the camera.

struct ViewMatrix { float m[4][4]; };

bool WorldToScreen(const Vec3& world, const ViewMatrix& vm,
                   int screenW, int screenH, Vec2& screenOut)
{
    // Transform to clip space
    float clipX = world.x * vm.m[0][0] + world.y * vm.m[1][0] +
                  world.z * vm.m[2][0] + vm.m[3][0];
    float clipY = world.x * vm.m[0][1] + world.y * vm.m[1][1] +
                  world.z * vm.m[2][1] + vm.m[3][1];
    float clipW = world.x * vm.m[0][3] + world.y * vm.m[1][3] +
                  world.z * vm.m[2][3] + vm.m[3][3];

    if (clipW < 0.001f) return false;   // behind camera

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    screenOut.x = (ndcX + 1.0f) * 0.5f * static_cast<float>(screenW);
    screenOut.y = (1.0f - ndcY) * 0.5f * static_cast<float>(screenH);
    return true;
}

// ---------------------------------------------------------------------------
// Module: cursor smoothing
// ---------------------------------------------------------------------------
// Blends current screen position toward target over successive calls.
// smoothFactor ∈ (0, 1]: 1.0 = instant snap, lower = slower approach.

struct SmoothState {
    Vec2  current{};
    float smoothFactor{ 0.1f };
};

Vec2 SmoothToward(SmoothState& state, const Vec2& target)
{
    state.current.x += (target.x - state.current.x) * state.smoothFactor;
    state.current.y += (target.y - state.current.y) * state.smoothFactor;
    return state.current;
}

// ---------------------------------------------------------------------------
// Wiring layer
// ---------------------------------------------------------------------------

// Delta callback: called once per frame with the final (dx, dy) to apply.
using DeltaCallback = std::function<void(float dx, float dy)>;

struct WireConfig {
    // Read params
    void*          device    { nullptr };
    std::uintptr_t baseAddr  { 0 };
    std::size_t    maxEnts   { 128 };

    // Projection params
    ViewMatrix     viewMatrix{};
    int            screenW   { 1920 };
    int            screenH   { 1080 };

    // Cone params – half-angle in degrees measured from screen-centre
    float          coneHalfDeg{ 20.0f };

    // Smoothing
    float          smoothFactor{ 0.1f };

    // Delta callback (required)
    DeltaCallback  onDelta;
};

// WireFrame
//   Runs one full entity-read → project → cone-filter → smooth → callback cycle.
//   Call once per frame from your tick/render hook.
//   Returns true if a valid target was found and the callback was invoked.
bool WireFrame(WireConfig& cfg, SmoothState& smooth)
{
    if (!cfg.onDelta) return false;

    // 1. Read entities
    EntityList ents;
    if (!ReadEntityPositions(cfg.device, cfg.baseAddr, cfg.maxEnts, ents))
        return false;

    // 2. Screen centre (crosshair position)
    const float cx = static_cast<float>(cfg.screenW) * 0.5f;
    const float cy = static_cast<float>(cfg.screenH) * 0.5f;

    // Convert cone half-angle to a pixel-radius for quick comparison.
    // We approximate in screen space: cone radius ≈ tan(halfAngle) * screenH/2.
    const float halfRad = cfg.coneHalfDeg * (3.14159265f / 180.0f);
    const float coneRadPx = std::tan(halfRad) * (static_cast<float>(cfg.screenH) * 0.5f);
    const float coneRadSq = coneRadPx * coneRadPx;

    // 3. Project each entity, filter by cone, keep nearest (smallest distance²)
    std::optional<Vec2> bestScreen;
    float bestDistSq = std::numeric_limits<float>::max();

    for (std::size_t i = 0; i < ents.positions.size(); ++i) {
        Vec2 sp{};
        if (!WorldToScreen(ents.positions[i], cfg.viewMatrix,
                           cfg.screenW, cfg.screenH, sp))
            continue;

        const float dx = sp.x - cx;
        const float dy = sp.y - cy;
        const float distSq = dx * dx + dy * dy;

        if (distSq > coneRadSq) continue;   // outside cone
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestScreen = sp;
        }
    }

    if (!bestScreen) return false;

    // 4. Smooth toward the best target
    const Vec2 smoothed = SmoothToward(smooth, *bestScreen);

    // 5. Compute delta from screen centre to smoothed position and dispatch
    cfg.onDelta(smoothed.x - cx, smoothed.y - cy);
    return true;
}

// ---------------------------------------------------------------------------
// Standalone smoke test (compile with /DSMOKE_TEST)
// ---------------------------------------------------------------------------
#ifdef SMOKE_TEST

#include <cstdio>
#include <cstring>

// Fake memory layout matching the offsets declared in ReadEntityPositions.
// We pack three entity slots at a known address so we can verify the read.
#pragma pack(push, 1)
struct FakeEntitySlot {
    std::uint64_t id;
    float         x, y, z;
    std::uint8_t  _pad[12];
};
static_assert(sizeof(FakeEntitySlot) == 0x20, "slot size mismatch");
#pragma pack(pop)

struct FakeMemory {
    std::uintptr_t  arrayPtr;          // at baseAddr + 0
    std::uint8_t    _gap[0xFFFFF - 8]; // pad to baseAddr offset (simplified)
    FakeEntitySlot  slots[3];
};

int main()
{
    // Build a tiny fake world: three entities in a line in front of the camera.
    static std::uint8_t mem[sizeof(std::uintptr_t) + 3 * sizeof(FakeEntitySlot)];
    std::memset(mem, 0, sizeof(mem));

    // Write array pointer at offset 0: points to right after the pointer itself
    std::uintptr_t slotBase = reinterpret_cast<std::uintptr_t>(mem) + sizeof(std::uintptr_t);
    std::memcpy(mem, &slotBase, sizeof(slotBase));

    // Write three entity slots
    FakeEntitySlot slots[3] = {
        { 1, -50.f, 0.f, 100.f, {} },
        { 2,   0.f, 0.f,  80.f, {} },   // ← nearest, on-axis
        { 3,  50.f, 0.f, 100.f, {} },
    };
    std::memcpy(mem + sizeof(std::uintptr_t), slots, sizeof(slots));

    // Identity-ish view matrix: simple perspective along +Z
    ViewMatrix vm{};
    const float fov  = 1.0f;      // scale factor (not a real FOV, just smoke)
    vm.m[0][0] = fov;
    vm.m[1][1] = fov;
    vm.m[2][3] = 1.0f;            // carry Z into W
    vm.m[3][3] = 0.0f;
    // For entity at (0,0,80): clipW = 80, ndcX=0, ndcY=0 → screen centre.

    SmoothState smooth{ {960.f, 540.f}, 0.5f };

    WireConfig cfg;
    cfg.device       = mem;
    cfg.baseAddr     = 0;
    cfg.maxEnts      = 3;
    cfg.viewMatrix   = vm;
    cfg.screenW      = 1920;
    cfg.screenH      = 1080;
    cfg.coneHalfDeg  = 20.0f;
    cfg.smoothFactor = smooth.smoothFactor;
    cfg.onDelta      = [](float dx, float dy) {
        std::printf("delta: dx=%.3f  dy=%.3f\n", dx, dy);
    };

    bool ok = WireFrame(cfg, smooth);
    std::printf("WireFrame result: %s\n", ok ? "target found" : "no target");
    return ok ? 0 : 1;
}
#endif // SMOKE_TEST
