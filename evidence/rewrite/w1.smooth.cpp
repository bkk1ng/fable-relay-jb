// smooth.cpp — cursor interpolation library
// Ease-out curve, max step clamp, settle radius, caller-supplied delta backend.
//
// Build standalone smoke test:
//   cl /EHsc /DSMOOTH_SMOKE /W4 smooth.cpp
//   g++ -std=c++17 -DSMOOTH_SMOKE -Wall smooth.cpp -o smooth_test

#include <cmath>
#include <cstdint>
#include <functional>
#include <algorithm>

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

/// Callback the caller supplies to get the elapsed seconds since the last
/// call.  Keeps the library free of any particular time API.
using DeltaFn = std::function<float()>;

/// Configuration for a Smoother instance.
struct SmoothConfig {
    /// Speed of approach: fraction of remaining distance covered per second
    /// (ease-out / exponential decay).  0 < rate <= 1.  Higher = snappier.
    float rate          = 8.0f;

    /// Hard clamp on movement per update (pixels).  0 = unlimited.
    float maxStep       = 0.0f;

    /// If the cursor is within this radius of the target, snap and stop.
    float settleRadius  = 0.5f;
};

/// Two-dimensional cursor smoother.
class Smoother {
public:
    /// Construct with a configuration and a caller-owned delta callback.
    /// @param cfg    Interpolation parameters.
    /// @param delta  Returns seconds elapsed since the previous call.
    Smoother(SmoothConfig cfg, DeltaFn delta)
        : m_cfg(cfg), m_delta(std::move(delta))
    {}

    // -----------------------------------------------------------------------
    // Target control
    // -----------------------------------------------------------------------

    /// Set a new absolute target in screen space.
    void setTarget(float x, float y) noexcept {
        m_tx = x;
        m_ty = y;
    }

    /// Move the target by a relative offset.
    void shiftTarget(float dx, float dy) noexcept {
        m_tx += dx;
        m_ty += dy;
    }

    // -----------------------------------------------------------------------
    // Current position
    // -----------------------------------------------------------------------

    /// Seed the current position without animating (e.g. on first frame).
    void warpTo(float x, float y) noexcept {
        m_cx = x;
        m_cy = y;
    }

    float currentX() const noexcept { return m_cx; }
    float currentY() const noexcept { return m_cy; }

    float targetX() const noexcept { return m_tx; }
    float targetY() const noexcept { return m_ty; }

    // -----------------------------------------------------------------------
    // Per-frame update
    // -----------------------------------------------------------------------

    /// Advance the cursor toward the target.
    /// Returns true when motion is still ongoing, false when settled.
    /// Call once per frame; uses the DeltaFn to fetch elapsed time.
    bool update() {
        const float dt = m_delta();
        if (dt <= 0.0f) return !settled();

        const float dx = m_tx - m_cx;
        const float dy = m_ty - m_cy;
        const float dist = std::sqrt(dx * dx + dy * dy);

        if (dist <= m_cfg.settleRadius) {
            m_cx = m_tx;
            m_cy = m_ty;
            return false; // settled
        }

        // Exponential ease-out: remaining fraction after dt seconds.
        // alpha = 1 - e^(-rate * dt)  gives the approached fraction.
        const float alpha = 1.0f - std::exp(-m_cfg.rate * dt);
        float stepX = dx * alpha;
        float stepY = dy * alpha;

        // Clamp step magnitude if requested.
        if (m_cfg.maxStep > 0.0f) {
            const float stepLen = std::sqrt(stepX * stepX + stepY * stepY);
            if (stepLen > m_cfg.maxStep) {
                const float scale = m_cfg.maxStep / stepLen;
                stepX *= scale;
                stepY *= scale;
            }
        }

        m_cx += stepX;
        m_cy += stepY;

        return !settled();
    }

    // -----------------------------------------------------------------------
    // Runtime reconfiguration
    // -----------------------------------------------------------------------

    void setConfig(SmoothConfig cfg) noexcept  { m_cfg = cfg; }
    const SmoothConfig& config() const noexcept { return m_cfg; }

    /// Replace the delta callback (e.g. switch from real-time to replay).
    void setDeltaFn(DeltaFn delta) { m_delta = std::move(delta); }

private:
    bool settled() const noexcept {
        const float dx = m_tx - m_cx;
        const float dy = m_ty - m_cy;
        return (dx * dx + dy * dy) <= (m_cfg.settleRadius * m_cfg.settleRadius);
    }

    SmoothConfig m_cfg;
    DeltaFn      m_delta;

    float m_cx = 0.0f, m_cy = 0.0f; // current position
    float m_tx = 0.0f, m_ty = 0.0f; // target position
};

// ---------------------------------------------------------------------------
// Smoke test
// ---------------------------------------------------------------------------
#ifdef SMOOTH_SMOKE
#include <cstdio>
#include <chrono>
#include <thread>

int main() {
    // Fixed-step delta for deterministic testing.
    const float fixedDt = 1.0f / 60.0f;
    DeltaFn fakeDelta = [=]() -> float { return fixedDt; };

    SmoothConfig cfg;
    cfg.rate         = 10.0f;
    cfg.maxStep      = 20.0f;
    cfg.settleRadius = 0.5f;

    Smoother s(cfg, fakeDelta);
    s.warpTo(0.0f, 0.0f);
    s.setTarget(200.0f, 150.0f);

    std::printf("frame,  cx,       cy,       dist\n");

    for (int i = 0; i < 120; ++i) {
        const bool moving = s.update();
        const float dx = s.targetX() - s.currentX();
        const float dy = s.targetY() - s.currentY();
        const float dist = std::sqrt(dx * dx + dy * dy);
        std::printf("%5d  %8.3f  %8.3f  %8.3f\n",
                    i, s.currentX(), s.currentY(), dist);
        if (!moving) {
            std::printf("settled at frame %d\n", i);
            break;
        }
    }

    // Test mid-flight target change.
    std::printf("\n--- mid-flight retarget ---\n");
    s.warpTo(0.0f, 0.0f);
    s.setTarget(100.0f, 0.0f);
    for (int i = 0; i < 120; ++i) {
        if (i == 10) {
            s.setTarget(300.0f, 200.0f);
            std::printf("retargeted at frame %d\n", i);
        }
        const bool moving = s.update();
        if (!moving) {
            std::printf("settled at frame %d  pos=(%.2f, %.2f)\n",
                        i, s.currentX(), s.currentY());
            break;
        }
    }

    return 0;
}
#endif // SMOOTH_SMOKE
