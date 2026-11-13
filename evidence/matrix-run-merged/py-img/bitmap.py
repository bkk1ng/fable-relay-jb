"""
bitmap.py — 24-bit BMP writer with nearest-neighbour and bilinear resize.
Pure Python 3.12, stdlib only.
"""

import struct
import math


# ---------------------------------------------------------------------------
# Pixel buffer
# ---------------------------------------------------------------------------

class PixelBuffer:
    """
    Row-padded 24-bit (BGR) pixel buffer.

    Pixels are stored as a flat bytearray; each row is zero-padded to a
    multiple of 4 bytes as the BMP format requires.
    """

    def __init__(self, width: int, height: int):
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive")
        self.width = width
        self.height = height
        # Row stride rounded up to the nearest multiple of 4 bytes.
        self.stride = (width * 3 + 3) & ~3
        self._buf = bytearray(self.stride * height)

    # ------------------------------------------------------------------
    # Pixel access
    # ------------------------------------------------------------------

    def set_pixel(self, x: int, y: int, r: int, g: int, b: int) -> None:
        """Write one pixel.  Origin is top-left; BMP rows are stored bottom-up
        so we flip y when writing into the buffer."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            return
        flipped_y = self.height - 1 - y
        offset = flipped_y * self.stride + x * 3
        self._buf[offset]     = b & 0xFF
        self._buf[offset + 1] = g & 0xFF
        self._buf[offset + 2] = r & 0xFF

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Return (r, g, b) for the pixel at (x, y), top-left origin."""
        x = max(0, min(self.width  - 1, x))
        y = max(0, min(self.height - 1, y))
        flipped_y = self.height - 1 - y
        offset = flipped_y * self.stride + x * 3
        b = self._buf[offset]
        g = self._buf[offset + 1]
        r = self._buf[offset + 2]
        return r, g, b

    # ------------------------------------------------------------------
    # BMP serialisation
    # ------------------------------------------------------------------

    def to_bmp_bytes(self) -> bytes:
        """
        Serialise the buffer as a valid 24-bit BMP file and return the
        raw bytes.

        Layout
        ------
        BITMAPFILEHEADER  (14 bytes)
        BITMAPINFOHEADER  (40 bytes)
        Pixel data        (stride * height bytes)
        """
        pixel_data_size = self.stride * self.height
        file_size       = 14 + 40 + pixel_data_size
        pixel_offset    = 14 + 40

        # BITMAPFILEHEADER
        # Signature 'BM', file size, reserved1, reserved2, pixel-data offset.
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",          # bfType
            file_size,      # bfSize
            0,              # bfReserved1
            0,              # bfReserved2
            pixel_offset,   # bfOffBits
        )

        # BITMAPINFOHEADER
        # Header size, width, height (positive → bottom-up, already handled),
        # colour planes, bits-per-pixel, compression (0 = none), image size,
        # pixels-per-metre X/Y, colours in table, important colours.
        info_header = struct.pack(
            "<IiiHHIIiiII",
            40,                  # biSize
            self.width,          # biWidth
            self.height,         # biHeight  (positive = bottom-up storage)
            1,                   # biPlanes
            24,                  # biBitCount
            0,                   # biCompression  (BI_RGB)
            pixel_data_size,     # biSizeImage
            2835,                # biXPelsPerMeter  (~72 dpi)
            2835,                # biYPelsPerMeter
            0,                   # biClrUsed
            0,                   # biClrImportant
        )

        return file_header + info_header + bytes(self._buf)

    def save(self, path: str) -> None:
        """Write the BMP file to *path*."""
        with open(path, "wb") as fh:
            fh.write(self.to_bmp_bytes())


# ---------------------------------------------------------------------------
# Resize helpers
# ---------------------------------------------------------------------------

def resize_nearest(src: PixelBuffer, dst_w: int, dst_h: int) -> PixelBuffer:
    """
    Nearest-neighbour resize.

    For each destination pixel (dx, dy) we map back to the closest source
    pixel using floor(dx * src_w / dst_w).
    """
    dst = PixelBuffer(dst_w, dst_h)
    for dy in range(dst_h):
        sy = int(dy * src.height / dst_h)
        for dx in range(dst_w):
            sx = int(dx * src.width / dst_w)
            r, g, b = src.get_pixel(sx, sy)
            dst.set_pixel(dx, dy, r, g, b)
    return dst


def resize_bilinear(src: PixelBuffer, dst_w: int, dst_h: int) -> PixelBuffer:
    """
    Bilinear resize.

    Maps each destination pixel centre back to fractional source coordinates
    and interpolates the four surrounding source pixels.

    We use the half-pixel centre convention:
        src_x = (dx + 0.5) * src_w / dst_w - 0.5
    so that the destination pixel centres align with the source sample grid.
    """
    dst = PixelBuffer(dst_w, dst_h)

    x_scale = src.width  / dst_w
    y_scale = src.height / dst_h

    for dy in range(dst_h):
        fy = (dy + 0.5) * y_scale - 0.5
        y0 = int(math.floor(fy))
        y1 = y0 + 1
        ty = fy - y0

        for dx in range(dst_w):
            fx = (dx + 0.5) * x_scale - 0.5
            x0 = int(math.floor(fx))
            x1 = x0 + 1
            tx = fx - x0

            # Four corners (clamping is handled inside get_pixel).
            r00, g00, b00 = src.get_pixel(x0, y0)
            r10, g10, b10 = src.get_pixel(x1, y0)
            r01, g01, b01 = src.get_pixel(x0, y1)
            r11, g11, b11 = src.get_pixel(x1, y1)

            # Bilinear blend: lerp along x, then lerp along y.
            def lerp(a: float, b: float, t: float) -> int:
                return round(a + (b - a) * t)

            r = lerp(lerp(r00, r10, tx), lerp(r01, r11, tx), ty)
            g = lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty)
            b = lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty)

            dst.set_pixel(dx, dy, r, g, b)

    return dst


# ---------------------------------------------------------------------------
# Gradient generator
# ---------------------------------------------------------------------------

def make_gradient(width: int, height: int) -> PixelBuffer:
    """
    Generate a simple gradient:
      - Red increases left → right.
      - Green increases top → bottom.
      - Blue is constant at 128.
    """
    buf = PixelBuffer(width, height)
    for y in range(height):
        g = round(y / (height - 1) * 255) if height > 1 else 0
        for x in range(width):
            r = round(x / (width  - 1) * 255) if width  > 1 else 0
            buf.set_pixel(x, y, r, g, 128)
    return buf


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    SOURCE_W, SOURCE_H = 320, 240
    SMALL_W,  SMALL_H  = 80,  60    # 4× down-scale
    LARGE_W,  LARGE_H  = 640, 480   # 2× up-scale

    print(f"Generating {SOURCE_W}×{SOURCE_H} gradient …")
    gradient = make_gradient(SOURCE_W, SOURCE_H)

    # --- nearest-neighbour (down-scale) ------------------------------------
    nn_path = "gradient_nn.bmp"
    print(f"Nearest-neighbour resize → {SMALL_W}×{SMALL_H} …")
    nn = resize_nearest(gradient, SMALL_W, SMALL_H)
    nn.save(nn_path)
    print(f"  Saved {nn_path}")

    # --- bilinear (up-scale) -----------------------------------------------
    bl_path = "gradient_bilinear.bmp"
    print(f"Bilinear resize → {LARGE_W}×{LARGE_H} …")
    bl = resize_bilinear(gradient, LARGE_W, LARGE_H)
    bl.save(bl_path)
    print(f"  Saved {bl_path}")

    print("Done.")
