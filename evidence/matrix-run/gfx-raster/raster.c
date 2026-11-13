/*
 * raster.c — C11 software rasterizer
 *
 * Features:
 *   - Barycentric triangle fill with z-buffer
 *   - Perspective-correct UV interpolation
 *   - Flat Lambert shading
 *   - PPM writer
 *
 * main() renders a cube at a "spinning" angle (Y=35°, X=22°) so that
 * three faces are visible via back-face culling, and writes out.ppm.
 *
 * Build:
 *   cc -std=c11 -O2 -o raster raster.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <float.h>

/* ------------------------------------------------------------------ */
/*  Config                                                              */
/* ------------------------------------------------------------------ */

#define WIDTH   640
#define HEIGHT  640

/* ------------------------------------------------------------------ */
/*  Math types                                                          */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z;    } Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float m[4][4];    } Mat4;

/* ------------------------------------------------------------------ */
/*  Vec3                                                                */
/* ------------------------------------------------------------------ */

static inline Vec3 v3(float x, float y, float z)
    { return (Vec3){x, y, z}; }

static inline Vec3 v3add(Vec3 a, Vec3 b)
    { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }

static inline Vec3 v3sub(Vec3 a, Vec3 b)
    { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }

static inline Vec3 v3scale(Vec3 a, float s)
    { return v3(a.x*s, a.y*s, a.z*s); }

static inline float v3dot(Vec3 a, Vec3 b)
    { return a.x*b.x + a.y*b.y + a.z*b.z; }

static inline Vec3 v3cross(Vec3 a, Vec3 b) {
    return v3(a.y*b.z - a.z*b.y,
              a.z*b.x - a.x*b.z,
              a.x*b.y - a.y*b.x);
}

static inline Vec3 v3norm(Vec3 a) {
    float l = sqrtf(v3dot(a, a));
    return l > 1e-8f ? v3scale(a, 1.0f / l) : v3(0, 1, 0);
}

/* ------------------------------------------------------------------ */
/*  Mat4                                                                */
/* ------------------------------------------------------------------ */

static inline Mat4 mat4_identity(void) {
    Mat4 m = {0};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 c = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                c.m[i][j] += a.m[i][k] * b.m[k][j];
    return c;
}

static Vec4 mat4_mulv(Mat4 m, Vec4 v) {
    return (Vec4){
        m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z + m.m[0][3]*v.w,
        m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z + m.m[1][3]*v.w,
        m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z + m.m[2][3]*v.w,
        m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3]*v.w,
    };
}

/* Multiply a point (w=1) and return Vec3, discarding w. */
static inline Vec3 mat4_mulp(Mat4 m, Vec3 p) {
    Vec4 r = mat4_mulv(m, (Vec4){p.x, p.y, p.z, 1.0f});
    return v3(r.x, r.y, r.z);
}

/* Multiply a direction (w=0) — ignores translation. */
static inline Vec3 mat4_muld(Mat4 m, Vec3 d) {
    Vec4 r = mat4_mulv(m, (Vec4){d.x, d.y, d.z, 0.0f});
    return v3(r.x, r.y, r.z);
}

static Mat4 mat4_rotate_y(float a) {
    Mat4 m = mat4_identity();
    float c = cosf(a), s = sinf(a);
    m.m[0][0] =  c; m.m[0][2] = s;
    m.m[2][0] = -s; m.m[2][2] = c;
    return m;
}

static Mat4 mat4_rotate_x(float a) {
    Mat4 m = mat4_identity();
    float c = cosf(a), s = sinf(a);
    m.m[1][1] =  c; m.m[1][2] = -s;
    m.m[2][1] =  s; m.m[2][2] =  c;
    return m;
}

/*
 * Standard right-handed perspective matrix (maps z from [near,far] to [-1,1]).
 * fovy in radians.
 */
static Mat4 mat4_perspective(float fovy, float aspect, float znear, float zfar) {
    Mat4 m = {0};
    float t = tanf(fovy * 0.5f);
    m.m[0][0] = 1.0f / (aspect * t);
    m.m[1][1] = 1.0f / t;
    m.m[2][2] = -(zfar + znear) / (zfar - znear);
    m.m[2][3] = -(2.0f * zfar * znear) / (zfar - znear);
    m.m[3][2] = -1.0f;
    return m;
}

/* Right-handed lookAt. */
static Mat4 mat4_lookat(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = v3norm(v3sub(center, eye));
    Vec3 r = v3norm(v3cross(f, up));
    Vec3 u = v3cross(r, f);
    Mat4 m = mat4_identity();
    m.m[0][0] =  r.x; m.m[0][1] =  r.y; m.m[0][2] =  r.z;
    m.m[1][0] =  u.x; m.m[1][1] =  u.y; m.m[1][2] =  u.z;
    m.m[2][0] = -f.x; m.m[2][1] = -f.y; m.m[2][2] = -f.z;
    m.m[0][3] = -v3dot(r, eye);
    m.m[1][3] = -v3dot(u, eye);
    m.m[2][3] =  v3dot(f, eye);
    return m;
}

/* ------------------------------------------------------------------ */
/*  Framebuffer                                                         */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t r, g, b; } Pixel;

static Pixel fb[HEIGHT][WIDTH];
static float zb[HEIGHT][WIDTH];  /* NDC depth, initialised to +1 (far) */

static void fb_clear(void) {
    memset(fb, 20, sizeof fb);   /* dark background */
    for (int i = 0; i < HEIGHT * WIDTH; i++)
        ((float *)zb)[i] = 1.0f;
}

/* ------------------------------------------------------------------ */
/*  Procedural texture — tiled checkerboard                            */
/* ------------------------------------------------------------------ */

static Pixel tex_sample(float u, float v) {
    /* wrap to [0,1) */
    u = u - floorf(u);
    v = v - floorf(v);
    int cu = (int)(u * 8.0f) & 1;
    int cv = (int)(v * 8.0f) & 1;
    return (cu ^ cv) ? (Pixel){230, 200, 60} : (Pixel){50, 50, 140};
}

/* ------------------------------------------------------------------ */
/*  Rasterizer                                                          */
/* ------------------------------------------------------------------ */

/*
 * Signed 2-D edge function.  Positive when (px,py) is to the left of
 * the directed edge from A to B (counter-clockwise convention).
 */
static inline float edge_fn(float ax, float ay,
                             float bx, float by,
                             float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

typedef struct {
    Vec4  clip;    /* position in clip space (before perspective divide) */
    float u, v;   /* texture coordinates                                 */
} Vert;

/*
 * draw_triangle — rasterise one triangle.
 *
 *   world_n    : face normal in world space (pre-normalised, flat shading)
 *   light_dir  : normalised light direction in world space
 *   shade_base : ambient + per-face Lambert combined to [0,1]
 *
 * Performs back-face culling (screen-space winding), perspective-correct UV
 * interpolation, and depth testing.
 */
static void draw_triangle(Vert v0, Vert v1, Vert v2, float shade) {
    /* ---- perspective divide ---------------------------------------- */
    if (v0.clip.w <= 0 || v1.clip.w <= 0 || v2.clip.w <= 0) return;

    float iw0 = 1.0f / v0.clip.w;
    float iw1 = 1.0f / v1.clip.w;
    float iw2 = 1.0f / v2.clip.w;

    float nx0 = v0.clip.x * iw0, ny0 = v0.clip.y * iw0, nz0 = v0.clip.z * iw0;
    float nx1 = v1.clip.x * iw1, ny1 = v1.clip.y * iw1, nz1 = v1.clip.z * iw1;
    float nx2 = v2.clip.x * iw2, ny2 = v2.clip.y * iw2, nz2 = v2.clip.z * iw2;

    /* ---- viewport transform: NDC -> screen pixels ------------------- */
    float hw = (float)(WIDTH  - 1) * 0.5f;
    float hh = (float)(HEIGHT - 1) * 0.5f;

    float sx0 = (nx0 + 1.0f) * hw,  sy0 = (1.0f - ny0) * hh;
    float sx1 = (nx1 + 1.0f) * hw,  sy1 = (1.0f - ny1) * hh;
    float sx2 = (nx2 + 1.0f) * hw,  sy2 = (1.0f - ny2) * hh;

    /* ---- back-face cull in screen space ----------------------------- */
    float area = edge_fn(sx0, sy0, sx1, sy1, sx2, sy2);
    if (area <= 0.0f) return;   /* CW winding or degenerate */

    float inv_area = 1.0f / area;

    /* ---- bounding box (clamped to framebuffer) ---------------------- */
    int minx = (int)fmaxf(0.0f, floorf(fminf(sx0, fminf(sx1, sx2))));
    int miny = (int)fmaxf(0.0f, floorf(fminf(sy0, fminf(sy1, sy2))));
    int maxx = (int)fminf((float)(WIDTH  - 1), ceilf(fmaxf(sx0, fmaxf(sx1, sx2))));
    int maxy = (int)fminf((float)(HEIGHT - 1), ceilf(fmaxf(sy0, fmaxf(sy1, sy2))));

    /* ---- perspective-correct UV attributes (u/w, v/w) --------------- */
    float u0w = v0.u * iw0,  uv0w = v0.v * iw0;
    float u1w = v1.u * iw1,  uv1w = v1.v * iw1;
    float u2w = v2.u * iw2,  uv2w = v2.v * iw2;

    /* ---- scan -------------------------------------------------------- */
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float pcx = (float)px + 0.5f;
            float pcy = (float)py + 0.5f;

            /* barycentric weights relative to each opposite edge */
            float w0 = edge_fn(sx1, sy1, sx2, sy2, pcx, pcy);
            float w1 = edge_fn(sx2, sy2, sx0, sy0, pcx, pcy);
            float w2 = edge_fn(sx0, sy0, sx1, sy1, pcx, pcy);

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            float l0 = w0 * inv_area;
            float l1 = w1 * inv_area;
            float l2 = w2 * inv_area;

            /* ---- depth test (perspective-correct NDC z) -------------- */
            /*
             * Interpolate z/w linearly in screen space, then divide by 1/w
             * (which is also linearly interpolated) to get the true NDC z.
             */
            float inv_w  = l0 * iw0 + l1 * iw1 + l2 * iw2;
            float z_over_w = l0 * (nz0 * iw0) + l1 * (nz1 * iw1) + l2 * (nz2 * iw2);
            float depth  = z_over_w / inv_w;

            if (depth < -1.0f || depth > 1.0f) continue;
            if (depth >= zb[py][px]) continue;
            zb[py][px] = depth;

            /* ---- perspective-correct UV ------------------------------ */
            float u = (l0 * u0w  + l1 * u1w  + l2 * u2w)  / inv_w;
            float v = (l0 * uv0w + l1 * uv1w + l2 * uv2w) / inv_w;

            /* ---- shade and write ------------------------------------- */
            Pixel t = tex_sample(u, v);
            fb[py][px] = (Pixel){
                (uint8_t)((float)t.r * shade),
                (uint8_t)((float)t.g * shade),
                (uint8_t)((float)t.b * shade),
            };
        }
    }
}

/* ------------------------------------------------------------------ */
/*  PPM writer                                                          */
/* ------------------------------------------------------------------ */

static int write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            Pixel p = fb[y][x];
            fputc(p.r, f);
            fputc(p.g, f);
            fputc(p.b, f);
        }
    }
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Cube geometry                                                       */
/* ------------------------------------------------------------------ */

/*
 * Eight cube corners at ±1.  Index layout:
 *
 *       3----2        (back face, z=+1)
 *      /|   /|
 *     7----6 |
 *     | 0--|-1        (front face, z=-1)
 *     |/   |/
 *     4----5
 *
 *   0=(-1,-1,-1)  1=(+1,-1,-1)  2=(+1,+1,-1)  3=(-1,+1,-1)
 *   4=(-1,-1,+1)  5=(+1,-1,+1)  6=(+1,+1,+1)  7=(-1,+1,+1)
 */
static const Vec3 cube_v[8] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};

/*
 * Six faces; quad indices ordered CCW when viewed from outside,
 * with their outward world-space normal.
 *
 * UVs are assigned at the quad corners in order:
 *   i0=(0,0), i1=(1,0), i2=(1,1), i3=(0,1)
 */
typedef struct {
    int  idx[4];  /* vertex indices into cube_v */
    Vec3 normal;  /* outward face normal (world space, unit length) */
} Face;

static const Face cube_faces[6] = {
    /* front  — z = -1 */  { {0, 1, 2, 3}, { 0, 0,-1} },
    /* back   — z = +1 */  { {5, 4, 7, 6}, { 0, 0, 1} },
    /* right  — x = +1 */  { {1, 5, 6, 2}, { 1, 0, 0} },
    /* left   — x = -1 */  { {4, 0, 3, 7}, {-1, 0, 0} },
    /* top    — y = +1 */  { {3, 2, 6, 7}, { 0, 1, 0} },
    /* bottom — y = -1 */  { {4, 5, 1, 0}, { 0,-1, 0} },
};

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* ---- matrices --------------------------------------------------- */
    /* Rotate the cube so three faces are clearly visible. */
    float angle_y =  0.610865f; /* ≈ 35° */
    float angle_x = -0.383972f; /* ≈ 22° (tilt down slightly) */

    Mat4 model = mat4_mul(mat4_rotate_x(angle_x), mat4_rotate_y(angle_y));

    Vec3 eye    = { 0.0f, 0.0f, 5.0f };
    Vec3 center = { 0.0f, 0.0f, 0.0f };
    Vec3 world_up = { 0.0f, 1.0f, 0.0f };
    Mat4 view   = mat4_lookat(eye, center, world_up);

    float fovy   = 0.785398f; /* 45° in radians */
    float aspect = (float)WIDTH / (float)HEIGHT;
    Mat4 proj    = mat4_perspective(fovy, aspect, 0.1f, 100.0f);

    Mat4 mv  = mat4_mul(view, model);
    Mat4 mvp = mat4_mul(proj, mv);

    /* ---- light ------------------------------------------------------- */
    /* Single directional light; direction in world space. */
    Vec3 light_dir = v3norm(v3(1.0f, 1.4f, -0.8f));

    float ambient = 0.12f;

    /* ---- render ------------------------------------------------------ */
    fb_clear();

    for (int fi = 0; fi < 6; fi++) {
        const Face *face = &cube_faces[fi];

        /* Transform the outward normal into world space (model matrix).
         * Since there is no non-uniform scale, the normal matrix equals
         * the upper-left 3×3 of the model matrix.                        */
        Vec3 wn = v3norm(mat4_muld(model, face->normal));

        /* Back-face cull: skip faces whose world normal points away from
         * the camera (dot product of normal with view direction ≥ 0).    */
        Vec3 face_center = mat4_mulp(model,
            v3scale(v3add(cube_v[face->idx[0]],
                    v3add(cube_v[face->idx[1]],
                    v3add(cube_v[face->idx[2]],
                          cube_v[face->idx[3]]))), 0.25f));

        Vec3 to_eye = v3sub(eye, face_center);
        if (v3dot(wn, to_eye) <= 0.0f) continue;

        /* Lambert shading for this face. */
        float lambert = fmaxf(0.0f, v3dot(wn, light_dir));
        float shade   = ambient + (1.0f - ambient) * lambert;
        if (shade > 1.0f) shade = 1.0f;

        /* UV corners for a quad. */
        static const float quad_u[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        static const float quad_v[4] = {0.0f, 0.0f, 1.0f, 1.0f};

        /* Transform vertices through MVP. */
        Vert tv[4];
        for (int k = 0; k < 4; k++) {
            Vec3 p = cube_v[face->idx[k]];
            tv[k].clip = mat4_mulv(mvp, (Vec4){p.x, p.y, p.z, 1.0f});
            tv[k].u    = quad_u[k];
            tv[k].v    = quad_v[k];
        }

        /* Split quad into two CCW triangles: (0,1,2) and (0,2,3). */
        draw_triangle(tv[0], tv[1], tv[2], shade);
        draw_triangle(tv[0], tv[2], tv[3], shade);
    }

    /* ---- output ------------------------------------------------------ */
    if (write_ppm("out.ppm"))
        puts("wrote out.ppm");
    else
        return 1;

    return 0;
}
