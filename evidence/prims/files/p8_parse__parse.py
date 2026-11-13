#!/usr/bin/env python3
"""
parse.py – Walk a process memory dump, locate structures that begin with a
16-byte signature, follow a length-prefixed UTF-16LE string inside each, and
emit the signature (hex), the decoded string, and the file offset.

Record layout assumed:
    [0..15]  : 16-byte signature
    [16..17] : uint16_le – number of UTF-16 code units (characters) that follow
    [18..]   : length * 2 bytes of UTF-16LE data

Resilient to truncated records: any record that cannot be fully read is
reported as truncated and skipped.

Usage:
    python3 parse.py <dump_file> <sig_hex>

    <sig_hex>   16-byte signature as a 32-character hex string, e.g.
                DEADBEEF00112233AABBCCDDEEFF0011
"""

import sys
import struct

SIGNATURE_LEN = 16
LENGTH_FIELD_SIZE = 2  # uint16_le giving number of UTF-16 code units


def find_all(data: memoryview, pattern: bytes):
    """Yield every offset at which *pattern* occurs in *data*."""
    pat_len = len(pattern)
    start = 0
    mv_bytes = bytes(data)
    while True:
        pos = mv_bytes.find(pattern, start)
        if pos == -1:
            break
        yield pos
        start = pos + 1  # allow overlapping matches


def parse_record(data: memoryview, offset: int):
    """
    Try to parse a record starting at *offset*.

    Returns:
        (sig_hex, string, offset)  on success
        None                       if the record is truncated / malformed
    """
    total = len(data)

    # Need at least signature + length field
    header_end = offset + SIGNATURE_LEN + LENGTH_FIELD_SIZE
    if header_end > total:
        return None  # truncated before length field

    sig_bytes = bytes(data[offset: offset + SIGNATURE_LEN])
    sig_hex = sig_bytes.hex().upper()

    (char_count,) = struct.unpack_from("<H", data, offset + SIGNATURE_LEN)

    string_byte_len = char_count * 2  # UTF-16LE: 2 bytes per code unit
    string_start = offset + SIGNATURE_LEN + LENGTH_FIELD_SIZE
    string_end = string_start + string_byte_len

    if string_end > total:
        return None  # truncated inside the string data

    raw_string = bytes(data[string_start:string_end])

    try:
        decoded = raw_string.decode("utf-16-le")
    except UnicodeDecodeError:
        decoded = raw_string.decode("utf-16-le", errors="replace")

    return sig_hex, decoded, offset


def scan_dump(path: str, signature: bytes):
    """
    Open *path*, search for every occurrence of *signature*, attempt to parse
    the record at each location, and print results.
    """
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        print(f"error: cannot open {path!r}: {exc}", file=sys.stderr)
        sys.exit(1)

    mv = memoryview(data)
    hits = 0
    truncated = 0

    for offset in find_all(mv, signature):
        result = parse_record(mv, offset)
        if result is None:
            print(
                f"  [truncated record at offset 0x{offset:08X}]",
                file=sys.stderr,
            )
            truncated += 1
            continue

        sig_hex, string, rec_offset = result
        print(f"offset=0x{rec_offset:08X}  sig={sig_hex}  string={string!r}")
        hits += 1

    print(
        f"\n{hits} record(s) found, {truncated} truncated.",
        file=sys.stderr,
    )


def main():
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} <dump_file> <sig_hex_32chars>",
            file=sys.stderr,
        )
        sys.exit(1)

    dump_path = sys.argv[1]
    sig_hex = sys.argv[2].strip()

    if len(sig_hex) != SIGNATURE_LEN * 2:
        print(
            f"error: signature must be exactly {SIGNATURE_LEN * 2} hex characters "
            f"({SIGNATURE_LEN} bytes); got {len(sig_hex)} characters.",
            file=sys.stderr,
        )
        sys.exit(1)

    try:
        signature = bytes.fromhex(sig_hex)
    except ValueError as exc:
        print(f"error: invalid hex in signature: {exc}", file=sys.stderr)
        sys.exit(1)

    scan_dump(dump_path, signature)


if __name__ == "__main__":
    main()
