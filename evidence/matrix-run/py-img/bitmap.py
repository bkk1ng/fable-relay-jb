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

    Pixels are stored as (R, G, B) tuples in a flat list indexed
    [y * width + x].  Rows are padded to a 4-byte boundary when the
    buffer is serialised to BMP bytes, as the format requires.
    """

    def __init__(self, width: int, height: int):
        if width <= 0 or height <= 0:
            raise ValueError(f"Dimensions must be positive, got {width}x{height}")
        self.width = width
        self.height = height
        # Initialise to opaque black.
        self._pixels: list[tuple[int, int, int]] = [(0, 0, 0)] * (width * height)

    # ------------------------------------------------------------------
    # Pixel access
    # ------------------------------------------------------------------

    def set_pixel(self, x: int, y: int, r: int, g: int, b: int) -> None:
        """Write a single pixel.  (0, 0) is the top-left corner."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"Pixel ({x}, {y}) out of bounds for {self.width}x{self.height}")
        self._pixels[y * self.width + x] = (
            max(0, min(255, r)),
            max(0, min(255, g)),
            max(0, min(255, b)),
        )

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Read a single pixel."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"Pixel ({x}, {y}) out of bounds")
        return self._pixels[y * self.width + x]

    # ------------------------------------------------------------------
    # BMP serialisation
    # ------------------------------------------------------------------

    def _row_stride(self) -> int:
        """BMP rows are padded to a multiple of 4 bytes."""
        raw = self.width * 3          # 3 bytes per pixel (BGR)
        return (raw + 3) & ~3

    def to_bmp_bytes(self) -> bytes:
        """
        Serialise the buffer to a complete 24-bit BMP file in memory.

        Layout
        ------
        BITMAPFILEHEADER  14 bytes
        BITMAPINFOHEADER  40 bytes
        Pixel data        height * stride bytes  (bottom-up row order)
        """
        stride   = self._row_stride()
        img_size = stride * self.height
        file_size = 14 + 40 + img_size
        pixel_offset = 14 + 40

        # -- BITMAPFILEHEADER (14 bytes) ---------------------------------
        # Signature 'BM', file size, reserved×2, pixel-data offset
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",          # bfType
            file_size,      # bfSize
            0,              # bfReserved1
            0,              # bfReserved2
            pixel_offset,   # bfOffBits
        )

        # -- BITMAPINFOHEADER (40 bytes) ---------------------------------
        # Size, width, height (positive = bottom-up), planes, bit-count,
        # compression (0 = BI_RGB), image size, X/Y pels per metre, colours
        info_header = struct.pack(
            "<IiiHHIIiiII",
            40,             # biSize
            self.width,     # biWidth
            self.height,    # biHeight  (positive → bottom-up storage)
            1,              # biPlanes
            24,             # biBitCount
            0,              # biCompression  (BI_RGB)
            img_size,       # biSizeImage
            2835,           # biXPelsPerMeter  (~72 dpi)
            2835,           # biYPelsPerMeter
            0,              # biClrUsed
            0,              # biClrImportant
        )

        # -- Pixel data --------------------------------------------------
        # BMP stores rows bottom-up and channels in BGR order.
        pad = bytes(stride - self.width * 3)
        rows: list[bytes] = []
        for y in range(self.height - 1, -1, -1):   # bottom row first
            row_bytes = bytearray(self.width * 3)
            for x in range(self.width):
                r, g, b = self._pixels[y * self.width + x]
                offset = x * 3
                row_bytes[offset]     = b
                row_bytes[offset + 1] = g
                row_bytes[offset + 2] = r
            rows.append(bytes(row_bytes) + pad)

        return file_header + info_header + b"".join(rows)

    def save(self, path: str) -> None:
        """Write the buffer to *path* as a 24-bit BMP file."""
        with open(path, "wb") as fh:
            fh.write(self.to_bmp_bytes())

    # ------------------------------------------------------------------
    # Convenience
    # ------------------------------------------------------------------

    def __repr__(self) -> str:
        return f"BitmapBuffer({self.width}x{self.height})"


# ---------------------------------------------------------------------------
# Resize helpers
# ---------------------------------------------------------------------------

def resize_nearest(src: BitmapBuffer, new_width: int, new_height: int) -> BitmapBuffer:
    """
    Nearest-neighbour resize.

    For each destination pixel (dx, dy) we map back to the closest
    source pixel using the ratio of the dimensions, then copy that colour
    without any blending.

        sx = floor(dx * src_w / dst_w)
        sy = floor(dy * src_h / dst_h)
    """
    dst = BitmapBuffer(new_width, new_height)
    x_ratio = src.width  / new_width
    y_ratio = src.height / new_height

    for dy in range(new_height):
        sy = int(dy * y_ratio)
        sy = min(sy, src.height - 1)
        for dx in range(new_width):
            sx = int(dx * x_ratio)
            sx = min(sx, src.width - 1)
            r, g, b = src.get_pixel(sx, sy)
            dst.set_pixel(dx, dy, r, g, b)

    return dst


def resize_bilinear(src: BitmapBuffer, new_width: int, new_height: int) -> BitmapBuffer:
    """
    Bilinear resize.

    Maps each destination pixel centre to a continuous source coordinate,
    then interpolates the four surrounding source pixels with weights
    proportional to the fractional overlap in x and y.

    The continuous source coordinate for destination pixel d in a
    dimension of size dst is:

        s = (d + 0.5) * (src_size / dst_size) - 0.5

    This aligns pixel centres rather than corners, which avoids the
    half-pixel shift that the simpler (d * src/dst) formula produces.
    """
    dst = BitmapBuffer(new_width, new_height)
    x_scale = src.width  / new_width
    y_scale = src.height / new_height

    def sample(fx: float, fy: float) -> tuple[int, int, int]:
        # Integer top-left corner and fractional parts.
        x0 = int(math.floor(fx))
        y0 = int(math.floor(fy))
        x1 = x0 + 1
        y1 = y0 + 1

        # Clamp to source bounds.
        x0 = max(0, min(x0, src.width  - 1))
        x1 = max(0, min(x1, src.width  - 1))
        y0 = max(0, min(y0, src.height - 1))
        y1 = max(0, min(y1, src.height - 1))

        tx = fx - math.floor(fx)   # weight toward x1
        ty = fy - math.floor(fy)   # weight toward y1

        # Four corner samples.
        c00 = src.get_pixel(x0, y0)
        c10 = src.get_pixel(x1, y0)
        c01 = src.get_pixel(x0, y1)
        c11 = src.get_pixel(x1, y1)

        # Bilinear blend per channel.
        result = []
        for ch in range(3):
            v = (
                c00[ch] * (1 - tx) * (1 - ty)
                + c10[ch] * tx       * (1 - ty)
                + c01[ch] * (1 - tx) * ty
                + c11[ch] * tx       * ty
            )
            result.append(int(round(v)))
        return (result[0], result[1], result[2])

    for dy in range(new_height):
        fy = (dy + 0.5) * y_scale - 0.5
        for dx in range(new_width):
            fx = (dx + 0.5) * x_scale - 0.5
            r, g, b = sample(fx, fy)
            dst.set_pixel(dx, dy, r, g, b)

    return dst


# ---------------------------------------------------------------------------
# Gradient generator
# ---------------------------------------------------------------------------

def make_gradient(width: int, height: int) -> BitmapBuffer:
    """
    Generate a smooth two-axis gradient:
      - red increases left → right
      - green increases top → bottom
      - blue sweeps diagonally (average of the two)
    """
    buf = BitmapBuffer(width, height)
    for y in range(height):
        for x in range(width):
            r = int(x / (width  - 1) * 255) if width  > 1 else 0
            g = int(y / (height - 1) * 255) if height > 1 else 0
            b = (r + g) // 2
            buf.set_pixel(x, y, r, g, b)
    return buf


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    # 1. Build a 256×256 gradient source image.
    src_w, src_h = 256, 256
    print(f"Generating {src_w}×{src_h} gradient …")
    src = make_gradient(src_w, src_h)

    # 2. Nearest-neighbour resize → 128×128, save.
    nn_w, nn_h = 128, 128
    print(f"Nearest-neighbour resize → {nn_w}×{nn_h} …")
    nn = resize_nearest(src, nn_w, nn_h)
    nn_path = "gradient_nn.bmp"
    nn.save(nn_path)
    print(f"Saved: {nn_path}")

    # 3. Bilinear resize → 128×128, save.
    bl_w, bl_h = 128, 128
    print(f"Bilinear resize          → {bl_w}×{bl_h} …")
    bl = resize_bilinear(src, bl_w, bl_h)
    bl_path = "gradient_bilinear.bmp"
    bl.save(bl_path)
    print(f"Saved: {bl_path}")

    # Quick sanity check: corner pixels of the source gradient.
    corners = {
        "top-left":     src.get_pixel(0,           0),
        "top-right":    src.get_pixel(src_w - 1,   0),
        "bottom-left":  src.get_pixel(0,           src_h - 1),
        "bottom-right": src.get_pixel(src_w - 1,   src_h - 1),
    }
    print("\nSource corner pixels:")
    for label, pixel in corners.items():
        print(f"  {label:>13}: R={pixel[0]:3d}  G={pixel[1]:3d}  B={pixel[2]:3d}")


if __name__ == "__main__":
    main()
