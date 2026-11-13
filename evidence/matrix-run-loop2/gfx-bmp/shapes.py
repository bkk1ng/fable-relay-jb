"""
shapes.py — tiny 2-D software-rasteriser + BMP writer.
Python 3.12, stdlib only.
"""

from __future__ import annotations

import math
import struct


# ---------------------------------------------------------------------------
# Canvas
# ---------------------------------------------------------------------------

class Canvas:
    """RGB bytearray canvas with basic 2-D drawing primitives."""

    def __init__(self, width: int, height: int, bg: tuple[int, int, int] = (255, 255, 255)) -> None:
        self.width = width
        self.height = height
        self._buf = bytearray(bg * 3 * width * height)
        # fill with bg colour
        r, g, b = bg
        for i in range(width * height):
            base = i * 3
            self._buf[base]     = r
            self._buf[base + 1] = g
            self._buf[base + 2] = b

    # ------------------------------------------------------------------
    # Low-level pixel access
    # ------------------------------------------------------------------

    def _in_bounds(self, x: int, y: int) -> bool:
        return 0 <= x < self.width and 0 <= y < self.height

    def put_pixel(self, x: int, y: int, colour: tuple[int, int, int]) -> None:
        if not self._in_bounds(x, y):
            return
        base = (y * self.width + x) * 3
        self._buf[base]     = colour[0]
        self._buf[base + 1] = colour[1]
        self._buf[base + 2] = colour[2]

    # ------------------------------------------------------------------
    # Filled rectangle
    # ------------------------------------------------------------------

    def fill_rect(
        self,
        x: int, y: int,
        w: int, h: int,
        colour: tuple[int, int, int],
    ) -> None:
        """Draw a solid-filled axis-aligned rectangle."""
        x0 = max(x, 0)
        y0 = max(y, 0)
        x1 = min(x + w, self.width)
        y1 = min(y + h, self.height)
        r, g, b = colour
        row_bytes = (x1 - x0) * 3
        row = bytes([r, g, b] * (x1 - x0))
        for row_y in range(y0, y1):
            base = (row_y * self.width + x0) * 3
            self._buf[base : base + row_bytes] = row

    # ------------------------------------------------------------------
    # Filled circle  (mid-point / Bresenham scan-fill)
    # ------------------------------------------------------------------

    def fill_circle(
        self,
        cx: int, cy: int,
        radius: int,
        colour: tuple[int, int, int],
    ) -> None:
        """Draw a solid-filled circle using the midpoint algorithm."""
        if radius <= 0:
            return
        r, g, b = colour

        def hline(lx: int, rx: int, hy: int) -> None:
            lx = max(lx, 0)
            rx = min(rx, self.width - 1)
            if lx > rx or not (0 <= hy < self.height):
                return
            seg = bytes([r, g, b] * (rx - lx + 1))
            base = (hy * self.width + lx) * 3
            self._buf[base : base + len(seg)] = seg

        x = 0
        y = radius
        d = 1 - radius
        while x <= y:
            hline(cx - y, cx + y, cy + x)
            hline(cx - y, cx + y, cy - x)
            hline(cx - x, cx + x, cy + y)
            hline(cx - x, cx + x, cy - y)
            if d < 0:
                d += 2 * x + 3
            else:
                d += 2 * (x - y) + 5
                y -= 1
            x += 1

    # ------------------------------------------------------------------
    # Line with thickness  (Wu / perpendicular-offset approach)
    # ------------------------------------------------------------------

    def draw_line(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
        colour: tuple[int, int, int],
        thickness: int = 1,
    ) -> None:
        """Draw a line from (x0, y0) to (x1, y1) with the given pixel thickness."""
        if thickness <= 1:
            self._bresenham(x0, y0, x1, y1, colour)
            return

        # Compute unit perpendicular vector
        dx = x1 - x0
        dy = y1 - y0
        length = math.hypot(dx, dy)
        if length == 0:
            self.fill_circle(x0, y0, thickness // 2, colour)
            return

        # Perpendicular unit vector scaled to half-thickness
        half = (thickness - 1) / 2.0
        px =  (-dy / length) * half
        py =  ( dx / length) * half

        # Build a filled quadrilateral by rasterising parallel Bresenham lines
        steps = max(thickness, 1)
        for i in range(steps):
            t = i / (steps - 1) if steps > 1 else 0.5
            ox = round(-px + t * 2 * px)   # interpolate across thickness
            oy = round(-py + t * 2 * py)
            self._bresenham(x0 + ox, y0 + oy, x1 + ox, y1 + oy, colour)

        # Round end-caps
        self.fill_circle(x0, y0, thickness // 2, colour)
        self.fill_circle(x1, y1, thickness // 2, colour)

    def _bresenham(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
        colour: tuple[int, int, int],
    ) -> None:
        """Classic Bresenham line, single-pixel width."""
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.put_pixel(x0, y0, colour)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    # ------------------------------------------------------------------
    # Flood fill  (iterative 4-connected, stack-based)
    # ------------------------------------------------------------------

    def flood_fill(
        self,
        x: int, y: int,
        fill_colour: tuple[int, int, int],
    ) -> None:
        """Replace the connected region at (x, y) with fill_colour."""
        if not self._in_bounds(x, y):
            return
        target = self._get_pixel(x, y)
        if target == fill_colour:
            return

        stack: list[tuple[int, int]] = [(x, y)]
        visited: set[int] = set()

        while stack:
            cx, cy = stack.pop()
            if not self._in_bounds(cx, cy):
                continue
            key = cy * self.width + cx
            if key in visited:
                continue
            if self._get_pixel(cx, cy) != target:
                continue
            visited.add(key)
            self.put_pixel(cx, cy, fill_colour)
            stack.append((cx + 1, cy))
            stack.append((cx - 1, cy))
            stack.append((cx, cy + 1))
            stack.append((cx, cy - 1))

    def _get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        base = (y * self.width + x) * 3
        return (self._buf[base], self._buf[base + 1], self._buf[base + 2])

    # ------------------------------------------------------------------
    # BMP writer
    # ------------------------------------------------------------------

    def save_bmp(self, path: str) -> None:
        """Write the canvas to a 24-bit uncompressed BMP file."""
        w, h = self.width, self.height

        # BMP rows are bottom-up and padded to a 4-byte boundary
        row_size = (w * 3 + 3) & ~3
        padding  = row_size - w * 3
        pixel_data_size = row_size * h

        file_size = 54 + pixel_data_size

        # File header (14 bytes)
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",
            file_size,
            0, 0,
            54,          # pixel data offset
        )

        # DIB / BITMAPINFOHEADER (40 bytes)
        dib_header = struct.pack(
            "<IiiHHIIiiII",
            40,          # header size
            w, -h,       # negative height → top-down row order
            1,           # colour planes
            24,          # bits per pixel
            0,           # BI_RGB (no compression)
            pixel_data_size,
            2835, 2835,  # ~72 DPI
            0, 0,        # colours in table / important colours
        )

        pad = bytes(padding)
        with open(path, "wb") as f:
            f.write(file_header)
            f.write(dib_header)
            # BMP normally stores rows bottom-up; we used negative height above
            # so the pixel order stays top-down — write rows as-is.
            for row_y in range(h):
                base = row_y * w * 3
                # BMP is BGR, our buffer is RGB — swap channels per pixel
                row_rgb = self._buf[base : base + w * 3]
                row_bgr = bytearray(w * 3)
                for i in range(w):
                    row_bgr[i * 3]     = row_rgb[i * 3 + 2]  # B ← R
                    row_bgr[i * 3 + 1] = row_rgb[i * 3 + 1]  # G
                    row_bgr[i * 3 + 2] = row_rgb[i * 3]      # R ← B
                f.write(row_bgr)
                f.write(pad)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def main() -> None:
    W, H = 640, 480
    c = Canvas(W, H, bg=(30, 30, 40))

    # --- background rect fills ------------------------------------------------
    # dark-teal panel, top-left quadrant
    c.fill_rect(0, 0, W // 2, H // 2, (20, 60, 70))
    # warm dark panel, bottom-right quadrant
    c.fill_rect(W // 2, H // 2, W // 2, H // 2, (50, 30, 20))

    # --- overlapping filled circles -------------------------------------------
    c.fill_circle(200, 160, 110, (220,  80,  60))   # red-orange, back
    c.fill_circle(300, 200, 100, ( 60, 160, 220))   # blue, overlapping
    c.fill_circle(260, 280,  80, ( 80, 200, 120))   # green, front overlap

    # --- filled rectangles (overlapping) --------------------------------------
    c.fill_rect(380, 60, 180, 120, (230, 190,  50))  # yellow
    c.fill_rect(420, 100, 180, 120, (160,  60, 220)) # purple, overlaps yellow

    # --- thick lines forming a star-like burst from centre --------------------
    cx, cy = 460, 320
    colour_line = (255, 220, 80)
    for angle_deg in range(0, 360, 30):
        angle = math.radians(angle_deg)
        ex = cx + int(math.cos(angle) * 90)
        ey = cy + int(math.sin(angle) * 90)
        c.draw_line(cx, cy, ex, ey, colour_line, thickness=4)
    # bright hub circle on top
    c.fill_circle(cx, cy, 18, (255, 255, 200))

    # --- flood fill demo: draw an unfilled rectangle border, then fill it -----
    # Draw a 1-px border by drawing four lines
    bx, by, bw, bh = 60, 300, 180, 120
    border_col = (255, 255, 255)
    c.draw_line(bx,          by,          bx + bw,     by,          border_col, 1)
    c.draw_line(bx + bw,     by,          bx + bw,     by + bh,     border_col, 1)
    c.draw_line(bx + bw,     by + bh,     bx,          by + bh,     border_col, 1)
    c.draw_line(bx,          by + bh,     bx,          by,          border_col, 1)
    # flood-fill the interior
    c.flood_fill(bx + bw // 2, by + bh // 2, (255, 140, 200))

    # --- a diagonal thick line across the whole canvas ------------------------
    c.draw_line(0, H, W, 0, (100, 220, 255), thickness=6)

    c.save_bmp("demo.bmp")
    print("Saved demo.bmp")


if __name__ == "__main__":
    main()
