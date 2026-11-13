"""
shapes.py — tiny 2-D canvas (Python 3.12, stdlib only)

Canvas:  RGB bytearray, row-major, top-left origin.
Ops:     filled_rect, filled_circle, line (with thickness), flood_fill
Export:  write_bmp  → uncompressed 24-bit BMP file
Demo:    main()     → overlapping-shapes scene → demo.bmp
"""

from __future__ import annotations

import math
import struct
from collections import deque
from pathlib import Path


# ---------------------------------------------------------------------------
# Canvas
# ---------------------------------------------------------------------------

class Canvas:
    """RGB bytearray canvas."""

    def __init__(self, width: int, height: int, bg: tuple[int, int, int] = (255, 255, 255)) -> None:
        self.w = width
        self.h = height
        self.buf = bytearray(width * height * 3)
        if bg != (255, 255, 255) or True:          # always fill bg
            r, g, b = bg
            for i in range(0, len(self.buf), 3):
                self.buf[i]     = r
                self.buf[i + 1] = g
                self.buf[i + 2] = b

    # ---- low-level pixel access ----------------------------------------

    def _in_bounds(self, x: int, y: int) -> bool:
        return 0 <= x < self.w and 0 <= y < self.h

    def put_pixel(self, x: int, y: int, color: tuple[int, int, int]) -> None:
        if self._in_bounds(x, y):
            idx = (y * self.w + x) * 3
            self.buf[idx]     = color[0]
            self.buf[idx + 1] = color[1]
            self.buf[idx + 2] = color[2]

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        idx = (y * self.w + x) * 3
        return (self.buf[idx], self.buf[idx + 1], self.buf[idx + 2])

    # ---- drawing primitives --------------------------------------------

    def filled_rect(
        self,
        x: int, y: int,
        width: int, height: int,
        color: tuple[int, int, int],
    ) -> None:
        """Fill an axis-aligned rectangle."""
        x0 = max(x, 0)
        y0 = max(y, 0)
        x1 = min(x + width,  self.w)
        y1 = min(y + height, self.h)
        r, g, b = color
        for row in range(y0, y1):
            base = (row * self.w + x0) * 3
            for col in range(x1 - x0):
                self.buf[base]     = r
                self.buf[base + 1] = g
                self.buf[base + 2] = b
                base += 3

    def filled_circle(
        self,
        cx: int, cy: int,
        radius: int,
        color: tuple[int, int, int],
    ) -> None:
        """Fill a circle using the midpoint scan-line approach."""
        r2 = radius * radius
        x0 = max(cx - radius, 0)
        x1 = min(cx + radius + 1, self.w)
        y0 = max(cy - radius, 0)
        y1 = min(cy + radius + 1, self.h)
        rc, gc, bc = color
        for y in range(y0, y1):
            dy = y - cy
            dx = int(math.sqrt(max(r2 - dy * dy, 0)))
            lx = max(cx - dx, 0)
            rx = min(cx + dx + 1, self.w)
            base = (y * self.w + lx) * 3
            for _ in range(rx - lx):
                self.buf[base]     = rc
                self.buf[base + 1] = gc
                self.buf[base + 2] = bc
                base += 3

    def line(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
        color: tuple[int, int, int],
        thickness: int = 1,
    ) -> None:
        """
        Bresenham line with optional thickness.

        Thickness is achieved by drawing a filled circle of radius
        ``thickness // 2`` at each step — simple and GPU-free.
        """
        thickness = max(1, thickness)
        half = thickness // 2

        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy

        x, y = x0, y0

        while True:
            if thickness == 1:
                self.put_pixel(x, y, color)
            else:
                self.filled_circle(x, y, half, color)

            if x == x1 and y == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x   += sx
            if e2 < dx:
                err += dx
                y   += sy

    def flood_fill(
        self,
        x: int, y: int,
        fill_color: tuple[int, int, int],
    ) -> None:
        """
        4-connected iterative flood fill.

        Replaces the color at (x, y) with *fill_color* across all
        contiguous pixels sharing the original color.
        """
        if not self._in_bounds(x, y):
            return
        target = self.get_pixel(x, y)
        if target == fill_color:
            return

        queue: deque[tuple[int, int]] = deque()
        queue.append((x, y))
        visited: set[tuple[int, int]] = set()
        visited.add((x, y))

        while queue:
            px, py = queue.popleft()
            self.put_pixel(px, py, fill_color)
            for nx, ny in ((px - 1, py), (px + 1, py), (px, py - 1), (px, py + 1)):
                if (
                    self._in_bounds(nx, ny)
                    and (nx, ny) not in visited
                    and self.get_pixel(nx, ny) == target
                ):
                    visited.add((nx, ny))
                    queue.append((nx, ny))


# ---------------------------------------------------------------------------
# BMP writer  (uncompressed 24-bit, no extra headers)
# ---------------------------------------------------------------------------

def write_bmp(canvas: Canvas, path: str | Path) -> None:
    """
    Write *canvas* to an uncompressed 24-bit BMP file.

    BMP stores rows bottom-to-top; each row is padded to a 4-byte boundary.
    """
    w, h = canvas.w, canvas.h
    row_stride = w * 3
    pad = (4 - row_stride % 4) % 4          # bytes of padding per row
    padded_stride = row_stride + pad
    pixel_data_size = padded_stride * h

    # --- BMP file header (14 bytes) ---
    file_size = 14 + 40 + pixel_data_size
    bfh = struct.pack(
        "<2sIHHI",
        b"BM",        # signature
        file_size,
        0,            # reserved
        0,            # reserved
        14 + 40,      # pixel data offset
    )

    # --- DIB / BITMAPINFOHEADER (40 bytes) ---
    dib = struct.pack(
        "<IiiHHIIiiII",
        40,            # header size
        w, h,          # positive h → bottom-up rows
        1,             # color planes
        24,            # bits per pixel
        0,             # BI_RGB (no compression)
        pixel_data_size,
        2835, 2835,    # ~72 DPI
        0, 0,          # colors in table / important colors
    )

    padding = b"\x00" * pad
    with open(path, "wb") as f:
        f.write(bfh)
        f.write(dib)
        # BMP rows are stored bottom-to-top, and each pixel is BGR not RGB.
        for row in range(h - 1, -1, -1):
            base = row * w * 3
            row_bytes = bytearray(row_stride)
            for col in range(w):
                src = base + col * 3
                dst = col * 3
                row_bytes[dst]     = canvas.buf[src + 2]   # B
                row_bytes[dst + 1] = canvas.buf[src + 1]   # G
                row_bytes[dst + 2] = canvas.buf[src]       # R
            f.write(row_bytes)
            f.write(padding)


# ---------------------------------------------------------------------------
# Demo scene
# ---------------------------------------------------------------------------

def main() -> None:
    W, H = 640, 480
    c = Canvas(W, H, bg=(30, 30, 40))           # dark charcoal background

    # --- back layer: big overlapping rectangles -------------------------
    c.filled_rect(40,  60, 220, 160, (200,  60,  60))   # red rect
    c.filled_rect(130, 100, 220, 160, ( 60, 160, 200))  # blue rect (overlaps red)
    c.filled_rect( 80, 180, 220, 140, ( 80, 190,  90))  # green rect (overlaps both)

    # --- mid layer: circles --------------------------------------------
    c.filled_circle(380, 160, 100, (230, 180,  40))     # yellow sun-ish
    c.filled_circle(460, 220,  70, (180,  80, 200))     # purple overlap
    c.filled_circle(310, 240,  60, (240, 120,  50))     # orange

    # --- flood-fill an enclosed region ---------------------------------
    # Draw a closed rectangle outline with thick lines, then fill inside.
    box_x, box_y, box_w, box_h = 420, 310, 150, 100
    outline_color = (240, 240, 240)
    c.line(box_x,           box_y,           box_x + box_w, box_y,           outline_color, 3)
    c.line(box_x + box_w,   box_y,           box_x + box_w, box_y + box_h,   outline_color, 3)
    c.line(box_x + box_w,   box_y + box_h,   box_x,         box_y + box_h,   outline_color, 3)
    c.line(box_x,           box_y + box_h,   box_x,         box_y,           outline_color, 3)
    # Flood-fill the interior (seed one pixel inside).
    c.flood_fill(box_x + box_w // 2, box_y + box_h // 2, (255, 220, 100))

    # --- foreground lines of varying thickness -------------------------
    c.line(  0, 400, 640,  80, (255, 255, 255), 2)   # thin white diagonal
    c.line(  0, 430, 640, 110, (200, 100, 100), 5)   # medium red diagonal
    c.line(  0, 460, 640, 140, ( 80, 180, 240), 10)  # thick blue diagonal

    # --- small decorative circles on top of everything -----------------
    for i, (px, py, pr, col) in enumerate([
        (100, 400, 25, (255, 200,  50)),
        (200, 420, 20, ( 50, 220, 180)),
        (300, 410, 30, (220,  80, 150)),
        (500, 390, 22, (100, 240,  80)),
        (570, 430, 18, (255, 140,  40)),
    ]):
        c.filled_circle(px, py, pr, col)

    out = Path("demo.bmp")
    write_bmp(c, out)
    print(f"Saved {out}  ({W}×{H} px)")


if __name__ == "__main__":
    main()
