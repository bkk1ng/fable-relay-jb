/*
 * raster.c — software rasterizer
 * C11, no dependencies beyond the C standard library.
 *
 * Features:
 *   - Barycentric triangle fill
 *   - Z-buffer (depth test)
 *   - Perspective-correct UV interpolation
 *   - Lambert diffuse shading
 *   - PPM image writer
 *
 * main() renders a spinning cube's three visible faces to out.ppm.
 *
 * Build:
 *   cc -std=c11 -O2 -o raster raster.c -lm
 * Run:
 *   ./raster
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Config                                                              */
/* ------------------------------------------------------------------ */

#define WIDTH   640
#define HEIGHT  480
#define FOV_DEG 60.0f

/* ------------------------------------------------------------------ */
/*  Math types                                                          */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; }    Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float x, y; }       Vec2;
typedef struct { float m[4][4]; }    Mat4;

typedef struct {
    uint8_t r, g, b;
} Color;

/* ------------------------------------------------------------------ */
/*  Framebuffer + z-buffer                                              */
/* ------------------------------------------------------------------ */

static Color  fb[HEIGHT][WIDTH];
static float  zb[HEIGHT][WIDTH];

static void fb_clear(Color bg) {
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            fb[y][x] = bg;
            zb[y][x] = 1.0f;   /* NDC far plane */
        }
}

/* ------------------------------------------------------------------ */
/*  Vec3 helpers                                                        */
/* ------------------------------------------------------------------ */

static inline Vec3 v3add(Vec3 a, Vec3 b) { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3 v3sub(Vec3 a, Vec3 b) { return (Vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 v3scale(Vec3 v, float s) { return (Vec3){v.x*s, v.y*s, v.z*s}; }
static inline float v3dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 v3cross(Vec3 a, Vec3 b) {
    return (Vec3){ a.y*b.z - a.z*b.y,
                   a.z*b.x - a.x*b.z,
                   a.x*b.y - a.y*b.x };
}
static inline Vec3 v3norm(Vec3 v) {
    float len = sqrtf(v3dot(v, v));
    if (len < 1e-9f) return (Vec3){0,0,0};
    return v3scale(v, 1.0f / len);
}

/* ------------------------------------------------------------------ */
/*  Mat4 helpers                                                        */
/* ------------------------------------------------------------------ */

static Mat4 mat4_identity(void) {
    Mat4 m = {0};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

static Vec4 mat4_mul_vec4(const Mat4 *m, Vec4 v) {
    Vec4 r;
    r.x = m->m[0][0]*v.x + m->m[0][1]*v.y + m->m[0][2]*v.z + m->m[0][3]*v.w;
    r.y = m->m[1][0]*v.x + m->m[1][1]*v.y + m->m[1][2]*v.z + m->m[1][3]*v.w;
    r.z = m->m[2][0]*v.x + m->m[2][1]*v.y + m->m[2][2]*v.z + m->m[2][3]*v.w;
    r.w = m->m[3][0]*v.x + m->m[3][1]*v.y + m->m[3][2]*v.z + m->m[3][3]*v.w;
    return r;
}

static Mat4 mat4_mul(const Mat4 *a, const Mat4 *b) {
    Mat4 r = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a->m[i][k] * b->m[k][j];
    return r;
}

/* Row-major rotation around Y */
static Mat4 mat4_rotate_y(float rad) {
    Mat4 m = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    m.m[0][0] =  c;  m.m[0][2] = s;
    m.m[2][0] = -s;  m.m[2][2] = c;
    return m;
}

/* Row-major rotation around X */
static Mat4 mat4_rotate_x(float rad) {
    Mat4 m = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    m.m[1][1] =  c;  m.m[1][2] = -s;
    m.m[2][1] =  s;  m.m[2][2] =  c;
    return m;
}

/* Perspective projection (symmetric frustum, NDC z in [-1,1]) */
static Mat4 mat4_perspective(float fov_deg, float aspect, float near, float far) {
    Mat4 m = {0};
    float f = 1.0f / tanf(fov_deg * (float)M_PI / 360.0f);
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (far + near) / (near - far);
    m.m[2][3] = (2.0f * far * near) / (near - far);
    m.m[3][2] = -1.0f;
    return m;
}

/* Translation */
static Mat4 mat4_translate(float tx, float ty, float tz) {
    Mat4 m = mat4_identity();
    m.m[0][3] = tx;
    m.m[1][3] = ty;
    m.m[2][3] = tz;
    return m;
}

/* ------------------------------------------------------------------ */
/*  Checkerboard procedural texture                                     */
/* ------------------------------------------------------------------ */

static Color tex_sample(Vec2 uv) {
    /* tile 4×4 */
    int u = (int)(fabsf(uv.x) * 4.0f) & 1;
    int v = (int)(fabsf(uv.y) * 4.0f) & 1;
    return (u ^ v) ? (Color){230, 220, 180} : (Color){60, 55, 50};
}

/* ------------------------------------------------------------------ */
/*  Rasterizer                                                          */
/* ------------------------------------------------------------------ */

/* Integer edge function: (b-a) × (p-a) */
static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/*
 * draw_triangle — perspective-correct barycentric fill.
 *
 * Inputs are clip-space Vec4 vertices (pre-division), their view-space
 * normals for Lambert, and per-vertex UVs.
 *
 * We carry (u/w, v/w, 1/w) through the rasterizer and recover u,v at
 * each fragment via the standard perspective-correct trick.
 */
static void draw_triangle(
    Vec4 c0, Vec4 c1, Vec4 c2,       /* clip-space positions          */
    Vec3 n0, Vec3 n1, Vec3 n2,       /* view-space normals            */
    Vec2 uv0, Vec2 uv1, Vec2 uv2,    /* texture coordinates           */
    Vec3 light_dir                    /* normalised, view-space        */
) {
    /* perspective divide → NDC */
    float inv_w0 = 1.0f / c0.w;
    float inv_w1 = 1.0f / c1.w;
    float inv_w2 = 1.0f / c2.w;

    float nx0 = c0.x * inv_w0, ny0 = c0.y * inv_w0, nz0 = c0.z * inv_w0;
    float nx1 = c1.x * inv_w1, ny1 = c1.y * inv_w1, nz1 = c1.z * inv_w1;
    float nx2 = c2.x * inv_w2, ny2 = c2.y * inv_w2, nz2 = c2.z * inv_w2;

    /* NDC → screen (y flipped so +y is up in NDC, down in image) */
    float hw = WIDTH  * 0.5f;
    float hh = HEIGHT * 0.5f;
    float sx0 = nx0 * hw + hw,  sy0 = -ny0 * hh + hh;
    float sx1 = nx1 * hw + hw,  sy1 = -ny1 * hh + hh;
    float sx2 = nx2 * hw + hw,  sy2 = -ny2 * hh + hh;

    /* Bounding box, clamped to screen */
    int min_x = (int)fmaxf(0.0f,         fminf(sx0, fminf(sx1, sx2)));
    int max_x = (int)fminf(WIDTH  - 1.0f, fmaxf(sx0, fmaxf(sx1, sx2)));
    int min_y = (int)fmaxf(0.0f,         fminf(sy0, fminf(sy1, sy2)));
    int max_y = (int)fminf(HEIGHT - 1.0f, fmaxf(sy0, fmaxf(sy1, sy2)));

    /* Triangle area (double) in screen space */
    float area = edge(sx0, sy0, sx1, sy1, sx2, sy2);
    if (fabsf(area) < 1e-6f) return;   /* degenerate */
    float inv_area = 1.0f / area;

    /* Perspective-correct attribute setup: divide by w now */
    float pu0 = uv0.x * inv_w0, pv0 = uv0.y * inv_w0;
    float pu1 = uv1.x * inv_w1, pv1 = uv1.y * inv_w1;
    float pu2 = uv2.x * inv_w2, pv2 = uv2.y * inv_w2;

    /* Normal components divided by w for perspective-correct interp */
    float pnx0 = n0.x * inv_w0, pny0 = n0.y * inv_w0, pnz0 = n0.z * inv_w0;
    float pnx1 = n1.x * inv_w1, pny1 = n1.y * inv_w1, pnz1 = n1.z * inv_w1;
    float pnx2 = n2.x * inv_w2, pny2 = n2.y * inv_w2, pnz2 = n2.z * inv_w2;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            float px = x + 0.5f, py = y + 0.5f;

            float w0 = edge(sx1, sy1, sx2, sy2, px, py) * inv_area;
            float w1 = edge(sx2, sy2, sx0, sy0, px, py) * inv_area;
            float w2 = 1.0f - w0 - w1;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            /* Interpolate NDC-z for depth test */
            float frag_z = w0 * nz0 + w1 * nz1 + w2 * nz2;
            if (frag_z >= zb[y][x]) continue;   /* depth test */
            zb[y][x] = frag_z;

            /* Perspective-correct 1/w at this fragment */
            float frag_inv_w = w0 * inv_w0 + w1 * inv_w1 + w2 * inv_w2;
            float frag_w = 1.0f / frag_inv_w;

            /* Recover UV */
            float u = (w0 * pu0 + w1 * pu1 + w2 * pu2) * frag_w;
            float v = (w0 * pv0 + w1 * pv1 + w2 * pv2) * frag_w;

            /* Recover normal and re-normalise */
            Vec3 n = {
                (w0 * pnx0 + w1 * pnx1 + w2 * pnx2) * frag_w,
                (w0 * pny0 + w1 * pny1 + w2 * pny2) * frag_w,
                (w0 * pnz0 + w1 * pnz1 + w2 * pnz2) * frag_w,
            };
            n = v3norm(n);

            /* Lambert */
            float ndotl = fmaxf(0.0f, v3dot(n, light_dir));
            float ambient = 0.15f;
            float diffuse = ndotl * 0.85f;
            float intensity = ambient + diffuse;

            /* Texture */
            Color tc = tex_sample((Vec2){u, v});

            /* Shade */
            fb[y][x] = (Color){
                (uint8_t)fminf(255.0f, tc.r * intensity),
                (uint8_t)fminf(255.0f, tc.g * intensity),
                (uint8_t)fminf(255.0f, tc.b * intensity),
            };
        }
    }
}

/* ------------------------------------------------------------------ */
/*  PPM writer                                                          */
/* ------------------------------------------------------------------ */

static int ppm_write(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            fwrite(&fb[y][x], 3, 1, f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Cube geometry                                                        */
/* ------------------------------------------------------------------ */

/*
 * Unit cube centred at origin.  We define six faces; main() picks the
 * three that face toward the camera after rotation, so we never need
 * back-face culling beyond checking the sign of the view-space normal.
 */

typedef struct {
    Vec3 pos[4];    /* quad corners, CCW when viewed from outside */
    Vec3 normal;    /* face normal (object space) */
    Vec2 uv[4];     /* texture coordinates */
} Face;

static const Face cube_faces[6] = {
    /* +Z front */
    { .pos = {{-0.5f,-0.5f, 0.5f},{0.5f,-0.5f, 0.5f},{0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}},
      .normal = {0,0,1},
      .uv = {{0,0},{1,0},{1,1},{0,1}} },
    /* -Z back */
    { .pos = {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}},
      .normal = {0,0,-1},
      .uv = {{0,0},{1,0},{1,1},{0,1}} },
    /* +X right */
    { .pos = {{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f}},
      .normal = {1,0,0},
      .uv = {{0,0},{1,0},{1,1},{0,1}} },
    /* -X left */
    { .pos = {{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f}},
      .normal = {-1,0,0},
      .uv = {{0,0},{1,0},{1,1},{0,1}} },
    /* +Y top */
    { .pos = {{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}},
      .normal = {0,1,0},
      .uv = {{0,0},{1,0},{1,1},{0,1}} },
    /* -Y bottom */
    { .pos = {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}},
      .normal = {0,-1,0},
      .uv = {{0,0},{1,0},{1,1},{0,1}} },
};

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* ---- Rotation: 35° around X, 45° around Y (shows 3 faces) ---- */
    float angle_y = 45.0f * (float)M_PI / 180.0f;
    float angle_x = 35.0f * (float)M_PI / 180.0f;

    Mat4 rot_y  = mat4_rotate_y(angle_y);
    Mat4 rot_x  = mat4_rotate_x(angle_x);
    Mat4 model  = mat4_mul(&rot_x, &rot_y);        /* rot_y first     */
    Mat4 view   = mat4_translate(0.0f, 0.0f, -2.5f);
    Mat4 mv     = mat4_mul(&view, &model);

    float aspect = (float)WIDTH / (float)HEIGHT;
    Mat4 proj   = mat4_perspective(FOV_DEG, aspect, 0.1f, 100.0f);
    Mat4 mvp    = mat4_mul(&proj, &mv);

    /* Light direction in view space — upper-left-front */
    Vec3 light_obj = v3norm((Vec3){ -0.6f, 0.8f, 0.5f });
    Vec4 lv4 = mat4_mul_vec4(&view, (Vec4){light_obj.x, light_obj.y, light_obj.z, 0.0f});
    Vec3 light_dir = v3norm((Vec3){lv4.x, lv4.y, lv4.z});

    fb_clear((Color){30, 30, 35});

    /* ---- Render each face ---- */
    for (int fi = 0; fi < 6; fi++) {
        const Face *f = &cube_faces[fi];

        /* Back-face cull: transform face normal to view space and check z */
        Vec4 nv4 = mat4_mul_vec4(&mv, (Vec4){f->normal.x, f->normal.y, f->normal.z, 0.0f});
        /*
         * We also check that the normal points toward the camera (nz < 0 in
         * right-handed view space where camera looks down -Z).
         */
        if (nv4.z >= 0.0f) continue;

        /*
         * Transform all 4 vertices.  Split the quad into two triangles:
         *   tri0: v0, v1, v2
         *   tri1: v0, v2, v3
         */
        Vec4 clip[4];
        Vec3 view_n[4];   /* per-vertex normals in view space — flat face, so identical */
        for (int vi = 0; vi < 4; vi++) {
            Vec4 p = {f->pos[vi].x, f->pos[vi].y, f->pos[vi].z, 1.0f};
            clip[vi] = mat4_mul_vec4(&mvp, p);
            /* For a hard-edged cube the face normal is uniform; we still
               store it per vertex so the interpolation path stays general. */
            view_n[vi] = (Vec3){nv4.x, nv4.y, nv4.z};
        }

        /* Triangle 0: 0-1-2 */
        draw_triangle(
            clip[0], clip[1], clip[2],
            view_n[0], view_n[1], view_n[2],
            f->uv[0], f->uv[1], f->uv[2],
            light_dir
        );
        /* Triangle 1: 0-2-3 */
        draw_triangle(
            clip[0], clip[2], clip[3],
            view_n[0], view_n[2], view_n[3],
            f->uv[0], f->uv[2], f->uv[3],
            light_dir
        );
    }

    if (ppm_write("out.ppm") == 0)
        printf("wrote out.ppm (%dx%d)\n", WIDTH, HEIGHT);

    return 0;
}
