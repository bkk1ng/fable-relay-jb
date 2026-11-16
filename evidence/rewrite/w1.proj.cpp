// proj.cpp
// World-to-screen projection using a 4x4 view-projection matrix.
// Returns true when the point is in front of the near plane and sets sx, sy, depth.
// Float only, no external dependencies.

#pragma once

#include <cmath>

// Row-major 4x4 matrix: m[row][col]
struct Mat4 {
    float m[4][4];
};

struct Vec3 {
    float x, y, z;
};

struct Vec4 {
    float x, y, z, w;
};

// Multiply a Vec4 by a 4x4 matrix (vec treated as column vector: M * v).
static inline Vec4 mat4_mul_vec4(const Mat4& M, const Vec4& v) {
    Vec4 out{};
    out.x = M.m[0][0]*v.x + M.m[0][1]*v.y + M.m[0][2]*v.z + M.m[0][3]*v.w;
    out.y = M.m[1][0]*v.x + M.m[1][1]*v.y + M.m[1][2]*v.z + M.m[1][3]*v.w;
    out.z = M.m[2][0]*v.x + M.m[2][1]*v.y + M.m[2][2]*v.z + M.m[2][3]*v.w;
    out.w = M.m[3][0]*v.x + M.m[3][1]*v.y + M.m[3][2]*v.z + M.m[3][3]*v.w;
    return out;
}

// world_to_screen
//
// Parameters:
//   vp          - combined view-projection matrix (column-vector convention, M * v)
//   world       - world-space position to project
//   screen_w    - viewport width in pixels (from camera intrinsics)
//   screen_h    - viewport height in pixels
//   out_sx      - output screen X in [0, screen_w]
//   out_sy      - output screen Y in [0, screen_h]  (top-left origin; flip sign if
//                 your coordinate system puts Y up)
//   out_depth   - output linear eye-space depth (positive = in front of camera)
//
// Returns false when the point is behind or on the near plane (w <= 0 after
// transform), so the caller can skip drawing it.
//
inline bool world_to_screen(
    const Mat4& vp,
    const Vec3& world,
    float       screen_w,
    float       screen_h,
    float&      out_sx,
    float&      out_sy,
    float&      out_depth)
{
    // Transform to clip space.
    Vec4 clip = mat4_mul_vec4(vp, { world.x, world.y, world.z, 1.0f });

    // w <= 0 means behind (or on) the near plane.
    if (clip.w <= 0.0f) {
        out_sx    = 0.0f;
        out_sy    = 0.0f;
        out_depth = clip.w;
        return false;
    }

    float inv_w = 1.0f / clip.w;

    // Normalized device coordinates in [-1, 1].
    float ndcx =  clip.x * inv_w;
    float ndcy = -clip.y * inv_w;   // flip Y: NDC +1 is top, screen +Y is down

    // Viewport transform to pixel coordinates.
    out_sx    = (ndcx + 1.0f) * 0.5f * screen_w;
    out_sy    = (ndcy + 1.0f) * 0.5f * screen_h;
    out_depth =  clip.w;             // positive eye-space depth

    return true;
}


// ---------------------------------------------------------------------------
// Smoke-test entry point (compile with -DPROJ_SMOKE_TEST).
// ---------------------------------------------------------------------------
#ifdef PROJ_SMOKE_TEST

#include <cstdio>
#include <cstring>

// Build a simple perspective projection matrix for the test.
// fovy_rad: vertical field of view in radians
// aspect:   width / height
// znear, zfar: clip planes
static Mat4 make_perspective(float fovy_rad, float aspect, float znear, float zfar)
{
    float f   = 1.0f / std::tan(fovy_rad * 0.5f);
    float dz  = znear - zfar;
    Mat4 M{};
    std::memset(&M, 0, sizeof(M));
    M.m[0][0] = f / aspect;
    M.m[1][1] = f;
    M.m[2][2] = (zfar + znear) / dz;
    M.m[2][3] = (2.0f * zfar * znear) / dz;
    M.m[3][2] = -1.0f;
    return M;
}

// Identity view (camera at origin, looking down -Z).
static Mat4 make_identity()
{
    Mat4 M{};
    std::memset(&M, 0, sizeof(M));
    M.m[0][0] = M.m[1][1] = M.m[2][2] = M.m[3][3] = 1.0f;
    return M;
}

// Multiply two 4x4 matrices: out = A * B
static Mat4 mat4_mul(const Mat4& A, const Mat4& B)
{
    Mat4 C{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            C.m[r][c] = 0.0f;
            for (int k = 0; k < 4; ++k)
                C.m[r][c] += A.m[r][k] * B.m[k][c];
        }
    return C;
}

int main()
{
    const float W = 1920.0f, H = 1080.0f;
    const float fovy = 3.14159265f / 2.0f;   // 90 degrees
    const float near = 0.1f, far = 1000.0f;

    Mat4 view = make_identity();
    Mat4 proj = make_perspective(fovy, W / H, near, far);
    Mat4 vp   = mat4_mul(proj, view);

    struct TestCase {
        Vec3        pos;
        const char* label;
    } cases[] = {
        {{ 0.0f,  0.0f, -10.0f}, "centre, 10 units ahead"},
        {{ 5.0f,  0.0f, -10.0f}, "right of centre"},
        {{ 0.0f,  5.0f, -10.0f}, "above centre"},
        {{ 0.0f,  0.0f,  10.0f}, "behind camera (expect false)"},
        {{ 0.0f,  0.0f, -0.05f}, "inside near plane (expect false)"},
    };

    bool all_ok = true;
    for (auto& tc : cases) {
        float sx = 0.0f, sy = 0.0f, depth = 0.0f;
        bool visible = world_to_screen(vp, tc.pos, W, H, sx, sy, depth);
        std::printf("[%s]\n  visible=%s  sx=%.1f  sy=%.1f  depth=%.4f\n\n",
                    tc.label,
                    visible ? "true" : "false",
                    sx, sy, depth);
        // Basic sanity: the two behind-camera cases must return false.
        if ((tc.pos.z > 0.0f || tc.pos.z > -near) && visible) {
            std::printf("  FAIL: expected not-visible\n");
            all_ok = false;
        }
    }

    return all_ok ? 0 : 1;
}

#endif // PROJ_SMOKE_TEST
