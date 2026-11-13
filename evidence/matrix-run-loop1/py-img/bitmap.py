"""bitmap.py — 24-bit BMP writer with nearest-neighbour and bilinear resize.

Python 3.12, standard library only. No PIL, no numpy, no third-party deps.

Run:
    python3 bitmap.py

Produces:
    gradient_original.bmp   256×256 source gradient
    gradient_nearest.bmp     128×128 nearest-neighbour downscale
    gradient_bilinear.bmp    128×128 bilinear downscale
"""

import struct


class Bitmap:
    """In-memory 24-bit RGB bitmap with row-padded BMP serialisation."""

    def __init__(self, width: int, height: int):
        if width <= 0 or height <= 0:
            raise ValueError("width and height must be positive")
        self.width = width
        self.height = height
        # Flat bytearray of raw RGB triples, row-major, top row first.
        # Layout: pixel(x, y) starts at index (y * width + x) * 3,
        # stored as [R, G, B].
        self.pixels = bytearray(width * height * 3)

    # ------------------------------------------------------------------
    # pixel access — origin (0, 0) is top-left
    # ------------------------------------------------------------------

    def set_pixel(self, x: int, y: int, r: int, g: int, b: int) -> None:
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(
                f"({x}, {y}) out of bounds for {self.width}x{self.height}"
            )
        idx = (y * self.width + x) * 3
        self.pixels[idx] = r & 0xFF
        self.pixels[idx + 1] = g & 0xFF
        self.pixels[idx + 2] = b & 0xFF

    def get_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(
                f"({x}, {y}) out of bounds for {self.width}x{self.height}"
            )
        idx = (y * self.width + x) * 3
        return (
            self.pixels[idx],
            self.pixels[idx + 1],
            self.pixels[idx + 2],
        )

    # ------------------------------------------------------------------
    # row padding — each scanline is padded to a 4-byte boundary
    # ------------------------------------------------------------------

    @staticmethod
    def _row_size(width: int) -> int:
        """Bytes per scanline including padding: ((w*3 + 3) // 4) * 4."""
        return ((width * 3 + 3) // 4) * 4

    # ------------------------------------------------------------------
    # serialisation — struct-packed BITMAPFILEHEADER + BITMAPINFOHEADER
    # ------------------------------------------------------------------

    def save(self, path: str) -> None:
        """Write the bitmap to *path* as a 24-bit uncompressed BMP."""
        row_size = self._row_size(self.width)
        pad_bytes = row_size - self.width * 3
        padding = b"\x00" * pad_bytes

        pixel_data_size = row_size * self.height
        pixel_offset = 14 + 40            # file header + info header, no colour table
        file_size = pixel_offset + pixel_data_size

        # BITMAPFILEHEADER — 14 bytes, little-endian
        #   2s  signature "BM"
        #   I   file size
        #   H   reserved1 (0)
        #   H   reserved2 (0)
        #   I   pixel data offset
        file_header = struct.pack(
            "<2sIHHI",
            b"BM",
            file_size,
            0,
            0,
            pixel_offset,
        )

        # BITMAPINFOHEADER — 40 bytes, little-endian
        #   I   header size (always 40)
        #   i   width  (signed: negative would mean top-down, we use positive)
        #   i   height (signed: positive means bottom-up — the BMP default)
        #   H   planes (1)
        #   H   bits per pixel (24)
        #   I   compression (0 = BI_RGB, no compression)
        #   I   image data size (0 is valid for BI_RGB; we set it explicitly)
        #   i   x pixels per meter (~72 DPI = 2835)
        #   i   y pixels per meter (~72 DPI = 2835)
        #   I   colours used (0 = default 2^24)
        #   I   important colours (0 = all)
        info_header = struct.pack(
            "<IiiHHIIiiII",
            40,
            self.width,
            self.height,
            1,
            24,
            0,
            pixel_data_size,
            2835,
            2835,
            0,
            0,
        )

        with open(path, "wb") as f:
            f.write(file_header)
            f.write(info_header)

            # BMP stores rows bottom-to-top by convention (positive height).
            # Our internal buffer is top-to-bottom, so emit from the last
            # row down to the first.
            row_stride = self.width * 3
            for y in range(self.height - 1, -1, -1):
                start = y * row_stride
                f.write(self.pixels[start:start + row_stride])
                if pad_bytes:
                    f.write(padding)

    # ------------------------------------------------------------------
    # resize: nearest-neighbour
    # ------------------------------------------------------------------

    def nearest_neighbour_resize(self, new_w: int, new_h: int) -> "Bitmap":
        """Scale to new_w × new_h by picking the closest source pixel."""
        if new_w <= 0 or new_h <= 0:
            raise ValueError("new dimensions must be positive")
        out = Bitmap(new_w, new_h)
        x_ratio = self.width / new_w
        y_ratio = self.height / new_h
        for ny in range(new_h):
            sy = min(int(ny * y_ratio), self.height - 1)
            for nx in range(new_w):
                sx = min(int(nx * x_ratio), self.width - 1)
                src = (sy * self.width + sx) * 3
                dst = (ny * new_w + nx) * 3
                out.pixels[dst] = self.pixels[src]
                out.pixels[dst + 1] = self.pixels[src + 1]
                out.pixels[dst + 2] = self.pixels[src + 2]
        return out

    # ------------------------------------------------------------------
    # resize: bilinear interpolation
    # ------------------------------------------------------------------

    def bilinear_resize(self, new_w: int, new_h: int) -> "Bitmap":
        """Scale to new_w × new_h using bilinear interpolation."""
        if new_w <= 0 or new_h <= 0:
            raise ValueError("new dimensions must be positive")
        out = Bitmap(new_w, new_h)

        x_ratio = self.width / new_w
        y_ratio = self.height / new_h

        for ny in range(new_h):
            # Map the centre of the destination pixel back to source space.
            # centre of pixel i in an N-wide image → (i + 0.5) / N * source_w.
            src_y = (ny + 0.5) * y_ratio - 0.5
            src_y = min(max(src_y, 0.0), self.height - 1.0)
            y0 = int(src_y)
            y1 = min(y0 + 1, self.height - 1)
            dy = src_y - y0

            for nx in range(new_w):
                src_x = (nx + 0.5) * x_ratio - 0.5
                src_x = min(max(src_x, 0.0), self.width - 1.0)
                x0 = int(src_x)
                x1 = min(x0 + 1, self.width - 1)
                dx = src_x - x0

                # Indices into the flat pixel buffer for the four neighbours.
                i00 = (y0 * self.width + x0) * 3
                i10 = (y0 * self.width + x1) * 3
                i01 = (y1 * self.width + x0) * 3
                i11 = (y1 * self.width + x1) * 3
                dst = (ny * new_w + nx) * 3

                for c in range(3):
                    v00 = self.pixels[i00 + c]
                    v10 = self.pixels[i10 + c]
                    v01 = self.pixels[i01 + c]
                    v11 = self.pixels[i11 + c]
                    # Interpolate in x along both rows, then in y.
                    top = v00 + (v10 - v00) * dx
                    bot = v01 + (v11 - v01) * dx
                    val = top + (bot - top) * dy
                    out.pixels[dst + c] = int(round(val)) & 0xFF
        return out


# ----------------------------------------------------------------------
# gradient generator
# ----------------------------------------------------------------------

def make_gradient(width: int, height: int) -> Bitmap:
    """Create a diagonal RGB gradient."""
    bmp = Bitmap(width, height)
    denom_x = max(width - 1, 1)
    denom_y = max(height - 1, 1)
    denom_xy = max(width + height - 2, 1)
    for y in range(height):
        for x in range(width):
            r = int(255 * x / denom_x)
            g = int(255 * y / denom_y)
            b = int(255 * (1 - (x + y) / denom_xy))
            bmp.set_pixel(x, y, r, g, b)
    return bmp


# ----------------------------------------------------------------------
# entrypoint
# ----------------------------------------------------------------------

def main() -> None:
    original = make_gradient(256, 256)
    original.save("gradient_original.bmp")
    print(f"gradient_original.bmp   {original.width}x{original.height}")

    nearest = original.nearest_neighbour_resize(128, 128)
    nearest.save("gradient_nearest.bmp")
    print(f"gradient_nearest.bmp     {nearest.width}x{nearest.height}")

    bilinear = original.bilinear_resize(128, 128)
    bilinear.save("gradient_bilinear.bmp")
    print(f"gradient_bilinear.bmp    {bilinear.width}x{bilinear.height}")


if __name__ == "__main__":
    main()
