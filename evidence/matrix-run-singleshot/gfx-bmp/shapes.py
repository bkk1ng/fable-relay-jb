"""
shapes.py — Tiny 2D raster canvas with primitives and BMP export.

Python 3.12, standard library only.

Provides a Canvas backed by an RGB bytearray with:
  - filled rectangles
  - filled circles
  - thick lines (capsule-style caps)
  - flood fill (stack-based, no recursion limit)
  - 24-bit BMP writer

Run `python shapes.py` to generate an overlapping-scene demo image.
"""

import struct
import sys
from collections import deque
from pathlib import Path


class Canvas:
    """An RGB raster stored as a flat bytearray (row-major, top-to-bottom, RGB)."""

    def __init__(self, width: int, height: int,
                 background: tuple[int, int, int] = (0, 0, 0)):
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive")
        self.width = width
        self.height = height
        self.pixels = bytearray(width * height * 3)
        self.fill_rect(0, 0, width - 1, height - 1, background)

    # --- internal helpers ------------------------------------------------

    def _index(self, x: int, y: int) -> int:
        """Byte offset of pixel (x, y) in the flat buffer."""
        return (y * self.width + x) * 3

    def _set_pixel(self, x: int, y: int,
                   color: tuple[int, int, int]) -> None:
        """Write a single RGB pixel, bounds-checked."""
        if 0 <= x < self.width and 0 <= y < self.height:
            i = self._index(x, y)
            self.pixels[i] = color[0]
            self.pixels[i + 1] = color[1]
            self.pixels[i + 2] = color[2]

    def _get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Read a single RGB pixel (caller must bounds-check)."""
        i = self._index(x, y)
        return (self.pixels[i], self.pixels[i + 1], self.pixels[i + 2])

    # --- public primitives ------------------------------------------------

    def fill_rect(self, x0: int, y0: int, x1: int, y1: int,
                  color: tuple[int, int, int]) -> None:
        """Fill an axis-aligned rectangle from (x0, y0) to (x1, y1) inclusive."""
        if x0 > x1:
            x0, x1 = x1, x0
        if y0 > y1:
            y0, y1 = y1, y0
        r, g, b = color
        x_start = max(0, x0)
        x_end = min(self.width, x1 + 1)
        for y in range(max(0, y0), min(self.height, y1 + 1)):
            base = y * self.width * 3
            for x in range(x_start, x_end):
                i = base + x * 3
                self.pixels[i] = r
                self.pixels[i + 1] = g
                self.pixels[i + 2] = b

    def fill_circle(self, cx: int, cy: int, radius: int,
                    color: tuple[int, int, int]) -> None:
        """Fill a circle centered at (cx, cy) with the given radius."""
        if radius < 0:
            return
        if radius == 0:
            self._set_pixel(cx, cy, color)
            return

        r, g, b = color
        r2 = radius * radius
        x0 = max(0, cx - radius)
        x1 = min(self.width - 1, cx + radius)
        y0 = max(0, cy - radius)
        y1 = min(self.height - 1, cy + radius)

        for y in range(y0, y1 + 1):
            dy = y - cy
            dy2 = dy * dy
            row_base = y * self.width * 3
            for x in range(x0, x1 + 1):
                dx = x - cx
                if dx * dx + dy2 <= r2:
                    i = row_base + x * 3
                    self.pixels[i] = r
                    self.pixels[i + 1] = g
                    self.pixels[i + 2] = b

    def draw_line(self, x0: int, y0: int, x1: int, y1: int,
                  thickness: int,
                  color: tuple[int, int, int]) -> None:
        """Draw a line with the given thickness.

        Uses Bresenham for the path.  For thickness > 1 a filled circle of
        radius ``thickness // 2`` is stamped at every step, producing a
        rounded-capsule shape that joins cleanly between segments.
        """
        if thickness <= 0:
            return
        radius = max(0, thickness // 2)

        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        x, y = x0, y0

        while True:
            if radius == 0:
                self._set_pixel(x, y, color)
            else:
                self.fill_circle(x, y, radius, color)

            if x == x1 and y == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x += sx
            if e2 <= dx:
                err += dx
                y += sy

    def flood_fill(self, x: int, y: int,
                   fill_color: tuple[int, int, int]) -> None:
        """Flood-fill from (x, y), replacing the contiguous color there.

        The seed pixel's current color is the target; every 4-connected
        pixel of that color becomes ``fill_color``.  Uses an explicit stack
        so large regions never hit Python's recursion limit.
        """
        if not (0 <= x < self.width and 0 <= y < self.height):
            return
        target = self._get_pixel(x, y)
        if target == fill_color:
            return

        tr, tg, tb = fill_color
        w, h = self.width, self.height
        pix = self.pixels
        stack: deque[tuple[int, int]] = deque()
        stack.append((x, y))

        while stack:
            px, py = stack.pop()
            if not (0 <= px < w and 0 <= py < h):
                continue
            i = (py * w + px) * 3
            if pix[i] != tr and pix[i + 1] != tg and pix[i + 2] != tb:
                # reject — not the target color
                pass
            # Re-check properly: all three channels must match target
            if (pix[i], pix[i + 1], pix[i + 2]) != target:
                continue
            pix[i] = tr
            pix[i + 1] = tg
            pix[i + 2] = tb
            stack.append((px + 1, py))
            stack.append((px - 1, py))
            stack.append((px, py + 1))
            stack.append((px, py - 1))

    # --- BMP export -------------------------------------------------------

    def write_bmp(self, path: str | Path) -> None:
        """Write the canvas as a 24-bit BMP (BGR, bottom-up, row-padded)."""
        w, h = self.width, self.height
        row_stride = (w * 3 + 3) & ~3       # pad each row to a 4-byte boundary
        image_size = row_stride * h
        file_size = 14 + 40 + image_size    # file-header + DIB-header + pixels

        out = bytearray(file_size)

        # --- File header (14 bytes) ---
        out[0:2] = b"BM"
        struct.pack_into("<I", out, 2, file_size)       # total file size
        # bytes 6-9 stay zero (reserved)
        struct.pack_into("<I", out, 10, 54)              # pixel data offset

        # --- DIB header — BITMAPINFOHEADER (40 bytes) ---
        struct.pack_into("<I", out, 14, 40)             # header size
        struct.pack_into("<i", out, 18, w)               # width (signed)
        struct.pack_into("<i", out, 22, h)               # height (signed, + = bottom-up)
        struct.pack_into("<H", out, 26, 1)               # planes
        struct.pack_into("<H", out, 28, 24)              # bits per pixel
        struct.pack_into("<I", out, 30, 0)               # compression (BI_RGB)
        struct.pack_into("<I", out, 34, image_size)     # image data size
        struct.pack_into("<I", out, 38, 2835)            # x pixels/meter (~72 dpi)
        struct.pack_into("<I", out, 42, 2835)            # y pixels/meter
        struct.pack_into("<I", out, 46, 0)               # colors in palette
        struct.pack_into("<I", out, 50, 0)               # important colors

        # --- Pixel data (bottom-up rows, BGR byte order) ---
        pix = self.pixels
        offset = 54
        for row in range(h):
            src_y = h - 1 - row                          # BMP stores bottom row first
            src_base = src_y * w * 3
            dst_base = offset + row * row_stride
            for x in range(w):
                si = src_base + x * 3
                di = dst_base + x * 3
                out[di] = pix[si + 2]                    # B
                out[di + 1] = pix[si + 1]                # G
                out[di + 2] = pix[si]                    # R
            # row padding bytes are already zero

        Path(path).write_bytes(out)


# --- demo ---------------------------------------------------------------

def main() -> int:
    """Draw an overlapping scene and save it as demo.bmp."""
    W, H = 640, 480
    canvas = Canvas(W, H, background=(18, 18, 24))

    # Ground band.
    canvas.fill_rect(0, 340, W - 1, H - 1, (34, 40, 52))

    # Overlapping circles — a sun being eclipsed.
    canvas.fill_circle(470, 130, 90, (220, 160, 60))
    canvas.fill_circle(520, 100, 80, (40, 44, 60))   # crescent mask

    # Overlapping rectangles painted in order (solid, no blending).
    canvas.fill_rect(120, 180, 320, 380, (60, 120, 200))
    canvas.fill_rect(220, 240, 420, 420, (200, 70, 90))
    canvas.fill_rect(80, 300, 260, 440, (70, 160, 90))

    # Thick cross lines over the whole scene.
    canvas.draw_line(60, 60, 580, 420, thickness=5, (240, 230, 210))
    canvas.draw_line(580, 60, 60, 420, thickness=5, (240, 230, 210))

    # Flood-fill a background corner to show the algorithm working.
    canvas.flood_fill(10, 10, (90, 70, 30))

    out_path = Path(__file__).with_name("demo.bmp")
    canvas.write_bmp(out_path)
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
