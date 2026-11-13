/*
 * lines.c — C11, no dependencies
 *
 * Features:
 *   - 8-bit framebuffer with bounds-checked putpixel
 *   - Bresenham line drawing
 *   - Cohen-Sutherland line clipping to a viewport
 *   - Midpoint circle (outline)
 *   - Filled circle
 *   - PGM (P5 binary) dump to stdout
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o lines lines.c -lm
 *
 * Run:
 *   ./lines > out.pgm
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Framebuffer
 * ---------------------------------------------------------------------- */

typedef struct {
    int      width;
    int      height;
    uint8_t *pixels;   /* row-major, top-left origin */
} Framebuffer;

/* Returns 0 on success, -1 on allocation failure. */
static int fb_init(Framebuffer *fb, int width, int height)
{
    fb->width  = width;
    fb->height = height;
    fb->pixels = (uint8_t *)calloc((size_t)(width * height), 1);
    return fb->pixels ? 0 : -1;
}

static void fb_free(Framebuffer *fb)
{
    free(fb->pixels);
    fb->pixels = NULL;
}

static void fb_clear(Framebuffer *fb, uint8_t value)
{
    memset(fb->pixels, value, (size_t)(fb->width * fb->height));
}

/* Bounds-checked pixel write.  Silently ignores out-of-range coordinates. */
static void putpixel(Framebuffer *fb, int x, int y, uint8_t value)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height)
        return;
    fb->pixels[y * fb->width + x] = value;
}

/* PGM binary (P5) dump.  Returns 0 on success, -1 on write error. */
static int fb_write_pgm(const Framebuffer *fb, FILE *out)
{
    if (fprintf(out, "P5\n%d %d\n255\n", fb->width, fb->height) < 0)
        return -1;
    size_t total = (size_t)(fb->width * fb->height);
    if (fwrite(fb->pixels, 1, total, out) != total)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Viewport for Cohen-Sutherland clipping
 * ---------------------------------------------------------------------- */

typedef struct {
    int xmin, ymin;   /* inclusive */
    int xmax, ymax;   /* inclusive */
} Viewport;

/* Region codes */
#define CS_INSIDE 0
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_BOTTOM 4
#define CS_TOP    8

static int cs_code(const Viewport *vp, int x, int y)
{
    int code = CS_INSIDE;
    if      (x < vp->xmin) code |= CS_LEFT;
    else if (x > vp->xmax) code |= CS_RIGHT;
    if      (y < vp->ymin) code |= CS_BOTTOM;
    else if (y > vp->ymax) code |= CS_TOP;
    return code;
}

/*
 * Cohen-Sutherland clip.
 * Modifies (x0,y0)-(x1,y1) in-place to the clipped segment.
 * Returns 1 if the segment is (partially) visible, 0 if entirely outside.
 *
 * Intersection coordinates are computed with integer arithmetic;
 * truncation is acceptable for rasterisation purposes.
 */
static int cs_clip(const Viewport *vp,
                   int *x0, int *y0, int *x1, int *y1)
{
    int code0 = cs_code(vp, *x0, *y0);
    int code1 = cs_code(vp, *x1, *y1);

    for (;;) {
        if (!(code0 | code1)) return 1;   /* trivially inside */
        if   (code0 & code1)  return 0;   /* trivially outside */

        int code_out = code0 ? code0 : code1;
        int dx = *x1 - *x0;
        int dy = *y1 - *y0;
        int x, y;

        if (code_out & CS_TOP) {
            y = vp->ymax;
            x = (dy != 0) ? *x0 + dx * (vp->ymax - *y0) / dy : *x0;
        } else if (code_out & CS_BOTTOM) {
            y = vp->ymin;
            x = (dy != 0) ? *x0 + dx * (vp->ymin - *y0) / dy : *x0;
        } else if (code_out & CS_RIGHT) {
            x = vp->xmax;
            y = (dx != 0) ? *y0 + dy * (vp->xmax - *x0) / dx : *y0;
        } else {
            x = vp->xmin;
            y = (dx != 0) ? *y0 + dy * (vp->xmin - *x0) / dx : *y0;
        }

        if (code_out == code0) {
            *x0 = x; *y0 = y;
            code0 = cs_code(vp, *x0, *y0);
        } else {
            *x1 = x; *y1 = y;
            code1 = cs_code(vp, *x1, *y1);
        }
    }
}

/* -------------------------------------------------------------------------
 * Bresenham line (no clipping; call cs_clip first if needed)
 * ---------------------------------------------------------------------- */

static void draw_line(Framebuffer *fb,
                      int x0, int y0, int x1, int y1,
                      uint8_t value)
{
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        putpixel(fb, x0, y0, value);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/*
 * Clip to vp, then rasterise.
 * Returns 1 if any pixels were drawn, 0 if entirely clipped away.
 */
static int draw_line_clipped(Framebuffer *fb, const Viewport *vp,
                             int x0, int y0, int x1, int y1,
                             uint8_t value)
{
    if (!cs_clip(vp, &x0, &y0, &x1, &y1))
        return 0;
    draw_line(fb, x0, y0, x1, y1, value);
    return 1;
}

/* -------------------------------------------------------------------------
 * Midpoint circle — outline only
 * ---------------------------------------------------------------------- */

static void draw_circle(Framebuffer *fb,
                        int cx, int cy, int r,
                        uint8_t value)
{
    if (r < 0) return;
    int x = 0, y = r;
    int d = 1 - r;

    while (x <= y) {
        putpixel(fb, cx + x, cy + y, value);
        putpixel(fb, cx - x, cy + y, value);
        putpixel(fb, cx + x, cy - y, value);
        putpixel(fb, cx - x, cy - y, value);
        putpixel(fb, cx + y, cy + x, value);
        putpixel(fb, cx - y, cy + x, value);
        putpixel(fb, cx + y, cy - x, value);
        putpixel(fb, cx - y, cy - x, value);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            --y;
        }
        ++x;
    }
}

/* -------------------------------------------------------------------------
 * Filled circle — scanline spans derived from midpoint stepping
 * ---------------------------------------------------------------------- */

static void draw_filled_circle(Framebuffer *fb,
                               int cx, int cy, int r,
                               uint8_t value)
{
    if (r < 0) return;
    int x = 0, y = r;
    int d = 1 - r;

#define HSPAN(yy, xa, xb)                                   \
    do {                                                     \
        int _xa = (xa), _xb = (xb);                         \
        if (_xa > _xb) { int _t = _xa; _xa = _xb; _xb = _t; } \
        for (int _xi = _xa; _xi <= _xb; ++_xi)              \
            putpixel(fb, _xi, (yy), value);                  \
    } while (0)

    while (x <= y) {
        HSPAN(cy + y, cx - x, cx + x);
        HSPAN(cy - y, cx - x, cx + x);
        HSPAN(cy + x, cx - y, cx + y);
        HSPAN(cy - x, cx - y, cx + y);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            --y;
        }
        ++x;
    }
#undef HSPAN
}

/* -------------------------------------------------------------------------
 * main — clipped test pattern
 * ---------------------------------------------------------------------- */

int main(void)
{
    const int W = 512, H = 512;

    Framebuffer fb;
    if (fb_init(&fb, W, H) != 0) {
        fputs("error: could not allocate framebuffer\n", stderr);
        return 1;
    }
    fb_clear(&fb, 20);

    /* Viewport: 40 px inset on every side */
    Viewport vp = { 40, 40, W - 41, H - 41 };

    /* Viewport border */
    draw_line(&fb, vp.xmin, vp.ymin, vp.xmax, vp.ymin, 180);
    draw_line(&fb, vp.xmax, vp.ymin, vp.xmax, vp.ymax, 180);
    draw_line(&fb, vp.xmax, vp.ymax, vp.xmin, vp.ymax, 180);
    draw_line(&fb, vp.xmin, vp.ymax, vp.xmin, vp.ymin, 180);

    int cx = W / 2, cy = H / 2;

    /* Starburst from centre — rays extend well past the viewport */
    int nrays = 36;
    for (int i = 0; i < nrays; ++i) {
        double angle = (2.0 * M_PI * i) / nrays;
        int ex = (int)(cx + 320.0 * cos(angle));
        int ey = (int)(cy + 320.0 * sin(angle));
        draw_line_clipped(&fb, &vp, cx, cy, ex, ey, 230);
    }

    /* Clipped diagonal grid */
    for (int k = -W; k < W * 2; k += 48) {
        draw_line_clipped(&fb, &vp, k,     0, k + H, H,   120);
        draw_line_clipped(&fb, &vp, k + H, 0, k,     H,   120);
    }

    /* Lines entirely outside — must not appear */
    draw_line_clipped(&fb, &vp, -200, -200, -100, -100, 255);
    draw_line_clipped(&fb, &vp,  600,  600,  700,  700, 255);

    /* Outline circles */
    draw_circle(&fb, cx, cy, 160, 240);
    draw_circle(&fb, cx, cy, 100,  80);
    draw_circle(&fb, cx, cy,  50, 200);

    /* Filled circles at compass points on the outer ring */
    const int pts[4][2] = { {0,-160}, {0,160}, {-160,0}, {160,0} };
    for (int i = 0; i < 4; ++i)
        draw_filled_circle(&fb, cx + pts[i][0], cy + pts[i][1], 18, 210);

    /* Dark filled circle at centre */
    draw_filled_circle(&fb, cx, cy, 30, 60);

    /* Full-frame diagonals (unclipped, beneath everything) */
    draw_line(&fb, 0, 0, W - 1, H - 1, 100);
    draw_line(&fb, W - 1, 0, 0, H - 1, 100);

    if (fb_write_pgm(&fb, stdout) != 0) {
        fputs("error: could not write PGM\n", stderr);
        fb_free(&fb);
        return 1;
    }

    fb_free(&fb);
    return 0;
}
