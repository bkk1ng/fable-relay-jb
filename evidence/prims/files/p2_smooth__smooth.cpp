// smooth.cpp
// Header-only cursor-movement interpolation library.
//
// Usage:
//   #include "smooth.cpp"
//
//   void my_move_delta(int dx, int dy, void* ctx) { /* send to input backend */ }
//
//   smooth::Config cfg;
//   cfg.max_step      = 20.0f;   // max pixels per tick
//   cfg.settle_radius = 1.5f;    // stop when within this many pixels of target
//   cfg.backend       = my_move_delta;
//   cfg.user_ctx      = nullptr;
//
//   smooth::State st = smooth::make_state(current_x, current_y);
//   smooth::set_target(st, target_x, target_y);
//
//   // call once per tick (game loop, timer, etc.)
//   bool done = smooth::tick(st, cfg);   // returns true when settled/arrived
//
// Exported functions:
//   smooth::make_state(float x, float y) -> smooth::State
//   smooth::set_target(State&, float tx, float ty) -> void
//   smooth::tick(State&, const Config&) -> bool   // true = movement finished
//   smooth::reset(State&) -> void                  // cancel movement in progress

#pragma once
#ifndef SMOOTH_CPP_INCLUDED
#define SMOOTH_CPP_INCLUDED

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace smooth {

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Called once per emitted delta.  dx/dy are pixel deltas (may be fractional
/// subpixel accumulation; cast to int inside your backend if the API needs it).
using DeltaFn = void(*)(float dx, float dy, void* user_ctx);

struct Config {
    /// Maximum movement per tick, in pixels.  Must be > 0.
    float   max_step      = 16.0f;

    /// Movement stops when the remaining distance falls below this radius.
    float   settle_radius = 1.0f;

    /// Ease-out exponent.  1 = linear, 2 = quadratic ease-out (default),
    /// higher = more aggressive deceleration tail.
    float   ease_exponent = 2.0f;

    /// Callback that consumes a delta.  Must not be null when tick() is called.
    DeltaFn backend       = nullptr;

    /// Passed verbatim to backend on every call.
    void*   user_ctx      = nullptr;
};

struct State {
    float cur_x, cur_y;     // current pointer position (tracking local copy)
    float tgt_x, tgt_y;     // desired target
    float sub_x, sub_y;     // subpixel accumulator so we don't lose fractional steps
    bool  has_target;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

/// Ease-out curve: maps [0,1] distance-fraction to a [0,1] speed scalar.
/// We express the remaining distance as a fraction of the *initial* distance
/// so the motion slows as it approaches the target.
///
/// speed = (remaining / initial)^(1 / exponent)
///
/// At the start remaining==initial → speed = 1.0 (full max_step).
/// As remaining → 0  → speed → 0  (smooth stop).
inline float ease_out_speed(float remaining, float initial, float exponent) noexcept {
    if (initial <= 0.0f) return 0.0f;
    float t = remaining / initial;
    t = std::max(0.0f, std::min(1.0f, t));
    // pow(t, 1/e) gives ease-out: fast start, slow finish
    return std::pow(t, 1.0f / std::max(exponent, 0.01f));
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Create a State anchored at the given current position.
/// No target is set; tick() is a no-op until set_target() is called.
inline State make_state(float current_x, float current_y) noexcept {
    State s{};
    s.cur_x      = current_x;
    s.cur_y      = current_y;
    s.tgt_x      = current_x;
    s.tgt_y      = current_y;
    s.sub_x      = 0.0f;
    s.sub_y      = 0.0f;
    s.has_target = false;
    return s;
}

/// Set or update the desired target position.
/// The state remembers the initial distance for the ease-out calculation;
/// calling set_target() mid-flight restarts the ease curve from the new
/// remaining distance treated as the new initial distance.
inline void set_target(State& s, float target_x, float target_y) noexcept {
    s.tgt_x      = target_x;
    s.tgt_y      = target_y;
    s.has_target = true;
    // Reset subpixel accumulator on new target so errors don't carry over.
    s.sub_x = 0.0f;
    s.sub_y = 0.0f;
}

/// Cancel any in-progress movement.
inline void reset(State& s) noexcept {
    s.has_target = false;
    s.sub_x      = 0.0f;
    s.sub_y      = 0.0f;
}

/// Advance one tick.
///
/// Computes an ease-out delta toward the target, clamps it to max_step,
/// accumulates subpixel remainders so they aren't lost, and fires cfg.backend
/// with the integer portion of the delta.
///
/// Returns true if the cursor has settled (within settle_radius of target, or
/// has_target is false).  Returns false if movement is still in progress.
///
/// cfg.backend must be non-null when has_target is true.
inline bool tick(State& s, const Config& cfg) noexcept {
    if (!s.has_target) return true;

    const float dx_total = s.tgt_x - s.cur_x;
    const float dy_total = s.tgt_y - s.cur_y;
    const float dist     = std::sqrt(dx_total * dx_total + dy_total * dy_total);

    // Stop condition: within settle radius.
    if (dist <= cfg.settle_radius) {
        // Snap to target and emit a final delta if there's any residual.
        const float snap_dx = dx_total + s.sub_x;
        const float snap_dy = dy_total + s.sub_y;
        if (std::fabs(snap_dx) >= 0.5f || std::fabs(snap_dy) >= 0.5f) {
            if (cfg.backend) cfg.backend(snap_dx, snap_dy, cfg.user_ctx);
        }
        s.cur_x      = s.tgt_x;
        s.cur_y      = s.tgt_y;
        s.sub_x      = 0.0f;
        s.sub_y      = 0.0f;
        s.has_target = false;
        return true;
    }

    // Ease-out speed scalar based on remaining distance.
    // We use `dist` itself as both remaining and initial for a per-tick
    // relative ease: each tick the step is proportional to the remaining gap.
    // To get a smooth curve we scale by ease_exponent directly:
    //   step_magnitude = min(dist * speed_fraction, max_step)
    // speed_fraction = (dist / dist)^(1/e) = 1 at full distance, but we want
    // deceleration as we close in.  We instead compute:
    //   speed_fraction = clamp(dist / max_step, 0, 1)^(1/e)
    // so near the target (dist << max_step) the speed is fractional.
    const float ratio         = std::min(dist / std::max(cfg.max_step, 0.001f), 1.0f);
    const float speed_frac    = detail::ease_out_speed(ratio, 1.0f, cfg.ease_exponent);
    const float step_mag      = std::min(dist, cfg.max_step * speed_frac);

    // Normalise direction and scale.
    const float inv_dist = 1.0f / dist;
    const float nx       = dx_total * inv_dist;
    const float ny       = dy_total * inv_dist;

    // Exact float delta for this tick.
    const float fdx = nx * step_mag;
    const float fdy = ny * step_mag;

    // Add subpixel accumulator.
    const float accum_x = fdx + s.sub_x;
    const float accum_y = fdy + s.sub_y;

    // Integer portion to emit.
    const float emit_x = std::round(accum_x);
    const float emit_y = std::round(accum_y);

    // Carry the remainder for the next tick.
    s.sub_x = accum_x - emit_x;
    s.sub_y = accum_y - emit_y;

    // Advance our tracking position by the full float delta (not the rounded
    // one) so we don't drift away from the true interpolated path.
    s.cur_x += fdx;
    s.cur_y += fdy;

    // Emit only if the rounded delta is non-zero.
    if ((emit_x != 0.0f || emit_y != 0.0f) && cfg.backend) {
        cfg.backend(emit_x, emit_y, cfg.user_ctx);
    }

    return false;
}

} // namespace smooth

// ---------------------------------------------------------------------------
// Standalone smoke-test (compile with -DSMOOTH_SMOKE_TEST)
// ---------------------------------------------------------------------------
#ifdef SMOOTH_SMOKE_TEST

#include <cstdio>
#include <cstdlib>

static int g_tick_count  = 0;
static float g_total_x   = 0.0f;
static float g_total_y   = 0.0f;

static void record_delta(float dx, float dy, void* /*ctx*/) {
    g_total_x += dx;
    g_total_y += dy;
    std::printf("  tick %3d  delta (%+8.3f, %+8.3f)  cumulative (%+9.3f, %+9.3f)\n",
                g_tick_count, dx, dy, g_total_x, g_total_y);
}

int main(void) {
    std::printf("smooth.cpp smoke test\n");
    std::printf("---------------------\n");

    smooth::Config cfg;
    cfg.max_step      = 20.0f;
    cfg.settle_radius = 1.5f;
    cfg.ease_exponent = 2.0f;
    cfg.backend       = record_delta;
    cfg.user_ctx      = nullptr;

    smooth::State st = smooth::make_state(0.0f, 0.0f);
    smooth::set_target(st, 150.0f, 100.0f);

    std::printf("Moving (0,0) -> (150,100)  max_step=%.1f  settle=%.1f  ease=%.1f\n\n",
                cfg.max_step, cfg.settle_radius, cfg.ease_exponent);

    bool done = false;
    for (int i = 0; i < 512 && !done; ++i) {
        g_tick_count = i + 1;
        done = smooth::tick(st, cfg);
    }

    std::printf("\nSettled=%s  ticks=%d  total_delta=(%.3f, %.3f)\n",
                done ? "yes" : "no (timeout)", g_tick_count,
                g_total_x, g_total_y);

    // Basic correctness: total accumulated delta must be within 2px of target.
    const float err_x = std::fabs(g_total_x - 150.0f);
    const float err_y = std::fabs(g_total_y - 100.0f);
    if (!done || err_x > 2.0f || err_y > 2.0f) {
        std::fprintf(stderr, "FAIL: error (%.3f, %.3f) out of tolerance\n", err_x, err_y);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}

#endif // SMOOTH_SMOKE_TEST
#endif // SMOOTH_CPP_INCLUDED
