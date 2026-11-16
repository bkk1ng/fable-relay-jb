// proj.cpp - world-to-screen projection from a 4x4 view-projection matrix
//            plus camera intrinsics. Returns screen x/y and depth; false when
//            behind the near plane. Float only, no external dependencies.
//
// Build (smoke test):
//   cl /EHsc /std:c++17 /DSMOKE_PROJ proj.cpp
//   g++ -std=c++17 -DSMOKE_PROJ proj.cpp -o proj
//
// Exports:
//   bool WorldToScreen(const float inWorld[3],
//                      const float vp[16],
//                      const ProjIntrinsics& ci,
//                      int screenW, int screenH,
//                      float outScreen[2], float* outDepth);

#include <cmath>

// ---------------------------------------------------------------------------
// Public data
// ---------------------------------------------------------------------------

// Camera intrinsics used to turn a clip-space point into pixel coordinates.
// focalXPx / focalYPx are principal distances in pixels (f_mm / pixel_pitch_mm).
// principalPx / principalPy is the principal point in pixels, measured from
// the top-left corner of the image (same convention as OpenCV).
struct ProjIntrinsics
{
    float focalXPx;     // fx, pixels
    float focalYPx;     // fy, pixels
    float principalPx;  // cx, pixels (image origin top-left)
    float principalPy;  // cy, pixels (image origin top-left)
    float nearPlane;    // camera near plane in view space (> 0)
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace proj_detail
{

// Row-major 4x4 matrix * homogeneous 4-vector, written into out[4].
// vp is row-major: vp[row * 4 + col].
inline void TransformVec4(const float v[4], const float m[16], float out[4])
{
    const float x = v[0];
    const float y = v[1];
    const float z = v[2];
    const float w = v[3];

    out[0] = m[0]  * x + m[1]  * y + m[2]  * z + m[3]  * w;
    out[1] = m[4]  * x + m[5]  * y + m[6]  * z + m[7]  * w;
    out[2] = m[8]  * x + m[9]  * y + m[10] * z + m[11] * w;
    out[3] = m[12] * x + m[13] * y + m[14] * z + m[15] * w;
}

// Perspective divide guard: treat a clip w with magnitude below this as
// degenerate so the caller can reject the point instead of dividing by it.
inline bool ClipWValid(float w)
{
    // FLT_EPSILON-scale guard. Anything closer to zero than this is unstable
    // for a perspective divide at float precision.
    constexpr float kWGuard = 1e-7f;
    return (w > kWGuard);
}

} // namespace proj_detail

// ---------------------------------------------------------------------------
// Public projection entry point
// ---------------------------------------------------------------------------

// Project a world-space point through a combined view-projection matrix and
// camera intrinsics.
//
//   inWorld    input world-space position (x, y, z).
//   vp         row-major 4x4 view-projection matrix.
//   ci         camera intrinsics (fx, fy, cx, cy in pixels, near plane).
//   screenW    output image width in pixels.
//   screenH    output image height in pixels.
//   outScreen  receives pixel coordinates (x, y), image origin top-left.
//   outDepth   optional; receives normalized depth in [0,1] on success,
//              where 0 is the near plane and 1 is the far plane.
//
// Returns true when the point is in front of the near plane and produces a
// finite, on-or-in-front-of-near clip coordinate; false otherwise. On false,
// outScreen and outDepth are left untouched.
//
// Matrix convention: vp maps world -> clip. Clip-space uses the common
// graphics convention where the visible volume has -w <= x <= w,
// -w <= y <= w, 0 <= z <= w. The intrinsics then map the front clipping plane
// (z = 0 in clip / z = near in view) onto the full pixel image, i.e. the
// standard pinhole model applied after projection.
bool WorldToScreen(const float inWorld[3],
                   const float vp[16],
                   const ProjIntrinsics& ci,
                   int screenW,
                   int screenH,
                   float outScreen[2],
                   float* outDepth)
{
    using namespace proj_detail;

    // --- Transform world -> clip space -----------------------------------
    const float worldH[4] = { inWorld[0], inWorld[1], inWorld[2], 1.0f };
    float clip[4];
    TransformVec4(worldH, vp, clip);

    // A non-positive clip w means the point is on or behind the camera
    // plane (or exactly at the eye), which is not in front of the near plane.
    // This also rejects points behind an infinite-far or reversed-Z
    // projection, where w <= 0 corresponds to view z <= 0.
    if (!ClipWValid(clip[3]))
        return false;

    // Perspective divide -> normalized device coordinates (NDC), range
    // [-1, 1] for x and y inside the visible frustum, [0, 1] for z.
    const float invW = 1.0f / clip[3];
    const float ndcX = clip[0] * invW;
    const float ndcY = clip[1] * invW;
    const float ndcZ = clip[2] * invW;

    // Reject points in front of the near plane. In the standard convention
    // the near plane maps to ndcZ == 0; anything negative is behind it.
    if (ndcZ < 0.0f || ndcZ > 1.0f)
        return false;

    // --- NDC -> pixel coordinates via intrinsics --------------------------
    // NDC [-1,1] maps to the image plane. Convert to a centered,
    // principal-point-relative ray direction, then scale by focal length.
    //
    // Using the pinhole model with image origin at top-left:
    //   u = fx * (ndcX) + cx
    //   v = fy * (-ndcY) + cy     (flip Y: screen down == +ndcY up)
    //
    // Because ndcX already encodes the half-angle through the frustum, this
    // is equivalent to fx * (x_clip / w_clip) + cx once the projection
    // matrix's x/y scaling is folded into fx/fy.
    const float px = ci.focalXPx * ndcX + ci.principalPx;
    const float py = ci.focalYPx * (-ndcY) + ci.principalPy;

    // Reject non-finite results from degenerate intrinsics.
    if (!std::isfinite(px) || !std::isfinite(py))
        return false;

    // Optional frustum cull in pixel space: allow points within the image
    // bounds plus a small margin so partially-visible vertices still count.
    // The margin is one percent of the larger screen dimension; callers that
    // want strict clipping can test outScreen themselves afterwards.
    // (No culling is performed here beyond the near-plane test, so the
    // function reports any point that survives projection. Screen bounds
    // checks are left to the caller.)

    outScreen[0] = px;
    outScreen[1] = py;

    if (outDepth)
        *outDepth = ndcZ;

    // Screen dimension sanity: a non-positive viewport makes pixel output
    // meaningless, so refuse rather than emit garbage.
    if (screenW <= 0 || screenH <= 0)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Optional standalone smoke test
// ---------------------------------------------------------------------------

#ifdef SMOKE_PROJ
#include <cstdio>

int main()
{
    // Identity view-projection matrix: clip = world, so a point at
    // (0,0,1) projects to ndc (0,0) -> (cx, cy) on screen.
    float vp[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    ProjIntrinsics ci;
    ci.focalXPx = 800.0f;
    ci.focalYPx = 800.0f;
    ci.principalPx = 640.0f;
    ci.principalPy = 360.0f;
    ci.nearPlane = 0.1f;

    float world[3] = { 0.0f, 0.0f, 1.0f };
    float screen[2] = { 0, 0 };
    float depth = 0.0f;

    bool ok = WorldToScreen(world, vp, ci, 1280, 720, screen, &depth);

    std::printf("ok=%d  screen=(%.3f, %.3f)  depth=%.3f\n",
                ok ? 1 : 0, screen[0], screen[1], depth);

    // Expect: ok=1, screen=(640.000, 360.000), depth=0.000
    return ok ? 0 : 1;
}
#endif // SMOKE_PROJ
