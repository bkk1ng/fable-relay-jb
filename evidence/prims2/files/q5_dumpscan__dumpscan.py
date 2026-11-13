#!/usr/bin/env python3
"""
dumpscan.py - scan a binary blob for length-prefixed UTF-16LE records.

Record layout (all little-endian):
    [16-byte signature][u32 length][payload]

* signature  - 16 opaque bytes identifying the record type.
* length      - number of bytes in the payload that follows.
* payload     - UTF-16LE encoded text.

A rolling byte-by-byte scan handles misalignment (garbage between records).
Records whose declared length exceeds the remaining bytes are skipped.

Public API:
    scan(blob) -> list[(offset, signature, text)]
"""

import struct
from typing import List, Tuple

# 16-byte signature + 4-byte little-endian u32 length.
HEADER_SIZE = 20
SIG_SIZE = 16
LEN_SIZE = 4


def scan(blob: bytes) -> List[Tuple[int, bytes, str]]:
    """
    Scan *blob* for records and return a list of (offset, signature, text).

    The scan advances byte-by-byte.  At each position it attempts to read a
    20-byte header, extract the u32 length, and validate that the payload
    fits within the remaining bytes and decodes cleanly as UTF-16LE.  When a
    valid record is found the scan jumps to the end of that record; otherwise
    it advances by one byte so that inter-record garbage is absorbed.
    """
    results: List[Tuple[int, bytes, str]] = []
    blob_len = len(blob)
    pos = 0

    while pos < blob_len:
        # Need at least HEADER_SIZE bytes to even read a header.
        if pos + HEADER_SIZE > blob_len:
            break

        signature = blob[pos:pos + SIG_SIZE]
        (length,) = struct.unpack_from('<I', blob, pos + SIG_SIZE)

        payload_start = pos + HEADER_SIZE
        payload_end = payload_start + length

        # Skip records whose declared length runs past the end of the blob.
        if payload_end > blob_len:
            pos += 1
            continue

        payload = blob[payload_start:payload_end]

        # Require a clean UTF-16LE decode so the rolling scan rejects noise
        # that happens to satisfy the length check.
        try:
            text = payload.decode('utf-16-le')
        except UnicodeDecodeError:
            pos += 1
            continue

        results.append((pos, signature, text))
        pos = payload_end

    return results


# --- smoke test (run: python3 dumpscan.py) ---------------------------------
if __name__ == '__main__':
    import sys

    def make_record(sig: bytes, text: str) -> bytes:
        """Build a single record from a 16-byte signature and text."""
        payload = text.encode('utf-16-le')
        return sig + struct.pack('<I', len(payload)) + payload

    blob = bytearray()

    # Record 0 at offset 0.
    blob += make_record(b'RECORD_SIG_0001', 'hello world')

    # 5 bytes of garbage to force a misalignment.
    blob += b'\x00\x01\x02\x03\x04'

    # Record 1 after the garbage.
    blob += make_record(b'RECORD_SIG_0002', 'second record')

    # A record whose declared length exceeds the remaining bytes (skipped).
    blob += b'TOO_SHORT_SIG___' + struct.pack('<I', 999) + b'x'

    found = scan(bytes(blob))
    for offset, sig, text in found:
        print(f'offset={offset:4d}  sig={sig!r}  text={text!r}')

    assert len(found) == 2, f'expected 2 records, got {len(found)}'
    assert found[0][2] == 'hello world'
    assert found[1][2] == 'second record'
    assert found[0][0] == 0
    assert found[1][0] == 47  # 42 (record 0) + 5 (garbage)

    # Empty blob should return nothing.
    assert scan(b'') == []

    # Blob smaller than a header should return nothing.
    assert scan(b'\x00' * 19) == []

    print('smoke test passed', file=sys.stderr)
    sys.exit(0)
