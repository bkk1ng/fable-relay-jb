// proj.cpp
// World-to-screen projection using a 4x4 row-major view-projection matrix.
//
// Exported interface:
//   bool Project(const float vp[16], float vpW, float vpH,
//                float wx, float wy, float wz,
//                float& sx, float& sy, float& depth);
//
//   struct CameraIntrinsics { float fovYDeg, aspect; };
//   void BuildViewProjection(const CameraIntrinsics& cam,
//                            const float view[16], float out[16]);

#pragma once
#include <cmath>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Core projection
// ---------------------------------------------------------------------------

/// Project a 3-D world point through a row-major 4×4 view-projection matrix.
///
/// @param vp      Row-major 4×4 matrix (row i, col j  =>  vp[i*4+j]).
/// @param vpW     Viewport width  in pixels.
/// @param vpH     Viewport height in pixels.
/// @param wx/wy/wz  World-space position.
/// @param sx      [out] Screen X in [0, vpW).
/// @param sy      [out] Screen Y in [0, vpH).
/// @param depth   [out] NDC Z value (0 = near, 1 = far for a standard D3D-style
///                matrix; negative means behind near plane).
/// @return        true  => point is in front of the camera (w > 0 and depth >= 0).
///                false => point is behind or on the near plane.
inline bool Project(
    const float vp[16],
    float vpW, float vpH,
    float wx, float wy, float wz,
    float& sx, float& sy, float& depth)
{
    // Multiply [wx, wy, wz, 1] by the row-major matrix.
    // clip = vp * worldPos   (column vector, right-multiplication)
    // Row-major layout: element (row r, col c) = vp[r*4+c]
    // clip.x = row0 · pos,  clip.y = row1 · pos, ...
    const float cx = vp[0]*wx + vp[1]*wy + vp[2]*wz + vp[3];
    const float cy = vp[4]*wx + vp[5]*wy + vp[6]*wz + vp[7];
    const float cz = vp[8]*wx + vp[9]*wy + vp[10]*wz + vp[11];
    const float cw = vp[12]*wx + vp[13]*wy + vp[14]*wz + vp[15];

    // Behind camera (or exactly on the near plane) — reject.
    if (cw <= 0.0f)
        return false;

    const float invW = 1.0f / cw;

    // NDC in [-1, 1] (X right, Y up in standard clip space).
    const float ndcX = cx * invW;
    const float ndcY = cy * invW;
    const float ndcZ = cz * invW;

    // Reject if outside the near/far clip range [0, 1] (D3D convention).
    // For an OpenGL-style matrix where near/far map to [-1,1],
    // change the bounds to -1 and 1 accordingly.
    depth = ndcZ;
    if (depth < 0.0f || depth > 1.0f)
        return false;

    // Map NDC → pixel coordinates.
    // NDC (-1,+1) → pixel (0, vpW) with Y flipped (NDC +1 = top).
    sx = ( ndcX * 0.5f + 0.5f) * vpW;
    sy = (-ndcY * 0.5f + 0.5f) * vpH;

    return true;
}

// ---------------------------------------------------------------------------
// Camera intrinsics helpers
// ---------------------------------------------------------------------------

struct CameraIntrinsics
{
    float fovYDeg;  ///< Vertical field of view in degrees.
    float aspect;   ///< Width / height.
};

/// Build a right-handed, row-major perspective projection matrix
/// with depth mapped to [0, 1] (DirectX / Vulkan convention).
/// Combine with a separate view matrix to get a full VP matrix.
///
/// @param fovYDeg  Vertical FOV in degrees.
/// @param aspect   Viewport width / height.
/// @param zNear    Near clip distance (positive).
/// @param zFar     Far  clip distance (positive, > zNear).
/// @param out      [out] Row-major 4×4 matrix.
inline void BuildPerspective(
    float fovYDeg, float aspect,
    float zNear, float zFar,
    float out[16])
{
    const float fovY = fovYDeg * (3.14159265358979323846f / 180.0f);
    const float tanHalfFov = std::tan(fovY * 0.5f);

    const float xScale = 1.0f / (aspect * tanHalfFov);
    const float yScale = 1.0f / tanHalfFov;
    const float zRange = zFar - zNear;

    // Row-major, right-handed, depth [0,1]:
    //   [ xScale   0        0                  0            ]
    //   [  0      yScale    0                  0            ]
    //   [  0       0      -zFar/zRange     -zNear*zFar/zRange ]
    //   [  0       0       -1                  0            ]
    std::memset(out, 0, 16 * sizeof(float));
    out[0]  =  xScale;
    out[5]  =  yScale;
    out[10] = -zFar / zRange;
    out[11] = -(zNear * zFar) / zRange;
    out[14] = -1.0f;
}

/// Multiply two row-major 4×4 matrices: result = a * b.
inline void MatMul4x4(const float a[16], const float b[16], float result[16])
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k)
                v += a[r*4+k] * b[k*4+c];
            result[r*4+c] = v;
        }
}

/// Convenience: build a full view-projection matrix from intrinsics + a
/// pre-built row-major view matrix.
///
/// @param cam   FOV-Y (degrees) and aspect ratio.
/// @param view  Row-major 4×4 view  matrix (world → camera).
/// @param out   [out] Row-major 4×4 view-projection matrix.
inline void BuildViewProjection(
    const CameraIntrinsics& cam,
    const float view[16],
    float out[16],
    float zNear = 0.1f,
    float zFar  = 1000.0f)
{
    float proj[16];
    BuildPerspective(cam.fovYDeg, cam.aspect, zNear, zFar, proj);
    MatMul4x4(proj, view, out);   // VP = P * V
}

// ---------------------------------------------------------------------------
// Smoke test (compiled only when PROJ_SMOKE_TEST is defined)
// ---------------------------------------------------------------------------
#ifdef PROJ_SMOKE_TEST
#include <cstdio>
#include <cassert>

int main()
{
    // Build an identity view (camera at origin, looking down -Z).
    float view[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };

    CameraIntrinsics cam { 90.0f, 16.0f / 9.0f };

    float vp[16];
    BuildViewProjection(cam, view, vp, 0.1f, 1000.0f);

    // A point directly in front at z = -10 (right-handed, -Z is forward).
    float sx = 0.f, sy = 0.f, depth = 0.f;
    bool visible = Project(vp, 1920.f, 1080.f, 0.f, 0.f, -10.f, sx, sy, depth);

    std::printf("visible=%d  sx=%.1f sy=%.1f depth=%.4f\n",
                (int)visible, sx, sy, depth);
    assert(visible);
    // Centre of viewport at ±1px tolerance.
    assert(sx > 959.f && sx < 961.f);
    assert(sy > 539.f && sy < 541.f);

    // A point behind the camera — must be rejected.
    bool behind = Project(vp, 1920.f, 1080.f, 0.f, 0.f, 10.f, sx, sy, depth);
    std::printf("behind=%d (expected 0)\n", (int)behind);
    assert(!behind);

    std::puts("All assertions passed.");
    return 0;
}
// Build: cl /DPROJ_SMOKE_TEST /std:c++17 /O2 proj.cpp /Fe:proj_test.exe
// Or:    g++ -DPROJ_SMOKE_TEST -std=c++17 -O2 proj.cpp -o proj_test
#endif // PROJ_SMOKE_TEST
