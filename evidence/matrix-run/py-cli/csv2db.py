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
from pathlib import Path

# ---------------------------------------------------------------------------
# Type inference
# ---------------------------------------------------------------------------

# ISO 8601 and common date/datetime patterns to probe
_DATE_FORMATS = (
    "%Y-%m-%d",
    "%Y-%m-%dT%H:%M:%S",
    "%Y-%m-%dT%H:%M:%SZ",
    "%Y-%m-%d %H:%M:%S",
    "%d/%m/%Y",
    "%m/%d/%Y",
    "%d-%m-%Y",
    "%m-%d-%Y",
)

_INT_RE = re.compile(r"^-?\d+$")
_FLOAT_RE = re.compile(r"^-?\d*\.\d+$|^-?\d+\.\d*$|^-?\d+[eE][+-]?\d+$")


def _is_integer(value: str) -> bool:
    return bool(_INT_RE.match(value))


def _is_float(value: str) -> bool:
    return bool(_FLOAT_RE.match(value))


def _is_date(value: str) -> bool:
    for fmt in _DATE_FORMATS:
        try:
            datetime.strptime(value, fmt)
            return True
        except ValueError:
            pass
    return False


def infer_column_type(samples: list[str]) -> str:
    """
    Given a list of non-null sample values for one column, return the
    SQLite affinity: INTEGER, REAL, TEXT, or DATE (stored as TEXT).
    Falls back toward TEXT on any mismatch.
    """
    if not samples:
        return "TEXT"

    candidate = "INTEGER"
    for val in samples:
        if candidate == "INTEGER":
            if _is_integer(val):
                continue
            elif _is_float(val):
                candidate = "REAL"
            elif _is_date(val):
                candidate = "DATE"
            else:
                return "TEXT"
        elif candidate == "REAL":
            if _is_integer(val) or _is_float(val):
                continue
            else:
                return "TEXT"
        elif candidate == "DATE":
            if _is_date(val):
                continue
            else:
                return "TEXT"

    return candidate


def coerce(value: str, col_type: str):
    """Convert a string value to the appropriate Python type for SQLite binding."""
    if col_type == "INTEGER":
        return int(value)
    if col_type == "REAL":
        return float(value)
    # DATE and TEXT: store as-is
    return value


# ---------------------------------------------------------------------------
# Schema inference from a sample of rows
# ---------------------------------------------------------------------------

SAMPLE_ROWS = 500  # rows to read before committing to types


def infer_schema(
    csv_path: Path,
    null_markers: set[str],
    delimiter: str,
    encoding: str,
) -> tuple[list[str], list[str]]:
    """
    Read up to SAMPLE_ROWS rows, then return (column_names, column_types).
    """
    columns: list[str] = []
    samples: dict[int, list[str]] = {}  # col_index -> non-null values

    with csv_path.open(newline="", encoding=encoding) as fh:
        reader = csv.reader(fh, delimiter=delimiter)
        try:
            columns = next(reader)
        except StopIteration:
            print("error: CSV file is empty or has no header row", file=sys.stderr)
            sys.exit(1)

        samples = {i: [] for i in range(len(columns))}

        for row_num, row in enumerate(reader, start=1):
            if row_num > SAMPLE_ROWS:
                break
            for i, cell in enumerate(row):
                if i >= len(columns):
                    break
                stripped = cell.strip()
                if stripped and stripped not in null_markers:
                    samples[i].append(stripped)

    types = [infer_column_type(samples[i]) for i in range(len(columns))]
    return columns, types


# ---------------------------------------------------------------------------
# DDL helpers
# ---------------------------------------------------------------------------

def _quote(name: str) -> str:
    """Double-quote an identifier, escaping any embedded quotes."""
    return '"' + name.replace('"', '""') + '"'


def build_create_table(table: str, columns: list[str], types: list[str]) -> str:
    col_defs = ",\n    ".join(
        f"{_quote(col)} {typ}" for col, typ in zip(columns, types)
    )
    return f"CREATE TABLE IF NOT EXISTS {_quote(table)} (\n    {col_defs}\n);"


# ---------------------------------------------------------------------------
# Streaming insert
# ---------------------------------------------------------------------------

BATCH_SIZE = 500


def stream_insert(
    conn: sqlite3.Connection,
    table: str,
    csv_path: Path,
    columns: list[str],
    types: list[str],
    null_markers: set[str],
    delimiter: str,
    encoding: str,
) -> int:
    """
    Stream the CSV into the database in batched transactions.
    Returns the total number of rows inserted.
    """
    placeholders = ", ".join("?" for _ in columns)
    col_list = ", ".join(_quote(c) for c in columns)
    insert_sql = (
        f"INSERT INTO {_quote(table)} ({col_list}) VALUES ({placeholders})"
    )

    total_rows = 0
    batch: list[tuple] = []

    with csv_path.open(newline="", encoding=encoding) as fh:
        reader = csv.reader(fh, delimiter=delimiter)
        next(reader)  # skip header

        for row_num, row in enumerate(reader, start=2):  # 2 = first data line
            # Pad short rows, truncate long ones silently
            padded: list = list(row) + [""] * max(0, len(columns) - len(row))
            padded = padded[: len(columns)]

            record: list = []
            for cell, col_type in zip(padded, types):
                stripped = cell.strip()
                if not stripped or stripped in null_markers:
                    record.append(None)
                else:
                    try:
                        record.append(coerce(stripped, col_type))
                    except (ValueError, OverflowError):
                        record.append(stripped)  # fall back to raw string

            batch.append(tuple(record))

            if len(batch) >= BATCH_SIZE:
                with conn:
                    conn.executemany(insert_sql, batch)
                total_rows += len(batch)
                batch = []
                _progress(total_rows, csv_path)

        if batch:
            with conn:
                conn.executemany(insert_sql, batch)
            total_rows += len(batch)
            _progress(total_rows, csv_path, final=True)

    return total_rows


def _progress(rows: int, csv_path: Path, *, final: bool = False) -> None:
    end = "\n" if final else "\r"
    print(f"  {rows:,} rows inserted from {csv_path.name}", end=end, file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="csv2db",
        description="Stream a CSV file into a SQLite database with type inference.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
examples:
  csv2db.py data.csv --db store.db --table sales
  csv2db.py data.csv --db store.db --table sales --dry-run
  csv2db.py data.csv --db store.db --table sales --null "" "NA" "N/A" "null"
  csv2db.py data.csv --db store.db --table sales --delimiter ";"
""",
    )
    p.add_argument("csv", metavar="CSV", type=Path, help="Input CSV file.")
    p.add_argument(
        "--db",
        required=True,
        metavar="DB",
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
        nargs="*",
        default=["", "NULL", "null", "NA", "N/A", "n/a", "nan", "NaN", "none", "None"],
        metavar="MARKER",
        help=(
            "One or more cell values to treat as SQL NULL. "
            'Default: empty string plus "NULL", "NA", "N/A", etc.'
        ),
    )
    p.add_argument(
        "--delimiter",
        default=",",
        metavar="CHAR",
        help="CSV field delimiter (default: comma).",
    )
    p.add_argument(
        "--encoding",
        default="utf-8-sig",  # handles BOM from Excel exports transparently
        metavar="ENC",
        help='File encoding (default: "utf-8-sig").',
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Infer schema and print the CREATE TABLE statement; do not write.",
    )
    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    csv_path: Path = args.csv
    if not csv_path.exists():
        print(f"error: file not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    null_markers: set[str] = set(args.null)

    # ---- Schema inference ------------------------------------------------
    print(f"Inferring schema from up to {SAMPLE_ROWS} rows …", file=sys.stderr)
    columns, types = infer_schema(csv_path, null_markers, args.delimiter, args.encoding)

    ddl = build_create_table(args.table, columns, types)

    if args.dry_run:
        print("\n-- Inferred schema (dry run) --\n")
        print(ddl)
        print(
            f"\n-- {len(columns)} column(s) detected",
            file=sys.stdout,
        )
        col_width = max(len(c) for c in columns)
        for col, typ in zip(columns, types):
            print(f"  {col:<{col_width}}  {typ}")
        return

    # ---- Connect and create table ----------------------------------------
    conn = sqlite3.connect(args.db)
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")

    try:
        with conn:
            conn.execute(ddl)
    except sqlite3.Error as exc:
        print(f"error: could not create table: {exc}", file=sys.stderr)
        conn.close()
        sys.exit(1)

    print(f"Table {args.table!r} ready in {args.db}.", file=sys.stderr)

    # ---- Stream rows -----------------------------------------------------
    try:
        total = stream_insert(
            conn,
            args.table,
            csv_path,
            columns,
            types,
            null_markers,
            args.delimiter,
            args.encoding,
        )
    except sqlite3.Error as exc:
        print(f"\nerror during insert: {exc}", file=sys.stderr)
        conn.close()
        sys.exit(1)
    finally:
        conn.close()

    print(f"Done. {total:,} rows loaded into {args.db}::{args.table}.", file=sys.stderr)


if __name__ == "__main__":
    main()
