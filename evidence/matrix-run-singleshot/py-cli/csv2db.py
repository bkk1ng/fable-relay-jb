#!/usr/bin/env python3
"""
csv2db.py — Stream a CSV file into a SQLite database with column type inference.

Usage:
    python csv2db.py input.csv --db out.db --table mytable
    python csv2db.py input.csv --db out.db --table mytable --dry-run
    python csv2db.py input.csv --db out.db --table mytable --null "" "NA" "N/A"
"""

import argparse
import csv
import sqlite3
import sys
import re
from datetime import datetime
from itertools import islice
from pathlib import Path

# ---------------------------------------------------------------------------
# Type inference
# ---------------------------------------------------------------------------

# ISO 8601 and common date/datetime patterns
_DATE_PATTERNS = [
    r"^\d{4}-\d{2}-\d{2}$",                          # 2024-01-15
    r"^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}(:\d{2})?$", # 2024-01-15T10:30 / 2024-01-15 10:30:00
    r"^\d{2}/\d{2}/\d{4}$",                           # 01/15/2024
    r"^\d{2}-\d{2}-\d{4}$",                           # 01-15-2024
]
_DATE_RE = [re.compile(p) for p in _DATE_PATTERNS]


def _looks_like_date(value: str) -> bool:
    return any(r.match(value) for r in _DATE_RE)


def infer_sqlite_type(values: list[str], null_markers: set[str]) -> str:
    """
    Examine a sample of non-null values and return the tightest SQLite type:
    INTEGER > REAL > DATE > TEXT.
    Falls back to TEXT on any mismatch.
    """
    non_null = [v for v in values if v not in null_markers]
    if not non_null:
        return "TEXT"

    # Try INTEGER
    try:
        for v in non_null:
            int(v)
        return "INTEGER"
    except ValueError:
        pass

    # Try REAL
    try:
        for v in non_null:
            float(v)
        return "REAL"
    except ValueError:
        pass

    # Try DATE
    if all(_looks_like_date(v) for v in non_null):
        return "DATE"

    return "TEXT"


# ---------------------------------------------------------------------------
# Schema inference from a header + sample rows
# ---------------------------------------------------------------------------

SAMPLE_ROWS = 500  # rows read purely for type inference


def _sanitize_name(name: str) -> str:
    """Quote an identifier for SQLite."""
    return '"' + name.replace('"', '""') + '"'


def infer_schema(
    csv_path: Path,
    null_markers: set[str],
    encoding: str = "utf-8-sig",
) -> tuple[list[str], list[str]]:
    """
    Return (column_names, sqlite_types) by scanning up to SAMPLE_ROWS rows.
    """
    with csv_path.open(newline="", encoding=encoding) as fh:
        reader = csv.reader(fh)
        try:
            headers = next(reader)
        except StopIteration:
            sys.exit("error: CSV file is empty or has no header row")

        samples: list[list[str]] = [[] for _ in headers]
        for row in islice(reader, SAMPLE_ROWS):
            # Pad short rows, truncate long rows
            row = (row + [""] * len(headers))[: len(headers)]
            for i, cell in enumerate(row):
                samples[i].append(cell)

    types = [infer_sqlite_type(col_values, null_markers) for col_values in samples]
    return headers, types


# ---------------------------------------------------------------------------
# DDL helpers
# ---------------------------------------------------------------------------

def build_create_ddl(table: str, headers: list[str], types: list[str]) -> str:
    cols = ",\n    ".join(
        f"{_sanitize_name(h)} {t}" for h, t in zip(headers, types)
    )
    return f'CREATE TABLE IF NOT EXISTS {_sanitize_name(table)} (\n    {cols}\n);'


# ---------------------------------------------------------------------------
# Value coercion at insert time
# ---------------------------------------------------------------------------

def coerce(value: str, sqlite_type: str, null_markers: set[str]):
    """Convert a raw string cell to the appropriate Python type for sqlite3."""
    if value in null_markers:
        return None
    if sqlite_type == "INTEGER":
        try:
            return int(value)
        except ValueError:
            return None
    if sqlite_type == "REAL":
        try:
            return float(value)
        except ValueError:
            return None
    # DATE and TEXT: store as string; SQLite will handle affinity
    return value


# ---------------------------------------------------------------------------
# Streaming insert
# ---------------------------------------------------------------------------

BATCH_SIZE = 500


def stream_insert(
    csv_path: Path,
    db_path: Path,
    table: str,
    headers: list[str],
    types: list[str],
    null_markers: set[str],
    encoding: str = "utf-8-sig",
) -> None:
    placeholders = ", ".join("?" * len(headers))
    quoted_cols = ", ".join(_sanitize_name(h) for h in headers)
    insert_sql = (
        f"INSERT INTO {_sanitize_name(table)} ({quoted_cols}) VALUES ({placeholders})"
    )

    total_rows = 0
    batch: list[tuple] = []

    con = sqlite3.connect(db_path)
    try:
        with csv_path.open(newline="", encoding=encoding) as fh:
            reader = csv.reader(fh)
            next(reader)  # skip header

            for row in reader:
                row = (row + [""] * len(headers))[: len(headers)]
                coerced = tuple(
                    coerce(cell, t, null_markers) for cell, t in zip(row, types)
                )
                batch.append(coerced)

                if len(batch) >= BATCH_SIZE:
                    with con:
                        con.executemany(insert_sql, batch)
                    total_rows += len(batch)
                    batch.clear()
                    _progress(total_rows)

            # Flush remaining rows
            if batch:
                with con:
                    con.executemany(insert_sql, batch)
                total_rows += len(batch)
                _progress(total_rows)

        # Final newline after progress line
        print(f"\rInserted {total_rows:,} rows into '{table}'.", file=sys.stderr)
    finally:
        con.close()


def _progress(n: int) -> None:
    print(f"\r  {n:>10,} rows processed…", end="", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Stream a CSV file into a SQLite table with type inference.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("csv", metavar="CSV_FILE", type=Path, help="Input CSV file.")
    p.add_argument(
        "--db",
        required=True,
        metavar="DB_FILE",
        type=Path,
        help="SQLite database file (created if absent).",
    )
    p.add_argument(
        "--table",
        required=True,
        metavar="TABLE",
        help="Destination table name (created if absent).",
    )
    p.add_argument(
        "--null",
        nargs="+",
        metavar="MARKER",
        default=["", "NULL", "null", "NA", "N/A", "n/a", "None", "none"],
        help='Strings treated as NULL. Default: "" NULL null NA N/A n/a None none',
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Infer schema and print the CREATE TABLE statement; do not write.",
    )
    p.add_argument(
        "--encoding",
        default="utf-8-sig",
        metavar="ENC",
        help="CSV file encoding. Default: utf-8-sig (handles UTF-8 BOM).",
    )
    return p


def main() -> None:
    args = build_parser().parse_args()

    csv_path: Path = args.csv
    if not csv_path.exists():
        sys.exit(f"error: file not found: {csv_path}")

    null_markers: set[str] = set(args.null)

    # --- Schema inference ---
    print(f"Inferring schema from up to {SAMPLE_ROWS} rows…", file=sys.stderr)
    headers, types = infer_schema(csv_path, null_markers, encoding=args.encoding)

    ddl = build_create_ddl(args.table, headers, types)

    if args.dry_run:
        print(ddl)
        return

    # --- Create table and stream data ---
    con = sqlite3.connect(args.db)
    try:
        with con:
            con.execute(ddl)
    finally:
        con.close()

    print(f"Streaming '{csv_path}' → '{args.db}' (table: {args.table})", file=sys.stderr)
    stream_insert(
        csv_path=csv_path,
        db_path=args.db,
        table=args.table,
        headers=headers,
        types=types,
        null_markers=null_markers,
        encoding=args.encoding,
    )


if __name__ == "__main__":
    main()
