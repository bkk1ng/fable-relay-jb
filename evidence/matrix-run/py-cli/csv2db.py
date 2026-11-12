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
from datetime import datetime
from pathlib import Path


# ── Type inference ─────────────────────────────────────────────────────────────

DATE_FORMATS = (
    "%Y-%m-%d",
    "%Y/%m/%d",
    "%d-%m-%Y",
    "%d/%m/%Y",
    "%m-%d-%Y",
    "%m/%d/%Y",
    "%Y-%m-%dT%H:%M:%S",
    "%Y-%m-%d %H:%M:%S",
)


def _try_int(value: str) -> bool:
    try:
        int(value)
        return True
    except ValueError:
        return False


def _try_float(value: str) -> bool:
    try:
        float(value)
        return True
    except ValueError:
        return False


def _try_date(value: str) -> bool:
    for fmt in DATE_FORMATS:
        try:
            datetime.strptime(value, fmt)
            return True
        except ValueError:
            continue
    return False


def infer_column_type(values: list[str], null_markers: set[str]) -> str:
    """
    Infer the SQLite affinity for a column given a sample of its non-null string values.
    Priority: INTEGER > REAL > DATE (stored as TEXT ISO-8601) > TEXT.
    """
    non_null = [v for v in values if v not in null_markers]
    if not non_null:
        return "TEXT"

    if all(_try_int(v) for v in non_null):
        return "INTEGER"
    if all(_try_float(v) for v in non_null):
        return "REAL"
    if all(_try_date(v) for v in non_null):
        return "DATE"
    return "TEXT"


# ── Schema derivation ──────────────────────────────────────────────────────────

def derive_schema(
    csv_path: Path,
    table: str,
    null_markers: set[str],
    sample_rows: int = 500,
) -> dict[str, str]:
    """
    Read up to `sample_rows` rows and return an ordered dict of {column: sqlite_type}.
    """
    with csv_path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            raise ValueError("CSV file has no header row.")

        columns = list(reader.fieldnames)
        samples: dict[str, list[str]] = {col: [] for col in columns}

        for i, row in enumerate(reader):
            if i >= sample_rows:
                break
            for col in columns:
                samples[col].append(row.get(col, ""))

    return {col: infer_column_type(samples[col], null_markers) for col in columns}


# ── DDL / DML helpers ──────────────────────────────────────────────────────────

def build_create_sql(table: str, schema: dict[str, str]) -> str:
    col_defs = ",\n    ".join(
        f'"{col}" {dtype}' for col, dtype in schema.items()
    )
    return f'CREATE TABLE IF NOT EXISTS "{table}" (\n    {col_defs}\n);'


def build_insert_sql(table: str, columns: list[str]) -> str:
    col_list = ", ".join(f'"{c}"' for c in columns)
    placeholders = ", ".join("?" for _ in columns)
    return f'INSERT INTO "{table}" ({col_list}) VALUES ({placeholders});'


# ── Import logic ───────────────────────────────────────────────────────────────

def coerce_value(raw: str, dtype: str, null_markers: set[str]):
    """Convert a raw CSV string to the appropriate Python type for SQLite binding."""
    if raw in null_markers:
        return None
    if dtype == "INTEGER":
        return int(raw)
    if dtype == "REAL":
        return float(raw)
    # DATE and TEXT are stored as strings; SQLite handles the affinity.
    return raw


def stream_csv_to_db(
    csv_path: Path,
    db_path: Path,
    table: str,
    schema: dict[str, str],
    null_markers: set[str],
    batch_size: int = 500,
) -> int:
    """
    Stream the CSV into SQLite using batched transactions.
    Returns the total number of rows inserted.
    """
    columns = list(schema.keys())
    insert_sql = build_insert_sql(table, columns)

    total = 0
    batch: list[tuple] = []

    con = sqlite3.connect(db_path)
    try:
        con.execute(build_create_sql(table, schema))
        con.commit()

        with csv_path.open(newline="", encoding="utf-8-sig") as fh:
            reader = csv.DictReader(fh)

            for row in reader:
                record = tuple(
                    coerce_value(row.get(col, ""), dtype, null_markers)
                    for col, dtype in schema.items()
                )
                batch.append(record)
                total += 1

                if len(batch) >= batch_size:
                    con.executemany(insert_sql, batch)
                    con.commit()
                    batch.clear()
                    print(f"\r  {total:,} rows inserted…", end="", file=sys.stderr, flush=True)

        if batch:
            con.executemany(insert_sql, batch)
            con.commit()
            print(f"\r  {total:,} rows inserted…", end="", file=sys.stderr, flush=True)

    finally:
        con.close()

    return total


# ── CLI ────────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="csv2db",
        description="Stream a CSV file into a SQLite database with column type inference.",
    )
    p.add_argument("csv", metavar="CSV_FILE", type=Path, help="Path to the input CSV file.")
    p.add_argument("--db", metavar="DB_FILE", type=Path, required=True, help="Path to the SQLite database file.")
    p.add_argument("--table", metavar="TABLE", required=True, help="Destination table name.")
    p.add_argument(
        "--null",
        metavar="MARKER",
        nargs="+",
        default=["", "NULL", "N/A", "NA", "null", "n/a", "na", "none", "None"],
        help="String values to treat as SQL NULL (default: empty string and common null tokens).",
    )
    p.add_argument(
        "--batch-size",
        metavar="N",
        type=int,
        default=500,
        help="Rows per INSERT transaction (default: 500).",
    )
    p.add_argument(
        "--sample-rows",
        metavar="N",
        type=int,
        default=500,
        help="Rows to sample for type inference (default: 500).",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the inferred CREATE TABLE statement and exit without writing anything.",
    )
    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    csv_path: Path = args.csv
    db_path: Path = args.db
    table: str = args.table
    null_markers: set[str] = set(args.null)

    if not csv_path.exists():
        print(f"error: CSV file not found: {csv_path}", file=sys.stderr)
        return 1

    # ── Derive schema ──────────────────────────────────────────────────────────
    print(f"Sampling up to {args.sample_rows:,} rows for type inference…", file=sys.stderr)
    try:
        schema = derive_schema(csv_path, table, null_markers, sample_rows=args.sample_rows)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    create_sql = build_create_sql(table, schema)

    if args.dry_run:
        print("\n── Inferred schema ───────────────────────────────────────")
        print(create_sql)
        print("──────────────────────────────────────────────────────────")
        print("\nDry run complete. No data written.")
        return 0

    # ── Import ─────────────────────────────────────────────────────────────────
    print(f"Importing into {db_path} → table '{table}'…", file=sys.stderr)
    try:
        total = stream_csv_to_db(
            csv_path=csv_path,
            db_path=db_path,
            table=table,
            schema=schema,
            null_markers=null_markers,
            batch_size=args.batch_size,
        )
    except Exception as exc:
        print(f"\nerror: {exc}", file=sys.stderr)
        return 1

    print(f"\nDone. {total:,} rows inserted into '{table}'.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
