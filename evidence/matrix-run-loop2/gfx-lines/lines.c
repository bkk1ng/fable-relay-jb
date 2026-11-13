/*
 * lines.c — C11, no dependencies
 *
 * Primitives:
 *   - 8-bit framebuffer with bounds-checked putpixel
 *   - Cohen-Sutherland line clipping to a viewport
 *   - Bresenham line drawing (clipped)
 *   - Midpoint circle (Bresenham variant)
 *   - Filled circle (scanline spans)
 *   - PGM dump to stdout or a named file
 *   - main(): clipped test pattern
 *
 * Build:   cc -std=c11 -Wall -Wextra -O2 -o lines lines.c
 * Run:     ./lines > out.pgm
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>   /* fabsf — only in test pattern; not needed by primitives */

/* -------------------------------------------------------------------------
 * Framebuffer
 * ---------------------------------------------------------------------- */

typedef struct {
    int      width;
    int      height;
    uint8_t *pixels;   /* row-major, top-left origin */
} Framebuffer;

/* Allocate a zeroed framebuffer.  Returns NULL on failure. */
Framebuffer *fb_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    Framebuffer *fb = malloc(sizeof *fb);
    if (!fb) return NULL;
    fb->pixels = calloc((size_t)width * (size_t)height, 1);
    if (!fb->pixels) { free(fb); return NULL; }
    fb->width  = width;
    fb->height = height;
    return fb;
}

void fb_destroy(Framebuffer *fb)
{
    if (fb) { free(fb->pixels); free(fb); }
}

void fb_clear(Framebuffer *fb, uint8_t value)
{
    memset(fb->pixels, value, (size_t)fb->width * (size_t)fb->height);
}

/* Bounds-checked pixel write.  Silently drops out-of-range coordinates. */
static inline void putpixel(Framebuffer *fb, int x, int y, uint8_t color)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    fb->pixels[(size_t)y * (size_t)fb->width + (size_t)x] = color;
}

/* -------------------------------------------------------------------------
 * Cohen-Sutherland clipping
 * ---------------------------------------------------------------------- */

typedef struct { int x0, y0, x1, y1; } Viewport;

/* Outcode bits */
#define CS_INSIDE 0
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_BOTTOM 4   /* y increases downward */
#define CS_TOP    8

static int cs_outcode(const Viewport *vp, int x, int y)
{
    int code = CS_INSIDE;
    if (x < vp->x0)      code |= CS_LEFT;
    else if (x > vp->x1) code |= CS_RIGHT;
    if (y < vp->y0)      code |= CS_TOP;
    else if (y > vp->y1) code |= CS_BOTTOM;
    return code;
}

/*
 * Clip the segment (x0,y0)-(x1,y1) to *vp using Cohen-Sutherland.
 * Returns 1 if any part of the segment is visible and writes the clipped
 * endpoints back through the pointers; returns 0 if wholly outside.
 */
int cs_clip(const Viewport *vp,
            int *x0, int *y0, int *x1, int *y1)
{
    int cx0 = *x0, cy0 = *y0, cx1 = *x1, cy1 = *y1;
    int out0 = cs_outcode(vp, cx0, cy0);
    int out1 = cs_outcode(vp, cx1, cy1);

    for (;;) {
        if (!(out0 | out1))  { /* trivially inside */
            *x0 = cx0; *y0 = cy0; *x1 = cx1; *y1 = cy1;
            return 1;
        }
        if (out0 & out1) return 0;   /* trivially outside */

        /* Pick a point that is outside the viewport. */
        int out_pick = out0 ? out0 : out1;
        int x, y;

        /* Intersect with the relevant edge.
         * Integer arithmetic: use 64-bit to avoid overflow on large coords. */
        int dx = cx1 - cx0;
        int dy = cy1 - cy0;

        if (out_pick & CS_BOTTOM) {
            x = cx0 + (int)((long long)dx * (vp->y1 - cy0) / dy);
            y = vp->y1;
        } else if (out_pick & CS_TOP) {
            x = cx0 + (int)((long long)dx * (vp->y0 - cy0) / dy);
            y = vp->y0;
        } else if (out_pick & CS_RIGHT) {
            y = cy0 + (int)((long long)dy * (vp->x1 - cx0) / dx);
            x = vp->x1;
        } else {   /* CS_LEFT */
            y = cy0 + (int)((long long)dy * (vp->x0 - cx0) / dx);
            x = vp->x0;
        }

        if (out_pick == out0) {
            cx0 = x; cy0 = y;
            out0 = cs_outcode(vp, cx0, cy0);
        } else {
            cx1 = x; cy1 = y;
            out1 = cs_outcode(vp, cx1, cy1);
        }
    }
}

/* -------------------------------------------------------------------------
 * Bresenham line (expects integer coords already within the buffer)
 * ---------------------------------------------------------------------- */

void draw_line_raw(Framebuffer *fb,
                   int x0, int y0, int x1, int y1,
                   uint8_t color)
{
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        putpixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/*
 * Clipped line: clips to *vp first, then rasterises.
 * Returns 1 if any pixels were drawn, 0 if clipped away.
 */
int draw_line(Framebuffer *fb, const Viewport *vp,
              int x0, int y0, int x1, int y1,
              uint8_t color)
{
    if (!cs_clip(vp, &x0, &y0, &x1, &y1)) return 0;
    draw_line_raw(fb, x0, y0, x1, y1, color);
    return 1;
}

/* -------------------------------------------------------------------------
 * Midpoint (Bresenham) circle — outline only
 * ---------------------------------------------------------------------- */

void draw_circle(Framebuffer *fb,
                 int cx, int cy, int r,
                 uint8_t color)
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
 * Filled circle — scanline spans derived from midpoint circle
 * ---------------------------------------------------------------------- */

void draw_filled_circle(Framebuffer *fb,
                        int cx, int cy, int r,
                        uint8_t color)
{
    if (r < 0) return;
    int x = 0, y = r, d = 1 - r;

    /* Helper: draw a horizontal span, bounds-checked via putpixel. */
    #define HSPAN(xx, yy, half)                           \
        do {                                              \
            int _hx, _x0 = (xx) - (half),                \
                      _x1 = (xx) + (half);                \
            for (_hx = _x0; _hx <= _x1; _hx++)           \
                putpixel(fb, _hx, (yy), color);           \
        } while (0)

    while (x <= y) {
        HSPAN(cx, cy + y,  x);
        HSPAN(cx, cy - y,  x);
        HSPAN(cx, cy + x,  y);
        HSPAN(cx, cy - x,  y);

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
 * PGM output (binary P5)
 * ---------------------------------------------------------------------- */

/*
 * Write the framebuffer as a binary PGM.
 * Pass path == NULL (or "-") to write to stdout.
 * Returns 0 on success, non-zero on I/O error.
 */
int fb_write_pgm(const Framebuffer *fb, const char *path)
{
    FILE *fp;
    if (!path || path[0] == '-') {
        fp = stdout;
    } else {
        fp = fopen(path, "wb");
        if (!fp) { perror(path); return 1; }
    }

    if (fprintf(fp, "P5\n%d %d\n255\n", fb->width, fb->height) < 0) {
        if (fp != stdout) fclose(fp);
        return 1;
    }

    size_t npix = (size_t)fb->width * (size_t)fb->height;
    if (fwrite(fb->pixels, 1, npix, fp) != npix) {
        if (fp != stdout) fclose(fp);
        return 1;
    }

    if (fp != stdout) fclose(fp);
    return 0;
}

/* -------------------------------------------------------------------------
 * main — clipped test pattern
 * ---------------------------------------------------------------------- */

int main(void)
{
    const int W = 512, H = 512;

    Framebuffer *fb = fb_create(W, H);
    if (!fb) { fprintf(stderr, "out of memory\n"); return 1; }

    /* Dark grey background */
    fb_clear(fb, 30);

    /*
     * Viewport: an inset rectangle that lets us demonstrate clipping.
     * Lines drawn outside this rect will be clipped.
     */
    const Viewport vp = { 64, 64, W - 65, H - 65 };

    /* Draw the viewport border itself (no clipping needed here). */
    draw_line_raw(fb, vp.x0, vp.y0, vp.x1, vp.y0, 80);
    draw_line_raw(fb, vp.x1, vp.y0, vp.x1, vp.y1, 80);
    draw_line_raw(fb, vp.x1, vp.y1, vp.x0, vp.y1, 80);
    draw_line_raw(fb, vp.x0, vp.y1, vp.x0, vp.y0, 80);

    /* ------------------------------------------------------------------ */
    /* 1. Star-burst of clipped lines radiating from the centre            */
    /* ------------------------------------------------------------------ */
    int cx = W / 2, cy = H / 2;
    int n_spokes = 36;
    for (int i = 0; i < n_spokes; i++) {
        double angle = (double)i * 3.14159265358979323846 * 2.0 / n_spokes;
        int x1 = cx + (int)(350.0 * cos(angle));
        int y1 = cy + (int)(350.0 * sin(angle));
        /* Start well outside the viewport to force clipping on some rays. */
        int x0 = cx + (int)(20.0 * cos(angle));
        int y0 = cy + (int)(20.0 * sin(angle));
        uint8_t bright = (uint8_t)(160 + (i * 95 / n_spokes));
        draw_line(fb, &vp, x0, y0, x1, y1, bright);
    }

    /* ------------------------------------------------------------------ */
    /* 2. Grid of horizontal and vertical lines (mostly clipped at edges)  */
    /* ------------------------------------------------------------------ */
    for (int s = 32; s < W; s += 64) {
        /* Extend lines past the frame boundaries to prove clipping. */
        draw_line(fb, &vp, s, -20,    s, H + 20, 55);
        draw_line(fb, &vp, -20, s, W + 20,    s, 55);
    }

    /* ------------------------------------------------------------------ */
    /* 3. Diagonal lines from corner to corner (outside-to-outside path)  */
    /* ------------------------------------------------------------------ */
    draw_line(fb, &vp, -50,  -50, W + 50, H + 50, 200);
    draw_line(fb, &vp, W + 50, -50, -50, H + 50, 200);

    /* ------------------------------------------------------------------ */
    /* 4. Concentric circles, outline                                      */
    /* ------------------------------------------------------------------ */
    for (int r = 20; r <= 160; r += 20) {
        uint8_t c = (uint8_t)(220 - r);
        draw_circle(fb, cx, cy, r, c);
    }

    /* ------------------------------------------------------------------ */
    /* 5. Filled circles in the corners, partially outside the viewport   */
    /* ------------------------------------------------------------------ */
    draw_filled_circle(fb,  vp.x0 - 20, vp.y0 - 20, 60, 180);
    draw_filled_circle(fb,  vp.x1 + 20, vp.y0 - 20, 60, 180);
    draw_filled_circle(fb,  vp.x0 - 20, vp.y1 + 20, 60, 180);
    draw_filled_circle(fb,  vp.x1 + 20, vp.y1 + 20, 60, 180);

    /* Smaller filled circles inside the viewport */
    draw_filled_circle(fb, cx - 100, cy,       35, 240);
    draw_filled_circle(fb, cx + 100, cy,       35, 240);
    draw_filled_circle(fb, cx,       cy - 100, 35, 110);
    draw_filled_circle(fb, cx,       cy + 100, 35, 110);

    /* ------------------------------------------------------------------ */
    /* 6. A few lines that are entirely outside the viewport               */
    /* (cs_clip should return 0; nothing drawn inside the viewport)        */
    /* ------------------------------------------------------------------ */
    draw_line(fb, &vp,  0,  0, 63, 63, 255);   /* top-left corner strip */
    draw_line(fb, &vp, W - 64, H - 64, W - 1, H - 1, 255);

    /* ------------------------------------------------------------------ */
    /* Write result                                                        */
    /* ------------------------------------------------------------------ */
    int rc = fb_write_pgm(fb, NULL);   /* NULL → stdout */
    fb_destroy(fb);
    return rc;
}
