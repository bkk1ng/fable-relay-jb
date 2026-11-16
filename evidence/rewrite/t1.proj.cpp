// proj.cpp
// World-to-screen projection using a combined 4x4 view-projection matrix
// plus optional camera intrinsics (focal length / principal point).
//
// Build standalone smoke test:
//   cl /EHsc /DPROJ_MAIN proj.cpp
// or
//   g++ -std=c++17 -DPROJ_MAIN proj.cpp -o proj

#include <array>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

using Mat4 = std::array<float, 16>;   // row-major: m[r*4+c]
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

struct CameraIntrinsics {
    float fx;   // focal length x  (pixels)
    float fy;   // focal length y  (pixels)
    float cx;   // principal point x (pixels)
    float cy;   // principal point y (pixels)
};

// ---------------------------------------------------------------------------
// Core projection
// ---------------------------------------------------------------------------

// Transform world position through the view-projection matrix.
// Returns false when the point is at or behind the near plane (clip.w <= 0
// covers both the camera-behind case and degenerate w).
// On true, screen_x / screen_y are in pixel coordinates given the intrinsics.
// depth is the positive eye-space depth (clip.w after the VP transform).
//
// If intrinsics is nullptr the function falls back to NDC->pixel via the
// assumption that NDC [-1,1] maps to [0, 2*cx] x [0, 2*cy] (i.e. cx == w/2,
// cy == h/2, fx == cx, fy == cy for a symmetric pinhole).
[[nodiscard]]
bool WorldToScreen(
    const Mat4&            vp,          // view-projection matrix, row-major
    const Vec3&            world,       // world-space position
    const CameraIntrinsics* intrinsics, // may be nullptr for NDC fallback
    float&                 screen_x,
    float&                 screen_y,
    float&                 depth)
{
    // Multiply: clip = VP * [wx, wy, wz, 1]
    const float wx = world[0], wy = world[1], wz = world[2];

    Vec4 clip = {
        vp[ 0]*wx + vp[ 1]*wy + vp[ 2]*wz + vp[ 3],
        vp[ 4]*wx + vp[ 5]*wy + vp[ 6]*wz + vp[ 7],
        vp[ 8]*wx + vp[ 9]*wy + vp[10]*wz + vp[11],
        vp[12]*wx + vp[13]*wy + vp[14]*wz + vp[15],
    };

    // Behind the near plane / degenerate
    if (clip[3] <= 0.0f) {
        screen_x = 0.0f;
        screen_y = 0.0f;
        depth    = 0.0f;
        return false;
    }

    depth = clip[3];  // positive eye-space depth

    const float inv_w = 1.0f / clip[3];

    if (intrinsics) {
        // The VP matrix already bakes in the view transform.
        // clip.x/w and clip.y/w are NDC.  Convert to pixels via pinhole model:
        //   pixel_x = fx * (clip.x / clip.w) + cx   (if NDC is [-1,1])
        // Most game VP matrices map to NDC [-1,1] with +X right, +Y up.
        // Adjust the sign of y to flip to screen-space (+Y down).
        screen_x =  intrinsics->fx * (clip[0] * inv_w) + intrinsics->cx;
        screen_y = -intrinsics->fy * (clip[1] * inv_w) + intrinsics->cy;
    } else {
        // NDC fallback: treat cx/cy as half-resolution; fx=cx, fy=cy implied.
        // screen = (ndc * 0.5 + 0.5) * resolution
        // Because we lack the resolution here we leave NDC as [-1,1] and the
        // caller must supply intrinsics for pixel coordinates.  As a minimal
        // fallback, return raw NDC in [-1,1] in screen_x/y.
        screen_x =  clip[0] * inv_w;
        screen_y = -clip[1] * inv_w;  // flip Y so +Y is down
    }

    return true;
}

// Convenience overload: takes separate view and projection matrices and
// multiplies them before projecting.  VP = P * V (column-vector convention).
[[nodiscard]]
bool WorldToScreen(
    const Mat4&             view,
    const Mat4&             proj,
    const Vec3&             world,
    const CameraIntrinsics* intrinsics,
    float&                  screen_x,
    float&                  screen_y,
    float&                  depth)
{
    // Build VP = proj * view  (row-major: result[r][c] = sum_k proj[r][k]*view[k][c])
    Mat4 vp{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                vp[r*4+c] += proj[r*4+k] * view[k*4+c];

    return WorldToScreen(vp, world, intrinsics, screen_x, screen_y, depth);
}

// ---------------------------------------------------------------------------
// Standalone smoke test
// ---------------------------------------------------------------------------
#ifdef PROJ_MAIN

#include <cassert>

int main()
{
    // Build a trivial camera looking down -Z at the origin.
    // View: identity (camera at world origin, looking down -Z)
    // Proj: simple symmetric perspective
    //   fovY = 90 deg  => f = 1, aspect = 1  => sx=sy=1
    //   near=0.1  far=100
    const float n = 0.1f, f = 100.0f;
    // Row-major perspective (OpenGL convention, NDC [-1,1]):
    //   [ sx  0   0        0      ]
    //   [  0  sy  0        0      ]
    //   [  0  0  -(f+n)/(f-n)  -2fn/(f-n) ]
    //   [  0  0  -1        0      ]
    // For 90-deg fovY, aspect=1: sx=sy=1
    const float sx = 1.0f, sy = 1.0f;
    const float A  = -(f + n) / (f - n);
    const float B  = -2.0f * f * n / (f - n);

    Mat4 view = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };  // identity
    Mat4 proj = {
        sx, 0,  0,  0,
        0,  sy, 0,  0,
        0,  0,  A,  B,
        0,  0, -1,  0,
    };

    // Camera intrinsics for a 1920x1080 sensor, focal = half-width/half-height
    CameraIntrinsics intr = { 960.0f, 540.0f, 960.0f, 540.0f };

    float sx_out, sy_out, depth;

    // Point directly in front of camera at z=-5 (NDC 0,0 expected)
    Vec3 front = {0.0f, 0.0f, -5.0f};
    bool vis = WorldToScreen(view, proj, front, &intr, sx_out, sy_out, depth);
    assert(vis);
    assert(std::fabsf(sx_out - 960.0f) < 0.5f);
    assert(std::fabsf(sy_out - 540.0f) < 0.5f);
    assert(depth > 0.0f);
    std::printf("front  vis=%d  screen=(%.1f, %.1f)  depth=%.3f\n", vis, sx_out, sy_out, depth);

    // Point behind near plane — must return false
    Vec3 behind = {0.0f, 0.0f, 5.0f};
    vis = WorldToScreen(view, proj, behind, &intr, sx_out, sy_out, depth);
    assert(!vis);
    std::printf("behind vis=%d\n", vis);

    // Off-centre point: world (5, 0, -5)  with sx=1 and aspect=1
    // NDC x = world_x / -world_z = 5/5 = 1.0  => pixel x = fx*1 + cx = 960+960 = 1920
    Vec3 edge = {5.0f, 0.0f, -5.0f};
    vis = WorldToScreen(view, proj, edge, &intr, sx_out, sy_out, depth);
    assert(vis);
    assert(std::fabsf(sx_out - 1920.0f) < 1.0f);
    std::printf("edge   vis=%d  screen=(%.1f, %.1f)  depth=%.3f\n", vis, sx_out, sy_out, depth);

    std::printf("All smoke tests passed.\n");
    return 0;
}
#endif // PROJ_MAIN
