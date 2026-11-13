#!/usr/bin/env python3
"""
dedupe.py — streaming JSON Lines ETL deduplicator

Strategy
--------
Each record's composite key is hashed to an 8-byte BLAKE2b digest (~0.1%
collision probability at 1 B records). Digests are held in an in-memory
``set``. When the set reaches ``--spill-at`` items the entire set is flushed
to an on-disk ``dbm`` store and cleared; subsequent lookups check memory
first, then disk. RAM usage is therefore proportional to ``--spill-at``
(~8 bytes × N) rather than to input size.

Duplicate counts are tracked in a separate dict keyed by digest. That dict
grows only when an actual duplicate is encountered, so it stays small when
duplication is sparse. It is never spilled — if you expect billions of
distinct duplicated keys, raise an issue.

Usage
-----
    # deduplicate by a single field
    python dedupe.py --key id --input raw.jsonl --output clean.jsonl

    # composite key, pipe-friendly
    cat events.jsonl | python dedupe.py --key user_id event_type > deduped.jsonl

    # lower memory ceiling (spill after 100 k items ≈ 800 KB)
    python dedupe.py --key id --spill-at 100000 --input big.jsonl --output out.jsonl
"""

from __future__ import annotations

import argparse
import dbm
import hashlib
import json
import os
import sys
import tempfile
from typing import IO, Any


# ---------------------------------------------------------------------------
# Key hashing
# ---------------------------------------------------------------------------

def _make_digest(record: dict[str, Any], fields: list[str]) -> bytes:
    """
    Return an 8-byte BLAKE2b digest of the composite key.

    Fields are serialised with ``json.dumps`` (sort_keys=True) so that
    dict-valued fields hash consistently.  Parts are NUL-separated so that
    ("a", "bc") and ("ab", "c") produce different digests.
    """
    parts = [
        json.dumps(record.get(field), sort_keys=True, ensure_ascii=False)
        for field in fields
    ]
    raw = "\x00".join(parts).encode()
    return hashlib.blake2b(raw, digest_size=8).digest()


# ---------------------------------------------------------------------------
# Bounded deduplication set
# ---------------------------------------------------------------------------

class BoundedSet:
    """
    A membership set of 8-byte digests with a configurable RAM ceiling.

    While ``len(mem) < spill_at`` everything stays in a Python ``set``.
    When the threshold is crossed the whole in-memory set is written to a
    ``dbm`` file and the memory set is cleared.  Later ``__contains__``
    calls check memory first; the disk store is only opened after the first
    spill, so a small input pays no I/O cost at all.

    Parameters
    ----------
    spill_at:
        Maximum in-memory digest count before a flush.
    db_path:
        Base filesystem path for the dbm file.  The underlying backend
        (gdbm, ndbm, or the pure-Python dumbdbm fallback) may append its
        own extension.
    """

    def __init__(self, spill_at: int, db_path: str) -> None:
        self._spill_at = spill_at
        self._db_path = db_path
        self._mem: set[bytes] = set()
        self._db: Any = None          # dbm database handle, opened lazily
        self.spill_count: int = 0

    # ------------------------------------------------------------------
    # Internal helpers

    def _open_db(self) -> None:
        if self._db is None:
            self._db = dbm.open(self._db_path, "c")

    def _flush(self) -> None:
        """Flush the in-memory set to disk and clear it."""
        self._open_db()
        for digest in self._mem:
            self._db[digest] = b"\x01"
        self._mem.clear()
        self.spill_count += 1

    # ------------------------------------------------------------------
    # Public interface

    def __contains__(self, digest: bytes) -> bool:
        if digest in self._mem:
            return True
        if self._db is not None and digest in self._db:
            return True
        return False

    def add(self, digest: bytes) -> None:
        """
        Add *digest* to the set.

        If the in-memory set is full, it is spilled to disk first so that
        after ``add`` returns memory usage is back below the ceiling.
        """
        if len(self._mem) >= self._spill_at:
            self._flush()
        self._mem.add(digest)

    def close(self) -> None:
        if self._db is not None:
            self._db.close()
            self._db = None

    # context-manager support

    def __enter__(self) -> "BoundedSet":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


# ---------------------------------------------------------------------------
# ETL pipeline
# ---------------------------------------------------------------------------

def run(
    *,
    key_fields: list[str],
    input_stream: IO[str],
    output_stream: IO[str],
    spill_at: int,
    db_path: str,
    error_stream: IO[str],
) -> None:
    """
    Read JSON Lines from *input_stream*, deduplicate by *key_fields*, and
    write unique records to *output_stream*.  Statistics are printed to
    *error_stream* when processing is complete.
    """
    lines_read = 0
    lines_written = 0
    lines_skipped = 0
    lines_malformed = 0

    # digest -> number of *extra* occurrences (first occurrence not counted)
    dupe_counts: dict[bytes, int] = {}

    with BoundedSet(spill_at=spill_at, db_path=db_path) as seen:
        for raw_line in input_stream:
            stripped = raw_line.rstrip("\n")
            if not stripped:
                continue

            lines_read += 1

            # --- parse ---------------------------------------------------
            try:
                record = json.loads(stripped)
            except json.JSONDecodeError as exc:
                lines_malformed += 1
                print(
                    f"[dedupe] malformed JSON on input line {lines_read}: {exc}",
                    file=error_stream,
                )
                continue

            if not isinstance(record, dict):
                lines_malformed += 1
                print(
                    f"[dedupe] line {lines_read} is not a JSON object — skipped",
                    file=error_stream,
                )
                continue

            # --- deduplicate ---------------------------------------------
            digest = _make_digest(record, key_fields)

            if digest in seen:
                lines_skipped += 1
                dupe_counts[digest] = dupe_counts.get(digest, 0) + 1
            else:
                seen.add(digest)
                output_stream.write(stripped)
                output_stream.write("\n")
                lines_written += 1

        spill_count = seen.spill_count

    # -------------------------------------------------------------------------
    # Print stats
    # -------------------------------------------------------------------------
    distinct_duped_keys = len(dupe_counts)
    total_dupe_occurrences = sum(dupe_counts.values())

    sep = "=" * 44
    print(f"\n{sep}", file=error_stream)
    print("  dedupe stats", file=error_stream)
    print(sep, file=error_stream)
    print(f"  {'lines read':<28}: {lines_read:>10,}", file=error_stream)
    print(f"  {'malformed / skipped':<28}: {lines_malformed:>10,}", file=error_stream)
    print(f"  {'unique records written':<28}: {lines_written:>10,}", file=error_stream)
    print(f"  {'duplicate records dropped':<28}: {lines_skipped:>10,}", file=error_stream)

    if lines_read:
        rate = lines_skipped / lines_read * 100.0
        print(f"  {'duplicate rate':<28}: {rate:>9.2f}%", file=error_stream)

    print(f"  {'distinct keys with dupes':<28}: {distinct_duped_keys:>10,}", file=error_stream)
    print(f"  {'disk spills':<28}: {spill_count:>10,}", file=error_stream)

    if dupe_counts:
        top_n = 10
        top = sorted(dupe_counts.items(), key=lambda kv: kv[1], reverse=True)[:top_n]
        print(
            f"\n  top {top_n} most-duplicated keys "
            f"(digest hex → total occurrences):",
            file=error_stream,
        )
        for digest, extra in top:
            print(
                f"    {digest.hex()}  ×{extra + 1}",
                file=error_stream,
            )

    print(sep, file=error_stream)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="dedupe.py",
        description="Stream-deduplicate JSON Lines by a composite key.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python dedupe.py --key id --input raw.jsonl --output clean.jsonl\n"
            "  cat data.jsonl | python dedupe.py --key user_id event_type\n"
        ),
    )
    p.add_argument(
        "--key",
        nargs="+",
        required=True,
        metavar="FIELD",
        help="One or more record fields that together form the composite key.",
    )
    p.add_argument(
        "--input",
        default="-",
        metavar="FILE",
        help="Input JSON Lines file (default: stdin).",
    )
    p.add_argument(
        "--output",
        default="-",
        metavar="FILE",
        help="Output JSON Lines file (default: stdout).",
    )
    p.add_argument(
        "--spill-at",
        type=int,
        default=1_000_000,
        metavar="N",
        help=(
            "Spill in-memory digest set to disk when it reaches N items. "
            "RAM ≈ 8 × N bytes for the digests plus Python set overhead. "
            "Default: 1_000_000 (~8 MB of raw digests)."
        ),
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)

    if args.spill_at < 1:
        print("dedupe.py: --spill-at must be >= 1", file=sys.stderr)
        return 2

    # Open I/O before creating the tempdir so errors surface early.
    input_owned = args.input != "-"
    output_owned = args.output != "-"

    input_stream: IO[str] = (
        open(args.input, encoding="utf-8") if input_owned else sys.stdin
    )
    output_stream: IO[str] = (
        open(args.output, "w", encoding="utf-8") if output_owned else sys.stdout
    )

    try:
        # The tempdir (and therefore the dbm files) is deleted automatically
        # when the context manager exits, even on exception.
        with tempfile.TemporaryDirectory(prefix="dedupe_spill_") as tmpdir:
            db_path = os.path.join(tmpdir, "seen")
            run(
                key_fields=args.key,
                input_stream=input_stream,
                output_stream=output_stream,
                spill_at=args.spill_at,
                db_path=db_path,
                error_stream=sys.stderr,
            )
    finally:
        if input_owned:
            input_stream.close()
        if output_owned:
            output_stream.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
