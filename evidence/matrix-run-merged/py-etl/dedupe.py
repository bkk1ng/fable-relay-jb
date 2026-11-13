#!/usr/bin/env python3
"""
dedupe.py — streaming JSON Lines deduplicator

Reads JSON Lines from stdin or a file, deduplicates records by a composite
key, and writes unique records to stdout or a file.

Memory strategy
───────────────
Phase 1 (in-memory): 8-byte blake2b digests are held in a plain Python set.
Phase 2 (on-disk):   Once the set reaches --spill-at entries the entire set is
                     migrated to a temporary SQLite database and the in-memory
                     set is released.  All subsequent lookups and inserts use
                     SQLite.  The temp file is deleted on clean exit.

Stats are printed to stderr so stdout stays clean when piped.

Usage examples
──────────────
  # deduplicate on a single field
  python dedupe.py --key id < raw.jsonl > clean.jsonl

  # composite key; explicit files; custom spill threshold
  python dedupe.py -k user_id event_type -i raw.jsonl -o clean.jsonl --spill-at 500000

  # pipe with low memory limit (useful for testing spill path)
  cat raw.jsonl | python dedupe.py -k id --spill-at 100
"""

import argparse
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path
from typing import Iterator

# ── constants ────────────────────────────────────────────────────────────────

DEFAULT_SPILL_AT   = 1_000_000   # digest count that triggers disk spill
DEFAULT_BATCH      = 10_000      # SQLite commit cadence (output records)
DEFAULT_TOP_DUPES  = 20          # rows shown in the duplicate-key report
DIGEST_BYTES       = 8           # blake2b output size in bytes


# ── key hashing ──────────────────────────────────────────────────────────────

def make_digest(record: dict, key_fields: list[str]) -> bytes:
    """
    Return an 8-byte blake2b digest of the composite key.

    Fields are JSON-serialised and joined with a null-byte separator so that
    ("ab", "c") and ("a", "bc") produce different digests.
    """
    parts = [
        json.dumps(record.get(f), sort_keys=True, ensure_ascii=False)
        for f in key_fields
    ]
    raw = "\x00".join(parts).encode()
    return hashlib.blake2b(raw, digest_size=DIGEST_BYTES).digest()


def key_label(record: dict, key_fields: list[str]) -> str:
    """Human-readable composite key for the stats report."""
    return "|".join(str(record.get(f)) for f in key_fields)


# ── deduplication store ───────────────────────────────────────────────────────

class DedupStore:
    """
    Bounded-memory set of 8-byte digests.

    Public methods: seen(), add(), flush(), close()
    Public properties: spilled, mem_size, db_path, db_size_bytes
    """

    def __init__(self, spill_at: int, tmp_dir: str | None = None) -> None:
        self._spill_at  = spill_at
        self._tmp_dir   = tmp_dir
        self._mem: set[bytes] = set()
        self._conn: sqlite3.Connection | None = None
        self._db_path: Path | None = None
        self._spilled   = False

    # ── read-only properties ──────────────────────────────────────────────

    @property
    def spilled(self) -> bool:
        return self._spilled

    @property
    def mem_size(self) -> int:
        """Current number of digests held in the in-memory set."""
        return len(self._mem)

    @property
    def db_path(self) -> Path | None:
        return self._db_path

    @property
    def db_size_bytes(self) -> int:
        """Size of the spill DB on disk, or 0 if not yet spilled."""
        if self._db_path and self._db_path.exists():
            return self._db_path.stat().st_size
        return 0

    # ── core API ──────────────────────────────────────────────────────────

    def seen(self, digest: bytes) -> bool:
        """Return True if *digest* has been recorded before."""
        if digest in self._mem:
            return True
        if self._spilled:
            row = self._conn.execute(          # type: ignore[union-attr]
                "SELECT 1 FROM seen WHERE digest = ? LIMIT 1", (digest,)
            ).fetchone()
            return row is not None
        return False

    def add(self, digest: bytes) -> None:
        """Record *digest* as seen."""
        if self._spilled:
            self._conn.execute(                # type: ignore[union-attr]
                "INSERT OR IGNORE INTO seen VALUES (?)", (digest,)
            )
        else:
            self._mem.add(digest)
            if len(self._mem) >= self._spill_at:
                self._spill()

    def flush(self) -> None:
        """Commit any pending SQLite writes."""
        if self._conn:
            self._conn.commit()

    def close(self) -> None:
        """Commit, close, and delete the spill database."""
        if self._conn:
            try:
                self._conn.commit()
                self._conn.close()
            finally:
                self._conn = None
        if self._db_path:
            self._db_path.unlink(missing_ok=True)
            self._db_path = None

    # ── internal ──────────────────────────────────────────────────────────

    def _spill(self) -> None:
        """
        Migrate everything from the in-memory set to SQLite, then clear RAM.

        SQLite is opened with WAL + NORMAL sync for throughput; PRIMARY KEY on
        the BLOB column gives O(log n) lookup and duplicate rejection for free.
        """
        fd, path = tempfile.mkstemp(
            suffix=".db", prefix="dedupe_spill_", dir=self._tmp_dir
        )
        os.close(fd)
        self._db_path = Path(path)

        conn = sqlite3.connect(str(self._db_path), isolation_level=None)
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous  = NORMAL")
        conn.execute(
            "CREATE TABLE seen (digest BLOB NOT NULL PRIMARY KEY) WITHOUT ROWID"
        )

        # Bulk-load existing in-memory digests in one transaction.
        conn.execute("BEGIN")
        conn.executemany("INSERT OR IGNORE INTO seen VALUES (?)",
                         ((d,) for d in self._mem))
        conn.execute("COMMIT")

        # Switch to manual transaction mode for subsequent writes.
        conn.isolation_level = ""  # '' = deferred, managed by our flush() calls
        conn.execute("BEGIN")

        self._conn    = conn
        self._spilled = True
        self._mem.clear()
        self._mem = set()   # release the old set's backing storage


# ── record iterator ───────────────────────────────────────────────────────────

def iter_lines(src) -> Iterator[tuple[int, str, dict | None]]:
    """
    Yield (lineno, raw_line, parsed) for every non-empty line.

    *parsed* is None when the line is not valid JSON.
    """
    for lineno, raw in enumerate(src, start=1):
        raw = raw.rstrip("\n")
        if not raw:
            continue
        try:
            yield lineno, raw, json.loads(raw)
        except json.JSONDecodeError:
            yield lineno, raw, None


# ── main logic ────────────────────────────────────────────────────────────────

def run(args: argparse.Namespace) -> int:
    key_fields: list[str] = args.key

    store = DedupStore(spill_at=args.spill_at, tmp_dir=args.tmp_dir)

    # Tracks how many extra copies were seen per composite key string.
    # Only populated for keys that actually have at least one duplicate.
    dupe_counts: dict[str, int] = defaultdict(int)

    total        = 0
    written      = 0
    skipped      = 0
    parse_errors = 0
    t0           = time.monotonic()

    try:
        in_fh  = open(args.input,  encoding="utf-8")           if args.input  else sys.stdin
        out_fh = open(args.output, "w", encoding="utf-8")      if args.output else sys.stdout

        try:
            for lineno, raw, record in iter_lines(in_fh):
                total += 1

                if record is None:
                    parse_errors += 1
                    print(
                        f"[dedupe] line {lineno}: JSON parse error — skipping",
                        file=sys.stderr,
                    )
                    continue

                digest = make_digest(record, key_fields)

                if store.seen(digest):
                    dupe_counts[key_label(record, key_fields)] += 1
                    skipped += 1
                else:
                    store.add(digest)
                    out_fh.write(raw + "\n")
                    written += 1
                    # Commit pending SQLite writes on a regular cadence so the
                    # WAL file stays bounded.
                    if store.spilled and written % args.batch == 0:
                        store.flush()

        finally:
            store.flush()
            if args.input  and not in_fh.closed:
                in_fh.close()
            if args.output and not out_fh.closed:
                out_fh.close()

    finally:
        db_bytes = store.db_size_bytes   # read before close() deletes the file
        store.close()

    elapsed = time.monotonic() - t0
    _print_stats(
        key_fields   = key_fields,
        total        = total,
        written      = written,
        skipped      = skipped,
        parse_errors = parse_errors,
        elapsed      = elapsed,
        spilled      = store.spilled,
        db_bytes     = db_bytes,
        dupe_counts  = dupe_counts,
        top_n        = args.top_dupes,
    )
    return 0


# ── stats printing ─────────────────────────────────────────────────────────────

def _fmt_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n //= 1024
    return f"{n:.1f} TB"


def _print_stats(
    *,
    key_fields:   list[str],
    total:        int,
    written:      int,
    skipped:      int,
    parse_errors: int,
    elapsed:      float,
    spilled:      bool,
    db_bytes:     int,
    dupe_counts:  dict[str, int],
    top_n:        int,
) -> None:
    w = sys.stderr
    sep = "─" * 54

    print(sep, file=w)
    print("  dedupe stats", file=w)
    print(sep, file=w)
    print(f"  key fields   : {', '.join(key_fields)}", file=w)
    print(f"  total lines  : {total:>12,}", file=w)
    print(f"  written      : {written:>12,}", file=w)
    print(f"  duplicates   : {skipped:>12,}", file=w)
    if parse_errors:
        print(f"  parse errors : {parse_errors:>12,}", file=w)
    print(f"  elapsed      : {elapsed:>11.2f}s", file=w)
    if elapsed > 0:
        print(f"  throughput   : {total / elapsed:>9,.0f} rec/s", file=w)
    print(f"  disk spill   : {'yes  (' + _fmt_bytes(db_bytes) + ')' if spilled else 'no':>12}", file=w)
    print(sep, file=w)

    if dupe_counts and top_n > 0:
        print(f"\n  top {top_n} duplicate keys  (extra copies → key)", file=w)
        ranked = sorted(dupe_counts.items(), key=lambda x: -x[1])[:top_n]
        for label, count in ranked:
            print(f"  {count:>10,}  {label}", file=w)
        if len(dupe_counts) > top_n:
            print(f"  … and {len(dupe_counts) - top_n:,} more duplicate key(s)", file=w)
        print(sep, file=w)


# ── CLI ───────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="dedupe.py",
        description="Stream-deduplicate JSON Lines by a composite key.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument(
        "--key", "-k",
        nargs="+",
        required=True,
        metavar="FIELD",
        help="One or more JSON field names that form the composite key.",
    )
    p.add_argument(
        "--input", "-i",
        default=None,
        metavar="FILE",
        help="Input JSON Lines file.  Reads stdin when omitted.",
    )
    p.add_argument(
        "--output", "-o",
        default=None,
        metavar="FILE",
        help="Output JSON Lines file.  Writes stdout when omitted.",
    )
    p.add_argument(
        "--spill-at",
        type=int,
        default=DEFAULT_SPILL_AT,
        metavar="N",
        help=(
            f"Migrate the in-memory digest set to a temporary SQLite database "
            f"after N entries.  Default: {DEFAULT_SPILL_AT:,}.  "
            f"Each entry is {DIGEST_BYTES} bytes, so the set uses roughly "
            f"N×{DIGEST_BYTES} bytes of raw storage (Python set overhead is "
            f"additional)."
        ),
    )
    p.add_argument(
        "--batch",
        type=int,
        default=DEFAULT_BATCH,
        metavar="N",
        help=(
            f"Commit pending SQLite writes every N output records.  "
            f"Default: {DEFAULT_BATCH:,}."
        ),
    )
    p.add_argument(
        "--tmp-dir",
        default=None,
        metavar="DIR",
        help="Directory for the spill database.  Defaults to the system temp directory.",
    )
    p.add_argument(
        "--top-dupes",
        type=int,
        default=DEFAULT_TOP_DUPES,
        metavar="N",
        help=(
            f"Show top N duplicate keys in the stats report.  "
            f"Default: {DEFAULT_TOP_DUPES}.  Set to 0 to suppress."
        ),
    )
    return p


if __name__ == "__main__":
    sys.exit(run(build_parser().parse_args()))
