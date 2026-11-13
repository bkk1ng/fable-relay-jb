"""
bitmap.py — 24-bit BMP writer with nearest-neighbour and bilinear resize.
Pure Python 3.12, stdlib only.
"""

import struct
import math


# ---------------------------------------------------------------------------
# Pixel buffer
# ---------------------------------------------------------------------------

class Bitmap:
    """
    24-bit (R, G, B) pixel buffer with BMP file serialisation.

    Rows are stored top-to-bottom internally; the BMP serialiser flips them
    to bottom-up as the format requires, and pads each row to a 4-byte boundary.
    """

    def __init__(self, width: int, height: int):
        if width < 1 or height < 1:
            raise ValueError("width and height must be >= 1")
        self.width = width
        self.height = height
        # Flat list of (r, g, b) tuples, row-major, top-to-bottom.
        self._pixels: list[tuple[int, int, int]] = [
            (0, 0, 0)
        ] * (width * height)

    # ------------------------------------------------------------------
    # Pixel access
    # ------------------------------------------------------------------

    def _idx(self, x: int, y: int) -> int:
        return y * self.width + x

    def set_pixel(self, x: int, y: int, r: int, g: int, b: int) -> None:
        self._pixels[self._idx(x, y)] = (
            max(0, min(255, r)),
            max(0, min(255, g)),
            max(0, min(255, b)),
        )

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        return self._pixels[self._idx(x, y)]

    # ------------------------------------------------------------------
    # BMP serialisation
    # ------------------------------------------------------------------

    def to_bytes(self) -> bytes:
        """Encode the bitmap as a 24-bit BMP byte string."""
        row_stride = self.width * 3                        # unpadded row size
        row_padding = (4 - row_stride % 4) % 4            # bytes to pad to 4-byte boundary
        padded_row = row_stride + row_padding
        pixel_data_size = padded_row * self.height

        # BITMAPFILEHEADER  (14 bytes)
        #   bfType      : 2s  "BM"
        #   bfSize      : I   total file size
        #   bfReserved1 : H   0
        #   bfReserved2 : H   0
        #   bfOffBits   : I   offset to pixel data
        header_size = 14
        info_size   = 40
        offset      = header_size + info_size
        file_size   = offset + pixel_data_size

        file_header = struct.pack(
            "<2sIHHI",
            b"BM",
            file_size,
            0,
            0,
            offset,
        )

        # BITMAPINFOHEADER  (40 bytes)
        #   biSize          : I   header size (40)
        #   biWidth         : i   width in pixels
        #   biHeight        : i   height (positive → bottom-up)
        #   biPlanes        : H   1
        #   biBitCount      : H   24
        #   biCompression   : I   0 (BI_RGB, no compression)
        #   biSizeImage     : I   pixel data size (may be 0 for BI_RGB, but set it)
        #   biXPelsPerMeter : i   0
        #   biYPelsPerMeter : i   0
        #   biClrUsed       : I   0
        #   biClrImportant  : I   0
        info_header = struct.pack(
            "<IiiHHIIiiII",
            info_size,
            self.width,
            self.height,   # positive: rows stored bottom-to-top in file
            1,
            24,
            0,
            pixel_data_size,
            0,
            0,
            0,
            0,
        )

        # Pixel data: bottom-up row order, BGR byte order, row-padded.
        pad = bytes(row_padding)
        rows = []
        for y in range(self.height - 1, -1, -1):   # flip: bottom row first
            row_bytes = bytearray(row_stride)
            for x in range(self.width):
                r, g, b = self.get_pixel(x, y)
                base = x * 3
                row_bytes[base]     = b             # BMP stores BGR
                row_bytes[base + 1] = g
                row_bytes[base + 2] = r
            rows.append(bytes(row_bytes) + pad)

        return file_header + info_header + b"".join(rows)

    def save(self, path: str) -> None:
        """Write the bitmap to *path* as a 24-bit BMP file."""
        with open(path, "wb") as f:
            f.write(self.to_bytes())


# ---------------------------------------------------------------------------
# Resize helpers
# ---------------------------------------------------------------------------

def _clamp(value: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, value))


def resize_nearest(src: Bitmap, new_width: int, new_height: int) -> Bitmap:
    """
    Return a new Bitmap scaled to (new_width × new_height) using
    nearest-neighbour interpolation.

    For each destination pixel (dx, dy) we map back to source space:
        sx = dx * src_width  / new_width   (use the centre of each pixel)
        sy = dy * src_height / new_height
    and round to the nearest integer.
    """
    dst = Bitmap(new_width, new_height)
    x_scale = src.width  / new_width
    y_scale = src.height / new_height

    for dy in range(new_height):
        sy = int((dy + 0.5) * y_scale)
        sy = _clamp(sy, 0, src.height - 1)
        for dx in range(new_width):
            sx = int((dx + 0.5) * x_scale)
            sx = _clamp(sx, 0, src.width - 1)
            r, g, b = src.get_pixel(sx, sy)
            dst.set_pixel(dx, dy, r, g, b)

    return dst


def resize_bilinear(src: Bitmap, new_width: int, new_height: int) -> Bitmap:
    """
    Return a new Bitmap scaled to (new_width × new_height) using
    bilinear interpolation.

    For each destination pixel (dx, dy) we map back to continuous source
    coordinates, identify the four surrounding pixels, and blend:

        src_x = (dx + 0.5) * (src_width  / new_width)  - 0.5
        src_y = (dy + 0.5) * (src_height / new_height) - 0.5

    The −0.5 / +0.5 shift aligns pixel centres rather than corners so the
    image edges are not shrunk inward.
    """
    dst = Bitmap(new_width, new_height)
    x_scale = src.width  / new_width
    y_scale = src.height / new_height

    w1 = src.width  - 1
    h1 = src.height - 1

    for dy in range(new_height):
        src_y = (dy + 0.5) * y_scale - 0.5
        y0 = _clamp(math.floor(src_y), 0, h1)
        y1 = _clamp(y0 + 1,            0, h1)
        fy = src_y - math.floor(src_y)         # fractional part [0, 1)

        for dx in range(new_width):
            src_x = (dx + 0.5) * x_scale - 0.5
            x0 = _clamp(math.floor(src_x), 0, w1)
            x1 = _clamp(x0 + 1,            0, w1)
            fx = src_x - math.floor(src_x)

            # Four corner samples
            r00, g00, b00 = src.get_pixel(x0, y0)
            r10, g10, b10 = src.get_pixel(x1, y0)
            r01, g01, b01 = src.get_pixel(x0, y1)
            r11, g11, b11 = src.get_pixel(x1, y1)

            # Bilinear blend: lerp along x, then lerp those results along y
            def blend(c00, c10, c01, c11):
                top    = c00 + (c10 - c00) * fx
                bottom = c01 + (c11 - c01) * fx
                return int(top + (bottom - top) * fy)

            dst.set_pixel(
                dx, dy,
                blend(r00, r10, r01, r11),
                blend(g00, g10, g01, g11),
                blend(b00, b10, b01, b11),
            )

    return dst


# ---------------------------------------------------------------------------
# Gradient generator
# ---------------------------------------------------------------------------

def make_gradient(width: int, height: int) -> Bitmap:
    """
    Generate a 2-D colour gradient:
      - Red   increases left → right  (x axis)
      - Green increases top  → bottom (y axis)
      - Blue  is a fixed mid-tone (96) for visual depth
    """
    bmp = Bitmap(width, height)
    for y in range(height):
        for x in range(width):
            r = int(x / (width  - 1) * 255) if width  > 1 else 0
            g = int(y / (height - 1) * 255) if height > 1 else 0
            b = 96
            bmp.set_pixel(x, y, r, g, b)
    return bmp


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    SRC_W, SRC_H   = 256, 256   # source gradient size
    NN_W,  NN_H    = 128, 96    # nearest-neighbour target (deliberately non-square)
    BL_W,  BL_H    = 384, 320   # bilinear target (upscale to show smoothness)

    print(f"Generating {SRC_W}×{SRC_H} gradient …")
    gradient = make_gradient(SRC_W, SRC_H)

    print(f"Nearest-neighbour resize → {NN_W}×{NN_H} …")
    nn = resize_nearest(gradient, NN_W, NN_H)
    nn.save("output_nearest.bmp")
    print("  Saved: output_nearest.bmp")

    print(f"Bilinear resize → {BL_W}×{BL_H} …")
    bl = resize_bilinear(gradient, BL_W, BL_H)
    bl.save("output_bilinear.bmp")
    print("  Saved: output_bilinear.bmp")


if __name__ == "__main__":
    main()
