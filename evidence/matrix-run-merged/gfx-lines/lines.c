/*
 * lines.c — C11, no dependencies
 *
 * Features:
 *   - 8-bit framebuffer with putpixel bounds checking
 *   - Cohen-Sutherland line clipping to a viewport
 *   - Bresenham line drawing (integer only)
 *   - Midpoint circle (8-way symmetry)
 *   - Filled circle (scanline fill via midpoint)
 *   - PGM (P5 binary) dump to stdout or a named file
 *   - main() draws a clipped test pattern
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o lines lines.c
 *
 * Run:
 *   ./lines > out.pgm
 *   open out.pgm   (macOS Preview, feh, GIMP, etc.)
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
    int      width;
    int      height;
    uint8_t *pixels;   /* row-major, top-left origin */
} Framebuffer;

/* Allocate a zeroed framebuffer.  Returns NULL on failure. */
static Framebuffer *fb_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    Framebuffer *fb = malloc(sizeof *fb);
    if (!fb) return NULL;
    fb->width  = width;
    fb->height = height;
    fb->pixels = calloc((size_t)width * (size_t)height, 1);
    if (!fb->pixels) { free(fb); return NULL; }
    return fb;
}

static void fb_destroy(Framebuffer *fb)
{
    if (fb) { free(fb->pixels); free(fb); }
}

/* Fill the entire framebuffer with a single grey value. */
static void fb_clear(Framebuffer *fb, uint8_t value)
{
    memset(fb->pixels, value, (size_t)fb->width * (size_t)fb->height);
}

/* Write one pixel; silently ignores out-of-bounds coordinates. */
static void putpixel(Framebuffer *fb, int x, int y, uint8_t value)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    fb->pixels[(size_t)y * (size_t)fb->width + (size_t)x] = value;
}

/* Write a PGM binary (P5) image to fp.  Returns 0 on success. */
static int fb_write_pgm(const Framebuffer *fb, FILE *fp)
{
    if (fprintf(fp, "P5\n%d %d\n255\n", fb->width, fb->height) < 0)
        return -1;
    size_t total = (size_t)fb->width * (size_t)fb->height;
    if (fwrite(fb->pixels, 1, total, fp) != total) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Cohen-Sutherland viewport clipping
 * ---------------------------------------------------------------------- */

typedef struct { int xmin, ymin, xmax, ymax; } Viewport;

/* Region codes */
#define CS_INSIDE 0
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_BOTTOM 4   /* y increases downward */
#define CS_TOP    8

static int cs_code(const Viewport *vp, int x, int y)
{
    int code = CS_INSIDE;
    if (x < vp->xmin) code |= CS_LEFT;
    else if (x > vp->xmax) code |= CS_RIGHT;
    if (y < vp->ymin) code |= CS_TOP;
    else if (y > vp->ymax) code |= CS_BOTTOM;
    return code;
}

/*
 * Clip the segment (x0,y0)-(x1,y1) to *vp using Cohen-Sutherland.
 * Returns 1 if any part of the segment is visible and writes the
 * clipped endpoints back; returns 0 if the segment is entirely outside.
 */
static int cs_clip(const Viewport *vp,
                   int *x0, int *y0, int *x1, int *y1)
{
    int code0 = cs_code(vp, *x0, *y0);
    int code1 = cs_code(vp, *x1, *y1);

    for (;;) {
        if (!(code0 | code1))   return 1;   /* trivially inside */
        if (code0 & code1)      return 0;   /* trivially outside */

        /* Pick the endpoint that is outside. */
        int code_out = code0 ? code0 : code1;
        int x, y;

        /* Clamp to the relevant edge.
         * Integer arithmetic: use the parametric form with rounding. */
        int dx = *x1 - *x0;
        int dy = *y1 - *y0;

        if (code_out & CS_BOTTOM) {
            /* y = vp->ymax */
            x = *x0 + (dx * (vp->ymax - *y0) + (dy / 2)) / dy;
            y = vp->ymax;
        } else if (code_out & CS_TOP) {
            /* y = vp->ymin */
            x = *x0 + (dx * (vp->ymin - *y0) + (dy / 2)) / dy;
            y = vp->ymin;
        } else if (code_out & CS_RIGHT) {
            /* x = vp->xmax */
            y = *y0 + (dy * (vp->xmax - *x0) + (dx / 2)) / dx;
            x = vp->xmax;
        } else {
            /* x = vp->xmin */
            y = *y0 + (dy * (vp->xmin - *x0) + (dx / 2)) / dx;
            x = vp->xmin;
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
 * Bresenham line drawing (after Cohen-Sutherland clipping)
 * ---------------------------------------------------------------------- */

static void draw_line(Framebuffer *fb, const Viewport *vp,
                      int x0, int y0, int x1, int y1,
                      uint8_t colour)
{
    if (vp && !cs_clip(vp, &x0, &y0, &x1, &y1)) return;

    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        putpixel(fb, x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* -------------------------------------------------------------------------
 * Midpoint circle (outline only, 8-way symmetry)
 * ---------------------------------------------------------------------- */

static void plot8(Framebuffer *fb, int cx, int cy, int x, int y,
                  uint8_t colour)
{
    putpixel(fb, cx + x, cy + y, colour);
    putpixel(fb, cx - x, cy + y, colour);
    putpixel(fb, cx + x, cy - y, colour);
    putpixel(fb, cx - x, cy - y, colour);
    putpixel(fb, cx + y, cy + x, colour);
    putpixel(fb, cx - y, cy + x, colour);
    putpixel(fb, cx + y, cy - x, colour);
    putpixel(fb, cx - y, cy - x, colour);
}

static void draw_circle(Framebuffer *fb, int cx, int cy, int r,
                        uint8_t colour)
{
    if (r < 0) return;
    int x = 0, y = r;
    int d = 1 - r;
    plot8(fb, cx, cy, x, y, colour);
    while (x < y) {
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
        plot8(fb, cx, cy, x, y, colour);
    }
}

/* -------------------------------------------------------------------------
 * Filled circle (scanline, derived from midpoint boundary)
 * ---------------------------------------------------------------------- */

/*
 * We walk the top-right octant of the boundary to find the half-width of
 * the circle at each scanline, then fill that horizontal span.
 */
static void draw_filled_circle(Framebuffer *fb, int cx, int cy, int r,
                                uint8_t colour)
{
    if (r < 0) return;

    /* Allocate a half-width table indexed by row offset [0..r]. */
    int *hw = malloc((size_t)(r + 1) * sizeof *hw);
    if (!hw) return;

    /* Initialise: nothing guaranteed yet. */
    for (int i = 0; i <= r; i++) hw[i] = 0;

    /* Walk the boundary and record the rightmost x for each y offset. */
    int x = 0, y = r;
    int d = 1 - r;
    /* Both (x,y) and (y,x) contribute to half-widths. */
    hw[y] = x;  /* row offset y → half-width x */
    hw[x] = y;  /* row offset x → half-width y */
    while (x < y) {
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
        if (x > hw[y]) hw[y] = x;
        if (y > hw[x]) hw[x] = y;
    }

    /* Fill each horizontal span. */
    for (int dy = 0; dy <= r; dy++) {
        int w = hw[dy];
        for (int px = cx - w; px <= cx + w; px++) {
            putpixel(fb, px, cy - dy, colour);
            putpixel(fb, px, cy + dy, colour);
        }
    }

    free(hw);
}

/* -------------------------------------------------------------------------
 * Test pattern
 * ---------------------------------------------------------------------- */

static void draw_test_pattern(Framebuffer *fb)
{
    int W = fb->width;
    int H = fb->height;

    /* Viewport sits 10% inside the framebuffer on each side. */
    Viewport vp = {
        .xmin = W / 10,
        .ymin = H / 10,
        .xmax = W - W / 10 - 1,
        .ymax = H - H / 10 - 1
    };

    /* --- Draw viewport border (no clipping applied to it) --- */
    draw_line(fb, NULL, vp.xmin, vp.ymin, vp.xmax, vp.ymin, 180);
    draw_line(fb, NULL, vp.xmax, vp.ymin, vp.xmax, vp.ymax, 180);
    draw_line(fb, NULL, vp.xmax, vp.ymax, vp.xmin, vp.ymax, 180);
    draw_line(fb, NULL, vp.xmin, vp.ymax, vp.xmin, vp.ymin, 180);

    /* --- Clipped star-burst from centre --- */
    int cx = W / 2, cy = H / 2;
    int r  = (W < H ? W : H) / 2 + 20;   /* radius beyond the viewport */
    for (int angle = 0; angle < 360; angle += 15) {
        double rad = angle * 3.14159265358979 / 180.0;
        int x1 = cx + (int)(r * cos(rad));
        int y1 = cy + (int)(r * sin(rad));
        draw_line(fb, &vp, cx, cy, x1, y1, 220);
    }

    /* --- Clipped diagonal cross --- */
    draw_line(fb, &vp, 0,   0,   W-1, H-1, 200);
    draw_line(fb, &vp, W-1, 0,   0,   H-1, 200);

    /* --- Clipped horizontal and vertical axes --- */
    draw_line(fb, &vp, 0, cy, W-1, cy, 160);
    draw_line(fb, &vp, cx, 0, cx, H-1, 160);

    /* --- Circle outlines at a few radii --- */
    int small_r  = (W < H ? W : H) / 8;
    int medium_r = (W < H ? W : H) / 5;
    draw_circle(fb, cx, cy, small_r,  240);
    draw_circle(fb, cx, cy, medium_r, 200);

    /* --- Off-centre filled circles --- */
    int fr = small_r / 2;
    draw_filled_circle(fb, cx - medium_r, cy, fr, 128);
    draw_filled_circle(fb, cx + medium_r, cy, fr, 128);
    draw_filled_circle(fb, cx, cy - medium_r, fr,  96);
    draw_filled_circle(fb, cx, cy + medium_r, fr,  96);

    /* --- A few lines that extend beyond the viewport to show clipping --- */
    draw_line(fb, &vp, -50,       cy,      W + 50, cy - 80, 255);
    draw_line(fb, &vp, cx - 80,  -50,      cx + 80, H + 50, 255);
    draw_line(fb, &vp, 0,         H + 100, W,       -100,   210);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int width  = 640;
    int height = 480;

    Framebuffer *fb = fb_create(width, height);
    if (!fb) {
        fprintf(stderr, "lines: failed to allocate framebuffer\n");
        return EXIT_FAILURE;
    }

    fb_clear(fb, 30);          /* dark background */
    draw_test_pattern(fb);

    /* Write PGM to a named file or stdout. */
    FILE *out = stdout;
    if (argc >= 2) {
        out = fopen(argv[1], "wb");
        if (!out) {
            perror(argv[1]);
            fb_destroy(fb);
            return EXIT_FAILURE;
        }
    } else {
        /* stdout: set binary mode on platforms that require it */
#ifdef _WIN32
        #include <fcntl.h>
        #include <io.h>
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    }

    int rc = fb_write_pgm(fb, out);
    if (rc != 0)
        fprintf(stderr, "lines: write error\n");

    if (out != stdout) fclose(out);
    fb_destroy(fb);

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
