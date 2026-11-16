// aim.cpp — crosshair delta solver
// Projects world-space entity head positions to screen, selects the nearest
// target within a cone, and delivers a smoothed, dead-zoned delta via callback.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <optional>
#include <functional>
#include <vector>
#include <limits>

// ---------------------------------------------------------------------------
// Math primitives
// ---------------------------------------------------------------------------

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

struct Matrix4x4 {
    float m[4][4];
};

static Vec2 vec2_sub(Vec2 a, Vec2 b)  { return { a.x - b.x, a.y - b.y }; }
static float vec2_len(Vec2 v)         { return sqrtf(v.x * v.x + v.y * v.y); }

// ---------------------------------------------------------------------------
// Solver configuration
// ---------------------------------------------------------------------------

struct AimConfig {
    // Projection
    float fov_y_rad    = 1.0472f;   // vertical FOV in radians (default 60°)
    int   screen_w     = 1920;
    int   screen_h     = 1080;

    // Selection
    float cone_px      = 240.0f;    // max screen-space distance from crosshair

    // Smoothing — independent per axis (0 < gain <= 1; 1 = instant)
    float gain_x       = 0.12f;
    float gain_y       = 0.12f;

    // Dead zone: ignore delta smaller than this (screen pixels)
    float dead_zone_px = 1.5f;

    // Settle radius: zero delta when already within this distance (screen px)
    float settle_px    = 3.0f;
};

// ---------------------------------------------------------------------------
// Solver state (one per weapon/session)
// ---------------------------------------------------------------------------

struct AimSolverState {
    float smoothed_dx = 0.0f;
    float smoothed_dy = 0.0f;
};

// ---------------------------------------------------------------------------
// Callback type delivered to callers
// ---------------------------------------------------------------------------

using DeltaCallback = std::function<void(float dx, float dy)>;

// ---------------------------------------------------------------------------
// World-to-screen projection
// Assumes a right-handed, Y-up view-space convention common in Unreal/Valorant.
// The caller supplies the current view/projection matrix (row-major, D3D style).
// Returns false when the point is behind the camera.
// ---------------------------------------------------------------------------

static bool world_to_screen(
    const Vec3&       world_pos,
    const Matrix4x4&  view_proj,
    int               screen_w,
    int               screen_h,
    Vec2&             out_screen)
{
    const float* m = reinterpret_cast<const float*>(view_proj.m);

    float clip_x = world_pos.x * m[0]  + world_pos.y * m[4]  + world_pos.z * m[8]  + m[12];
    float clip_y = world_pos.x * m[1]  + world_pos.y * m[5]  + world_pos.z * m[9]  + m[13];
    float clip_w = world_pos.x * m[3]  + world_pos.y * m[7]  + world_pos.z * m[11] + m[15];

    if (clip_w <= 0.001f)
        return false;

    float ndc_x = clip_x / clip_w;
    float ndc_y = clip_y / clip_w;

    out_screen.x = ( ndc_x * 0.5f + 0.5f) * static_cast<float>(screen_w);
    out_screen.y = (-ndc_y * 0.5f + 0.5f) * static_cast<float>(screen_h);
    return true;
}

// ---------------------------------------------------------------------------
// Core solver
//
// head_positions   — world-space head positions of visible entities this frame
// view_proj        — current combined view*projection matrix (row-major)
// cfg              — tuning parameters
// state            — persistent smoothing state (mutated by this call)
// on_delta         — called once if a target was found; not called otherwise
// ---------------------------------------------------------------------------

void aim_solve(
    const std::vector<Vec3>& head_positions,
    const Matrix4x4&         view_proj,
    const AimConfig&         cfg,
    AimSolverState&          state,
    const DeltaCallback&     on_delta)
{
    const Vec2 crosshair = {
        static_cast<float>(cfg.screen_w) * 0.5f,
        static_cast<float>(cfg.screen_h) * 0.5f
    };

    // --- find nearest projected head inside the cone ---
    float best_dist = std::numeric_limits<float>::max();
    std::optional<Vec2> best_screen;

    for (const Vec3& wp : head_positions) {
        Vec2 sp{};
        if (!world_to_screen(wp, view_proj, cfg.screen_w, cfg.screen_h, sp))
            continue;

        Vec2  diff = vec2_sub(sp, crosshair);
        float dist = vec2_len(diff);

        if (dist > cfg.cone_px)
            continue;

        if (dist < best_dist) {
            best_dist  = dist;
            best_screen = sp;
        }
    }

    if (!best_screen.has_value()) {
        // No target — bleed smoothed state toward zero so there is no residual
        // velocity when a target reappears.
        state.smoothed_dx *= (1.0f - cfg.gain_x);
        state.smoothed_dy *= (1.0f - cfg.gain_y);
        return;
    }

    Vec2 diff = vec2_sub(*best_screen, crosshair);

    // Settle radius: already on target
    if (best_dist <= cfg.settle_px) {
        state.smoothed_dx = 0.0f;
        state.smoothed_dy = 0.0f;
        return;
    }

    // Exponential smoothing (low-pass)
    state.smoothed_dx += (diff.x - state.smoothed_dx) * cfg.gain_x;
    state.smoothed_dy += (diff.y - state.smoothed_dy) * cfg.gain_y;

    float out_dx = state.smoothed_dx;
    float out_dy = state.smoothed_dy;

    // Dead zone: suppress micro-jitter below threshold
    if (fabsf(out_dx) < cfg.dead_zone_px) out_dx = 0.0f;
    if (fabsf(out_dy) < cfg.dead_zone_px) out_dy = 0.0f;

    if (out_dx == 0.0f && out_dy == 0.0f)
        return;

    on_delta(out_dx, out_dy);
}

// ---------------------------------------------------------------------------
// Smoke / standalone build — compile with /DSMOKE_TEST
// ---------------------------------------------------------------------------
#ifdef SMOKE_TEST

#include <cstdio>

int main() {
    // Build a trivial identity-ish view_proj where world == screen (NDC trick):
    // place a fake head slightly right of centre.
    Matrix4x4 vp{};
    vp.m[0][0] = 1.0f; vp.m[1][1] = 1.0f; vp.m[2][2] = 1.0f; vp.m[3][3] = 1.0f;

    // world_to_screen with this matrix: ndc_x = wx/ww, ww = 1  => ndc_x = wx
    // screen_x = (wx*0.5 + 0.5)*1920
    // For screen_x == 960+50 => ndc_x = 100/1920 => wx = 0.0520833
    std::vector<Vec3> heads = {
        { 0.0520833f, 0.0f, 0.0f }   // ~50 px right of centre
    };

    AimConfig       cfg;
    AimSolverState  state;
    int             calls = 0;

    for (int frame = 0; frame < 20; ++frame) {
        aim_solve(heads, vp, cfg, state, [&](float dx, float dy) {
            ++calls;
            printf("frame %2d  dx=%.3f  dy=%.3f\n", frame, dx, dy);
        });
    }

    printf("smoke test done — callback fired %d times\n", calls);
    return calls > 0 ? 0 : 1;
}

#endif // SMOKE_TEST
