/*
 * raster.c — software rasterizer
 *
 * Features:
 *   - Barycentric triangle fill
 *   - Z-buffer (depth test)
 *   - Perspective-correct UV interpolation
 *   - Lambert diffuse shading
 *   - PPM writer
 *   - Demo: spinning cube, three visible faces
 *
 * Build (C11):
 *   cc -std=c11 -O2 -o raster raster.c -lm
 * Run:
 *   ./raster        # writes out.ppm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH   640
#define HEIGHT  480
#define FOV_DEG 60.0f

/* ------------------------------------------------------------------ */
/*  Math types                                                          */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; }    Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float u, v; }        Vec2;

typedef struct {
    float m[4][4];
} Mat4;

static inline Vec3 vec3(float x, float y, float z) { return (Vec3){x,y,z}; }

static inline Vec3 v3add(Vec3 a, Vec3 b) { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3 v3sub(Vec3 a, Vec3 b) { return (Vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 v3scale(Vec3 a, float s) { return (Vec3){a.x*s, a.y*s, a.z*s}; }
static inline float v3dot(Vec3 a, Vec3 b)  { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 v3cross(Vec3 a, Vec3 b) {
    return (Vec3){ a.y*b.z - a.z*b.y,
                   a.z*b.x - a.x*b.z,
                   a.x*b.y - a.y*b.x };
}
static inline Vec3 v3norm(Vec3 a) {
    float l = sqrtf(v3dot(a, a));
    if (l < 1e-9f) return (Vec3){0,0,0};
    return v3scale(a, 1.0f / l);
}

static inline Mat4 mat4_identity(void) {
    Mat4 m = {0};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

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
        m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3]*v.w
    };
}

/* Row-major, right-handed, column-vector convention (v' = M * v). */

static Mat4 mat4_rotate_y(float rad) {
    Mat4 m = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    m.m[0][0] =  c;  m.m[0][2] = s;
    m.m[2][0] = -s;  m.m[2][2] = c;
    return m;
}

static Mat4 mat4_rotate_x(float rad) {
    Mat4 m = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    m.m[1][1] =  c;  m.m[1][2] = -s;
    m.m[2][1] =  s;  m.m[2][2] =  c;
    return m;
}

static Mat4 mat4_translate(float tx, float ty, float tz) {
    Mat4 m = mat4_identity();
    m.m[0][3] = tx;
    m.m[1][3] = ty;
    m.m[2][3] = tz;
    return m;
}

/* Perspective projection, NDC: x,y in [-1,1], z in [-1,1] (OpenGL style). */
static Mat4 mat4_perspective(float fov_rad, float aspect, float near, float far) {
    Mat4 m = {0};
    float f = 1.0f / tanf(fov_rad * 0.5f);
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (far + near) / (near - far);
    m.m[2][3] = (2.0f * far * near) / (near - far);
    m.m[3][2] = -1.0f;
    return m;
}

/* ------------------------------------------------------------------ */
/*  Framebuffer                                                         */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t r, g, b; } Pixel;

static Pixel  fb[HEIGHT][WIDTH];
static float  zb[HEIGHT][WIDTH];   /* depth buffer */

static void fb_clear(void) {
    /* Background: dark gray */
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            fb[y][x] = (Pixel){30, 30, 35};
            zb[y][x] = FLT_MAX;
        }
}

static void fb_write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++)
        fwrite(fb[y], sizeof(Pixel), WIDTH, f);
    fclose(f);
}

static inline uint8_t clamp_byte(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

/* ------------------------------------------------------------------ */
/*  Checkerboard procedural texture                                     */
/* ------------------------------------------------------------------ */

static Pixel sample_checker(float u, float v, Pixel c0, Pixel c1) {
    /* 8×8 checker tiles */
    int iu = (int)floorf(u * 8.0f) & 1;
    int iv = (int)floorf(v * 8.0f) & 1;
    return (iu ^ iv) ? c1 : c0;
}

/* ------------------------------------------------------------------ */
/*  Triangle rasterizer                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    Vec3 pos;    /* world position */
    Vec3 normal; /* world normal   */
    Vec2 uv;
} Vertex;

/*
 * Rasterize one triangle.
 *
 * clip0..clip2 : homogeneous clip-space positions (pre-division).
 * v0..v2       : corresponding vertex attributes (world-space).
 * light_dir    : normalized light direction (world space, toward light).
 * face_color   : base color pair for the checker texture.
 */
static void draw_triangle(
    Vec4 clip0, Vec4 clip1, Vec4 clip2,
    Vertex v0,  Vertex v1,  Vertex v2,
    Vec3 light_dir,
    Pixel face_c0, Pixel face_c1)
{
    /* Back-face cull in clip space using 2D signed area of NDC triangle. */
    /* NDC (after divide) */
    float ndcx0 = clip0.x / clip0.w, ndcy0 = clip0.y / clip0.w;
    float ndcx1 = clip1.x / clip1.w, ndcy1 = clip1.y / clip1.w;
    float ndcx2 = clip2.x / clip2.w, ndcy2 = clip2.y / clip2.w;

    float signed_area2 = (ndcx1 - ndcx0) * (ndcy2 - ndcy0)
                       - (ndcx2 - ndcx0) * (ndcy1 - ndcy0);
    if (signed_area2 >= 0.0f) return; /* back-facing */

    /* Viewport transform */
    float hw = WIDTH  * 0.5f;
    float hh = HEIGHT * 0.5f;

    float sx0 = ndcx0 * hw + hw,  sy0 = -ndcy0 * hh + hh;
    float sx1 = ndcx1 * hw + hw,  sy1 = -ndcy1 * hh + hh;
    float sx2 = ndcx2 * hw + hw,  sy2 = -ndcy2 * hh + hh;

    /* Bounding box, clamped to screen */
    int minx = (int)fmaxf(0.0f,         fminf(sx0, fminf(sx1, sx2)));
    int miny = (int)fmaxf(0.0f,         fminf(sy0, fminf(sy1, sy2)));
    int maxx = (int)fminf(WIDTH  - 1.f, fmaxf(sx0, fmaxf(sx1, sx2))) + 1;
    int maxy = (int)fminf(HEIGHT - 1.f, fmaxf(sy0, fmaxf(sy1, sy2))) + 1;

    /* 1/w for each vertex (for perspective-correct interpolation) */
    float iw0 = 1.0f / clip0.w;
    float iw1 = 1.0f / clip1.w;
    float iw2 = 1.0f / clip2.w;

    /* NDC z for depth buffer */
    float nz0 = clip0.z / clip0.w;
    float nz1 = clip1.z / clip1.w;
    float nz2 = clip2.z / clip2.w;

    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float pcx = (float)px + 0.5f;
            float pcy = (float)py + 0.5f;

            /* Barycentric coordinates in screen space */
            float denom = (sy1 - sy2) * (sx0 - sx2) + (sx2 - sx1) * (sy0 - sy2);
            if (fabsf(denom) < 1e-9f) continue;
            float inv_denom = 1.0f / denom;

            float b0 = ((sy1 - sy2) * (pcx - sx2) + (sx2 - sx1) * (pcy - sy2)) * inv_denom;
            float b1 = ((sy2 - sy0) * (pcx - sx2) + (sx0 - sx2) * (pcy - sy2)) * inv_denom;
            float b2 = 1.0f - b0 - b1;

            if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) continue;

            /* Interpolated depth */
            float z = b0 * nz0 + b1 * nz1 + b2 * nz2;

            if (z >= zb[py][px]) continue;
            zb[py][px] = z;

            /* Perspective-correct attribute interpolation.
             * For any attribute A: A_persp = (b0*A0/w0 + b1*A1/w1 + b2*A2/w2)
             *                               / (b0/w0  + b1/w1  + b2/w2)       */
            float denom_w = b0 * iw0 + b1 * iw1 + b2 * iw2;
            float inv_w   = (fabsf(denom_w) > 1e-9f) ? 1.0f / denom_w : 0.0f;

            float u = (b0 * v0.uv.u * iw0 + b1 * v1.uv.u * iw1 + b2 * v2.uv.u * iw2) * inv_w;
            float v = (b0 * v0.uv.v * iw0 + b1 * v1.uv.v * iw1 + b2 * v2.uv.v * iw2) * inv_w;

            /* Interpolate world-space normal and normalize */
            Vec3 n = {
                (b0 * v0.normal.x * iw0 + b1 * v1.normal.x * iw1 + b2 * v2.normal.x * iw2) * inv_w,
                (b0 * v0.normal.y * iw0 + b1 * v1.normal.y * iw1 + b2 * v2.normal.y * iw2) * inv_w,
                (b0 * v0.normal.z * iw0 + b1 * v1.normal.z * iw1 + b2 * v2.normal.z * iw2) * inv_w,
            };
            n = v3norm(n);

            /* Lambert */
            float ndotl = fmaxf(0.0f, v3dot(n, light_dir));
            float ambient = 0.15f;
            float light   = ambient + (1.0f - ambient) * ndotl;

            Pixel tex = sample_checker(u, v, face_c0, face_c1);

            fb[py][px] = (Pixel){
                clamp_byte(tex.r * light),
                clamp_byte(tex.g * light),
                clamp_byte(tex.b * light)
            };
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Cube geometry                                                       */
/* ------------------------------------------------------------------ */

/*
 * A cube has 8 vertices and 6 faces (2 triangles each = 12 triangles).
 * We define each face with its four corners and flat normal.
 */

static const Vec3 CUBE_VERTS[8] = {
    {-0.5f, -0.5f, -0.5f}, /* 0 */
    { 0.5f, -0.5f, -0.5f}, /* 1 */
    { 0.5f,  0.5f, -0.5f}, /* 2 */
    {-0.5f,  0.5f, -0.5f}, /* 3 */
    {-0.5f, -0.5f,  0.5f}, /* 4 */
    { 0.5f, -0.5f,  0.5f}, /* 5 */
    { 0.5f,  0.5f,  0.5f}, /* 6 */
    {-0.5f,  0.5f,  0.5f}, /* 7 */
};

/* Each face: 4 vertex indices (CCW from outside), flat normal */
typedef struct {
    int    idx[4];
    Vec3   normal;
    Pixel  c0, c1; /* checker colors */
} Face;

static const Face CUBE_FACES[6] = {
    /* front  (+Z) */ { {4,5,6,7}, { 0, 0, 1}, {220,80,60},  {240,200,180} },
    /* back   (-Z) */ { {1,0,3,2}, { 0, 0,-1}, {60,120,220}, {180,210,240} },
    /* right  (+X) */ { {5,1,2,6}, { 1, 0, 0}, {80,200,100}, {180,240,190} },
    /* left   (-X) */ { {0,4,7,3}, {-1, 0, 0}, {200,180,60}, {240,230,160} },
    /* top    (+Y) */ { {7,6,2,3}, { 0, 1, 0}, {180,80,220}, {220,190,240} },
    /* bottom (-Y) */ { {0,1,5,4}, { 0,-1, 0}, {80,180,200}, {170,230,240} },
};

/* ------------------------------------------------------------------ */
/*  Render                                                              */
/* ------------------------------------------------------------------ */

static void render_cube(float angle_y, float angle_x) {
    fb_clear();

    float aspect = (float)WIDTH / (float)HEIGHT;
    float fov    = FOV_DEG * ((float)M_PI / 180.0f);

    Mat4 model     = mat4_mul(mat4_rotate_x(angle_x), mat4_rotate_y(angle_y));
    Mat4 view      = mat4_translate(0.0f, 0.0f, -2.5f);
    Mat4 proj      = mat4_perspective(fov, aspect, 0.1f, 100.0f);
    Mat4 mv        = mat4_mul(view, model);
    Mat4 mvp       = mat4_mul(proj, mv);

    /* Normal matrix: upper-left 3×3 of inverse-transpose of MV.
     * For uniform scale + rotation only, MV upper-left = normal matrix. */

    /* Light direction in world space (from scene toward light) */
    Vec3 light_world = v3norm(vec3(1.0f, 1.5f, 2.0f));

    for (int f = 0; f < 6; f++) {
        const Face *face = &CUBE_FACES[f];

        /* Build two triangles from the quad (0,1,2) and (0,2,3). */
        for (int tri = 0; tri < 2; tri++) {
            int ia = face->idx[0];
            int ib = face->idx[tri + 1];
            int ic = face->idx[tri + 2];

            Vec3 wp[3] = { CUBE_VERTS[ia], CUBE_VERTS[ib], CUBE_VERTS[ic] };

            /* UVs: map quad corners 0-3 to (0,0),(1,0),(1,1),(0,1) */
            static const Vec2 quad_uvs[4] = {{0,0},{1,0},{1,1},{0,1}};
            Vec2 uv[3] = { quad_uvs[0], quad_uvs[tri+1], quad_uvs[tri+2] };

            /* Transform positions to clip space */
            Vec4 clip[3];
            for (int k = 0; k < 3; k++) {
                Vec4 p4 = {wp[k].x, wp[k].y, wp[k].z, 1.0f};
                clip[k] = mat4_mulv(mvp, p4);
            }

            /* Transform normal to world space (model is pure rotation, so just apply model) */
            Vec4 nw4 = {face->normal.x, face->normal.y, face->normal.z, 0.0f};
            nw4 = mat4_mulv(model, nw4);
            Vec3 world_normal = v3norm(vec3(nw4.x, nw4.y, nw4.z));

            Vertex verts[3];
            for (int k = 0; k < 3; k++) {
                /* world position for the vertex (not strictly needed here) */
                Vec4 wpos = mat4_mulv(model, (Vec4){wp[k].x,wp[k].y,wp[k].z,1.0f});
                verts[k].pos    = vec3(wpos.x, wpos.y, wpos.z);
                verts[k].normal = world_normal; /* flat shaded; same normal per vertex */
                verts[k].uv     = uv[k];
            }

            draw_triangle(clip[0], clip[1], clip[2],
                          verts[0], verts[1], verts[2],
                          light_world,
                          face->c0, face->c1);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void) {
    /*
     * Spin the cube to a pose where three faces are clearly visible:
     * ~30° around Y and ~20° around X gives a classic "three-quarter" view.
     */
    float angle_y =  0.52f; /* ~30 degrees */
    float angle_x = -0.35f; /* ~20 degrees, tilted slightly up */

    render_cube(angle_y, angle_x);
    fb_write_ppm("out.ppm");

    printf("Written: out.ppm (%dx%d)\n", WIDTH, HEIGHT);
    return 0;
}
