"""
bitmap.py — 24-bit BMP writer with nearest-neighbour and bilinear resize.

Python 3.12, stdlib only (no PIL, no numpy).

Run:
    python bitmap.py

Produces:
    gradient_nn.bmp — nearest-neighbour 4x upscale of a 64x64 gradient
    gradient_bl.bmp — bilinear 4x upscale of the same 64x64 gradient
"""

import struct


class PixelBuffer:
    """A simple RGB pixel buffer with row-padded BMP serialisation.

    Internal layout is row-major, top-to-bottom, each pixel an (r, g, b)
    tuple. BMP stores rows bottom-to-top in BGR order, so that translation
    happens at save time.
    """

    def __init__(self, width: int, height: int):
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive")
        self.width = width
        self.height = height
        self.pixels: list[tuple[int, int, int]] = [(0, 0, 0)] * (width * height)

    def _index(self, x: int, y: int) -> int:
        return y * self.width + x

    def set_pixel(self, x: int, y: int, r: int, g: int, b: int) -> None:
        self.pixels[self._index(x, y)] = (
            max(0, min(255, int(r))),
            max(0, min(255, int(g))),
            max(0, min(255, int(b))),
        )

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        return self.pixels[self._index(x, y)]

    def _row_byte_size(self) -> int:
        # Each pixel is 3 bytes; rows are padded to a 4-byte boundary.
        row_bytes = self.width * 3
        padding = (4 - (row_bytes % 4)) % 4
        return row_bytes + padding

    def save_bmp(self, path: str) -> None:
        """Write the buffer as a 24-bit uncompressed BMP file."""
        row_size = self._row_byte_size()
        pixel_data_size = row_size * self.height
        file_header_size = 14
        info_header_size = 40
        pixel_offset = file_header_size + info_header_size
        total_size = pixel_offset + pixel_data_size

        # BITMAPFILEHEADER — 14 bytes, little-endian
        #   Signature      2  "BM"
        #   FileSize       4  uint32
        #   Reserved1      2  uint16  (0)
        #   Reserved2      2  uint16  (0)
        #   PixelOffset    4  uint32
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",
            total_size,
            0,
            0,
            pixel_offset,
        )

        # BITMAPINFOHEADER — 40 bytes, little-endian
        #   HeaderSize     4  uint32   (40)
        #   Width          4  int32
        #   Height         4  int32   (positive => bottom-up rows)
        #   Planes         2  uint16   (1)
        #   BitCount       2  uint16   (24)
        #   Compression    4  uint32   (0 = BI_RGB)
        #   ImageSize      4  uint32   (0 is valid for BI_RGB)
        #   XPelsPerMeter  4  int32
        #   YPelsPerMeter  4  int32
        #   ClrUsed        4  uint32   (0)
        #   ClrImportant   4  uint32   (0)
        info_header = struct.pack(
            "<IiiHHIIiiII",
            info_header_size,
            self.width,
            self.height,
            1,        # planes
            24,       # bits per pixel
            0,        # BI_RGB (no compression)
            0,        # image size (0 valid for BI_RGB)
            2835,     # ~72 DPI in pixels/meter
            2835,
            0,        # colors used
            0,        # important colors
        )

        # Build pixel data. Our internal buffer is top-to-bottom; BMP stores
        # rows bottom-to-top, so iterate in reverse.
        row_bytes = self.width * 3
        padding = (4 - (row_bytes % 4)) % 4
        pad_bytes = b"\x00" * padding

        data = bytearray()
        for y in range(self.height - 1, -1, -1):
            row = bytearray()
            for x in range(self.width):
                r, g, b = self.get_pixel(x, y)
                # BMP stores pixels as BGR
                row += struct.pack("BBB", b & 0xFF, g & 0xFF, r & 0xFF)
            data += row + pad_bytes

        with open(path, "wb") as f:
            f.write(file_header)
            f.write(info_header)
            f.write(data)


def generate_gradient(width: int, height: int) -> PixelBuffer:
    """Generate a smooth diagonal RGB gradient."""
    buf = PixelBuffer(width, height)
    for y in range(height):
        for x in range(width):
            r = int(255 * x / max(1, width - 1))
            g = int(255 * y / max(1, height - 1))
            b = 128
            buf.set_pixel(x, y, r, g, b)
    return buf


def resize_nearest_neighbour(
    src: PixelBuffer, new_width: int, new_height: int
) -> PixelBuffer:
    """Nearest-neighbour resize — picks the closest source pixel."""
    dst = PixelBuffer(new_width, new_height)
    x_ratio = src.width / new_width
    y_ratio = src.height / new_height
    for y in range(new_height):
        src_y = min(int(y * y_ratio), src.height - 1)
        for x in range(new_width):
            src_x = min(int(x * x_ratio), src.width - 1)
            dst.set_pixel(x, y, *src.get_pixel(src_x, src_y))
    return dst


def resize_bilinear(
    src: PixelBuffer, new_width: int, new_height: int
) -> PixelBuffer:
    """Bilinear resize — linearly interpolates between the four nearest source pixels.

    Uses the half-pixel-center convention: dst pixel center maps to
    src = (dst + 0.5) * scale - 0.5, so the interpolation weight is the
    fractional part of that mapped coordinate.
    """
    dst = PixelBuffer(new_width, new_height)
    x_scale = src.width / new_width
    y_scale = src.height / new_height

    for y in range(new_height):
        src_y = (y + 0.5) * y_scale - 0.5
        y0 = int(src_y)
        wy = src_y - y0
        y0 = max(y0, 0)
        y1 = min(y0 + 1, src.height - 1)
        if y0 == y1:
            wy = 0.0
        wy = max(0.0, min(1.0, wy))

        for x in range(new_width):
            src_x = (x + 0.5) * x_scale - 0.5
            x0 = int(src_x)
            wx = src_x - x0
            x0 = max(x0, 0)
            x1 = min(x0 + 1, src.width - 1)
            if x0 == x1:
                wx = 0.0
            wx = max(0.0, min(1.0, wx))

            tl = src.get_pixel(x0, y0)
            tr = src.get_pixel(x1, y0)
            bl = src.get_pixel(x0, y1)
            br = src.get_pixel(x1, y1)

            r = (
                tl[0] * (1 - wx) * (1 - wy)
                + tr[0] * wx * (1 - wy)
                + bl[0] * (1 - wx) * wy
                + br[0] * wx * wy
            )
            g = (
                tl[1] * (1 - wx) * (1 - wy)
                + tr[1] * wx * (1 - wy)
                + bl[1] * (1 - wx) * wy
                + br[1] * wx * wy
            )
            b = (
                tl[2] * (1 - wx) * (1 - wy)
                + tr[2] * wx * (1 - wy)
                + bl[2] * (1 - wx) * wy
                + br[2] * wx * wy
            )

            dst.set_pixel(x, y, r, g, b)
    return dst


def main() -> None:
    """Generate a gradient, upscale it two ways, and save both BMPs."""
    source = generate_gradient(64, 64)

    nn = resize_nearest_neighbour(source, 256, 256)
    bl = resize_bilinear(source, 256, 256)

    nn.save_bmp("gradient_nn.bmp")
    bl.save_bmp("gradient_bl.bmp")

    print(f"Saved gradient_nn.bmp ({nn.width}x{nn.height})")
    print(f"Saved gradient_bl.bmp ({bl.width}x{bl.height})")


if __name__ == "__main__":
    main()
