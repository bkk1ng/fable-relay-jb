/*
 * raster.c — software rasterizer
 * C11, no external dependencies.
 *
 * Features:
 *   - Barycentric triangle fill
 *   - Per-pixel z-buffer (depth test)
 *   - Perspective-correct UV interpolation
 *   - Lambert diffuse shading
 *   - PPM writer
 *
 * main: renders a spinning cube (three visible faces) to out.ppm.
 *
 * Build:
 *   cc -std=c11 -O2 -o raster raster.c -lm
 * Run:
 *   ./raster
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; }    Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float u, v; }        Vec2;

typedef struct {
    float m[4][4];
} Mat4;

typedef struct {
    unsigned char r, g, b;
} Pixel;

/* ------------------------------------------------------------------ */
/* Framebuffer                                                           */
/* ------------------------------------------------------------------ */

#define WIDTH  800
#define HEIGHT 600

static Pixel  fb[HEIGHT][WIDTH];
static float  zb[HEIGHT][WIDTH];   /* depth buffer, +∞ = far */

static void fb_clear(void) {
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            fb[y][x] = (Pixel){30, 30, 40};
            zb[y][x] = FLT_MAX;
        }
}

static void ppm_write(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++)
        fwrite(fb[y], sizeof(Pixel), WIDTH, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Math                                                                  */
/* ------------------------------------------------------------------ */

static Vec3 v3_add(Vec3 a, Vec3 b) { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static Vec3 v3_sub(Vec3 a, Vec3 b) { return (Vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static Vec3 v3_scale(Vec3 v, float s) { return (Vec3){v.x*s, v.y*s, v.z*s}; }
static float v3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec3 v3_cross(Vec3 a, Vec3 b) {
    return (Vec3){ a.y*b.z - a.z*b.y,
                   a.z*b.x - a.x*b.z,
                   a.x*b.y - a.y*b.x };
}
static Vec3 v3_norm(Vec3 v) {
    float len = sqrtf(v3_dot(v, v));
    if (len < 1e-9f) return (Vec3){0,0,1};
    return v3_scale(v, 1.0f / len);
}

/* 4×4 identity */
static Mat4 mat4_identity(void) {
    Mat4 m = {0};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

/* column-major multiply */
static Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

static Vec4 mat4_mulv(Mat4 m, Vec4 v) {
    return (Vec4){
        m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z + m.m[0][3]*v.w,
        m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z + m.m[1][3]*v.w,
        m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z + m.m[2][3]*v.w,
        m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3]*v.w,
    };
}

static Mat4 mat4_rotate_y(float a) {
    Mat4 m = mat4_identity();
    m.m[0][0] =  cosf(a);
    m.m[0][2] =  sinf(a);
    m.m[2][0] = -sinf(a);
    m.m[2][2] =  cosf(a);
    return m;
}

static Mat4 mat4_rotate_x(float a) {
    Mat4 m = mat4_identity();
    m.m[1][1] =  cosf(a);
    m.m[1][2] = -sinf(a);
    m.m[2][1] =  sinf(a);
    m.m[2][2] =  cosf(a);
    return m;
}

static Mat4 mat4_translate(float tx, float ty, float tz) {
    Mat4 m = mat4_identity();
    m.m[0][3] = tx;
    m.m[1][3] = ty;
    m.m[2][3] = tz;
    return m;
}

/* Symmetric perspective — FOV in radians, standard near/far */
static Mat4 mat4_perspective(float fov, float aspect, float near, float far) {
    float f = 1.0f / tanf(fov * 0.5f);
    Mat4 m = {0};
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (far + near) / (near - far);
    m.m[2][3] = (2.0f * far * near) / (near - far);
    m.m[3][2] = -1.0f;
    return m;
}

/* ------------------------------------------------------------------ */
/* Checkerboard procedural texture                                       */
/* ------------------------------------------------------------------ */

static Pixel tex_checker(float u, float v, Pixel ca, Pixel cb) {
    int cu = (int)(u * 8.0f) & 1;
    int cv = (int)(v * 8.0f) & 1;
    return (cu ^ cv) ? ca : cb;
}

/* ------------------------------------------------------------------ */
/* Rasterizer core                                                       */
/* ------------------------------------------------------------------ */

/*
 * A vertex after vertex processing:
 *   clip  — clip-space Vec4
 *   world — world-space position for lighting
 *   n     — world-space normal (per-vertex, flat face here)
 *   uv    — texture coordinate
 */
typedef struct {
    Vec4 clip;
    Vec3 world;
    Vec3 n;
    Vec2 uv;
} Vert;

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/*
 * draw_triangle — rasterizes one triangle with:
 *   - z-buffer depth test
 *   - perspective-correct UV interpolation
 *   - Lambert shading with ambient + diffuse
 *
 * light_dir: normalised direction FROM surface TO light, world space
 * face_col:  base diffuse colour tint (0-1 each channel)
 */
static void draw_triangle(
    Vert v0, Vert v1, Vert v2,
    Vec3 light_dir,
    Vec3 face_col,
    int use_texture
) {
    /* ---- perspective divide to NDC -------------------------------- */
    if (v0.clip.w <= 0.0f || v1.clip.w <= 0.0f || v2.clip.w <= 0.0f)
        return;   /* skip if behind camera */

    float inv_w0 = 1.0f / v0.clip.w;
    float inv_w1 = 1.0f / v1.clip.w;
    float inv_w2 = 1.0f / v2.clip.w;

    float nx0 = v0.clip.x * inv_w0;
    float ny0 = v0.clip.y * inv_w0;
    float nz0 = v0.clip.z * inv_w0;

    float nx1 = v1.clip.x * inv_w1;
    float ny1 = v1.clip.y * inv_w1;
    float nz1 = v1.clip.z * inv_w1;

    float nx2 = v2.clip.x * inv_w2;
    float ny2 = v2.clip.y * inv_w2;
    float nz2 = v2.clip.z * inv_w2;

    /* ---- NDC → screen -------------------------------------------- */
    float hw = WIDTH  * 0.5f;
    float hh = HEIGHT * 0.5f;

    float sx0 = ( nx0 + 1.0f) * hw;
    float sy0 = (-ny0 + 1.0f) * hh;
    float sx1 = ( nx1 + 1.0f) * hw;
    float sy1 = (-ny1 + 1.0f) * hh;
    float sx2 = ( nx2 + 1.0f) * hw;
    float sy2 = (-ny2 + 1.0f) * hh;

    /* ---- bounding box -------------------------------------------- */
    int minx = (int)fmaxf(0,         fminf(sx0, fminf(sx1, sx2)));
    int maxx = (int)fminf(WIDTH - 1, fmaxf(sx0, fmaxf(sx1, sx2)));
    int miny = (int)fmaxf(0,         fminf(sy0, fminf(sy1, sy2)));
    int maxy = (int)fminf(HEIGHT- 1, fmaxf(sy0, fmaxf(sy1, sy2)));

    /* ---- total edge area (sign determines winding) --------------- */
    float area = edge(sx0, sy0, sx1, sy1, sx2, sy2);
    if (fabsf(area) < 1e-6f) return;   /* degenerate */
    float inv_area = 1.0f / area;

    /* ---- per-vertex Lambert (flat-shaded: same normal per face) --- */
    /* Use v0's normal; faces share one normal. */
    Vec3 n = v3_norm(v0.n);
    float ndotl = fmaxf(0.0f, v3_dot(n, light_dir));
    float ambient   = 0.18f;
    float diffuse   = ndotl * 0.82f;
    float intensity = ambient + diffuse;

    /* ---- scan ---------------------------------------------------- */
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float pcx = (float)px + 0.5f;
            float pcy = (float)py + 0.5f;

            /* barycentric weights */
            float w0 = edge(sx1, sy1, sx2, sy2, pcx, pcy) * inv_area;
            float w1 = edge(sx2, sy2, sx0, sy0, pcx, pcy) * inv_area;
            float w2 = edge(sx0, sy0, sx1, sy1, pcx, pcy) * inv_area;

            /* point must be inside all three edges */
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            /* interpolate depth (NDC z) */
            float z = w0 * nz0 + w1 * nz1 + w2 * nz2;
            if (z >= zb[py][px]) continue;
            zb[py][px] = z;

            /* perspective-correct UV:
             * interpolate u/w and v/w and 1/w, then divide */
            float u_over_w = w0 * (v0.uv.u * inv_w0)
                           + w1 * (v1.uv.u * inv_w1)
                           + w2 * (v2.uv.u * inv_w2);
            float v_over_w = w0 * (v0.uv.v * inv_w0)
                           + w1 * (v1.uv.v * inv_w1)
                           + w2 * (v2.uv.v * inv_w2);
            float one_over_w = w0 * inv_w0 + w1 * inv_w1 + w2 * inv_w2;
            float u = u_over_w / one_over_w;
            float v_tex = v_over_w / one_over_w;
            u = clampf(u, 0.0f, 1.0f);
            v_tex = clampf(v_tex, 0.0f, 1.0f);

            /* colour from texture + shading */
            Pixel texel;
            if (use_texture) {
                Pixel ca = {220, 200, 160};
                Pixel cb = { 80,  60,  40};
                texel = tex_checker(u, v_tex, ca, cb);
            } else {
                texel = (Pixel){
                    (unsigned char)(face_col.x * 255.0f),
                    (unsigned char)(face_col.y * 255.0f),
                    (unsigned char)(face_col.z * 255.0f)
                };
            }

            fb[py][px].r = (unsigned char)clampf(texel.r * intensity, 0, 255);
            fb[py][px].g = (unsigned char)clampf(texel.g * intensity, 0, 255);
            fb[py][px].b = (unsigned char)clampf(texel.b * intensity, 0, 255);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Cube geometry                                                         */
/* ------------------------------------------------------------------ */

/*
 * Unit cube with 8 vertices.  We define 6 faces; main selects the
 * three that face the camera at the given rotation.
 */
static const Vec3 cube_verts[8] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},  /* back  */
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},  /* front */
};

/* Each face: 4 vertex indices (quad, split to 2 triangles), UV coords,
   and an outward normal. */
typedef struct {
    int    idx[4];         /* vertex indices (CCW front face) */
    Vec2   uv[4];
    Vec3   normal;
    Vec3   color;          /* fallback tint if no texture */
} Face;

static const Face cube_faces[6] = {
    /* front (+Z) */
    { {4,5,6,7}, {{0,1},{1,1},{1,0},{0,0}}, { 0, 0, 1}, {0.9f,0.4f,0.4f} },
    /* back  (-Z) */
    { {1,0,3,2}, {{0,1},{1,1},{1,0},{0,0}}, { 0, 0,-1}, {0.4f,0.4f,0.9f} },
    /* right (+X) */
    { {5,1,2,6}, {{0,1},{1,1},{1,0},{0,0}}, { 1, 0, 0}, {0.4f,0.9f,0.4f} },
    /* left  (-X) */
    { {0,4,7,3}, {{0,1},{1,1},{1,0},{0,0}}, {-1, 0, 0}, {0.9f,0.9f,0.4f} },
    /* top   (+Y) */
    { {7,6,2,3}, {{0,1},{1,1},{1,0},{0,0}}, { 0, 1, 0}, {0.4f,0.9f,0.9f} },
    /* bottom(-Y) */
    { {0,1,5,4}, {{0,1},{1,1},{1,0},{0,0}}, { 0,-1, 0}, {0.9f,0.4f,0.9f} },
};

/* ------------------------------------------------------------------ */
/* Rendering a frame                                                     */
/* ------------------------------------------------------------------ */

static void render_frame(float angle_y, float angle_x) {
    fb_clear();

    /* Model: rotate the cube */
    Mat4 model = mat4_mul(mat4_rotate_x(angle_x), mat4_rotate_y(angle_y));

    /* View: pull camera back along +Z */
    Mat4 view  = mat4_translate(0.0f, 0.0f, -4.0f);

    /* Projection: 60° FOV */
    float fov    = 60.0f * ((float)M_PI / 180.0f);
    float aspect = (float)WIDTH / (float)HEIGHT;
    Mat4 proj    = mat4_perspective(fov, aspect, 0.1f, 100.0f);

    Mat4 mv   = mat4_mul(view, model);
    Mat4 mvp  = mat4_mul(proj, mv);

    /* Light direction in world space (from upper-left-front) */
    Vec3 light_dir = v3_norm((Vec3){0.6f, 1.0f, 0.8f});

    /* For each face of the cube */
    for (int fi = 0; fi < 6; fi++) {
        const Face *f = &cube_faces[fi];

        /* Back-face culling in world space:
         * transform the face normal by the model matrix's 3×3 part */
        Vec4 tn = mat4_mulv(model, (Vec4){f->normal.x, f->normal.y, f->normal.z, 0.0f});
        Vec3 world_n = v3_norm((Vec3){tn.x, tn.y, tn.z});

        /* Camera looks down -Z in view space → (0,0,-1) in world
         * after the inverse-view.  With our simple translate-only view
         * the camera direction (surface→camera) for a face centred near
         * origin is approximately (0,0,1) in world space. */
        Vec3 view_dir = {0.0f, 0.0f, 1.0f};
        if (v3_dot(world_n, view_dir) <= 0.0f) continue;  /* back face */

        /* Transform all 4 vertices */
        Vert qv[4];
        for (int i = 0; i < 4; i++) {
            Vec3 wp = cube_verts[f->idx[i]];
            /* world position */
            Vec4 wpos = mat4_mulv(model, (Vec4){wp.x, wp.y, wp.z, 1.0f});
            qv[i].world = (Vec3){wpos.x, wpos.y, wpos.z};
            qv[i].n     = world_n;
            qv[i].uv    = f->uv[i];
            qv[i].clip  = mat4_mulv(mvp, (Vec4){wp.x, wp.y, wp.z, 1.0f});
        }

        /* Split quad into 2 triangles (0-1-2 and 0-2-3) */
        draw_triangle(qv[0], qv[1], qv[2], light_dir, f->color, 1);
        draw_triangle(qv[0], qv[2], qv[3], light_dir, f->color, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                           */
/* ------------------------------------------------------------------ */

int main(void) {
    /*
     * "Spinning cube" — pick an angle that exposes three faces nicely,
     * as if frozen mid-spin.  Rotating ~35° around Y and ~25° around X
     * shows the front, right, and top faces simultaneously.
     */
    float angle_y = 35.0f * ((float)M_PI / 180.0f);
    float angle_x = 25.0f * ((float)M_PI / 180.0f);

    render_frame(angle_y, angle_x);
    ppm_write("out.ppm");

    printf("Wrote out.ppm (%dx%d)\n", WIDTH, HEIGHT);
    return 0;
}
