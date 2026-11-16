#!/usr/bin/env python3
"""
parse.py – Parse a minidump's memory regions, locate a byte signature,
and walk length-prefixed UTF-16LE records out of the matching sections,
printing offsets and decoded strings.

Usage:
    python parse.py <minidump.dmp> <hex_signature> [--min-len N] [--max-len N]

Example:
    python parse.py crash.dmp DEADBEEF01020304 --min-len 2 --max-len 512
"""

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Minidump structure constants  (MINIDUMP_* from DbgHelp / winnt.h)
# ---------------------------------------------------------------------------

MINIDUMP_SIGNATURE = 0x504D444D  # 'MDMP'
MINIDUMP_VERSION   = 0x0000A793

# Stream type IDs
STREAM_TYPE_MEMORY_LIST      = 5
STREAM_TYPE_MEMORY64_LIST    = 9
STREAM_TYPE_MEMORY_INFO_LIST = 16

HEADER_FMT  = "<IHHIQQI"   # Signature, Version, NumberOfStreams, StreamDirectoryRva, CheckSum, TimeDateStamp, Flags
HEADER_SIZE = struct.calcsize(HEADER_FMT)

DIRECTORY_FMT  = "<IIQ"    # StreamType, DataSize, Rva
DIRECTORY_SIZE = struct.calcsize(DIRECTORY_FMT)

# MINIDUMP_MEMORY_DESCRIPTOR  (32-bit RVA variant, stream 5)
MEM_DESC_FMT  = "<QII"     # StartOfMemoryRange, DataSize, Rva
MEM_DESC_SIZE = struct.calcsize(MEM_DESC_FMT)

# MINIDUMP_MEMORY_DESCRIPTOR64 header
MEM64_HEADER_FMT  = "<QQ"  # NumberOfMemoryRanges, BaseRva
MEM64_HEADER_SIZE = struct.calcsize(MEM64_HEADER_FMT)

# MINIDUMP_MEMORY_DESCRIPTOR64 entry (no per-entry RVA; data is contiguous from BaseRva)
MEM64_ENTRY_FMT  = "<QQ"   # StartOfMemoryRange, DataSize
MEM64_ENTRY_SIZE = struct.calcsize(MEM64_ENTRY_FMT)


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class MemoryRegion:
    """A mapped memory region backed by bytes in the dump file."""
    va_start:  int   # virtual address of the first byte
    va_end:    int   # exclusive
    file_offset: int # offset in the dump file where the data begins
    size:      int

    def contains_va(self, va: int) -> bool:
        return self.va_start <= va < self.va_end

    def file_offset_for(self, va: int) -> int:
        return self.file_offset + (va - self.va_start)


@dataclass
class FoundRecord:
    region_va_start: int
    record_va:       int
    file_offset:     int
    char_length:     int
    text:            str


# ---------------------------------------------------------------------------
# Minidump reader
# ---------------------------------------------------------------------------

class MinidumpParser:
    def __init__(self, path: str) -> None:
        self.path = path
        self.data = Path(path).read_bytes()
        self._regions: List[MemoryRegion] = []
        self._parse_header()
        self._parse_streams()

    # ------------------------------------------------------------------
    # Header & stream directory
    # ------------------------------------------------------------------

    def _parse_header(self) -> None:
        if len(self.data) < HEADER_SIZE:
            raise ValueError("File too small to be a minidump.")
        (sig, ver, n_streams, dir_rva,
         checksum, timestamp, flags) = struct.unpack_from(HEADER_FMT, self.data, 0)
        if sig != MINIDUMP_SIGNATURE:
            raise ValueError(f"Bad signature: 0x{sig:08X} (expected 0x{MINIDUMP_SIGNATURE:08X})")
        self._n_streams = n_streams
        self._dir_rva   = dir_rva

    def _parse_streams(self) -> None:
        offset = self._dir_rva
        for _ in range(self._n_streams):
            if offset + DIRECTORY_SIZE > len(self.data):
                break
            stype, dsize, rva = struct.unpack_from(DIRECTORY_FMT, self.data, offset)
            offset += DIRECTORY_SIZE
            if stype == STREAM_TYPE_MEMORY_LIST:
                self._parse_memory_list(int(rva), int(dsize))
            elif stype == STREAM_TYPE_MEMORY64_LIST:
                self._parse_memory64_list(int(rva), int(dsize))

    # ------------------------------------------------------------------
    # Stream parsers
    # ------------------------------------------------------------------

    def _parse_memory_list(self, rva: int, size: int) -> None:
        """MINIDUMP_MEMORY_LIST (stream 5) – 32-bit-RVA descriptors."""
        if rva + 4 > len(self.data):
            return
        (n_ranges,) = struct.unpack_from("<I", self.data, rva)
        base = rva + 4
        for i in range(n_ranges):
            off = base + i * MEM_DESC_SIZE
            if off + MEM_DESC_SIZE > len(self.data):
                break
            va, dsize, data_rva = struct.unpack_from(MEM_DESC_FMT, self.data, off)
            if data_rva + dsize > len(self.data):
                continue
            self._regions.append(MemoryRegion(
                va_start=va, va_end=va + dsize,
                file_offset=data_rva, size=dsize
            ))

    def _parse_memory64_list(self, rva: int, size: int) -> None:
        """MINIDUMP_MEMORY64_LIST (stream 9) – contiguous data from a base RVA."""
        if rva + MEM64_HEADER_SIZE > len(self.data):
            return
        n_ranges, base_rva = struct.unpack_from(MEM64_HEADER_FMT, self.data, rva)
        entry_base = rva + MEM64_HEADER_SIZE
        current_rva = base_rva
        for i in range(n_ranges):
            off = entry_base + i * MEM64_ENTRY_SIZE
            if off + MEM64_ENTRY_SIZE > len(self.data):
                break
            va, dsize = struct.unpack_from(MEM64_ENTRY_FMT, self.data, off)
            if current_rva + dsize > len(self.data):
                current_rva += dsize
                continue
            self._regions.append(MemoryRegion(
                va_start=va, va_end=va + dsize,
                file_offset=current_rva, size=dsize
            ))
            current_rva += dsize

    # ------------------------------------------------------------------
    # Public accessors
    # ------------------------------------------------------------------

    @property
    def regions(self) -> List[MemoryRegion]:
        return self._regions

    def read_at_va(self, va: int, length: int) -> Optional[bytes]:
        """Return *length* bytes from virtual address *va*, or None."""
        for r in self._regions:
            if r.contains_va(va) and va + length <= r.va_end:
                fo = r.file_offset_for(va)
                if fo + length <= len(self.data):
                    return self.data[fo:fo + length]
        return None

    def region_data(self, region: MemoryRegion) -> bytes:
        end = min(region.file_offset + region.size, len(self.data))
        return self.data[region.file_offset:end]


# ---------------------------------------------------------------------------
# Signature search
# ---------------------------------------------------------------------------

def find_signature_in_region(
    dump: MinidumpParser,
    region: MemoryRegion,
    signature: bytes,
) -> Iterator[Tuple[int, int]]:
    """
    Yield (virtual_address, file_offset) for every occurrence of *signature*
    inside *region*.
    """
    data = dump.region_data(region)
    start = 0
    sig_len = len(signature)
    while True:
        idx = data.find(signature, start)
        if idx == -1:
            break
        va  = region.va_start + idx
        fo  = region.file_offset + idx
        yield va, fo
        start = idx + 1


def find_signature(
    dump: MinidumpParser,
    signature: bytes,
) -> Iterator[Tuple[MemoryRegion, int, int]]:
    """
    Search every region for *signature*.
    Yield (region, virtual_address, file_offset).
    """
    for region in dump.regions:
        for va, fo in find_signature_in_region(dump, region, signature):
            yield region, va, fo


# ---------------------------------------------------------------------------
# Length-prefixed UTF-16LE record walker
# ---------------------------------------------------------------------------

def walk_utf16_records(
    dump: MinidumpParser,
    region: MemoryRegion,
    min_len: int = 1,
    max_len: int = 1024,
) -> Iterator[FoundRecord]:
    """
    Walk *region* treating its content as a stream of length-prefixed UTF-16LE
    records.  Each record is:

        UINT32  char_count   – number of UTF-16 code units (not bytes)
        BYTE[char_count * 2] – UTF-16LE payload

    Records where char_count falls outside [min_len, max_len] are skipped
    (the walker advances by 2 bytes and retries, so it never gets stuck).

    Yields FoundRecord for every successfully decoded record.
    """
    data = dump.region_data(region)
    length = len(data)
    pos = 0

    while pos + 4 <= length:
        (char_count,) = struct.unpack_from("<I", data, pos)

        if min_len <= char_count <= max_len:
            byte_count = char_count * 2
            payload_start = pos + 4
            if payload_start + byte_count <= length:
                raw = data[payload_start:payload_start + byte_count]
                try:
                    text = raw.decode("utf-16-le")
                    # Require at least half the characters to be printable ASCII
                    # or common Unicode so we filter pure binary false positives.
                    printable = sum(1 for c in text if c.isprintable())
                    if printable >= max(1, char_count // 2):
                        va  = region.va_start + pos
                        fo  = region.file_offset + pos
                        yield FoundRecord(
                            region_va_start=region.va_start,
                            record_va=va,
                            file_offset=fo,
                            char_length=char_count,
                            text=text,
                        )
                        pos = payload_start + byte_count
                        continue
                except (UnicodeDecodeError, ValueError):
                    pass

        # Did not consume; advance by 2 (UTF-16 alignment)
        pos += 2


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Parse a minidump: locate a signature, walk UTF-16LE records."
    )
    p.add_argument("dump", help="Path to the .dmp minidump file.")
    p.add_argument(
        "signature",
        help="Hex byte string to search for (e.g. DEADBEEF or DE AD BE EF).",
    )
    p.add_argument(
        "--min-len", type=int, default=2,
        help="Minimum char_count for a UTF-16LE record (default: 2).",
    )
    p.add_argument(
        "--max-len", type=int, default=512,
        help="Maximum char_count for a UTF-16LE record (default: 512).",
    )
    p.add_argument(
        "--all-regions", action="store_true",
        help="Walk UTF-16LE records in every region, not just those containing the signature.",
    )
    return p.parse_args()


def hex_to_bytes(s: str) -> bytes:
    cleaned = s.replace(" ", "").replace("\\x", "").replace("0x", "")
    try:
        return bytes.fromhex(cleaned)
    except ValueError as e:
        raise ValueError(f"Invalid hex signature '{s}': {e}") from e


def main() -> None:
    args = parse_args()

    try:
        signature = hex_to_bytes(args.signature)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[*] Loading dump: {args.dump}")
    try:
        dump = MinidumpParser(args.dump)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[*] Memory regions found : {len(dump.regions)}")
    print(f"[*] Searching for signature: {signature.hex().upper()}")

    hit_regions: List[MemoryRegion] = []
    for region, va, fo in find_signature(dump, signature):
        print(
            f"  [SIG] VA=0x{va:016X}  FileOffset=0x{fo:08X}  "
            f"Region=[0x{region.va_start:016X}–0x{region.va_end:016X}]"
        )
        if region not in hit_regions:
            hit_regions.append(region)

    if not hit_regions:
        print("[!] Signature not found in any region.")
        if not args.all_regions:
            sys.exit(0)

    walk_targets = dump.regions if args.all_regions else hit_regions

    print(f"\n[*] Walking UTF-16LE records in {len(walk_targets)} region(s)  "
          f"(len range [{args.min_len}, {args.max_len}])")

    total = 0
    for region in walk_targets:
        region_total = 0
        for rec in walk_utf16_records(dump, region, args.min_len, args.max_len):
            if region_total == 0:
                print(f"\n  Region 0x{region.va_start:016X} – 0x{region.va_end:016X}  "
                      f"({region.size} bytes)")
            print(
                f"    VA=0x{rec.record_va:016X}  "
                f"FO=0x{rec.file_offset:08X}  "
                f"Chars={rec.char_length:5d}  "
                f"{rec.text!r}"
            )
            region_total += 1
            total += 1

    print(f"\n[*] Total UTF-16LE records printed: {total}")


if __name__ == "__main__":
    main()
