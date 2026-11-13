/*
 * lines.c — C11, no dependencies
 *
 * - 8-bit framebuffer with bounds-checked putpixel
 * - PGM (P5 binary) dump to stdout
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

/* -------------------------------------------------------------------------
 * Framebuffer
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *pixels;
    int      width;
    int      height;
} Framebuffer;

/* Allocate a zeroed framebuffer; returns NULL on failure. */
Framebuffer *fb_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    Framebuffer *fb = malloc(sizeof(*fb));
    if (!fb) return NULL;
    fb->pixels = calloc((size_t)width * (size_t)height, 1);
    if (!fb->pixels) { free(fb); return NULL; }
    fb->width  = width;
    fb->height = height;
    return fb;
}

void fb_destroy(Framebuffer *fb)
{
    if (!fb) return;
    free(fb->pixels);
    free(fb);
}

void fb_clear(Framebuffer *fb, uint8_t color)
{
    memset(fb->pixels, color, (size_t)fb->width * (size_t)fb->height);
}

/* Bounds-checked putpixel — silently ignores out-of-range coordinates. */
static inline void putpixel(Framebuffer *fb, int x, int y, uint8_t color)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    fb->pixels[(size_t)y * (size_t)fb->width + (size_t)x] = color;
}

/* Write a PGM P5 (binary) image to fp. */
void fb_write_pgm(const Framebuffer *fb, FILE *fp)
{
    fprintf(fp, "P5\n%d %d\n255\n", fb->width, fb->height);
    fwrite(fb->pixels, 1, (size_t)fb->width * (size_t)fb->height, fp);
}

/* -------------------------------------------------------------------------
 * Viewport
 * ---------------------------------------------------------------------- */

typedef struct { int xmin, ymin, xmax, ymax; } Viewport;

/* -------------------------------------------------------------------------
 * Cohen-Sutherland clipping
 * ---------------------------------------------------------------------- */

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
    if (y < vp->ymin)      code |= CS_BOTTOM;
    else if (y > vp->ymax) code |= CS_TOP;
    return code;
}

/*
 * Clip line (x0,y0)-(x1,y1) to *vp using Cohen-Sutherland.
 * Returns 1 if any part of the line is inside; *x0..*y1 updated.
 * Returns 0 if the line is entirely outside.
 */
int cs_clip(const Viewport *vp,
            int *x0, int *y0, int *x1, int *y1)
{
    int oc0 = cs_outcode(vp, *x0, *y0);
    int oc1 = cs_outcode(vp, *x1, *y1);

    while (1) {
        if (!(oc0 | oc1)) return 1;        /* trivially inside */
        if (oc0 & oc1)    return 0;        /* trivially outside */

        /* Pick an endpoint outside the viewport. */
        int oc = oc0 ? oc0 : oc1;
        int x, y;

        /* Intersect with the relevant edge.
         * Integer arithmetic with rounding avoids fp dependency. */
        int dx = *x1 - *x0;
        int dy = *y1 - *y0;

        if (oc & CS_TOP) {
            x = *x0 + (int)((long)dx * (vp->ymax - *y0) / dy);
            y = vp->ymax;
        } else if (oc & CS_BOTTOM) {
            x = *x0 + (int)((long)dx * (vp->ymin - *y0) / dy);
            y = vp->ymin;
        } else if (oc & CS_RIGHT) {
            y = *y0 + (int)((long)dy * (vp->xmax - *x0) / dx);
            x = vp->xmax;
        } else { /* CS_LEFT */
            y = *y0 + (int)((long)dy * (vp->xmin - *x0) / dx);
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

/* -------------------------------------------------------------------------
 * Bresenham line drawing (no clipping — call cs_clip first if needed)
 * ---------------------------------------------------------------------- */

void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint8_t color)
{
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        putpixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/*
 * Clip then draw.  Skips the draw entirely if the line is outside *vp.
 */
void draw_line_clipped(Framebuffer *fb, const Viewport *vp,
                       int x0, int y0, int x1, int y1, uint8_t color)
{
    if (cs_clip(vp, &x0, &y0, &x1, &y1))
        draw_line(fb, x0, y0, x1, y1, color);
}

/* -------------------------------------------------------------------------
 * Midpoint circle (outline, 8-way symmetry)
 * ---------------------------------------------------------------------- */

void draw_circle(Framebuffer *fb, int cx, int cy, int r, uint8_t color)
{
    if (r < 0) return;
    int x = 0, y = r, d = 1 - r;

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

/* -------------------------------------------------------------------------
 * Filled circle (scanline horizontal spans, midpoint decision per row)
 * ---------------------------------------------------------------------- */

void draw_filled_circle(Framebuffer *fb, int cx, int cy, int r, uint8_t color)
{
    if (r < 0) return;
    int x = 0, y = r, d = 1 - r;

    /* Draw horizontal span from (cx-spanx, row) to (cx+spanx, row). */
    #define HSPAN(row, spanx) \
        do { \
            int _r = (row), _s = (spanx); \
            for (int _x = cx - _s; _x <= cx + _s; _x++) \
                putpixel(fb, _x, _r, color); \
        } while (0)

    while (x <= y) {
        HSPAN(cy + y, x);
        HSPAN(cy - y, x);
        HSPAN(cy + x, y);
        HSPAN(cy - x, y);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
    #undef HSPAN
}

/* -------------------------------------------------------------------------
 * main — test pattern
 * ---------------------------------------------------------------------- */

int main(void)
{
    const int W = 512, H = 512;
    Framebuffer *fb = fb_create(W, H);
    if (!fb) { fputs("fb_create failed\n", stderr); return 1; }

    fb_clear(fb, 20);   /* dark background */

    /* Viewport: inset 60 px on each side */
    Viewport vp = { 60, 60, W - 61, H - 61 };

    /* Draw the viewport border itself */
    draw_line(fb, vp.xmin, vp.ymin, vp.xmax, vp.ymin, 180);
    draw_line(fb, vp.xmax, vp.ymin, vp.xmax, vp.ymax, 180);
    draw_line(fb, vp.xmax, vp.ymax, vp.xmin, vp.ymax, 180);
    draw_line(fb, vp.xmin, vp.ymax, vp.xmin, vp.ymin, 180);

    /* Radiating lines from centre — most extend beyond the viewport,
     * demonstrating Cohen-Sutherland clipping. */
    int cx = W / 2, cy = H / 2;
    for (int i = 0; i < 36; i++) {
        double angle = i * (3.14159265358979 * 2.0 / 36.0);
        int ex = cx + (int)(320.0 * cos(angle));
        int ey = cy + (int)(320.0 * sin(angle));
        uint8_t bright = (uint8_t)(80 + (i % 6) * 29);
        draw_line_clipped(fb, &vp, cx, cy, ex, ey, bright);
    }

    /* Grid of clipped diagonal lines */
    for (int i = 0; i < W; i += 40) {
        draw_line_clipped(fb, &vp,  i,    0,  i + 80, H - 1, 60);
        draw_line_clipped(fb, &vp,  0,    i,  W - 1,  i + 80, 60);
    }

    /* Filled circles — some overlap the viewport edge to show clipping via
     * putpixel bounds check */
    draw_filled_circle(fb, cx,       cy,       70, 100);
    draw_filled_circle(fb, cx - 110, cy + 90,  40, 140);
    draw_filled_circle(fb, cx + 110, cy - 90,  40, 140);
    draw_filled_circle(fb, 20,       20,        35, 90);  /* corner, mostly clipped */

    /* Outline circles */
    draw_circle(fb, cx, cy, 90,  220);
    draw_circle(fb, cx, cy, 120, 200);
    draw_circle(fb, cx, cy, 150, 180);
    draw_circle(fb, cx, cy, 190, 160);

    /* Cross-hair */
    draw_line(fb, cx - 200, cy, cx + 200, cy, 240);
    draw_line(fb, cx, cy - 200, cx, cy + 200, 240);

    fb_write_pgm(fb, stdout);
    fb_destroy(fb);
    return 0;
}
