"""
shapes.py — tiny 2D software-rasteriser + BMP writer (Python 3.12, stdlib only)

Public API
----------
Canvas(width, height, bg=(255,255,255))
    .fill_rect(x, y, w, h, color)
    .fill_circle(cx, cy, r, color)
    .draw_line(x0, y0, x1, y1, color, thickness=1)
    .flood_fill(x, y, color)
    .save_bmp(path)

Color = tuple[int, int, int]  — (R, G, B), each 0-255
"""

from __future__ import annotations

import struct
import math
from collections import deque


# ---------------------------------------------------------------------------
# Canvas
# ---------------------------------------------------------------------------

class Canvas:
    """RGB bytearray canvas with software-rasterised drawing primitives."""

    def __init__(self, width: int, height: int, bg: tuple[int,int,int] = (255, 255, 255)) -> None:
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive")
        self.width  = width
        self.height = height
        # flat array: pixel (x,y) → offset (y*width+x)*3
        self._buf: bytearray = bytearray(width * height * 3)
        self.fill_rect(0, 0, width, height, bg)

    # ------------------------------------------------------------------
    # Low-level pixel access
    # ------------------------------------------------------------------

    def _offset(self, x: int, y: int) -> int:
        return (y * self.width + x) * 3

    def _set(self, x: int, y: int, color: tuple[int,int,int]) -> None:
        """Write one pixel; silently clips to canvas bounds."""
        if 0 <= x < self.width and 0 <= y < self.height:
            o = self._offset(x, y)
            self._buf[o]     = color[0]
            self._buf[o + 1] = color[1]
            self._buf[o + 2] = color[2]

    def _get(self, x: int, y: int) -> tuple[int,int,int]:
        o = self._offset(x, y)
        return (self._buf[o], self._buf[o + 1], self._buf[o + 2])

    # ------------------------------------------------------------------
    # Filled rectangle
    # ------------------------------------------------------------------

    def fill_rect(
        self,
        x: int, y: int,
        w: int, h: int,
        color: tuple[int,int,int],
    ) -> None:
        """Fill axis-aligned rectangle.  Clips to canvas bounds."""
        x0 = max(x, 0)
        y0 = max(y, 0)
        x1 = min(x + w, self.width)
        y1 = min(y + h, self.height)
        r, g, b = color
        row_bytes = bytes([r, g, b]) * (x1 - x0)
        stride    = self.width * 3
        for row in range(y0, y1):
            base = row * stride + x0 * 3
            self._buf[base : base + len(row_bytes)] = row_bytes

    # ------------------------------------------------------------------
    # Filled circle  (Bresenham midpoint)
    # ------------------------------------------------------------------

    def fill_circle(
        self,
        cx: int, cy: int,
        r: int,
        color: tuple[int,int,int],
    ) -> None:
        """Fill axis-aligned circle using the midpoint scan-line algorithm."""
        if r <= 0:
            return
        x = 0
        y = r
        d = 1 - r
        while x <= y:
            # fill the two horizontal spans at ±y and ±x
            self._hspan(cx - y, cx + y, cy + x, color)
            self._hspan(cx - y, cx + y, cy - x, color)
            self._hspan(cx - x, cx + x, cy + y, color)
            self._hspan(cx - x, cx + x, cy - y, color)
            if d < 0:
                d += 2 * x + 3
            else:
                d += 2 * (x - y) + 5
                y -= 1
            x += 1

    def _hspan(self, x0: int, x1: int, y: int, color: tuple[int,int,int]) -> None:
        """Draw a clipped horizontal run of pixels."""
        if y < 0 or y >= self.height:
            return
        xa = max(x0, 0)
        xb = min(x1 + 1, self.width)
        if xa >= xb:
            return
        r, g, b = color
        row_bytes = bytes([r, g, b]) * (xb - xa)
        base = y * self.width * 3 + xa * 3
        self._buf[base : base + len(row_bytes)] = row_bytes

    # ------------------------------------------------------------------
    # Thick line  (Xiaolin Wu centre-line + perpendicular rect widening)
    # ------------------------------------------------------------------

    def draw_line(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
        color: tuple[int,int,int],
        thickness: int = 1,
    ) -> None:
        """
        Draw a line from (x0,y0) to (x1,y1).
        thickness ≥ 1; for thickness == 1 uses a standard Bresenham rasteriser;
        for thickness > 1 stamps a filled circle at each pixel along the spine.
        """
        thickness = max(1, int(thickness))
        if thickness == 1:
            self._bresenham(x0, y0, x1, y1, color)
        else:
            # Walk the Bresenham spine and stamp filled circles for rounded caps
            # and uniform width.
            r = thickness // 2
            for px, py in self._bresenham_points(x0, y0, x1, y1):
                self.fill_circle(px, py, r, color)

    def _bresenham(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
        color: tuple[int,int,int],
    ) -> None:
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self._set(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0  += sx
            if e2 <  dx:
                err += dx
                y0  += sy

    def _bresenham_points(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
    ) -> list[tuple[int,int]]:
        """Return the pixel coordinates of a Bresenham line."""
        points: list[tuple[int,int]] = []
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            points.append((x0, y0))
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0  += sx
            if e2 <  dx:
                err += dx
                y0  += sy
        return points

    # ------------------------------------------------------------------
    # Flood fill  (BFS, 4-connected)
    # ------------------------------------------------------------------

    def flood_fill(
        self,
        x: int, y: int,
        color: tuple[int,int,int],
    ) -> None:
        """
        Flood-fill starting at (x, y) replacing its color with *color*.
        Uses a scanline-BFS approach to keep the queue small on large fills.
        """
        if not (0 <= x < self.width and 0 <= y < self.height):
            return
        target = self._get(x, y)
        if target == color:
            return

        queue: deque[tuple[int,int]] = deque()
        queue.append((x, y))
        visited: set[int] = set()
        visited.add(y * self.width + x)

        while queue:
            px, py = queue.popleft()
            if self._get(px, py) != target:
                continue
            # extend left and right along this row
            lx = px
            while lx - 1 >= 0 and self._get(lx - 1, py) == target:
                lx -= 1
            rx = px
            while rx + 1 < self.width and self._get(rx + 1, py) == target:
                rx += 1
            # paint the span
            self._hspan(lx, rx, py, color)
            # enqueue spans above and below
            for nx in range(lx, rx + 1):
                for ny in (py - 1, py + 1):
                    if 0 <= ny < self.height:
                        idx = ny * self.width + nx
                        if idx not in visited and self._get(nx, ny) == target:
                            visited.add(idx)
                            queue.append((nx, ny))

    # ------------------------------------------------------------------
    # BMP writer (24-bit uncompressed, bottom-up rows)
    # ------------------------------------------------------------------

    def save_bmp(self, path: str) -> None:
        """Write the canvas to *path* as a 24-bit uncompressed BMP."""
        w, h       = self.width, self.height
        row_bytes  = w * 3
        pad        = (4 - row_bytes % 4) % 4          # each row padded to 4 bytes
        padded_row = row_bytes + pad
        pixel_data_size = padded_row * h
        file_size       = 54 + pixel_data_size

        # BITMAPFILEHEADER (14 bytes)
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",          # signature
            file_size,      # file size
            0,              # reserved1
            0,              # reserved2
            54,             # pixel data offset
        )
        # BITMAPINFOHEADER (40 bytes)
        info_header = struct.pack(
            "<IiiHHIIiiII",
            40,             # header size
            w,              # width
            -h,             # negative height → top-down row order
            1,              # color planes
            24,             # bits per pixel
            0,              # BI_RGB compression
            pixel_data_size,
            2835, 2835,     # ~72 dpi
            0, 0,           # colors in table / important colors
        )

        padding = b"\x00" * pad
        with open(path, "wb") as f:
            f.write(file_header)
            f.write(info_header)
            stride = w * 3
            for row in range(h):
                base = row * stride
                # BMP stores pixels as BGR
                rgb_row = self._buf[base : base + stride]
                bgr_row = bytearray(stride)
                for i in range(w):
                    bgr_row[i * 3]     = rgb_row[i * 3 + 2]
                    bgr_row[i * 3 + 1] = rgb_row[i * 3 + 1]
                    bgr_row[i * 3 + 2] = rgb_row[i * 3]
                f.write(bgr_row)
                f.write(padding)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def main() -> None:
    W, H = 480, 360
    c = Canvas(W, H, bg=(30, 30, 40))

    # --- background grid of dark rectangles ---
    for col in range(0, W, 60):
        for row in range(0, H, 60):
            c.fill_rect(col + 2, row + 2, 54, 54, (40, 40, 55))

    # --- large overlapping filled circles ---
    c.fill_circle(160, 160, 100, (220,  60,  60))   # red
    c.fill_circle(260, 160, 100, ( 60, 180,  60))   # green  — overlaps red
    c.fill_circle(210, 240,  90, ( 60,  80, 220))   # blue   — overlaps both

    # --- filled rectangles overlapping the circles ---
    c.fill_rect(60,  260, 120, 70, (240, 180,  30))  # yellow
    c.fill_rect(140, 280, 120, 60, (180,  30, 200))  # purple — overlaps yellow

    # --- thick diagonal lines ---
    c.draw_line(  0,   0, W-1, H-1, (255, 255, 255), thickness=4)  # white diagonal
    c.draw_line(W-1,   0,   0, H-1, (255, 200,   0), thickness=3)  # gold cross-diagonal

    # --- thin accent lines forming a cross-hair ---
    c.draw_line(W//2,  0, W//2, H-1, (100, 220, 220), thickness=1)
    c.draw_line(  0, H//2, W-1, H//2, (100, 220, 220), thickness=1)

    # --- small filled circles at the quadrant centres ---
    for qx, qy, col in [
        (W//4,     H//4,   (255, 128,   0)),
        (3*W//4,   H//4,   (  0, 200, 255)),
        (W//4,   3*H//4,   (200, 255,   0)),
        (3*W//4, 3*H//4,   (255,   0, 128)),
    ]:
        c.fill_circle(qx, qy, 22, col)

    # --- flood fill: replace the dark corner squares with a teal tint ---
    # top-left corner square background cell
    c.flood_fill(1, 1, (20, 80, 80))

    # --- frame around the canvas ---
    border = (200, 200, 200)
    c.fill_rect(0,     0,     W,  3, border)   # top
    c.fill_rect(0,   H-3,     W,  3, border)   # bottom
    c.fill_rect(0,     0,     3,  H, border)   # left
    c.fill_rect(W-3,   0,     3,  H, border)   # right

    out = "demo.bmp"
    c.save_bmp(out)
    print(f"Saved {out}  ({W}×{H} px)")


if __name__ == "__main__":
    main()
