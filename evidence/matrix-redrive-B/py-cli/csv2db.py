#!/usr/bin/env python3
"""
csv2db.py — Stream a CSV file into a SQLite database with column type inference.

Usage:
    python csv2db.py input.csv --db out.db --table mytable
    python csv2db.py input.csv --db out.db --table mytable --dry-run
    python csv2db.py input.csv --db out.db --table mytable --null-markers "" "NULL" "N/A"
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

# ISO 8601 and common date/datetime patterns
_DATE_PATTERNS = [
    r"^\d{4}-\d{2}-\d{2}$",                        # 2024-01-15
    r"^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}(:\d{2})?$",  # 2024-01-15T12:30 / with seconds
    r"^\d{2}/\d{2}/\d{4}$",                        # 01/15/2024
    r"^\d{2}-\d{2}-\d{4}$",                        # 01-15-2024
]
_DATE_RE = [re.compile(p) for p in _DATE_PATTERNS]


def _is_date(value: str) -> bool:
    for pat in _DATE_RE:
        if pat.match(value):
            return True
    return False


def _infer_type(value: str) -> str:
    """Return 'integer', 'real', 'date', or 'text' for a single string value."""
    if not value:
        return "text"
    try:
        int(value)
        return "integer"
    except ValueError:
        pass
    try:
        float(value)
        return "real"
    except ValueError:
        pass
    if _is_date(value):
        return "date"
    return "text"


# Type precedence when merging across rows (wider type wins)
_PRECEDENCE = {"integer": 0, "date": 1, "real": 2, "text": 3}


def _merge_types(a: str, b: str) -> str:
    return a if _PRECEDENCE[a] >= _PRECEDENCE[b] else b


def _sqlite_affinity(inferred: str) -> str:
    """Map our inferred type to a SQLite column type declaration."""
    return {
        "integer": "INTEGER",
        "real":    "REAL",
        "date":    "TEXT",   # SQLite has no native DATE; store as ISO text
        "text":    "TEXT",
    }[inferred]


# ---------------------------------------------------------------------------
# Schema inference (single-pass over a sample of rows)
# ---------------------------------------------------------------------------

def infer_schema(
    csv_path: str,
    null_markers: set[str],
    delimiter: str = ",",
    sample_rows: int = 500,
) -> tuple[list[str], list[str]]:
    """
    Read up to `sample_rows` data rows and return (column_names, inferred_types).
    Types are the widest seen across the sample; a column that is entirely
    null-markers stays 'text'.
    """
    col_types: list[str] = []
    col_names: list[str] = []

    with open(csv_path, newline="", encoding="utf-8-sig") as fh:
        reader = csv.reader(fh, delimiter=delimiter)
        try:
            col_names = next(reader)
        except StopIteration:
            return [], []

        col_types = ["integer"] * len(col_names)  # start optimistic

        for i, row in enumerate(reader):
            if i >= sample_rows:
                break
            # Pad short rows, truncate long rows
            row = (row + [""] * len(col_names))[: len(col_names)]
            for j, cell in enumerate(row):
                if cell in null_markers:
                    continue
                col_types[j] = _merge_types(col_types[j], _infer_type(cell))

    return col_names, col_types


# ---------------------------------------------------------------------------
# DDL helpers
# ---------------------------------------------------------------------------

def _sanitize_name(name: str) -> str:
    """Quote an identifier for SQLite."""
    return '"' + name.replace('"', '""') + '"'


def build_create_sql(table: str, col_names: list[str], col_types: list[str]) -> str:
    cols = ",\n    ".join(
        f"{_sanitize_name(n)} {_sqlite_affinity(t)}"
        for n, t in zip(col_names, col_types)
    )
    return f"CREATE TABLE IF NOT EXISTS {_sanitize_name(table)} (\n    {cols}\n);"


# ---------------------------------------------------------------------------
# Value coercion
# ---------------------------------------------------------------------------

def _coerce(value: str, col_type: str, null_markers: set[str]):
    """Return a Python value ready for sqlite3 parameter binding."""
    if value in null_markers:
        return None
    if col_type == "integer":
        try:
            return int(value)
        except ValueError:
            return value or None
    if col_type == "real":
        try:
            return float(value)
        except ValueError:
            return value or None
    # date and text: store as-is
    return value or None


# ---------------------------------------------------------------------------
# Core import
# ---------------------------------------------------------------------------

def import_csv(
    csv_path: str,
    db_path: str,
    table: str,
    col_names: list[str],
    col_types: list[str],
    null_markers: set[str],
    batch_size: int = 500,
    delimiter: str = ",",
) -> int:
    """Stream CSV into SQLite in batched transactions. Returns total rows inserted."""
    placeholders = ", ".join("?" * len(col_names))
    insert_sql = (
        f"INSERT INTO {_sanitize_name(table)} "
        f"({', '.join(_sanitize_name(n) for n in col_names)}) "
        f"VALUES ({placeholders})"
    )

    total = 0
    batch: list[tuple] = []

    con = sqlite3.connect(db_path)
    try:
        with con:
            con.executescript(build_create_sql(table, col_names, col_types))

        with open(csv_path, newline="", encoding="utf-8-sig") as fh:
            reader = csv.reader(fh, delimiter=delimiter)
            next(reader)  # skip header

            for row in reader:
                row = (row + [""] * len(col_names))[: len(col_names)]
                values = tuple(
                    _coerce(cell, col_types[j], null_markers)
                    for j, cell in enumerate(row)
                )
                batch.append(values)

                if len(batch) >= batch_size:
                    with con:
                        con.executemany(insert_sql, batch)
                    total += len(batch)
                    _progress(total)
                    batch.clear()

            if batch:
                with con:
                    con.executemany(insert_sql, batch)
                total += len(batch)
                _progress(total)

    finally:
        con.close()

    return total


# ---------------------------------------------------------------------------
# Progress reporting
# ---------------------------------------------------------------------------

def _progress(total: int) -> None:
    print(f"\r  rows inserted: {total:,}", end="", flush=True, file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="csv2db",
        description="Stream a CSV file into a SQLite database with type inference.",
    )
    p.add_argument("csv", metavar="CSV", help="Path to the input CSV file.")
    p.add_argument("--db", required=True, help="Path to the SQLite database file.")
    p.add_argument("--table", required=True, help="Target table name.")
    p.add_argument(
        "--null-markers",
        nargs="*",
        default=["", "NULL", "null", "N/A", "n/a", "NA", "na", "\\N"],
        metavar="MARKER",
        help=(
            "One or more cell values to treat as SQL NULL. "
            "Defaults: empty string, NULL, null, N/A, n/a, NA, na, \\N"
        ),
    )
    p.add_argument(
        "--delimiter",
        default=",",
        help="CSV field delimiter (default: comma).",
    )
    p.add_argument(
        "--batch-size",
        type=int,
        default=500,
        metavar="N",
        help="Rows per INSERT transaction (default: 500).",
    )
    p.add_argument(
        "--sample-rows",
        type=int,
        default=500,
        metavar="N",
        help="Rows sampled for type inference (default: 500).",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the inferred CREATE TABLE statement and exit without writing.",
    )
    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    csv_path = args.csv
    if not Path(csv_path).is_file():
        print(f"error: file not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    null_markers: set[str] = set(args.null_markers)

    print(f"Inferring schema from up to {args.sample_rows} rows …", file=sys.stderr)
    col_names, col_types = infer_schema(
        csv_path,
        null_markers=null_markers,
        delimiter=args.delimiter,
        sample_rows=args.sample_rows,
    )

    if not col_names:
        print("error: CSV appears to be empty or has no header row.", file=sys.stderr)
        sys.exit(1)

    ddl = build_create_sql(args.table, col_names, col_types)

    if args.dry_run:
        print("\n-- Dry run: inferred schema --\n")
        print(ddl)
        print("\n-- Column type mapping --")
        width = max(len(n) for n in col_names)
        for name, typ in zip(col_names, col_types):
            print(f"  {name:<{width}}  →  {_sqlite_affinity(typ)}")
        return

    print(f"Importing into {args.db} / table '{args.table}' …", file=sys.stderr)
    total = import_csv(
        csv_path=csv_path,
        db_path=args.db,
        table=args.table,
        col_names=col_names,
        col_types=col_types,
        null_markers=null_markers,
        batch_size=args.batch_size,
        delimiter=args.delimiter,
    )

    # Final newline after the progress line
    print(file=sys.stderr)
    print(f"Done. {total:,} rows imported.", file=sys.stderr)


if __name__ == "__main__":
    main()
