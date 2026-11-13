"""
bitmap.py — 24-bit BMP writer with nearest-neighbour and bilinear resize.
stdlib only, no third-party dependencies.
"""

import struct
import math


# ---------------------------------------------------------------------------
# BitmapBuffer
# ---------------------------------------------------------------------------

class BitmapBuffer:
    """
    Row-padded 24-bit pixel buffer.

    Pixels are stored bottom-up (standard BMP convention).
    Each row is padded to a multiple of 4 bytes.
    """

    def __init__(self, width: int, height: int) -> None:
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive integers")
        self.width = width
        self.height = height
        # Row stride including padding
        self.stride = (width * 3 + 3) & ~3
        # Allocate zeroed buffer (black image)
        self._data = bytearray(self.stride * height)

    # ------------------------------------------------------------------
    # Pixel access
    # ------------------------------------------------------------------

    def set_pixel(self, x: int, y: int, r: int, g: int, b: int) -> None:
        """Set the pixel at (x, y) with 8-bit RGB values (0-255)."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"pixel ({x}, {y}) out of bounds for {self.width}x{self.height}")
        # BMP stores rows bottom-up
        row = self.height - 1 - y
        offset = row * self.stride + x * 3
        self._data[offset]     = b   # blue
        self._data[offset + 1] = g   # green
        self._data[offset + 2] = r   # red

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Return (r, g, b) for the pixel at (x, y)."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"pixel ({x}, {y}) out of bounds for {self.width}x{self.height}")
        row = self.height - 1 - y
        offset = row * self.stride + x * 3
        b = self._data[offset]
        g = self._data[offset + 1]
        r = self._data[offset + 2]
        return r, g, b

    # ------------------------------------------------------------------
    # BMP serialisation
    # ------------------------------------------------------------------

    def to_bmp_bytes(self) -> bytes:
        """
        Serialise the buffer to a complete 24-bit BMP file in memory.

        BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes) + pixel data.
        """
        pixel_data_size = self.stride * self.height
        file_size = 14 + 40 + pixel_data_size
        pixel_offset = 14 + 40

        # BITMAPFILEHEADER
        # Signature: 'BM' (2 bytes)
        # File size  (4 bytes, little-endian uint32)
        # Reserved1  (2 bytes)
        # Reserved2  (2 bytes)
        # Pixel data offset (4 bytes)
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",
            file_size,
            0,            # reserved1
            0,            # reserved2
            pixel_offset,
        )

        # BITMAPINFOHEADER
        # Header size     (4 bytes) = 40
        # Width           (4 bytes, signed int32)
        # Height          (4 bytes, signed int32) — positive = bottom-up
        # Color planes    (2 bytes) = 1
        # Bits per pixel  (2 bytes) = 24
        # Compression     (4 bytes) = 0 (BI_RGB)
        # Image size      (4 bytes) = 0 for BI_RGB (may also be exact size)
        # X pixels/metre  (4 bytes) = 0
        # Y pixels/metre  (4 bytes) = 0
        # Colors in table (4 bytes) = 0 (full palette for 24-bit)
        # Important colors(4 bytes) = 0
        info_header = struct.pack(
            "<IiiHHIIiiII",
            40,
            self.width,
            self.height,
            1,            # planes
            24,           # bits per pixel
            0,            # compression (BI_RGB)
            pixel_data_size,
            0,            # x pixels per metre
            0,            # y pixels per metre
            0,            # colours in colour table
            0,            # important colours
        )

        return file_header + info_header + bytes(self._data)

    def save(self, path: str) -> None:
        """Write the BMP to *path*."""
        with open(path, "wb") as f:
            f.write(self.to_bmp_bytes())


# ---------------------------------------------------------------------------
# Resize helpers
# ---------------------------------------------------------------------------

def resize_nearest(src: BitmapBuffer, new_width: int, new_height: int) -> BitmapBuffer:
    """
    Nearest-neighbour resize.

    For each destination pixel (dx, dy) we map back to source coordinates:
        sx = dx * src_width  / new_width
        sy = dy * src_height / new_height
    and copy the rounded source pixel directly.
    """
    dst = BitmapBuffer(new_width, new_height)
    x_ratio = src.width / new_width
    y_ratio = src.height / new_height

    for dy in range(new_height):
        sy = int(dy * y_ratio)
        sy = min(sy, src.height - 1)
        for dx in range(new_width):
            sx = int(dx * x_ratio)
            sx = min(sx, src.width - 1)
            dst.set_pixel(dx, dy, *src.get_pixel(sx, sy))

    return dst


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def resize_bilinear(src: BitmapBuffer, new_width: int, new_height: int) -> BitmapBuffer:
    """
    Bilinear resize.

    Maps each destination pixel back to a floating-point source location, then
    interpolates across the four nearest source pixels using their fractional
    distances as weights:

        weight_tl = (1 - fx) * (1 - fy)
        weight_tr =      fx  * (1 - fy)
        weight_bl = (1 - fx) *      fy
        weight_br =      fx  *      fy

    where fx, fy are the fractional parts of the mapped source coordinates.
    """
    dst = BitmapBuffer(new_width, new_height)

    # Scale factors that keep the image edges aligned at the sample centres
    x_ratio = src.width  / new_width
    y_ratio = src.height / new_height

    for dy in range(new_height):
        # Map destination centre back to source space
        sy_f = (dy + 0.5) * y_ratio - 0.5
        sy0  = int(math.floor(sy_f))
        sy1  = sy0 + 1
        fy   = sy_f - sy0

        sy0 = int(_clamp(sy0, 0, src.height - 1))
        sy1 = int(_clamp(sy1, 0, src.height - 1))

        for dx in range(new_width):
            sx_f = (dx + 0.5) * x_ratio - 0.5
            sx0  = int(math.floor(sx_f))
            sx1  = sx0 + 1
            fx   = sx_f - sx0

            sx0 = int(_clamp(sx0, 0, src.width - 1))
            sx1 = int(_clamp(sx1, 0, src.width - 1))

            # Fetch the four neighbours
            r00, g00, b00 = src.get_pixel(sx0, sy0)
            r10, g10, b10 = src.get_pixel(sx1, sy0)
            r01, g01, b01 = src.get_pixel(sx0, sy1)
            r11, g11, b11 = src.get_pixel(sx1, sy1)

            # Bilinear blend
            w_tl = (1.0 - fx) * (1.0 - fy)
            w_tr =        fx  * (1.0 - fy)
            w_bl = (1.0 - fx) *        fy
            w_br =        fx  *        fy

            r = int(r00 * w_tl + r10 * w_tr + r01 * w_bl + r11 * w_br + 0.5)
            g = int(g00 * w_tl + g10 * w_tr + g01 * w_bl + g11 * w_br + 0.5)
            b = int(b00 * w_tl + b10 * w_tr + b01 * w_bl + b11 * w_br + 0.5)

            dst.set_pixel(dx, dy,
                          int(_clamp(r, 0, 255)),
                          int(_clamp(g, 0, 255)),
                          int(_clamp(b, 0, 255)))

    return dst


# ---------------------------------------------------------------------------
# Gradient generator
# ---------------------------------------------------------------------------

def make_gradient(width: int, height: int) -> BitmapBuffer:
    """
    Generate a simple RGB gradient:
      - Red increases left → right   (x axis)
      - Green increases top → bottom (y axis)
      - Blue is constant at 128

    This gives a visually interesting source that exercises both resize paths.
    """
    buf = BitmapBuffer(width, height)
    for y in range(height):
        g = int(y / (height - 1) * 255) if height > 1 else 0
        for x in range(width):
            r = int(x / (width - 1) * 255) if width > 1 else 0
            buf.set_pixel(x, y, r, g, 128)
    return buf


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    SOURCE_W, SOURCE_H = 256, 256
    TARGET_W, TARGET_H = 64, 64

    print(f"Generating {SOURCE_W}×{SOURCE_H} gradient …")
    gradient = make_gradient(SOURCE_W, SOURCE_H)

    # --- nearest-neighbour ---
    print(f"Nearest-neighbour resize → {TARGET_W}×{TARGET_H} …")
    nn = resize_nearest(gradient, TARGET_W, TARGET_H)
    nn.save("output_nearest.bmp")
    print("  Saved: output_nearest.bmp")

    # --- bilinear ---
    print(f"Bilinear resize → {TARGET_W}×{TARGET_H} …")
    bl = resize_bilinear(gradient, TARGET_W, TARGET_H)
    bl.save("output_bilinear.bmp")
    print("  Saved: output_bilinear.bmp")

    print("Done.")


if __name__ == "__main__":
    main()
