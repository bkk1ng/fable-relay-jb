#!/usr/bin/env python3
"""
csv2db.py — Stream a CSV file into a SQLite database with column type inference.

Usage:
    python csv2db.py input.csv --db output.db --table my_table
    python csv2db.py input.csv --db output.db --table my_table --dry-run
    python csv2db.py input.csv --db output.db --table my_table --null "" "N/A" "NULL"
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

# ISO 8601 and common date/datetime formats to probe
_DATE_FORMATS = (
    "%Y-%m-%d",
    "%Y/%m/%d",
    "%d-%m-%Y",
    "%d/%m/%Y",
    "%m/%d/%Y",
    "%Y-%m-%dT%H:%M:%S",
    "%Y-%m-%d %H:%M:%S",
    "%Y-%m-%dT%H:%M:%SZ",
)

_INT_RE = re.compile(r"^-?\d+$")
_FLOAT_RE = re.compile(r"^-?\d*\.\d+$|^-?\d+\.\d*$|^-?\d+[eE][+-]?\d+$")


def _is_integer(value: str) -> bool:
    return bool(_INT_RE.match(value.strip()))


def _is_float(value: str) -> bool:
    return bool(_FLOAT_RE.match(value.strip()))


def _is_date(value: str) -> bool:
    v = value.strip()
    for fmt in _DATE_FORMATS:
        try:
            datetime.strptime(v, fmt)
            return True
        except ValueError:
            pass
    return False


# SQLite affinities in preference order: INTEGER > REAL > TEXT (DATE stored as TEXT)
_AFFINITY_RANK = {"INTEGER": 0, "REAL": 1, "DATE": 2, "TEXT": 3}


def _infer_type(value: str) -> str:
    """Return the most specific SQLite type for a single non-null string value."""
    if _is_integer(value):
        return "INTEGER"
    if _is_float(value):
        return "REAL"
    if _is_date(value):
        return "DATE"
    return "TEXT"


def _merge_type(current: str, new_type: str) -> str:
    """
    Widen the inferred column type so it can accommodate all seen values.
    Widening order: INTEGER -> REAL -> DATE -> TEXT
    """
    if current == new_type:
        return current
    return max(current, new_type, key=lambda t: _AFFINITY_RANK[t])


def infer_column_types(
    path: Path,
    null_markers: set[str],
    encoding: str = "utf-8",
    sample_rows: int = 0,
) -> list[tuple[str, str]]:
    """
    Read the CSV header and up to `sample_rows` rows (0 = all) to infer column types.
    Returns a list of (column_name, sqlite_type) pairs.
    """
    with path.open(newline="", encoding=encoding) as fh:
        reader = csv.reader(fh)
        try:
            headers = next(reader)
        except StopIteration:
            return []

        # Start every column as INTEGER (most specific); widen as needed.
        # None means we haven't seen any non-null value yet.
        types: list[str | None] = [None] * len(headers)

        for i, row in enumerate(reader):
            if sample_rows and i >= sample_rows:
                break
            for col_idx, raw in enumerate(row):
                if col_idx >= len(types):
                    # Extra columns beyond the header — extend
                    types.append(None)
                    headers.append(f"col_{col_idx}")
                if raw in null_markers:
                    continue
                inferred = _infer_type(raw)
                if types[col_idx] is None:
                    types[col_idx] = inferred
                else:
                    types[col_idx] = _merge_type(types[col_idx], inferred)

    # Any column where every value was null defaults to TEXT
    final_types = [t if t is not None else "TEXT" for t in types]
    return list(zip(headers, final_types))


# ---------------------------------------------------------------------------
# Schema helpers
# ---------------------------------------------------------------------------

def _quoted(name: str) -> str:
    """Double-quote an identifier, escaping any embedded double-quotes."""
    return '"' + name.replace('"', '""') + '"'


def build_create_sql(table: str, columns: list[tuple[str, str]]) -> str:
    col_defs = ",\n    ".join(f"{_quoted(col)} {typ}" for col, typ in columns)
    return f"CREATE TABLE IF NOT EXISTS {_quoted(table)} (\n    {col_defs}\n);"


def build_insert_sql(table: str, columns: list[tuple[str, str]]) -> str:
    col_names = ", ".join(_quoted(col) for col, _ in columns)
    placeholders = ", ".join("?" * len(columns))
    return f"INSERT INTO {_quoted(table)} ({col_names}) VALUES ({placeholders});"


# ---------------------------------------------------------------------------
# Value coercion
# ---------------------------------------------------------------------------

def coerce(value: str, col_type: str, null_markers: set[str]):
    """Convert a raw CSV string to the appropriate Python value for SQLite."""
    if value in null_markers:
        return None
    if col_type == "INTEGER":
        try:
            return int(value.strip())
        except ValueError:
            return value
    if col_type == "REAL":
        try:
            return float(value.strip())
        except ValueError:
            return value
    # DATE and TEXT are stored as strings
    return value


# ---------------------------------------------------------------------------
# Main streaming import
# ---------------------------------------------------------------------------

def stream_csv_to_sqlite(
    csv_path: Path,
    db_path: Path,
    table: str,
    null_markers: set[str],
    batch_size: int,
    columns: list[tuple[str, str]],
    encoding: str = "utf-8",
) -> int:
    """
    Stream the CSV into SQLite using batched transactions.
    Returns the total number of rows inserted.
    """
    create_sql = build_create_sql(table, columns)
    insert_sql = build_insert_sql(table, columns)

    total = 0
    col_types = [t for _, t in columns]

    with sqlite3.connect(db_path) as conn:
        conn.execute(create_sql)
        conn.commit()

        with csv_path.open(newline="", encoding=encoding) as fh:
            reader = csv.reader(fh)
            next(reader)  # skip header — already consumed for type inference

            batch: list[tuple] = []

            def flush():
                nonlocal total
                conn.execute("BEGIN")
                conn.executemany(insert_sql, batch)
                conn.execute("COMMIT")
                total += len(batch)
                batch.clear()
                _report_progress(total)

            for row in reader:
                # Pad short rows, truncate long rows to match header width
                n = len(columns)
                padded = (row + [""] * n)[:n]
                values = tuple(
                    coerce(padded[i], col_types[i], null_markers)
                    for i in range(n)
                )
                batch.append(values)
                if len(batch) >= batch_size:
                    flush()

            if batch:
                flush()

    return total


# ---------------------------------------------------------------------------
# Progress reporting
# ---------------------------------------------------------------------------

def _report_progress(total: int) -> None:
    print(f"\rRows inserted: {total:,}", end="", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="csv2db",
        description="Stream a CSV file into a SQLite database with column type inference.",
    )
    parser.add_argument(
        "csv",
        metavar="CSV_FILE",
        help="Path to the input CSV file.",
    )
    parser.add_argument(
        "--db",
        required=True,
        metavar="DB_FILE",
        help="Path to the SQLite database file (created if absent).",
    )
    parser.add_argument(
        "--table",
        required=True,
        metavar="TABLE",
        help="Name of the destination table.",
    )
    parser.add_argument(
        "--null",
        nargs="+",
        default=["", "NULL", "null", "N/A", "n/a", "NA", "na", "None", "none"],
        metavar="MARKER",
        dest="null_markers",
        help="One or more strings to treat as NULL (default: empty string, NULL, N/A, etc.).",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=1000,
        metavar="N",
        dest="batch_size",
        help="Number of rows per INSERT transaction (default: 1000).",
    )
    parser.add_argument(
        "--encoding",
        default="utf-8",
        help="CSV file encoding (default: utf-8).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        dest="dry_run",
        help="Print the inferred schema and exit without writing to the database.",
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    csv_path = Path(args.csv)
    if not csv_path.is_file():
        print(f"error: CSV file not found: {csv_path}", file=sys.stderr)
        return 1

    null_markers: set[str] = set(args.null_markers)

    # Infer column types (full scan; use --batch-size as a proxy for sample if desired)
    print("Inferring column types…", file=sys.stderr)
    columns = infer_column_types(csv_path, null_markers, encoding=args.encoding)

    if not columns:
        print("error: CSV file is empty or has no headers.", file=sys.stderr)
        return 1

    create_sql = build_create_sql(args.table, columns)

    if args.dry_run:
        print("\n-- Inferred schema --")
        print(create_sql)
        print("\n-- Column mapping --")
        width = max(len(col) for col, _ in columns)
        for col, typ in columns:
            print(f"  {col:<{width}}  →  {typ}")
        return 0

    db_path = Path(args.db)
    print(f"Writing to {db_path} …", file=sys.stderr)

    total = stream_csv_to_sqlite(
        csv_path=csv_path,
        db_path=db_path,
        table=args.table,
        null_markers=null_markers,
        batch_size=args.batch_size,
        columns=columns,
        encoding=args.encoding,
    )

    print(f"\nDone. {total:,} rows inserted into '{args.table}'.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
