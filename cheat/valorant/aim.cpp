// aim.cpp
// UE4 head projection + smoothed crosshair delta, 240 px cone.
// Target : Valorant local test client (UE4, Windows x64)
// Standard: C++17
//
// MSVC  : cl /std:c++17 /O2 aim.cpp
// Clang : clang-cl /std:c++17 /O2 aim.cpp   -or-
//         clang++ -std=c++17 -O2 aim.cpp -o aim   (mingw / llvm-on-win)
//
// Standalone smoke test:
//   cl /std:c++17 /DAIM_STANDALONE_TEST aim.cpp && aim.exe

#include <algorithm>   // std::clamp
#include <cmath>       // std::sqrtf
#include <cstdint>     // uint32_t, etc.
#include <limits>      // std::numeric_limits
#include <optional>    // std::optional / std::nullopt
#include <vector>      // std::vector

// ---------------------------------------------------------------------------
// Math primitives – names and memory layout match UE4 SDK dumps
// ---------------------------------------------------------------------------

struct FVector2D {
    float X{0.f}, Y{0.f};

    constexpr FVector2D operator+(const FVector2D& o) const noexcept { return {X + o.X, Y + o.Y}; }
    constexpr FVector2D operator-(const FVector2D& o) const noexcept { return {X - o.X, Y - o.Y}; }
    constexpr FVector2D operator*(float s)             const noexcept { return {X * s,   Y * s};   }

    float LengthSq() const noexcept { return X * X + Y * Y; }
    float Length()   const noexcept { return std::sqrt(LengthSq()); }
};

struct FVector {
    float X{0.f}, Y{0.f}, Z{0.f};
};

// Row-major 4×4 matrix matching UE4 FMatrix memory layout.
// Vectors are treated as row vectors; multiply on the right:
//   out = vec × M
struct FMatrix {
    float M[4][4]{};
};

// ---------------------------------------------------------------------------
// Per-frame view state – sourced from FSceneView / FMinimalViewInfo each tick
// ---------------------------------------------------------------------------

struct FViewState {
    FMatrix ViewProjection;   // combined view×projection matrix from LocalPlayer
    float   ScreenWidth;      // viewport pixel width
    float   ScreenHeight;     // viewport pixel height
};

// ---------------------------------------------------------------------------
// Entity descriptor – head world position pre-resolved from the bone cache
// (UE4 SkeletalMeshComponent::GetBoneLocation or equivalent SDK call)
// ---------------------------------------------------------------------------

struct FEntityState {
    FVector HeadWorldPos;
    bool    IsAlive{false};
    bool    IsEnemy{false};
};

// ---------------------------------------------------------------------------
// Persistent smoothing state; allocate one per aim session, pass by ref
// ---------------------------------------------------------------------------

struct FAimState {
    FVector2D SmoothedDelta{};   // carries the last smoothed result across frames
};

// ---------------------------------------------------------------------------
// Internal: project a world-space point to window-space pixels.
//
// UE4 row-vector × matrix (TransformFVector4):
//   clip.x = X*M[0][0] + Y*M[1][0] + Z*M[2][0] + M[3][0]
//   clip.y = X*M[0][1] + Y*M[1][1] + Z*M[2][1] + M[3][1]
//   clip.w = X*M[0][3] + Y*M[1][3] + Z*M[2][3] + M[3][3]
//
// NDC → pixel  (UE4 clip-Y points up, screen-Y points down):
//   px = (1 + clip.x / clip.w) * 0.5 * W
//   py = (1 - clip.y / clip.w) * 0.5 * H
// ---------------------------------------------------------------------------
static std::optional<FVector2D> WorldToScreen(
    const FVector&    world,
    const FViewState& view) noexcept
{
    const auto& m = view.ViewProjection.M;

    const float cx = world.X * m[0][0] + world.Y * m[1][0] + world.Z * m[2][0] + m[3][0];
    const float cy = world.X * m[0][1] + world.Y * m[1][1] + world.Z * m[2][1] + m[3][1];
    const float cw = world.X * m[0][3] + world.Y * m[1][3] + world.Z * m[2][3] + m[3][3];

    if (cw < 0.001f)   // behind or on the near plane – no valid projection
        return std::nullopt;

    const float rhw = 1.f / cw;
    return FVector2D{
        (1.f + cx * rhw) * 0.5f * view.ScreenWidth,
        (1.f - cy * rhw) * 0.5f * view.ScreenHeight
    };
}

// ---------------------------------------------------------------------------
// Internal: single-step exponential smoothing toward a target.
//
//   out = current + (target - current) * factor
//
// factor 1.0 → instant snap
// factor ~0.1–0.3 → visible smoothing arc (humanised feel)
// ---------------------------------------------------------------------------
static FVector2D SmoothDelta(
    const FVector2D& current,
    const FVector2D& target,
    float            factor) noexcept
{
    factor = std::clamp(factor, 0.01f, 1.0f);
    return FVector2D{
        current.X + (target.X - current.X) * factor,
        current.Y + (target.Y - current.Y) * factor
    };
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct FAimConfig {
    float ConePxRadius{240.f};   // screen-space cone radius, pixels
    float SmoothFactor{0.25f};   // exponential smooth weight  (0.01 – 1.0)
};

struct FAimResult {
    FVector2D Delta;          // smoothed pixel delta: +X → right, +Y → down
    bool      TargetFound;    // false when no live enemy is inside the cone
};

// Project every live enemy head to screen, select the one closest to the
// crosshair inside ConePxRadius, smooth the delta, and return it.
//
// Call once per frame.  `state` accumulates the smoothed value across frames.
FAimResult ComputeAimDelta(
    const FViewState&                view,
    const std::vector<FEntityState>& entities,
    const FAimConfig&                cfg,
    FAimState&                       state)
{
    const FVector2D crosshair{view.ScreenWidth * 0.5f, view.ScreenHeight * 0.5f};

    // Track the best (closest-to-crosshair) screen-space head inside the cone.
    FVector2D bestScreen{};
    float     bestDistSq = cfg.ConePxRadius * cfg.ConePxRadius;   // inclusive threshold
    bool      found      = false;

    for (const auto& ent : entities) {
        if (!ent.IsAlive || !ent.IsEnemy)
            continue;

        const auto screen = WorldToScreen(ent.HeadWorldPos, view);
        if (!screen)
            continue;

        // Discard heads that project outside the visible viewport.
        if (screen->X < 0.f || screen->X > view.ScreenWidth  ||
            screen->Y < 0.f || screen->Y > view.ScreenHeight)
            continue;

        const float distSq = (*screen - crosshair).LengthSq();

        if (distSq <= bestDistSq) {
            bestDistSq = distSq;
            bestScreen = *screen;
            found      = true;
        }
    }

    if (!found) {
        // No target in cone – decay the stored delta back toward zero so the
        // cursor returns to neutral rather than freezing in place.
        state.SmoothedDelta = SmoothDelta(state.SmoothedDelta, FVector2D{}, cfg.SmoothFactor);
        return {state.SmoothedDelta, false};
    }

    // Raw pixel delta from crosshair to the winning head.
    const FVector2D rawDelta = bestScreen - crosshair;

    // Smooth toward the raw delta and store for the next frame.
    state.SmoothedDelta = SmoothDelta(state.SmoothedDelta, rawDelta, cfg.SmoothFactor);

    return {state.SmoothedDelta, true};
}

// ---------------------------------------------------------------------------
// Standalone smoke test
// Compile with /DAIM_STANDALONE_TEST to include main().
// ---------------------------------------------------------------------------
#ifdef AIM_STANDALONE_TEST

#include <cstdio>

int main() {
    // Build a minimal view state.
    // The identity VP maps NDC directly: a world point at (ndcX, ndcY, 0)
    // lands on screen at ((1+ndcX)*W/2, (1-ndcY)*H/2).
    FViewState view{};
    view.ScreenWidth  = 1920.f;
    view.ScreenHeight = 1080.f;
    view.ViewProjection.M[0][0] = 1.f;
    view.ViewProjection.M[1][1] = 1.f;
    view.ViewProjection.M[2][2] = 1.f;
    view.ViewProjection.M[3][3] = 1.f;

    // Place an enemy head 80 px right, 40 px above the crosshair in screen
    // space.  With the identity VP, that means world coords:
    //   X_ndc =  80 / (W/2) = 80/960 ≈ 0.0833   → world.X ≈ 0.0833
    //   Y_ndc =  40 / (H/2) = 40/540 ≈ 0.0741   → world.Y ≈ 0.0741  (Y up in clip)
    FEntityState ent{};
    ent.HeadWorldPos = {80.f / (view.ScreenWidth  * 0.5f),
                        40.f / (view.ScreenHeight * 0.5f),
                        0.f};
    ent.IsAlive = true;
    ent.IsEnemy = true;

    std::vector<FEntityState> entities{ent};

    FAimConfig cfg{};   // 240 px cone, 0.25 smooth
    FAimState  state{};

    std::printf("%-6s  %-6s  %-10s  %-10s\n", "frame", "found", "delta.X", "delta.Y");
    for (int f = 0; f < 8; ++f) {
        const auto r = ComputeAimDelta(view, entities, cfg, state);
        std::printf("%-6d  %-6d  %-10.3f  %-10.3f\n",
                    f, static_cast<int>(r.TargetFound), r.Delta.X, r.Delta.Y);
    }

    // Drop the enemy; delta should decay toward zero.
    entities.clear();
    std::printf("--- target removed ---\n");
    for (int f = 0; f < 4; ++f) {
        const auto r = ComputeAimDelta(view, entities, cfg, state);
        std::printf("%-6d  %-6d  %-10.3f  %-10.3f\n",
                    f, static_cast<int>(r.TargetFound), r.Delta.X, r.Delta.Y);
    }

    return 0;
}

#endif // AIM_STANDALONE_TEST
