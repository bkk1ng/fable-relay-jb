// aim.cpp
// Projects entity head positions to screen, returns a smoothed crosshair
// delta inside a 240 px cone, with per-axis gain and a dead zone.
//
// Build (standalone smoke test):
//   cl /std:c++17 /DAIM_SMOKE /W4 /O2 aim.cpp /Fe:aim_test.exe
//
// Exported logic:
//   AimInit          – store screen dimensions and tuning parameters
//   AimProject       – world-space head pos -> screen-space pixel coords
//   AimComputeDelta  – screen target -> smoothed delta (honours cone/dead zone)
//   AimReset         – clear smoothing state

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

// View / projection parameters supplied by the caller each frame.
struct AimViewParams
{
    float viewMatrix[16]; // column-major 4x4 view matrix
    float projMatrix[16]; // column-major 4x4 projection matrix
    int   screenW;
    int   screenH;
};

// Tuning knobs.
struct AimConfig
{
    float coneRadiusPx;  // only targets within this radius are considered
    float gainX;         // multiplier applied to raw X delta before smoothing
    float gainY;         // multiplier applied to raw Y delta before smoothing
    float deadZonePx;    // deltas smaller than this magnitude are zeroed
    float smoothFactor;  // exponential smoothing [0..1]; 1 = no smoothing
};

// Output of AimComputeDelta.
struct AimDelta
{
    float dx;
    float dy;
    bool  inCone; // false → target outside cone; dx/dy are 0
};

// ---------------------------------------------------------------------------
// Module state  (single-instance; protect with a critical section if needed)
// ---------------------------------------------------------------------------

static AimConfig  g_cfg  = { 240.0f, 1.0f, 1.0f, 1.5f, 0.35f };
static int        g_scrW = 1920;
static int        g_scrH = 1080;
static float      g_smoothX = 0.0f;
static float      g_smoothY = 0.0f;

// ---------------------------------------------------------------------------
// AimInit
// ---------------------------------------------------------------------------
// Store screen dimensions and per-session tuning parameters.
// Call once before any other function, or whenever settings change.
void AimInit(int screenW, int screenH, const AimConfig& cfg)
{
    g_scrW   = (screenW  > 0) ? screenW  : 1;
    g_scrH   = (screenH  > 0) ? screenH  : 1;
    g_cfg    = cfg;
    g_smoothX = 0.0f;
    g_smoothY = 0.0f;
}

// ---------------------------------------------------------------------------
// Internal: multiply 4x4 column-major matrix by a Vec4
// ---------------------------------------------------------------------------
static void Mat4MulVec4(const float m[16],
                        float ix, float iy, float iz, float iw,
                        float& ox, float& oy, float& oz, float& ow)
{
    ox = m[0]*ix + m[4]*iy + m[8] *iz + m[12]*iw;
    oy = m[1]*ix + m[5]*iy + m[9] *iz + m[13]*iw;
    oz = m[2]*ix + m[6]*iy + m[10]*iz + m[14]*iw;
    ow = m[3]*ix + m[7]*iy + m[11]*iz + m[15]*iw;
}

// ---------------------------------------------------------------------------
// AimProject
// ---------------------------------------------------------------------------
// Transform a world-space head position through view + projection matrices
// into screen-space pixel coordinates.
// Returns false when the point is behind the near plane (w <= 0).
bool AimProject(const Vec3& worldPos, const AimViewParams& vp, Vec2& screenOut)
{
    // View transform
    float vx, vy, vz, vw;
    Mat4MulVec4(vp.viewMatrix,
                worldPos.x, worldPos.y, worldPos.z, 1.0f,
                vx, vy, vz, vw);

    // Projection transform
    float cx, cy, cz, cw;
    Mat4MulVec4(vp.projMatrix,
                vx, vy, vz, vw,
                cx, cy, cz, cw);

    if (cw <= 0.0f)
        return false; // behind camera

    // Perspective divide -> NDC in [-1, 1]
    float ndcX =  cx / cw;
    float ndcY =  cy / cw;

    // NDC to pixel (Y axis flipped: NDC +1 is top of screen)
    screenOut.x = ( ndcX * 0.5f + 0.5f) * static_cast<float>(vp.screenW);
    screenOut.y = (-ndcY * 0.5f + 0.5f) * static_cast<float>(vp.screenH);

    return true;
}

// ---------------------------------------------------------------------------
// AimComputeDelta
// ---------------------------------------------------------------------------
// Given a projected screen-space target position and the current crosshair
// centre (usually screenW/2, screenH/2), compute a smoothed, gained delta.
//
//   targetScreen – pixel coords of the entity's head after AimProject
//   crosshair    – current cursor / crosshair pixel position
//
// The function:
//   1. Rejects targets outside the configured cone radius.
//   2. Applies per-axis gain.
//   3. Applies exponential smoothing against the previous frame's output.
//   4. Zeroes the output when the smoothed magnitude is below the dead zone.
AimDelta AimComputeDelta(const Vec2& targetScreen, const Vec2& crosshair)
{
    AimDelta result = { 0.0f, 0.0f, false };

    float rawDx = targetScreen.x - crosshair.x;
    float rawDy = targetScreen.y - crosshair.y;

    float dist = static_cast<float>(
        sqrt(static_cast<double>(rawDx * rawDx + rawDy * rawDy)));

    // Cone check
    if (dist > g_cfg.coneRadiusPx)
    {
        // Reset smoothing so we don't bleed into the next valid target.
        g_smoothX = 0.0f;
        g_smoothY = 0.0f;
        return result;
    }

    result.inCone = true;

    // Per-axis gain
    float gainedX = rawDx * g_cfg.gainX;
    float gainedY = rawDy * g_cfg.gainY;

    // Exponential smoothing:  s_n = alpha * gained + (1 - alpha) * s_{n-1}
    float alpha = std::clamp(g_cfg.smoothFactor, 0.0f, 1.0f);
    g_smoothX = alpha * gainedX + (1.0f - alpha) * g_smoothX;
    g_smoothY = alpha * gainedY + (1.0f - alpha) * g_smoothY;

    // Dead zone on the smoothed magnitude
    float smoothMag = static_cast<float>(
        sqrt(static_cast<double>(g_smoothX * g_smoothX + g_smoothY * g_smoothY)));
    if (smoothMag < g_cfg.deadZonePx)
        return result; // dx/dy remain 0, inCone is true

    result.dx = g_smoothX;
    result.dy = g_smoothY;
    return result;
}

// ---------------------------------------------------------------------------
// AimReset
// ---------------------------------------------------------------------------
// Clear accumulated smoothing state (call when switching targets or on
// loss-of-lock so stale velocity doesn't carry over).
void AimReset()
{
    g_smoothX = 0.0f;
    g_smoothY = 0.0f;
}

// ---------------------------------------------------------------------------
// Standalone smoke test
// ---------------------------------------------------------------------------
#ifdef AIM_SMOKE

#include <cstdio>

// Minimal identity matrices for a flat 2-D projection sanity check.
static void IdentityMat(float m[16])
{
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

int main()
{
    AimConfig cfg;
    cfg.coneRadiusPx = 240.0f;
    cfg.gainX        = 1.2f;
    cfg.gainY        = 1.0f;
    cfg.deadZonePx   = 1.5f;
    cfg.smoothFactor = 0.4f;

    AimInit(1920, 1080, cfg);

    // Build a view/proj that maps world coords 1:1 to NDC for unit testing.
    AimViewParams vp;
    vp.screenW = 1920;
    vp.screenH = 1080;
    IdentityMat(vp.viewMatrix);
    IdentityMat(vp.projMatrix);

    // A head at world (0, 0, 0) should project to screen centre with identity
    // matrices only when NDC origin = (0,0).  Offset slightly to get a delta.
    Vec3 head = { 0.05f, -0.03f, 0.0f }; // small offset in NDC units
    Vec2 projected;
    bool ok = AimProject(head, vp, projected);
    printf("AimProject ok=%d  screen=(%.1f, %.1f)\n",
           (int)ok, projected.x, projected.y);

    Vec2 crosshair = { 960.0f, 540.0f }; // screen centre

    // Simulate several frames of smoothing.
    for (int frame = 0; frame < 8; ++frame)
    {
        AimDelta d = AimComputeDelta(projected, crosshair);
        printf("frame %d  inCone=%d  dx=%.3f  dy=%.3f\n",
               frame, (int)d.inCone, d.dx, d.dy);
    }

    // Test cone rejection: target far outside 240 px radius.
    Vec2 farTarget = { 1800.0f, 100.0f };
    AimDelta rejected = AimComputeDelta(farTarget, crosshair);
    printf("cone rejection inCone=%d (expect 0)\n", (int)rejected.inCone);

    AimReset();
    printf("AimReset OK\n");
    return 0;
}

#endif // AIM_SMOKE
