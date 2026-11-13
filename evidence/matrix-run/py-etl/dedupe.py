#!/usr/bin/env python3
"""
dedupe.py — Streaming JSON Lines deduplicator with bounded-memory strategy.

Reads JSON Lines from a file or stdin, deduplicates records by a composite key,
and writes unique records to a file or stdout.  Stats go to stderr.

Memory model
------------
  An in-memory Python set holds 8-byte BLAKE2b digests of composite keys.
  When that set reaches --spill (default 500 000) entries, its contents are
  flushed to a temporary dbm database and the set is cleared.  Every lookup
  checks the in-memory set first, then the on-disk store if one exists.
  Peak RSS stays close to: threshold × 8 bytes  (≈ 4 MB at the default).

Usage
-----
  python dedupe.py -k id,timestamp input.jsonl -o output.jsonl
  cat records.jsonl | python dedupe.py -k user_id,event > clean.jsonl

Options
-------
  -k / --keys         Comma-separated composite-key field names  [required]
  -o / --output       Output file path                           [default: stdout]
  -n / --spill        In-memory digest cap before disk spill     [default: 500 000]
  --null-missing      Treat absent key fields as null instead of skipping the record
  positional INPUT    Input JSON Lines file                      [default: stdin]
"""

from __future__ import annotations

import argparse
import dbm
import hashlib
import json
import os
import sys
import tempfile
import time
from collections import defaultdict


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DIGEST_BYTES = 8  # 64-bit digest; birthday collision at ~4 billion unique keys


# ---------------------------------------------------------------------------
# Key hashing
# ---------------------------------------------------------------------------

def encode_key(record: dict, fields: list[str], null_missing: bool) -> bytes:
    """
    Return a canonical UTF-8 byte string representing the composite key.

    The key is the JSON encoding of the ordered list of field values, so it
    is unambiguous even when values contain commas or other delimiters.
    """
    parts: list = []
    for f in fields:
        if f not in record:
            if null_missing:
                parts.append(None)
            else:
                raise KeyError(f)
        else:
            parts.append(record[f])
    return json.dumps(parts, separators=(",", ":"), sort_keys=True,
                      ensure_ascii=False).encode()


def digest(key_bytes: bytes) -> bytes:
    """Return an 8-byte BLAKE2b digest of the composite-key bytes."""
    return hashlib.blake2b(key_bytes, digest_size=DIGEST_BYTES).digest()


# ---------------------------------------------------------------------------
# Bounded-memory digest set
# ---------------------------------------------------------------------------

class DigestSet:
    """
    Set of 8-byte digests with an on-disk spill tier.

    When the in-memory set reaches `threshold` items, every entry is written
    to a dbm file and the set is cleared.  Membership checks probe memory
    first, then disk.
    """

    def __init__(self, threshold: int, spill_dir: str) -> None:
        if threshold < 1:
            raise ValueError("threshold must be >= 1")
        self.threshold = threshold
        self._mem: set[bytes] = set()
        self._db: dbm.error | None = None          # opened on first spill
        self._db_path: str = os.path.join(spill_dir, "digest_spill")
        self.spill_count: int = 0

    # -- public interface ----------------------------------------------------

    def __contains__(self, d: bytes) -> bool:
        return d in self._mem or (self._db is not None and d in self._db)

    def add(self, d: bytes) -> None:
        """Add a digest; spill in-memory set to disk if threshold is reached."""
        self._mem.add(d)
        if len(self._mem) >= self.threshold:
            self._spill()

    def close(self) -> None:
        if self._db is not None:
            self._db.close()
            self._db = None

    # -- properties ----------------------------------------------------------

    @property
    def mem_size(self) -> int:
        return len(self._mem)

    # -- internals -----------------------------------------------------------

    def _spill(self) -> None:
        """Flush in-memory set to the dbm file and clear it."""
        if self._db is None:
            self._db = dbm.open(self._db_path, "c")
        for d in self._mem:
            self._db[d] = b""
        self._mem.clear()
        self.spill_count += 1


# ---------------------------------------------------------------------------
# ETL core
# ---------------------------------------------------------------------------

def process(
    in_stream,
    out_stream,
    fields: list[str],
    threshold: int,
    null_missing: bool,
    spill_dir: str,
) -> dict:
    """
    Stream records, deduplicate, and write clean output.

    Returns a stats dict suitable for display.
    """
    seen = DigestSet(threshold, spill_dir)
    # Maps digest → number of *extra* occurrences (i.e. dupe count, not total).
    # Keyed by digest to avoid storing the full key string for every dupe.
    dupe_counts: defaultdict[bytes, int] = defaultdict(int)

    total = 0
    dupes = 0
    skipped = 0
    t0 = time.monotonic()

    for lineno, raw in enumerate(in_stream, 1):
        line = raw.rstrip("\n")
        if not line:
            continue

        # Parse ---------------------------------------------------------------
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            print(f"[warn] line {lineno}: JSON parse error — {exc}", file=sys.stderr)
            skipped += 1
            continue

        if not isinstance(record, dict):
            print(f"[warn] line {lineno}: expected JSON object, got "
                  f"{type(record).__name__}", file=sys.stderr)
            skipped += 1
            continue

        total += 1

        # Composite key -------------------------------------------------------
        try:
            key_bytes = encode_key(record, fields, null_missing)
        except KeyError as exc:
            field = exc.args[0]
            print(f"[warn] line {lineno}: missing key field {field!r} — "
                  "skipping (use --null-missing to treat as null)", file=sys.stderr)
            skipped += 1
            total -= 1
            continue

        d = digest(key_bytes)

        # Deduplicate ---------------------------------------------------------
        if d in seen:
            dupe_counts[d] += 1
            dupes += 1
        else:
            seen.add(d)
            out_stream.write(line + "\n")

    elapsed = time.monotonic() - t0
    seen.close()

    return {
        "total": total,
        "unique": total - dupes,
        "duplicates": dupes,
        "skipped": skipped,
        "spills": seen.spill_count,
        "elapsed": elapsed,
        "dupe_counts": dict(dupe_counts),   # digest(bytes) → extra count
    }


# ---------------------------------------------------------------------------
# Stats output
# ---------------------------------------------------------------------------

def print_stats(stats: dict, fields: list[str], threshold: int) -> None:
    total    = stats["total"]
    unique   = stats["unique"]
    dupes    = stats["duplicates"]
    skipped  = stats["skipped"]
    spills   = stats["spills"]
    elapsed  = stats["elapsed"]
    rate     = int(total / elapsed) if elapsed > 0 else 0

    sep = "─" * 52
    out = sys.stderr

    print(sep, file=out)
    print(f"  key fields       : {', '.join(fields)}", file=out)
    print(f"  spill threshold  : {threshold:,}", file=out)
    print(sep, file=out)
    print(f"  records read     : {total:>12,}", file=out)
    print(f"  unique written   : {unique:>12,}", file=out)
    print(f"  duplicates found : {dupes:>12,}", file=out)
    print(f"  skipped (errors) : {skipped:>12,}", file=out)
    print(f"  disk spills      : {spills:>12,}", file=out)
    print(f"  elapsed          : {elapsed:>11.3f}s", file=out)
    print(f"  throughput       : {rate:>9,} rec/s", file=out)

    # Top duplicates by dupe count
    dupe_counts = stats["dupe_counts"]
    if dupe_counts:
        top = sorted(dupe_counts.items(), key=lambda kv: kv[1], reverse=True)[:10]
        print(sep, file=out)
        print("  top duplicate keys  (digest → extra occurrences):", file=out)
        for d, count in top:
            print(f"    {d.hex()}  ×{count + 1:,} total ({count:,} extra)", file=out)

    print(sep, file=out)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="dedupe.py",
        description=(
            "Streaming JSON Lines deduplicator.  "
            "Deduplicates by a composite key using a bounded-memory strategy "
            "(in-memory BLAKE2b digest set + on-disk dbm spill)."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage")[1] if "Usage" in __doc__ else "",
    )
    p.add_argument(
        "input",
        nargs="?",
        metavar="INPUT",
        help="Input JSON Lines file (default: stdin).",
    )
    p.add_argument(
        "-k", "--keys",
        required=True,
        metavar="FIELD[,FIELD...]",
        help="Comma-separated composite key field names.",
    )
    p.add_argument(
        "-o", "--output",
        metavar="FILE",
        help="Output file path (default: stdout).",
    )
    p.add_argument(
        "-n", "--spill",
        type=int,
        default=500_000,
        metavar="N",
        help="In-memory digest count before spilling to disk (default: 500,000).",
    )
    p.add_argument(
        "--null-missing",
        action="store_true",
        help=(
            "Treat absent key fields as null instead of skipping the record "
            "with a warning."
        ),
    )
    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    fields = [f.strip() for f in args.keys.split(",") if f.strip()]
    if not fields:
        parser.error("--keys requires at least one non-empty field name.")

    if args.spill < 1:
        parser.error("--spill must be a positive integer.")

    # --- open input ----------------------------------------------------------
    if args.input:
        try:
            in_stream = open(args.input, "r", encoding="utf-8")
        except OSError as exc:
            print(f"error: cannot open input — {exc}", file=sys.stderr)
            sys.exit(1)
    else:
        in_stream = sys.stdin

    # --- open output ---------------------------------------------------------
    if args.output:
        try:
            out_stream = open(args.output, "w", encoding="utf-8")
        except OSError as exc:
            print(f"error: cannot open output — {exc}", file=sys.stderr)
            if args.input:
                in_stream.close()
            sys.exit(1)
    else:
        out_stream = sys.stdout

    # --- temp dir for spill --------------------------------------------------
    spill_dir = tempfile.mkdtemp(prefix="dedupe_")

    exit_code = 0
    try:
        stats = process(
            in_stream, out_stream,
            fields, args.spill, args.null_missing, spill_dir,
        )
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        exit_code = 1
        stats = None
    finally:
        if args.input:
            in_stream.close()
        if args.output:
            out_stream.close()
        # Clean up spill files
        for entry in os.listdir(spill_dir):
            try:
                os.remove(os.path.join(spill_dir, entry))
            except OSError:
                pass
        try:
            os.rmdir(spill_dir)
        except OSError:
            pass

    if stats is not None:
        print_stats(stats, fields, args.spill)

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
