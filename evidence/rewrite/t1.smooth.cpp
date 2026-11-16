// smooth.cpp — cursor interpolation library
// Target set by caller; ease-out curve; max step clamp; settle radius;
// caller-supplied delta callback.
//
// Build standalone smoke test:
//   cl /W4 /std:c++17 /DSMOOTH_SMOKE_TEST smooth.cpp /Fe:smooth_test.exe

#pragma once

#include <cmath>
#include <functional>
#include <algorithm>
#include <cstdio>

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Prototype for the caller-supplied delta provider.
/// Should return elapsed seconds since the previous call (or since
/// construction on the first call).  The library never reads a clock itself.
using DeltaFn = std::function<float()>;

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

inline Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
inline float dot(Vec2 a, Vec2 b)      noexcept { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 v)           noexcept { return std::sqrt(dot(v, v)); }

/// Interpolator configuration.
struct SmoothConfig {
    /// Ease-out speed factor.  Higher = snappier approach.  Typical: 8–20.
    float speed       = 12.f;

    /// Maximum distance the cursor may travel per second (pixels/s).
    /// 0 = unclamped.
    float maxStepPerSec = 0.f;

    /// If the cursor is within this radius of the target it is snapped and
    /// motion stops.  Prevents endless sub-pixel oscillation.
    float settleRadius  = 0.5f;
};

// ---------------------------------------------------------------------------
// CursorSmoother
// ---------------------------------------------------------------------------

class CursorSmoother {
public:
    /// Construct with an initial position, configuration, and delta callback.
    /// @param initial  Starting cursor position.
    /// @param cfg      Tuning parameters; may be changed at any time via reconfigure().
    /// @param deltaFn  Callable that returns dt (seconds) each time tick() is called.
    explicit CursorSmoother(Vec2 initial, SmoothConfig cfg, DeltaFn deltaFn)
        : m_pos(initial)
        , m_target(initial)
        , m_cfg(cfg)
        , m_deltaFn(std::move(deltaFn))
    {}

    // -- target control -------------------------------------------------------

    /// Replace the current target.
    void setTarget(Vec2 target) noexcept { m_target = target; }

    /// Return the current target.
    Vec2 target() const noexcept { return m_target; }

    // -- configuration --------------------------------------------------------

    void reconfigure(SmoothConfig cfg) noexcept { m_cfg = cfg; }
    const SmoothConfig& config() const noexcept { return m_cfg; }

    // -- state ----------------------------------------------------------------

    /// Current smoothed position.
    Vec2 position() const noexcept { return m_pos; }

    /// True when the cursor is within settleRadius of the target.
    bool settled() const noexcept {
        return length(m_target - m_pos) <= m_cfg.settleRadius;
    }

    /// Teleport the cursor without interpolation.
    void snap(Vec2 pos) noexcept { m_pos = pos; }

    // -- tick -----------------------------------------------------------------

    /// Advance the simulation by one frame.
    /// Invokes the delta callback, applies an ease-out step, clamps to
    /// maxStepPerSec, and snaps when inside settleRadius.
    /// @return The updated cursor position.
    Vec2 tick() {
        const float dt = m_deltaFn();
        return tickDt(dt);
    }

    /// Advance the simulation with an explicit delta (seconds).
    /// Useful when the caller wants to drive time manually.
    Vec2 tickDt(float dt) noexcept {
        if (dt <= 0.f) return m_pos;

        Vec2 diff = m_target - m_pos;
        const float dist = length(diff);

        // Already settled — snap exactly.
        if (dist <= m_cfg.settleRadius) {
            m_pos = m_target;
            return m_pos;
        }

        // Ease-out: exponential decay toward the target.
        //   factor = 1 - exp(-speed * dt)
        // This is frame-rate independent and approaches the target
        // asymptotically without overshoot.
        const float factor = 1.f - std::exp(-m_cfg.speed * dt);
        float step = dist * factor;   // desired move distance this frame

        // Clamp to max pixels-per-second.
        if (m_cfg.maxStepPerSec > 0.f) {
            const float maxStep = m_cfg.maxStepPerSec * dt;
            step = std::min(step, maxStep);
        }

        // Snap inside settle radius.
        if (step >= dist - m_cfg.settleRadius) {
            m_pos = m_target;
        } else {
            // Normalise diff then apply step.
            const Vec2 dir = diff * (1.f / dist);
            m_pos = m_pos + dir * step;
        }

        return m_pos;
    }

private:
    Vec2        m_pos;
    Vec2        m_target;
    SmoothConfig m_cfg;
    DeltaFn      m_deltaFn;
};

// ---------------------------------------------------------------------------
// Standalone smoke test
// ---------------------------------------------------------------------------

#ifdef SMOOTH_SMOKE_TEST

#include <cstdio>
#include <vector>

int main() {
    // Simulate a fixed 60 Hz tick.
    constexpr float kDt = 1.f / 60.f;
    int frame = 0;
    auto deltaFn = [&]() -> float {
        (void)frame;
        return kDt;
    };

    SmoothConfig cfg;
    cfg.speed        = 12.f;
    cfg.maxStepPerSec = 400.f;  // pixels/s
    cfg.settleRadius  = 0.5f;

    CursorSmoother smoother({0.f, 0.f}, cfg, deltaFn);
    smoother.setTarget({200.f, 150.f});

    std::printf("%-6s  %-10s  %-10s  %s\n", "frame", "x", "y", "settled");
    std::printf("%-6s  %-10s  %-10s  %s\n", "-----", "---------", "---------", "-------");

    for (frame = 0; frame < 120; ++frame) {
        Vec2 pos = smoother.tick();
        std::printf("%-6d  %-10.3f  %-10.3f  %s\n",
                    frame, pos.x, pos.y,
                    smoother.settled() ? "yes" : "no");
        if (smoother.settled()) break;
    }

    // ---- mid-flight target change ----------------------------------------
    std::printf("\n-- Target changed to (50, 300) at frame %d --\n\n", frame);
    smoother.setTarget({50.f, 300.f});

    std::printf("%-6s  %-10s  %-10s  %s\n", "frame", "x", "y", "settled");
    std::printf("%-6s  %-10s  %-10s  %s\n", "-----", "---------", "---------", "-------");

    for (int i = 0; i < 120; ++i, ++frame) {
        Vec2 pos = smoother.tick();
        std::printf("%-6d  %-10.3f  %-10.3f  %s\n",
                    frame, pos.x, pos.y,
                    smoother.settled() ? "yes" : "no");
        if (smoother.settled()) break;
    }

    // ---- unclamped fast snap test ----------------------------------------
    cfg.maxStepPerSec = 0.f;    // unclamped
    cfg.speed         = 25.f;
    smoother.reconfigure(cfg);
    smoother.snap({0.f, 0.f});
    smoother.setTarget({1.f, 1.f});

    Vec2 p = smoother.tickDt(1.f);   // big dt — must settle, not overshoot
    std::printf("\nUnclamped snap test  x=%.4f y=%.4f  settled=%s\n",
                p.x, p.y, smoother.settled() ? "yes" : "no");

    return 0;
}

#endif // SMOOTH_SMOKE_TEST
