"""
shapes.py — tiny 2D canvas with drawing primitives and BMP output
Python 3.12, stdlib only
"""

import struct
import math
from collections import deque


# ---------------------------------------------------------------------------
# Canvas
# ---------------------------------------------------------------------------

class Canvas:
    """RGB bytearray canvas with 2-D drawing primitives."""

    def __init__(self, width: int, height: int, bg: tuple[int, int, int] = (255, 255, 255)):
        self.width = width
        self.height = height
        # flat RGB storage: pixel (x, y) lives at index (y * width + x) * 3
        self._buf = bytearray(bg[0:3] * width * height)
        # fill the buffer properly
        self._buf = bytearray(width * height * 3)
        r, g, b = bg
        for i in range(width * height):
            base = i * 3
            self._buf[base]     = r
            self._buf[base + 1] = g
            self._buf[base + 2] = b

    # ------------------------------------------------------------------
    # low-level pixel access
    # ------------------------------------------------------------------

    def _in_bounds(self, x: int, y: int) -> bool:
        return 0 <= x < self.width and 0 <= y < self.height

    def put_pixel(self, x: int, y: int, color: tuple[int, int, int]) -> None:
        if not self._in_bounds(x, y):
            return
        base = (y * self.width + x) * 3
        self._buf[base]     = color[0]
        self._buf[base + 1] = color[1]
        self._buf[base + 2] = color[2]

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        base = (y * self.width + x) * 3
        return (self._buf[base], self._buf[base + 1], self._buf[base + 2])

    # ------------------------------------------------------------------
    # filled rectangle
    # ------------------------------------------------------------------

    def filled_rect(
        self,
        x: int, y: int,
        w: int, h: int,
        color: tuple[int, int, int],
    ) -> None:
        """Draw a filled axis-aligned rectangle.

        (x, y) is the top-left corner; w and h are width and height in pixels.
        """
        x1 = max(x, 0)
        y1 = max(y, 0)
        x2 = min(x + w, self.width)
        y2 = min(y + h, self.height)
        r, g, b = color
        row_bytes = bytearray()
        for _ in range(x2 - x1):
            row_bytes += bytes([r, g, b])
        for row in range(y1, y2):
            base = (row * self.width + x1) * 3
            self._buf[base : base + len(row_bytes)] = row_bytes

    # ------------------------------------------------------------------
    # filled circle  (mid-point / Bresenham scan-fill)
    # ------------------------------------------------------------------

    def filled_circle(
        self,
        cx: int, cy: int,
        radius: int,
        color: tuple[int, int, int],
    ) -> None:
        """Draw a filled circle centred at (cx, cy) with the given radius."""
        r, g, b = color
        for dy in range(-radius, radius + 1):
            # half-chord width at this scanline via Pythagoras
            dx = int(math.sqrt(max(0, radius * radius - dy * dy)))
            row = cy + dy
            if not (0 <= row < self.height):
                continue
            x1 = max(cx - dx, 0)
            x2 = min(cx + dx + 1, self.width)
            if x1 >= x2:
                continue
            seg = bytearray()
            for _ in range(x2 - x1):
                seg += bytes([r, g, b])
            base = (row * self.width + x1) * 3
            self._buf[base : base + len(seg)] = seg

    # ------------------------------------------------------------------
    # line with thickness  (draw a filled circle at every Bresenham pixel)
    # ------------------------------------------------------------------

    def line(
        self,
        x0: int, y0: int,
        x1: int, y1: int,
        color: tuple[int, int, int],
        thickness: int = 1,
    ) -> None:
        """Draw a line from (x0, y0) to (x1, y1) with the given pixel thickness.

        Uses Bresenham's algorithm for the centreline and stamps a filled
        circle of radius thickness//2 at each point, giving round caps and
        consistent width on diagonals.
        """
        half = max(0, thickness // 2)
        # Bresenham integer line
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        x, y = x0, y0
        while True:
            if half == 0:
                self.put_pixel(x, y, color)
            else:
                self.filled_circle(x, y, half, color)
            if x == x1 and y == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x  += sx
            if e2 <  dx:
                err += dx
                y  += sy

    # ------------------------------------------------------------------
    # flood fill  (iterative BFS — avoids recursion-depth limits)
    # ------------------------------------------------------------------

    def flood_fill(
        self,
        x: int, y: int,
        fill_color: tuple[int, int, int],
    ) -> None:
        """Flood-fill starting at (x, y), replacing the seed color with fill_color.

        Uses an iterative BFS queue so deep/large fills don't hit Python's
        recursion limit.
        """
        if not self._in_bounds(x, y):
            return
        target = self.get_pixel(x, y)
        if target == tuple(fill_color):
            return

        fill_color = tuple(fill_color)
        queue: deque[tuple[int, int]] = deque()
        queue.append((x, y))
        visited: set[tuple[int, int]] = set()
        visited.add((x, y))

        W, H = self.width, self.height

        while queue:
            cx, cy = queue.popleft()
            self.put_pixel(cx, cy, fill_color)

            for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                if (nx, ny) in visited:
                    continue
                if not (0 <= nx < W and 0 <= ny < H):
                    continue
                if self.get_pixel(nx, ny) == target:
                    visited.add((nx, ny))
                    queue.append((nx, ny))

    # ------------------------------------------------------------------
    # BMP writer  (24-bit uncompressed DIB, no extra libraries)
    # ------------------------------------------------------------------

    def save_bmp(self, path: str) -> None:
        """Write the canvas as a 24-bit uncompressed BMP file."""
        W, H = self.width, self.height

        # BMP rows must be padded to a multiple of 4 bytes
        row_stride = W * 3
        pad = (4 - row_stride % 4) % 4
        padded_stride = row_stride + pad

        pixel_data_size = padded_stride * H
        file_size       = 14 + 40 + pixel_data_size   # file header + DIB header + pixels
        pixel_offset    = 14 + 40

        # --- file header (14 bytes) ---
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",          # signature
            file_size,
            0,              # reserved1
            0,              # reserved2
            pixel_offset,
        )

        # --- DIB (BITMAPINFOHEADER, 40 bytes) ---
        dib_header = struct.pack(
            "<IiiHHIIiiII",
            40,             # header size
            W,
            -H,             # negative height → top-down row order
            1,              # color planes
            24,             # bits per pixel
            0,              # compression (BI_RGB)
            pixel_data_size,
            2835,           # horizontal pixels/metre (~72 dpi)
            2835,
            0,              # colors in table
            0,              # important colors
        )

        padding = bytes(pad)
        with open(path, "wb") as f:
            f.write(file_header)
            f.write(dib_header)
            for row in range(H):
                start = row * W * 3
                # BMP stores pixels as BGR, not RGB
                row_data = bytearray(row_stride)
                for col in range(W):
                    src = start + col * 3
                    dst = col * 3
                    row_data[dst]     = self._buf[src + 2]  # B
                    row_data[dst + 1] = self._buf[src + 1]  # G
                    row_data[dst + 2] = self._buf[src]      # R
                f.write(row_data)
                f.write(padding)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def main() -> None:
    W, H = 480, 360
    c = Canvas(W, H, bg=(30, 30, 40))           # dark background

    # --- sky-gradient suggestion: stacked thin rects ---
    for row in range(H // 2):
        t = row / (H // 2)
        sky_r = int(20  + t * 80)
        sky_g = int(30  + t * 110)
        sky_b = int(80  + t * 140)
        c.filled_rect(0, row, W, 1, (sky_r, sky_g, sky_b))

    # --- ground ---
    c.filled_rect(0, H // 2, W, H // 2, (45, 90, 50))

    # --- large sun (circle, partially behind a rect overlap) ---
    c.filled_circle(360, 80, 55, (255, 220, 60))

    # --- building silhouettes (rects overlap intentionally) ---
    c.filled_rect(20,  160, 80, 200, (60,  60,  80))
    c.filled_rect(70,  200, 60, 160, (50,  50,  70))
    c.filled_rect(160, 180, 90, 180, (70,  65,  90))
    c.filled_rect(220, 140, 70, 220, (55,  55,  75))
    c.filled_rect(300, 190, 80, 170, (65,  60,  85))
    c.filled_rect(360, 210, 60, 150, (50,  55,  72))
    c.filled_rect(400, 170, 70, 190, (60,  58,  80))

    # --- windows (small bright rects punched onto buildings) ---
    window_color = (255, 240, 160)
    dark_window  = (30, 30, 50)
    import random
    rng = random.Random(42)                      # deterministic output
    for bx, by, bw, bh in [
        (20, 160, 80, 200), (160, 180, 90, 180),
        (220, 140, 70, 220), (300, 190, 80, 170),
        (400, 170, 70, 190),
    ]:
        for wy in range(by + 10, by + bh - 10, 20):
            for wx in range(bx + 8, bx + bw - 8, 16):
                col = window_color if rng.random() > 0.35 else dark_window
                c.filled_rect(wx, wy, 8, 10, col)

    # --- river / road: thick diagonal lines ---
    c.line(0, H - 60, W, H - 10, (80, 130, 180), thickness=18)   # river
    c.line(0, H - 50, W, H - 5,  (90, 140, 190), thickness=6)    # highlight

    # --- road markings: short dashed lines ---
    dash_y = H - 30
    for dx in range(0, W, 40):
        c.line(dx, dash_y, dx + 20, dash_y, (220, 220, 100), thickness=2)

    # --- flood-fill: replace one building's interior with a richer color ---
    # paint a solid rect then flood-fill with a different shade to demo the fill
    c.filled_rect(100, 220, 50, 100, (80, 80, 100))   # placeholder block
    c.flood_fill(125, 260, (100, 80, 130))             # flood into it

    # --- foreground tree silhouettes (circle + rect trunk) ---
    for tx, ty in [(50, 290), (140, 300), (430, 285), (460, 295)]:
        c.filled_circle(tx, ty - 20, 22, (25, 70, 30))
        c.filled_rect(tx - 4, ty, 8, 30, (60, 40, 20))

    # --- stars in the upper sky ---
    for _ in range(60):
        sx = rng.randint(0, W - 1)
        sy = rng.randint(0, 70)
        brightness = rng.randint(160, 255)
        c.put_pixel(sx, sy, (brightness, brightness, brightness))

    # --- frame border ---
    border = (200, 180, 100)
    c.line(0,     0,     W - 1, 0,     border, thickness=3)
    c.line(W - 1, 0,     W - 1, H - 1, border, thickness=3)
    c.line(W - 1, H - 1, 0,     H - 1, border, thickness=3)
    c.line(0,     H - 1, 0,     0,     border, thickness=3)

    output = "demo.bmp"
    c.save_bmp(output)
    print(f"Saved {output}  ({W}×{H} px)")


if __name__ == "__main__":
    main()
