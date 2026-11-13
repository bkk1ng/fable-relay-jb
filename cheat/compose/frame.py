"""
frame.py – fixed-size buffer frame builder for exploitation / fuzzing research.

Public API
----------
build_frame(size, value, offset, filler=b'A', little_endian=True) -> bytes
    Build a frame of `size` bytes: `filler` repeated to fill the buffer, then
    a packed 64-bit `value` written at byte `offset`.

try_offsets(size, value, offsets, **kw) -> dict[int, bytes]
    Return {offset: frame} for every offset in `offsets`.

range_frames(size, value, start, stop, step=8, **kw) -> dict[int, bytes]
    Convenience wrapper: try every offset in range(start, stop, step).

cyclic_pattern(length, block=4) -> bytes
    Generate a de Bruijn-like cyclic pattern (ASCII, every block-byte
    sub-sequence unique) useful for locating the write offset empirically.

find_offset(pattern, marker, block=4) -> int | None
    Given the pattern produced by cyclic_pattern and a marker bytes object
    (e.g. the value read from a crashed register), return the byte offset at
    which that marker first appears, or None if not found.
"""

import struct
import itertools
import string
from typing import Iterator


# ---------------------------------------------------------------------------
# Core frame builder
# ---------------------------------------------------------------------------

def build_frame(
    size: int,
    value: int,
    offset: int,
    filler: bytes = b'A',
    little_endian: bool = True,
) -> bytes:
    """Return a *size*-byte frame with a packed 64-bit *value* at *offset*.

    Parameters
    ----------
    size:          Total buffer length in bytes.
    value:         64-bit integer to embed.
    offset:        Byte offset at which the 8-byte packed value is written.
    filler:        One-or-more-byte pattern repeated to fill the background.
                   Defaults to b'A'.
    little_endian: Pack as little-endian when True, big-endian when False.

    Raises
    ------
    ValueError: if the value does not fit in 8 bytes, or if offset + 8 > size.
    """
    if not (0 <= value < 2**64):
        raise ValueError(f"value {value!r} does not fit in a uint64")
    if offset < 0 or offset + 8 > size:
        raise ValueError(
            f"offset {offset} + 8 bytes exceeds buffer size {size}"
        )
    if not filler:
        raise ValueError("filler must not be empty")

    # Build background
    if len(filler) == 1:
        buf = bytearray(filler * size)
    else:
        # Tile the multi-byte pattern
        repeats = (size + len(filler) - 1) // len(filler)
        buf = bytearray((filler * repeats)[:size])

    # Stamp the 64-bit value
    fmt = '<Q' if little_endian else '>Q'
    struct.pack_into(fmt, buf, offset, value)
    return bytes(buf)


# ---------------------------------------------------------------------------
# Multi-offset helpers
# ---------------------------------------------------------------------------

def try_offsets(
    size: int,
    value: int,
    offsets,
    **kw,
) -> dict:
    """Return ``{offset: frame}`` for every offset in *offsets*.

    Extra keyword arguments are forwarded to :func:`build_frame`
    (``filler``, ``little_endian``).  Offsets that would overflow the buffer
    are silently skipped.
    """
    result: dict[int, bytes] = {}
    for off in offsets:
        try:
            result[off] = build_frame(size, value, off, **kw)
        except ValueError:
            pass
    return result


def range_frames(
    size: int,
    value: int,
    start: int = 0,
    stop: int | None = None,
    step: int = 8,
    **kw,
) -> dict:
    """Try every offset in ``range(start, stop, step)``.

    *stop* defaults to ``size - 8 + 1`` so the last valid offset is included.
    Extra keyword arguments are forwarded to :func:`build_frame`.
    """
    if stop is None:
        stop = size - 8 + 1
    return try_offsets(size, value, range(start, stop, step), **kw)


# ---------------------------------------------------------------------------
# Cyclic / de Bruijn pattern  (empirical offset location)
# ---------------------------------------------------------------------------

_PATTERN_ALPHABET = (string.ascii_uppercase + string.ascii_lowercase + string.digits).encode()


def _debruijn(alphabet: bytes, n: int) -> Iterator[int]:
    """Yield byte values forming a de Bruijn sequence B(|alphabet|, n)."""
    # Martin & Nilsson algorithm
    k = len(alphabet)
    a = [0] * k * n

    def db(t: int, p: int):
        if t > n:
            if n % p == 0:
                yield from a[1 : p + 1]
        else:
            a[t] = a[t - p]
            yield from db(t + 1, p)
            for j in range(a[t - p] + 1, k):
                a[t] = j
                yield from db(t + 1, t)

    yield from db(1, 1)


def cyclic_pattern(length: int, block: int = 4) -> bytes:
    """Return *length* bytes where every *block*-byte sub-sequence is unique.

    Uses a de Bruijn sequence over a 64-symbol alphabet.  Suitable for
    passing as the input buffer and then searching for a register value to
    find the exact write offset.

    Parameters
    ----------
    length: Desired pattern length in bytes.
    block:  Sub-sequence length to make unique (default 4; use 8 for 64-bit).
    """
    if block < 1:
        raise ValueError("block must be >= 1")
    buf = bytearray()
    for b in _debruijn(_PATTERN_ALPHABET, block):
        buf.append(_PATTERN_ALPHABET[b])
        if len(buf) >= length:
            break
    # If the de Bruijn sequence is shorter than requested (very small alphabet
    # + large block rarely happens at normal sizes), tile it.
    while len(buf) < length:
        buf.extend(buf)
    return bytes(buf[:length])


def find_offset(pattern: bytes, marker: bytes, block: int = 4) -> int | None:
    """Return the byte offset of *marker* in *pattern*, or ``None``.

    Parameters
    ----------
    pattern: The cyclic pattern originally sent (from :func:`cyclic_pattern`).
    marker:  The bytes observed in the target register / memory location
             (e.g. ``struct.pack('<I', rsp_value)`` for a 32-bit search).
    block:   Must match the *block* used when creating the pattern.
    """
    idx = pattern.find(marker)
    return idx if idx != -1 else None


# ---------------------------------------------------------------------------
# Smoke / demo  (compiled-in entry point, guarded by macro equivalent)
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    import sys

    SIZE   = 128
    VALUE  = 0xDEADBEEFCAFEBABE
    OFFSET = 40

    print("=== build_frame demo ===")
    frame = build_frame(SIZE, VALUE, OFFSET)
    print(f"  size={SIZE}, value=0x{VALUE:016X}, offset={OFFSET}")
    print(f"  frame[{OFFSET}:{OFFSET+8}] = {frame[OFFSET:OFFSET+8].hex()}")
    assert struct.unpack_from('<Q', frame, OFFSET)[0] == VALUE

    print("\n=== range_frames (offsets 0..127, step 8) ===")
    frames = range_frames(SIZE, VALUE, start=0, stop=SIZE, step=8)
    for off, f in sorted(frames.items()):
        v = struct.unpack_from('<Q', f, off)[0]
        assert v == VALUE, f"mismatch at offset {off}"
    print(f"  {len(frames)} frames verified OK")

    print("\n=== cyclic_pattern + find_offset ===")
    pat = cyclic_pattern(512, block=4)
    # Simulate: target crashes with register holding the 4 bytes at offset 100
    target_offset = 100
    marker = pat[target_offset : target_offset + 4]
    found = find_offset(pat, marker)
    print(f"  planted marker at offset {target_offset}, found at {found}")
    assert found == target_offset

    print("\nAll assertions passed.")
    sys.exit(0)
