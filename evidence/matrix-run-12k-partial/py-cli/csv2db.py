#!/usr/bin/env python3
"""csv2db.py — Stream a CSV file into a SQLite database with type inference."""

import argparse
import csv
import re
import sqlite3
import sys
from datetime import datetime
from typing import Any

# ---------------------------------------------------------------------------
# Type inference
# ---------------------------------------------------------------------------

# ISO-8601 date / datetime patterns we recognise
_DATE_PATTERNS = [
    re.compile(r"^\d{4}-\d{2}-\d{2}$"),
    re.compile(r"^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}(:\d{2})?$"),
    re.compile(r"^\d{2}/\d{2}/\d{4}$"),
    re.compile(r"^\d{2}-\d{2}-\d{4}$"),
]


def _looks_like_date(value: str) -> bool:
    for pat in _DATE_PATTERNS:
        if pat.match(value):
            return True
    return False


def infer_type(value: str) -> str:
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
    if _looks_like_date(value):
        return "date"
    return "text"


def _type_rank(a: str, b: str) -> str:
    """
    Merge two inferred column types so the result can hold both.

    Promotion ladder (most specific → least): integer → real → date → text
    date and integer/real don't mix cleanly, so promote to text.
    """
    if a == b:
        return a
    # Any unknown pairing falls through to text
    pairs = {frozenset({"integer", "real"}): "real"}
    merged = pairs.get(frozenset({a, b}))
    return merged if merged else "text"


# SQLite affinities for our inferred types
_AFFINITY = {
    "integer": "INTEGER",
    "real":    "REAL",
    "date":    "TEXT",   # SQLite has no native DATE type
    "text":    "TEXT",
}


# ---------------------------------------------------------------------------
# Schema inference (single-pass over sample rows, then reset)
# ---------------------------------------------------------------------------

def infer_schema(
    reader: csv.DictReader,
    null_markers: set[str],
    sample: int = 500,
) -> dict[str, str]:
    """
    Read up to *sample* rows and return {column: inferred_type}.
    Returns row buffer so callers can replay it without seeking.
    """
    col_types: dict[str, str] = {}
    buffered: list[dict[str, str]] = []

    for i, row in enumerate(reader):
        if i >= sample:
            break
        buffered.append(row)
        for col, raw in row.items():
            value = "" if raw in null_markers else raw.strip()
            t = infer_type(value) if value else "text"
            if col not in col_types:
                col_types[col] = t
            else:
                col_types[col] = _type_rank(col_types[col], t)

    return col_types, buffered


# ---------------------------------------------------------------------------
# Value coercion
# ---------------------------------------------------------------------------

def coerce(raw: str, col_type: str, null_markers: set[str]) -> Any:
    """Convert a raw CSV string to the appropriate Python type for SQLite."""
    if raw in null_markers or raw.strip() == "":
        return None
    value = raw.strip()
    if col_type == "integer":
        try:
            return int(value)
        except ValueError:
            return value
    if col_type == "real":
        try:
            return float(value)
        except ValueError:
            return value
    # date and text are stored as strings
    return value


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="csv2db",
        description="Stream a CSV file into a SQLite database with column type inference.",
    )
    p.add_argument("csv_file", help="Path to the input CSV file (use '-' for stdin)")
    p.add_argument("--db",    required=True,  help="Path to the SQLite database file")
    p.add_argument("--table", required=True,  help="Destination table name")
    p.add_argument(
        "--null",
        dest="null_markers",
        metavar="MARKER",
        action="append",
        default=["", "NULL", "null", "NA", "N/A", "n/a", "none", "None"],
        help="String values treated as NULL (may be given multiple times; "
             "defaults: empty string, NULL, NA, N/A, none)",
    )
    p.add_argument(
        "--batch-size",
        type=int,
        default=1000,
        metavar="N",
        help="Rows per INSERT transaction (default: 1000)",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the inferred CREATE TABLE statement and exit without writing",
    )
    p.add_argument(
        "--encoding",
        default="utf-8-sig",
        help="CSV file encoding (default: utf-8-sig, strips BOM if present)",
    )
    p.add_argument(
        "--delimiter",
        default=",",
        help="CSV delimiter character (default: ',')",
    )
    return p


# ---------------------------------------------------------------------------
# Core logic
# ---------------------------------------------------------------------------

def _quote(name: str) -> str:
    """Double-quote an identifier, escaping any embedded double-quotes."""
    return '"' + name.replace('"', '""') + '"'


def build_create_sql(table: str, col_types: dict[str, str]) -> str:
    cols = ",\n    ".join(
        f"{_quote(col)} {_affinity}" for col, _affinity in
        ((_c, _AFFINITY[_t]) for _c, _t in col_types.items())
    )
    return f"CREATE TABLE IF NOT EXISTS {_quote(table)} (\n    {cols}\n);"


def run(args: argparse.Namespace) -> None:
    null_markers: set[str] = set(args.null_markers)

    # Open source
    if args.csv_file == "-":
        src = sys.stdin
    else:
        src = open(args.csv_file, encoding=args.encoding, newline="")

    try:
        reader = csv.DictReader(src, delimiter=args.delimiter)

        # --- Phase 1: infer schema from first N rows -------------------------
        col_types, buffered_rows = infer_schema(reader, null_markers)

        if not col_types:
            print("csv2db: error: CSV has no columns.", file=sys.stderr)
            sys.exit(1)

        create_sql = build_create_sql(args.table, col_types)

        if args.dry_run:
            print(create_sql)
            return

        # --- Phase 2: open DB, create table, insert --------------------------
        con = sqlite3.connect(args.db)
        con.execute("PRAGMA journal_mode=WAL;")
        con.execute("PRAGMA synchronous=NORMAL;")

        with con:
            con.execute(create_sql)

        columns = list(col_types.keys())
        placeholders = ", ".join("?" * len(columns))
        quoted_cols = ", ".join(_quote(c) for c in columns)
        insert_sql = (
            f"INSERT INTO {_quote(args.table)} ({quoted_cols}) "
            f"VALUES ({placeholders})"
        )

        def _row_to_tuple(row: dict[str, str]) -> tuple:
            return tuple(
                coerce(row.get(c, ""), col_types[c], null_markers)
                for c in columns
            )

        total = 0
        batch: list[tuple] = []

        def flush(batch: list[tuple], con: sqlite3.Connection) -> None:
            with con:
                con.executemany(insert_sql, batch)

        def _progress(total: int) -> None:
            print(f"\rcsv2db: {total:,} rows inserted", end="", file=sys.stderr)

        # Insert buffered rows first
        for row in buffered_rows:
            batch.append(_row_to_tuple(row))
            total += 1
            if len(batch) >= args.batch_size:
                flush(batch, con)
                batch.clear()
                _progress(total)

        # Stream the rest
        for row in reader:
            batch.append(_row_to_tuple(row))
            total += 1
            if len(batch) >= args.batch_size:
                flush(batch, con)
                batch.clear()
                _progress(total)

        # Final partial batch
        if batch:
            flush(batch, con)
            _progress(total)

        print(file=sys.stderr)  # newline after progress
        print(
            f"csv2db: done — {total:,} rows → table '{args.table}' in '{args.db}'",
            file=sys.stderr,
        )

        con.close()

    finally:
        if src is not sys.stdin:
            src.close()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    run(args)


if __name__ == "__main__":
    main()
