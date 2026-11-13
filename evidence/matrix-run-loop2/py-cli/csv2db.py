#!/usr/bin/env python3
"""csv2db.py — Stream a CSV into SQLite with column type inference.

Python 3.12, stdlib only.

Type inference scans the first N rows (configurable via --sample-size) and
widens each column from int -> float -> date -> text as values demand. Null
markers (--null, repeatable; empty string is always NULL) become SQL NULL.

Streaming design:
  Phase 1  Read the header + up to --sample-size rows into memory, infer
           column types, and (if not --dry-run) create the table.
  Phase 2  Insert the buffered sample rows, then keep iterating the same
           csv.reader for the rest of the file, batch-inserting every
           --batch-size rows inside an explicit transaction.

Only the sample (bounded by --sample-size) and the current batch (bounded by
--batch-size) are held in memory at once.

Examples:
    python csv2db.py data.csv
    python csv2db.py data.csv --table sales --db sales.db --null '' --null N/A
    python csv2db.py data.csv --dry-run
    python csv2db.py big.csv --batch-size 5000 --sample-size 50000
"""

import argparse
import csv
import re
import sqlite3
import sys
from datetime import datetime
from pathlib import Path


# ── Type inference ──────────────────────────────────────────────────────

_INT_RE = re.compile(r'^[+-]?\d+$')

# Common date/datetime formats. Order matters only for speed, not correctness,
# since a value either parses under exactly one or none of them.
_DATE_FORMATS = [
    '%Y-%m-%d',
    '%Y/%m/%d',
    '%m/%d/%Y',
    '%d/%m/%Y',
    '%d-%m-%Y',
    '%Y-%m-%dT%H:%M:%S',
    '%Y-%m-%d %H:%M:%S',
    '%Y-%m-%d %H:%M',
    '%m/%d/%Y %I:%M:%S %p',
]

# Widening order — a column starts at int and can only move right.
_RANK = {'int': 0, 'float': 1, 'date': 2, 'text': 3}
_SQL_TYPE = {'int': 'INTEGER', 'float': 'REAL', 'date': 'TEXT', 'text': 'TEXT'}


def _is_int(val: str) -> bool:
    return bool(_INT_RE.match(val))


def _is_float(val: str) -> bool:
    try:
        float(val)
        return True
    except ValueError:
        return False


def _is_date(val: str) -> bool:
    for fmt in _DATE_FORMATS:
        try:
            datetime.strptime(val, fmt)
            return True
        except ValueError:
            continue
    return False


def _classify(val: str, nulls: set[str]) -> str | None:
    """Return the narrowest type a value fits, or None for null markers."""
    if val in nulls:
        return None
    if _is_int(val):
        return 'int'
    if _is_float(val):
        return 'float'
    if _is_date(val):
        return 'date'
    return 'text'


def infer_types(header: list[str], sample: list[list[str]],
                nulls: set[str]) -> list[str]:
    """Infer a logical type for each column from sampled rows.

    Each column starts as 'int' (narrowest) and widens to float, date, or text
    as soon as a value demands it. Columns that contained only null markers
    default to 'text'.
    """
    ncols = len(header)
    if not sample:
        return ['text'] * ncols

    col_types = ['int'] * ncols
    seen = [False] * ncols

    for row in sample:
        for i in range(min(len(row), ncols)):
            vt = _classify(row[i], nulls)
            if vt is None:
                continue
            seen[i] = True
            if _RANK[vt] > _RANK[col_types[i]]:
                col_types[i] = vt

    return [t if s else 'text' for t, s in zip(col_types, seen)]


# ── Value conversion ───────────────────────────────────────────────────

def convert_row(row: list[str], col_types: list[str],
                nulls: set[str]) -> tuple:
    """Convert a CSV row to a tuple for SQLite, applying types and nulls.

    Short rows are padded with None. Extra trailing values are dropped.
    If inference was wrong for a later row (e.g. a non-int in an int column),
    the original string is kept — SQLite's dynamic typing handles it.
    """
    result: list = []
    for i in range(len(col_types)):
        if i >= len(row):
            result.append(None)
            continue
        val = row[i]
        if val in nulls:
            result.append(None)
            continue
        lt = col_types[i]
        if lt == 'int':
            try:
                result.append(int(val))
            except ValueError:
                result.append(val)
        elif lt == 'float':
            try:
                result.append(float(val))
            except ValueError:
                result.append(val)
        else:
            result.append(val)
    return tuple(result)


# ── Schema helpers ─────────────────────────────────────────────────────

def build_create_sql(table: str, header: list[str],
                     col_types: list[str]) -> str:
    cols = ',\n  '.join(
        f'"{name}" {_SQL_TYPE[lt]}' for name, lt in zip(header, col_types)
    )
    return f'CREATE TABLE "{table}" (\n  {cols}\n)'


def print_schema(table: str, header: list[str], col_types: list[str],
                 n_sampled: int) -> None:
    """Print the inferred CREATE TABLE plus a column-type summary to stdout."""
    print(build_create_sql(table, header, col_types))
    print()
    print(f'-- table: {table}')
    print(f'-- columns: {len(header)}')
    print(f'-- rows sampled: {n_sampled}')
    print('--')
    print('-- column types:')
    for name, lt in zip(header, col_types):
        print(f'--   {name:<32} {_SQL_TYPE[lt]:<8}  ({lt})')


# ── Import ──────────────────────────────────────────────────────────────

def run(csv_path: Path, table: str, db_path: str | None,
        nulls: set[str], batch_size: int, dry_run: bool,
        sample_size: int) -> None:
    """Two-phase streaming import: sample -> infer -> create -> stream insert."""

    with open(csv_path, newline='', encoding='utf-8') as f:
        reader = csv.reader(f)

        try:
            header = next(reader)
        except StopIteration:
            sys.stderr.write('error: CSV is empty\n')
            sys.exit(1)

        # ── Phase 1: buffer sample rows for inference ──
        sample: list[list[str]] = []
        for _ in range(sample_size):
            try:
                sample.append(next(reader))
            except StopIteration:
                break

        col_types = infer_types(header, sample, nulls)

        if dry_run:
            print_schema(table, header, col_types, len(sample))
            return

        # ── Phase 2: create table, then stream inserts ──
        if db_path is None:
            db_path = str(csv_path.with_suffix('.db'))

        conn = sqlite3.connect(db_path)
        try:
            conn.execute(f'DROP TABLE IF EXISTS "{table}"')
            conn.execute(build_create_sql(table, header, col_types))
            conn.commit()

            placeholders = ', '.join('?' * len(header))
            insert_sql = f'INSERT INTO "{table}" VALUES ({placeholders})'

            batch: list[tuple] = []
            total = 0

            def flush() -> None:
                nonlocal total
                if not batch:
                    return
                conn.execute('BEGIN')
                conn.executemany(insert_sql, batch)
                conn.commit()
                total += len(batch)
                batch.clear()
                sys.stderr.write(f'\rinserted {total:>10,} rows')
                sys.stderr.flush()

            # Insert the sampled rows that are already in memory.
            for row in sample:
                batch.append(convert_row(row, col_types, nulls))
                if len(batch) >= batch_size:
                    flush()

            # Continue streaming the rest of the file — same reader, so we
            # pick up exactly where the sampling loop left off.
            for row in reader:
                batch.append(convert_row(row, col_types, nulls))
                if len(batch) >= batch_size:
                    flush()

            flush()
        finally:
            conn.close()

        sys.stderr.write(f'\rinserted {total:>10,} rows into "{table}"\n')
        sys.stderr.write(f'database: {db_path}\n')


# ── CLI ─────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description='Stream a CSV into SQLite with column type inference.',
    )
    parser.add_argument('csv', type=Path, help='CSV file to import')
    parser.add_argument(
        '--table', default=None,
        help='table name (default: CSV filename stem)',
    )
    parser.add_argument(
        '--db', default=None,
        help='database file (default: <csv_stem>.db)',
    )
    parser.add_argument(
        '--null', action='append', default=[], metavar='MARKER',
        help='value to treat as NULL (repeatable; empty string is always NULL)',
    )
    parser.add_argument(
        '--batch-size', type=int, default=1000,
        help='rows per transaction (default: 1000)',
    )
    parser.add_argument(
        '--sample-size', type=int, default=10000,
        help='rows to scan for type inference (default: 10000)',
    )
    parser.add_argument(
        '--dry-run', action='store_true',
        help='print inferred schema and exit without writing',
    )

    args = parser.parse_args()

    if not args.csv.exists():
        sys.stderr.write(f'error: file not found: {args.csv}\n')
        sys.exit(1)

    table = args.table or args.csv.stem
    nulls = set(args.null) | {''}

    run(
        csv_path=args.csv,
        table=table,
        db_path=args.db,
        nulls=nulls,
        batch_size=args.batch_size,
        dry_run=args.dry_run,
        sample_size=args.sample_size,
    )


if __name__ == '__main__':
    main()
