/*
 * lines.c — C11, no dependencies
 *
 * - 8-bit framebuffer with bounds-checked putpixel
 * - PGM (P5) dump to stdout
 * - Cohen-Sutherland line clipping to a viewport
 * - Bresenham line drawing
 * - Midpoint circle (8-way symmetry)
 * - Filled circle (scanline spans)
 * - main: clipped test pattern
 *
 * Build:  cc -std=c11 -O2 -Wall -Wextra -o lines lines.c
 * Run:    ./lines > out.pgm
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Framebuffer                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *pixels;
    int      width;
    int      height;
} Framebuf;

/* Returns 0 on success, -1 on allocation failure. */
static int fb_init(Framebuf *fb, int w, int h)
{
    fb->pixels = calloc((size_t)w * (size_t)h, 1);
    if (!fb->pixels) return -1;
    fb->width  = w;
    fb->height = h;
    return 0;
}

static void fb_free(Framebuf *fb)
{
    free(fb->pixels);
    fb->pixels = NULL;
}

/* Bounds-checked single-pixel write. Silently clips out-of-range coords. */
static inline void putpixel(Framebuf *fb, int x, int y, uint8_t v)
{
    if ((unsigned)x < (unsigned)fb->width &&
        (unsigned)y < (unsigned)fb->height)
        fb->pixels[y * fb->width + x] = v;
}

/* Fill entire framebuffer with one value. */
static void fb_clear(Framebuf *fb, uint8_t v)
{
    memset(fb->pixels, v, (size_t)fb->width * (size_t)fb->height);
}

/* Dump as raw PGM (P5) to fp. */
static void fb_write_pgm(const Framebuf *fb, FILE *fp)
{
    fprintf(fp, "P5\n%d %d\n255\n", fb->width, fb->height);
    fwrite(fb->pixels, 1, (size_t)fb->width * (size_t)fb->height, fp);
}

/* ------------------------------------------------------------------ */
/* Viewport for clipping                                               */
/* ------------------------------------------------------------------ */

typedef struct { int xmin, ymin, xmax, ymax; } Viewport;

/* ------------------------------------------------------------------ */
/* Cohen-Sutherland clipping                                           */
/* ------------------------------------------------------------------ */

#define CS_INSIDE 0
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_BOTTOM 4
#define CS_TOP    8

static int cs_outcode(const Viewport *vp, int x, int y)
{
    int code = CS_INSIDE;
    if (x < vp->xmin)      code |= CS_LEFT;
    else if (x > vp->xmax) code |= CS_RIGHT;
    if (y < vp->ymin)      code |= CS_TOP;
    else if (y > vp->ymax) code |= CS_BOTTOM;
    return code;
}

/*
 * Clip segment (x0,y0)-(x1,y1) to *vp using Cohen-Sutherland.
 * Writes clipped endpoints back to *x0..*y1.
 * Returns 1 if any part of the segment is visible, 0 if fully clipped.
 */
static int cs_clip(const Viewport *vp,
                   int *x0, int *y0, int *x1, int *y1)
{
    int oc0 = cs_outcode(vp, *x0, *y0);
    int oc1 = cs_outcode(vp, *x1, *y1);

    for (;;) {
        if (!(oc0 | oc1))  return 1;   /* trivially inside */
        if (oc0 & oc1)     return 0;   /* trivially outside */

        /* Pick the endpoint outside the viewport. */
        int oc = oc0 ? oc0 : oc1;
        int x, y;

        if (oc & CS_BOTTOM) {
            x = *x0 + (*x1 - *x0) * (vp->ymax - *y0) / (*y1 - *y0);
            y = vp->ymax;
        } else if (oc & CS_TOP) {
            x = *x0 + (*x1 - *x0) * (vp->ymin - *y0) / (*y1 - *y0);
            y = vp->ymin;
        } else if (oc & CS_RIGHT) {
            y = *y0 + (*y1 - *y0) * (vp->xmax - *x0) / (*x1 - *x0);
            x = vp->xmax;
        } else { /* CS_LEFT */
            y = *y0 + (*y1 - *y0) * (vp->xmin - *x0) / (*x1 - *x0);
            x = vp->xmin;
        }

        if (oc == oc0) {
            *x0 = x; *y0 = y;
            oc0 = cs_outcode(vp, *x0, *y0);
        } else {
            *x1 = x; *y1 = y;
            oc1 = cs_outcode(vp, *x1, *y1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bresenham line (no clipping — apply cs_clip first)                 */
/* ------------------------------------------------------------------ */

static void draw_line(Framebuf *fb, int x0, int y0, int x1, int y1,
                      uint8_t color)
{
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        putpixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { if (x0 == x1) break; err += dy; x0 += sx; }
        if (e2 <= dx) { if (y0 == y1) break; err += dx; y0 += sy; }
    }
}

/*
 * Clip then draw.  Convenience wrapper used in the test pattern.
 * Returns 1 if any pixels were drawn, 0 if fully clipped.
 */
static int draw_line_clipped(Framebuf *fb, const Viewport *vp,
                             int x0, int y0, int x1, int y1,
                             uint8_t color)
{
    if (!cs_clip(vp, &x0, &y0, &x1, &y1)) return 0;
    draw_line(fb, x0, y0, x1, y1, color);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Midpoint circle (outline, 8-way symmetry)                           */
/* ------------------------------------------------------------------ */

static void draw_circle(Framebuf *fb, int cx, int cy, int r, uint8_t color)
{
    int x = 0, y = r;
    int d = 1 - r;

    while (x <= y) {
        putpixel(fb, cx + x, cy + y, color);
        putpixel(fb, cx - x, cy + y, color);
        putpixel(fb, cx + x, cy - y, color);
        putpixel(fb, cx - x, cy - y, color);
        putpixel(fb, cx + y, cy + x, color);
        putpixel(fb, cx - y, cy + x, color);
        putpixel(fb, cx + y, cy - x, color);
        putpixel(fb, cx - y, cy - x, color);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

/* ------------------------------------------------------------------ */
/* Filled circle (horizontal spans, midpoint decision per row)         */
/* ------------------------------------------------------------------ */

static void draw_circle_filled(Framebuf *fb, int cx, int cy, int r,
                                uint8_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        /* half-width at this row via integer sqrt approximation */
        int dx = (int)sqrt((double)(r * r - dy * dy));
        for (int x = cx - dx; x <= cx + dx; x++)
            putpixel(fb, x, cy + dy, color);
    }
}

/* ------------------------------------------------------------------ */
/* Test pattern                                                        */
/* ------------------------------------------------------------------ */

/*
 * Draws:
 *   - a viewport border
 *   - a fan of lines radiating from outside the viewport so clipping fires
 *   - a grid of lines crossing the viewport boundary
 *   - midpoint circles at three sizes, partly outside the buffer
 *   - two filled circles
 */
static void draw_test_pattern(Framebuf *fb)
{
    const int W = fb->width, H = fb->height;

    /* Viewport: an inset rectangle roughly 20 % from each edge. */
    Viewport vp = {
        .xmin = W / 5,
        .ymin = H / 5,
        .xmax = W - W / 5 - 1,
        .ymax = H - H / 5 - 1
    };

    /* 1. Draw viewport border (white). */
    draw_line(fb, vp.xmin, vp.ymin, vp.xmax, vp.ymin, 255);
    draw_line(fb, vp.xmax, vp.ymin, vp.xmax, vp.ymax, 255);
    draw_line(fb, vp.xmax, vp.ymax, vp.xmin, vp.ymax, 255);
    draw_line(fb, vp.xmin, vp.ymax, vp.xmin, vp.ymin, 255);

    /* 2. Fan of 36 lines from a point above-left of the viewport.
     *    Many will be clipped; the visible portions are drawn in mid-gray. */
    int fx = vp.xmin - W / 4;
    int fy = vp.ymin - H / 4;
    for (int i = 0; i <= 36; i++) {
        double t  = (double)i / 36.0;
        int    tx = (int)(vp.xmin + t * (vp.xmax - vp.xmin));
        int    ty = vp.ymax + H / 8;
        draw_line_clipped(fb, &vp, fx, fy, tx, ty, 180);
    }
    for (int i = 0; i <= 24; i++) {
        double t  = (double)i / 24.0;
        int    ty = (int)(vp.ymin + t * (vp.ymax - vp.ymin));
        draw_line_clipped(fb, &vp, fx, fy, vp.xmax + W / 8, ty, 180);
    }

    /* 3. Diagonal grid crossing the viewport edges (light gray). */
    int step = (vp.xmax - vp.xmin) / 8;
    if (step < 1) step = 1;
    for (int i = 0; i <= 8; i++) {
        int ox = vp.xmin + i * step;
        draw_line_clipped(fb, &vp,
                          ox - step * 2, vp.ymin - H / 4,
                          ox + step * 2, vp.ymax + H / 4, 128);
    }

    /* 4. Three midpoint circle outlines; some extend beyond the buffer. */
    int cx = W / 2, cy = H / 2;
    draw_circle(fb, cx, cy, (W < H ? W : H) / 3,     220);
    draw_circle(fb, cx, cy, (W < H ? W : H) / 5,     200);
    draw_circle(fb, W * 7 / 8, H / 4, H / 4,          160);

    /* 5. Two filled circles. */
    draw_circle_filled(fb, cx,          cy,          (W < H ? W : H) / 10, 100);
    draw_circle_filled(fb, W / 4,       H * 3 / 4,   H / 10,              140);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void)
{
    const int W = 640, H = 480;

    Framebuf fb;
    if (fb_init(&fb, W, H) != 0) {
        fprintf(stderr, "lines: out of memory\n");
        return EXIT_FAILURE;
    }

    fb_clear(&fb, 0);
    draw_test_pattern(&fb);
    fb_write_pgm(&fb, stdout);

    fb_free(&fb);
    return EXIT_SUCCESS;
}
