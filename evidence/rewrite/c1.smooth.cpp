/**
 * smooth.cpp — cursor interpolation library
 *
 * Public API:
 *   SmoothCtx  smooth_create(SmoothConfig)         — allocate + initialize context
 *   void       smooth_destroy(SmoothCtx*)           — free context
 *   void       smooth_set_target(SmoothCtx*, float tx, float ty)
 *   void       smooth_step(SmoothCtx*)              — advance one tick, calls delta callback
 *   void       smooth_get_position(const SmoothCtx*, float* x, float* y)
 *   bool       smooth_settled(const SmoothCtx*)     — true when inside settle radius
 *
 * Build standalone smoke test:
 *   cl /std:c++17 /DSMOOTH_SMOKE /W4 smooth.cpp
 *   g++ -std=c++17 -DSMOOTH_SMOKE -Wall -o smooth smooth.cpp
 */

#include <cmath>
#include <cstdlib>
#include <cstdio>

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/** Called by smooth_step() to retrieve the elapsed time (seconds) since the
 *  last call.  The user supplies this so the library has no OS dependency.   */
typedef float (*DeltaFn)(void* userdata);

struct SmoothConfig {
    float    start_x;       ///< initial cursor x
    float    start_y;       ///< initial cursor y
    float    ease;          ///< ease-out factor in (0, 1]: fraction of remaining
                            ///< distance closed per second (applied continuously)
    float    max_step;      ///< maximum pixels moved per second (speed cap)
    float    settle_radius; ///< distance below which the cursor is considered settled
    DeltaFn  delta_fn;      ///< user-supplied time-delta callback
    void*    userdata;      ///< forwarded verbatim to delta_fn
};

struct SmoothCtx {
    float     x, y;
    float     target_x, target_y;
    SmoothConfig cfg;
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float distance2(float dx, float dy) {
    return std::sqrtf(dx * dx + dy * dy);
}

/**
 * Allocate and return a new SmoothCtx.  Caller owns it; free with
 * smooth_destroy().  Returns nullptr on allocation failure.
 */
SmoothCtx* smooth_create(SmoothConfig cfg) {
    SmoothCtx* ctx = static_cast<SmoothCtx*>(std::malloc(sizeof(SmoothCtx)));
    if (!ctx) return nullptr;

    ctx->x        = cfg.start_x;
    ctx->y        = cfg.start_y;
    ctx->target_x = cfg.start_x;
    ctx->target_y = cfg.start_y;
    ctx->cfg      = cfg;
    return ctx;
}

/** Release a context previously returned by smooth_create(). */
void smooth_destroy(SmoothCtx* ctx) {
    std::free(ctx);
}

/** Set a new target.  The cursor will ease toward it on subsequent steps. */
void smooth_set_target(SmoothCtx* ctx, float tx, float ty) {
    if (!ctx) return;
    ctx->target_x = tx;
    ctx->target_y = ty;
}

/**
 * Advance the simulation by one tick.
 *
 * Algorithm:
 *   1. Query elapsed seconds via cfg.delta_fn.
 *   2. Compute the continuous ease-out fraction:
 *        alpha = 1 - exp(-ease * dt)
 *      This is the same as applying the per-frame factor (1-ease)^(dt/frame_ref)
 *      in a frame-rate-independent way.
 *   3. Move x/y toward target by alpha of the remaining gap.
 *   4. Clamp the displacement to max_step * dt so very large targets or long
 *      delta times do not cause teleports.
 */
void smooth_step(SmoothCtx* ctx) {
    if (!ctx || !ctx->cfg.delta_fn) return;

    float dt = ctx->cfg.delta_fn(ctx->cfg.userdata);
    if (dt <= 0.0f) return;

    // Ease-out fraction for this tick (frame-rate independent)
    float alpha = 1.0f - std::expf(-ctx->cfg.ease * dt);

    float dx = ctx->target_x - ctx->x;
    float dy = ctx->target_y - ctx->y;

    // Desired move this tick
    float move_x = dx * alpha;
    float move_y = dy * alpha;

    // Speed cap: limit to max_step pixels per second
    float max_dist = ctx->cfg.max_step * dt;
    float move_dist = distance2(move_x, move_y);
    if (move_dist > max_dist && move_dist > 0.0f) {
        float scale = max_dist / move_dist;
        move_x *= scale;
        move_y *= scale;
    }

    ctx->x += move_x;
    ctx->y += move_y;
}

/** Read back current interpolated position. */
void smooth_get_position(const SmoothCtx* ctx, float* x, float* y) {
    if (!ctx) return;
    if (x) *x = ctx->x;
    if (y) *y = ctx->y;
}

/**
 * Returns true when the cursor is within settle_radius of the target.
 * Callers may use this to skip further processing when motion has
 * effectively stopped.
 */
bool smooth_settled(const SmoothCtx* ctx) {
    if (!ctx) return true;
    float dx = ctx->target_x - ctx->x;
    float dy = ctx->target_y - ctx->y;
    return distance2(dx, dy) <= ctx->cfg.settle_radius;
}

// ---------------------------------------------------------------------------
// Standalone smoke test
// ---------------------------------------------------------------------------
#ifdef SMOOTH_SMOKE

#include <cstdio>

static float g_fake_dt = 1.0f / 60.0f;   // 60 Hz

static float fake_delta(void* /*userdata*/) {
    return g_fake_dt;
}

int main() {
    SmoothConfig cfg{};
    cfg.start_x       = 0.0f;
    cfg.start_y       = 0.0f;
    cfg.ease          = 12.0f;   // close ~63% of remaining gap per second (tau = 1/12 s)
    cfg.max_step      = 800.0f;  // pixels/sec cap
    cfg.settle_radius = 0.5f;
    cfg.delta_fn      = fake_delta;
    cfg.userdata      = nullptr;

    SmoothCtx* ctx = smooth_create(cfg);
    if (!ctx) { std::fprintf(stderr, "smooth_create failed\n"); return 1; }

    smooth_set_target(ctx, 400.0f, 300.0f);

    int ticks = 0;
    while (!smooth_settled(ctx) && ticks < 10000) {
        smooth_step(ctx);
        ++ticks;
    }

    float x, y;
    smooth_get_position(ctx, &x, &y);
    std::printf("Settled after %d ticks at (%.3f, %.3f)\n", ticks, x, y);

    // Expect near (400, 300)
    bool ok = std::fabsf(x - 400.0f) < cfg.settle_radius &&
              std::fabsf(y - 300.0f) < cfg.settle_radius;
    std::printf("Test %s\n", ok ? "PASSED" : "FAILED");

    smooth_destroy(ctx);
    return ok ? 0 : 1;
}

#endif // SMOOTH_SMOKE
