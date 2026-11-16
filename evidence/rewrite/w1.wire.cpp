// wire.cpp
// Wiring module: reads entity positions via device, projects them, applies
// cone filtering, selects nearest, and drives the cursor toward it.
// Calls user-supplied delta callback with (dx, dy) each frame.
//
// Depends on three sibling translation units (linked, not #included):
//   projection_module  – world_to_screen(Vec3 world, Vec2* screen) -> bool
//   smoothing_module   – smooth_delta(float raw_dx, float raw_dy,
//                                     float* out_dx, float* out_dy)
//   readpath_module    – read_entities(EntityEntry* buf, size_t cap,
//                                      size_t* out_count) -> bool
//
// Build (MSVC):
//   cl /std:c++17 /O2 /W3 wire.cpp /link user32.lib
// Build with smoke block:
//   cl /std:c++17 /O2 /W3 /DWIRE_SMOKE wire.cpp /link user32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// ---------------------------------------------------------------------------
// Shared types (also declared in the companion modules)
// ---------------------------------------------------------------------------

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

struct EntityEntry {
    Vec3  world_pos;
    int   team;      // 0 = enemy, 1 = friendly, -1 = unknown
    bool  alive;
};

// ---------------------------------------------------------------------------
// Forward declarations for the three sibling modules
// ---------------------------------------------------------------------------

extern "C" bool world_to_screen(Vec3 world, Vec2* screen);
extern "C" void smooth_delta(float raw_dx, float raw_dy,
                             float* out_dx, float* out_dy);
extern "C" bool read_entities(EntityEntry* buf, size_t cap, size_t* out_count);

// ---------------------------------------------------------------------------
// Wire configuration
// ---------------------------------------------------------------------------

struct WireConfig {
    float cone_half_angle_deg = 25.0f;  // half-angle of the aimcone in degrees
    float max_distance_px     = 300.0f; // ignore targets beyond this many pixels
    int   enemy_team          = 0;      // which team value counts as a target
    size_t entity_buf_cap     = 256;    // max entities read per tick
};

// ---------------------------------------------------------------------------
// Delta callback type
//   dx, dy : smoothed cursor displacement in pixels (may be fractional)
//   user   : opaque pointer forwarded from wire_tick_params
// ---------------------------------------------------------------------------

using DeltaCallback = void (*)(float dx, float dy, void* user);

// ---------------------------------------------------------------------------
// Per-tick parameters
// ---------------------------------------------------------------------------

struct WireTickParams {
    const WireConfig* cfg;      // must not be null
    DeltaCallback     on_delta; // called once if a target is found; may be null
    void*             user;     // forwarded to on_delta
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Returns the angle (radians) between vector (ax,ay) and (bx,by).
inline float angle_between(float ax, float ay, float bx, float by) {
    float dot  = ax * bx + ay * by;
    float magA = std::sqrt(ax * ax + ay * ay);
    float magB = std::sqrt(bx * bx + by * by);
    if (magA < 1e-6f || magB < 1e-6f) return 0.0f;
    float cosA = dot / (magA * magB);
    // Clamp to [-1,1] to guard against floating-point drift.
    if (cosA >  1.0f) cosA =  1.0f;
    if (cosA < -1.0f) cosA = -1.0f;
    return std::acos(cosA);
}

// Returns the screen-centre of the primary monitor.
inline Vec2 screen_centre() {
    Vec2 c;
    c.x = static_cast<float>(GetSystemMetrics(SM_CXSCREEN)) * 0.5f;
    c.y = static_cast<float>(GetSystemMetrics(SM_CYSCREEN)) * 0.5f;
    return c;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// wire_tick
//
// One frame of work:
//   1. Read entity list through the device.
//   2. Project each living enemy to screen.
//   3. Filter to those inside the cone (centred on screen-centre).
//   4. Pick the nearest (Euclidean distance from screen-centre).
//   5. Smooth the delta and invoke the callback.
//
// Returns true  if a target was found and the callback was invoked.
// Returns false if no target was inside the cone, or the read failed.
// ---------------------------------------------------------------------------

bool wire_tick(const WireTickParams& params) {
    if (!params.cfg) return false;

    const WireConfig& cfg = *params.cfg;

    // -- 1. Read entities --------------------------------------------------
    static EntityEntry s_entity_buf[256]; // static to avoid per-frame alloc
    const size_t cap = (cfg.entity_buf_cap <= 256) ? cfg.entity_buf_cap : 256;
    size_t count = 0;
    if (!read_entities(s_entity_buf, cap, &count) || count == 0) return false;

    // -- 2 & 3. Project + cone filter --------------------------------------
    Vec2 centre = screen_centre();
    const float half_rad = cfg.cone_half_angle_deg * kDegToRad;

    Vec2  best_screen     = {};
    float best_dist_sq    = std::numeric_limits<float>::max();
    bool  found           = false;

    for (size_t i = 0; i < count; ++i) {
        const EntityEntry& e = s_entity_buf[i];
        if (!e.alive)                    continue;
        if (e.team != cfg.enemy_team)    continue;

        Vec2 sp;
        if (!world_to_screen(e.world_pos, &sp)) continue;

        // Vector from screen-centre to projected point.
        float vx = sp.x - centre.x;
        float vy = sp.y - centre.y;
        float dist_sq = vx * vx + vy * vy;

        // Distance check (early reject before trig).
        if (dist_sq > cfg.max_distance_px * cfg.max_distance_px) continue;

        // Cone check: angle between (0,-1) (straight up / forward) and (vx,vy).
        // "Forward" on-screen is typically toward the crosshair, i.e. (0,0)
        // relative to centre, so we just check that the target is within the
        // half-angle cone measured from the screen-centre origin.
        float ang = angle_between(0.0f, -1.0f, vx, vy);
        // Use atan2 for a full 360° comparison against half_rad.
        // Simpler: just measure angle from the crosshair vector.
        // We define "in cone" as the angular offset from (vx,vy) to the
        // forward direction being <= half_angle.  Forward is defined as the
        // direction from centre toward the target (all targets qualify if
        // dist > 0); the cone is therefore an angular window centred on the
        // cursor itself.  Concretely: any target whose screen vector from
        // the cursor has magnitude > 0 qualifies if its distance is below
        // max_distance_px AND the angle measured from screen-up is within
        // the half-angle.  For strict cone-from-crosshair semantics:
        (void)ang; // suppress unused if we use the atan2 path below

        // Recompute as angle from screen centre to point vs. a neutral axis.
        // "Inside cone" means within half_rad of centre regardless of direction
        // — which at zero offset is trivially true, so we simply use max_dist.
        // For a non-trivial cone, compare the angular deviation from forward.
        float angle_from_forward = std::atan2(
            std::sqrt(vx * vx + vy * vy), 0.0f); // always >= 0
        // A proper directional cone requires a known forward vector from game
        // state; here we use the screen-centre-relative cone: any point closer
        // than max_distance_px and within the angular aperture qualifies.
        // Aperture measured from straight-ahead (0,-1 in screen space):
        float aperture = std::atan2(std::abs(vx), std::abs(vy)); // 0..pi/2
        // For a proper symmetric cone:
        //   angle = atan2(length(perp), dot(forward, dir))
        // With forward = (0, -1):
        float fwd_dot  = -vy; // dot((0,-1),(vx,vy))
        float perp_mag = std::abs(vx);
        float cone_angle = std::atan2(perp_mag,
                                      fwd_dot > 0.0f ? fwd_dot : 0.0f);
        (void)aperture;
        (void)angle_from_forward;
        if (cone_angle > half_rad) continue;

        // -- 4. Nearest ----------------------------------------------------
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_screen  = sp;
            found        = true;
        }
    }

    if (!found) return false;

    // -- 5. Smooth and dispatch --------------------------------------------
    float raw_dx = best_screen.x - centre.x;
    float raw_dy = best_screen.y - centre.y;

    float sdx = 0.0f, sdy = 0.0f;
    smooth_delta(raw_dx, raw_dy, &sdx, &sdy);

    if (params.on_delta) {
        params.on_delta(sdx, sdy, params.user);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Smoke block — compiled only with /DWIRE_SMOKE
// Provides stub implementations of the three sibling modules so the file
// links standalone.  Not for production use.
// ---------------------------------------------------------------------------

#ifdef WIRE_SMOKE

#include <cstdio>
#include <cstdlib>
#include <ctime>

// ---- stub: projection_module ---------------------------------------------
extern "C" bool world_to_screen(Vec3 world, Vec2* screen) {
    // Simple orthographic drop for smoke purposes.
    screen->x = world.x * 2.0f + 960.0f;
    screen->y = world.y * 2.0f + 540.0f;
    (void)world.z;
    return true;
}

// ---- stub: smoothing_module ----------------------------------------------
extern "C" void smooth_delta(float raw_dx, float raw_dy,
                             float* out_dx, float* out_dy) {
    // 0.3 smoothing factor.
    *out_dx = raw_dx * 0.3f;
    *out_dy = raw_dy * 0.3f;
}

// ---- stub: readpath_module -----------------------------------------------
extern "C" bool read_entities(EntityEntry* buf, size_t cap, size_t* out_count) {
    static unsigned int seed = 42;
    size_t n = (cap < 8) ? cap : 8;
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        buf[i].world_pos = { static_cast<float>((int)(seed >> 16) % 400 - 200),
                             static_cast<float>((int)(seed >> 8)  % 400 - 200),
                             0.0f };
        buf[i].team  = (seed & 1) ? 1 : 0;
        buf[i].alive = true;
    }
    *out_count = n;
    return true;
}

// ---- delta callback for smoke test ---------------------------------------
static void smoke_callback(float dx, float dy, void* user) {
    (void)user;
    printf("  delta -> dx=%.3f  dy=%.3f\n", (double)dx, (double)dy);
}

int main(void) {
    puts("wire.cpp smoke test");

    WireConfig cfg;
    cfg.cone_half_angle_deg = 30.0f;
    cfg.max_distance_px     = 500.0f;
    cfg.enemy_team          = 0;
    cfg.entity_buf_cap      = 8;

    WireTickParams p;
    p.cfg      = &cfg;
    p.on_delta = smoke_callback;
    p.user     = nullptr;

    int hits = 0;
    for (int i = 0; i < 20; ++i) {
        bool ok = wire_tick(p);
        printf("tick %2d: %s\n", i, ok ? "target found" : "no target");
        if (ok) ++hits;
    }
    printf("\n%d / 20 ticks acquired a target.\n", hits);
    return 0;
}

#endif // WIRE_SMOKE
