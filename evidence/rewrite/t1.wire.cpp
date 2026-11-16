// wire.cpp
// Wiring module: reads entity positions, projects them, finds the nearest
// target inside a cone around the current cursor, smooths toward it, and
// fires a delta callback.
//
// Assumes three companion modules supply:
//   projection.h  – WorldToScreen(Vec3 world, Vec2* screen) -> bool
//   smooth.h      – ApplySmoothing(Vec2 current, Vec2 target, float strength) -> Vec2
//   readpath.h    – ReadEntities(EntityEntry* buf, DWORD maxCount) -> DWORD
//
// Exported entry point:
//   WireTickOnce(const WireConfig&, WireDeltaCallback) -> bool
//     Reads entities, finds nearest projected point inside the cone, smooths
//     the cursor toward it, and calls the callback with the raw delta.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cmath>
#include <cfloat>
#include <cstring>

// --------------------------------------------------------------------------
// Companion module interfaces (forward declarations / stub headers)
// --------------------------------------------------------------------------

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

struct EntityEntry {
    Vec3  worldPos;
    bool  valid;
};

// Provided by projection module
extern bool WorldToScreen(Vec3 world, Vec2* screen);

// Provided by smooth module
extern Vec2 ApplySmoothing(Vec2 current, Vec2 target, float strength);

// Provided by readpath module (device-backed memory reader)
extern DWORD ReadEntities(EntityEntry* buf, DWORD maxCount);

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

static constexpr DWORD kMaxEntities = 2048;

struct WireConfig {
    float smoothStrength;   // 0.0 (instant) .. 1.0 (very slow)
    float coneHalfAngleDeg; // half-angle of target cone in screen degrees
    float screenCX;         // screen-space cone origin X (usually crosshair X)
    float screenCY;         // screen-space cone origin Y (usually crosshair Y)
    float screenW;          // screen width  (pixels)
    float screenH;          // screen height (pixels)
};

struct WireDelta {
    float dx;   // pixels to move in X
    float dy;   // pixels to move in Y
};

// Callback: fired once per tick when a target is found.
// Return true to apply the move, false to suppress it.
using WireDeltaCallback = bool(*)(const WireDelta* delta, void* userCtx);

// --------------------------------------------------------------------------
// Internal helpers
// --------------------------------------------------------------------------

static inline float Vec2Dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

static inline float Vec2Length(Vec2 v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

static inline Vec2 Vec2Normalize(Vec2 v) {
    float len = Vec2Length(v);
    if (len < 1e-6f) return { 0.f, 0.f };
    return { v.x / len, v.y / len };
}

// Returns the angle in degrees between two screen-space directions from origin.
static float ScreenAngleDeg(Vec2 origin, Vec2 point) {
    Vec2 d = { point.x - origin.x, point.y - origin.y };
    float len = Vec2Length(d);
    if (len < 0.5f) return 0.f;   // point is essentially at origin
    // We use the length in pixels as the "angular" proxy; for small angles
    // at typical FOVs this is equivalent to a proper angular check.
    // For a true angle we need a reference direction.  Use (0,-1) = straight
    // up as reference so the cone points where the crosshair faces.
    Vec2 ref = { 0.f, -1.f };
    Vec2 dn  = Vec2Normalize(d);
    float cosA = Vec2Dot(ref, dn);
    // clamp numerical error
    if (cosA >  1.f) cosA =  1.f;
    if (cosA < -1.f) cosA = -1.f;
    return acosf(cosA) * (180.f / 3.14159265f);
}

// --------------------------------------------------------------------------
// WireTickOnce
// --------------------------------------------------------------------------
// Returns true if a target was found and the callback accepted the move.

bool WireTickOnce(const WireConfig& cfg, WireDeltaCallback callback, void* userCtx = nullptr) {
    if (!callback) return false;

    // 1. Read entity positions through the device-backed read path.
    static EntityEntry s_buf[kMaxEntities];
    DWORD count = ReadEntities(s_buf, kMaxEntities);
    if (count == 0) return false;

    // 2. Project every valid entity; find the nearest one inside the cone.
    Vec2 origin  = { cfg.screenCX, cfg.screenCY };
    float bestDistSq = FLT_MAX;
    Vec2  bestScreen = { 0.f, 0.f };
    bool  found  = false;

    for (DWORD i = 0; i < count; ++i) {
        if (!s_buf[i].valid) continue;

        Vec2 screen;
        if (!WorldToScreen(s_buf[i].worldPos, &screen)) continue;

        // Skip entities outside the visible screen area.
        if (screen.x < 0.f || screen.x > cfg.screenW) continue;
        if (screen.y < 0.f || screen.y > cfg.screenH) continue;

        // Cone test: use the angle from the crosshair to this projected point.
        float angleDeg = ScreenAngleDeg(origin, screen);
        if (angleDeg > cfg.coneHalfAngleDeg) continue;

        float dx = screen.x - origin.x;
        float dy = screen.y - origin.y;
        float distSq = dx * dx + dy * dy;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestScreen = screen;
            found = true;
        }
    }

    if (!found) return false;

    // 3. Apply smoothing from the current cursor position toward the best target.
    Vec2 smoothed = ApplySmoothing(origin, bestScreen, cfg.smoothStrength);

    // 4. Compute the delta and fire the callback.
    WireDelta delta;
    delta.dx = smoothed.x - origin.x;
    delta.dy = smoothed.y - origin.y;

    return callback(&delta, userCtx);
}

// --------------------------------------------------------------------------
// Smoke-test entry point (compile with -DWIRE_SMOKE_TEST)
// --------------------------------------------------------------------------
#ifdef WIRE_SMOKE_TEST

#include <cstdio>

// Stub implementations so the file links standalone.

bool WorldToScreen(Vec3 world, Vec2* screen) {
    // Trivial orthographic drop: ignore Z, map X/Y directly.
    screen->x = world.x;
    screen->y = world.y;
    return true;
}

Vec2 ApplySmoothing(Vec2 current, Vec2 target, float strength) {
    float t = 1.f - strength;   // strength 0 => t=1 (instant), 1 => t=0 (frozen)
    return { current.x + (target.x - current.x) * t,
             current.y + (target.y - current.y) * t };
}

static EntityEntry s_smokeEntities[] = {
    { { 640.f, 400.f, 10.f }, true  },   // near, inside cone
    { { 100.f, 100.f, 20.f }, true  },   // far, likely outside cone
    { { 650.f, 410.f,  5.f }, true  },   // very near, inside cone
    { {   0.f,   0.f,  0.f }, false },   // invalid
};

DWORD ReadEntities(EntityEntry* buf, DWORD maxCount) {
    DWORD n = (DWORD)(sizeof(s_smokeEntities) / sizeof(s_smokeEntities[0]));
    if (n > maxCount) n = maxCount;
    memcpy(buf, s_smokeEntities, n * sizeof(EntityEntry));
    return n;
}

static bool OnDelta(const WireDelta* d, void* /*ctx*/) {
    printf("delta: dx=%.3f  dy=%.3f\n", d->dx, d->dy);
    return true;
}

int main(void) {
    WireConfig cfg;
    cfg.smoothStrength   = 0.5f;
    cfg.coneHalfAngleDeg = 30.f;
    cfg.screenCX         = 640.f;
    cfg.screenCY         = 360.f;
    cfg.screenW          = 1280.f;
    cfg.screenH          = 720.f;

    bool hit = WireTickOnce(cfg, OnDelta, nullptr);
    printf("WireTickOnce returned: %s\n", hit ? "true" : "false");
    return 0;
}

// Build (MSVC):
//   cl /std:c++17 /DWIRE_SMOKE_TEST /EHsc wire.cpp
// Build (MinGW / GCC):
//   g++ -std=c++17 -DWIRE_SMOKE_TEST -o wire_smoke wire.cpp -lm

#endif // WIRE_SMOKE_TEST
