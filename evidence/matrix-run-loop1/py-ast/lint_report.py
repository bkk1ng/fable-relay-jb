#!/usr/bin/env python3
"""lint_report.py — static lint report for Python source files.

Checks performed
----------------
E001  bare except clause
W001  unused import
W002  function longer than --max-lines
W003  function has more than --max-args parameters
I001  function missing docstring
I002  module missing docstring
I003  class missing docstring

Usage
-----
    python lint_report.py path/to/file.py
    python lint_report.py path/to/file.py --max-lines 40 --max-args 4 --strict
"""

import ast
import argparse
import sys
from pathlib import Path
from typing import NamedTuple

# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

# Lower value = higher severity; used for sort key
_SEVERITY_RANK: dict[str, int] = {"ERROR": 0, "WARNING": 1, "INFO": 2}


class Finding(NamedTuple):
    severity: str  # ERROR | WARNING | INFO
    line: int
    code: str
    message: str


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _has_docstring(node: ast.AST) -> bool:
    """Return True when *node*'s body begins with a string-literal expression."""
    body = getattr(node, "body", [])
    if not body:
        return False
    first = body[0]
    return (
        isinstance(first, ast.Expr)
        and isinstance(first.value, ast.Constant)
        and isinstance(first.value.value, str)
    )


# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------

def check_bare_excepts(tree: ast.AST) -> list[Finding]:
    """E001 — except clauses with no exception type specified."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ExceptHandler) and node.type is None:
            findings.append(Finding(
                severity="ERROR",
                line=node.lineno,
                code="E001",
                message="bare except catches everything including SystemExit and KeyboardInterrupt",
            ))
    return findings


def check_unused_imports(tree: ast.AST) -> list[Finding]:
    """W001 — imported names that are never referenced in the module body."""
    # bound_name -> (human-readable import text, line number)
    imported: dict[str, tuple[str, int]] = {}

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                # `import os.path` binds the name `os`
                bound = alias.asname if alias.asname else alias.name.split(".")[0]
                label = f"import {alias.name}"
                if alias.asname:
                    label += f" as {alias.asname}"
                imported[bound] = (label, node.lineno)

        elif isinstance(node, ast.ImportFrom):
            # Skip `from __future__ import ...` — those never appear as Name nodes
            if node.module == "__future__":
                continue
            for alias in node.names:
                if alias.name == "*":
                    continue  # star imports are untrackable
                bound = alias.asname if alias.asname else alias.name
                module = node.module or ""
                label = f"from {module} import {alias.name}"
                if alias.asname:
                    label += f" as {alias.asname}"
                imported[bound] = (label, node.lineno)

    # Collect every Name reference in the tree.
    # `import os; os.path.join()` produces Name(id='os') as the root of the
    # Attribute chain, so simple Name walking is sufficient.
    used: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Name):
            used.add(node.id)

    # Treat string literals inside `__all__ = [...]` as references so that
    # re-exported names are not flagged.
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Assign)
            and any(
                isinstance(t, ast.Name) and t.id == "__all__"
                for t in node.targets
            )
            and isinstance(node.value, (ast.List, ast.Tuple))
        ):
            for elt in node.value.elts:
                if isinstance(elt, ast.Constant) and isinstance(elt.value, str):
                    used.add(elt.value)

    findings: list[Finding] = []
    for bound, (label, lineno) in imported.items():
        if bound not in used:
            findings.append(Finding(
                severity="WARNING",
                line=lineno,
                code="W001",
                message=f"unused import: {label}",
            ))
    return findings


def check_functions(
    tree: ast.AST,
    max_lines: int,
    max_args: int,
) -> list[Finding]:
    """W002 / W003 / I001 — length, argument count, and docstring checks."""
    findings: list[Finding] = []
    func_types = (ast.FunctionDef, ast.AsyncFunctionDef)

    for node in ast.walk(tree):
        if not isinstance(node, func_types):
            continue

        name: str = node.name
        lineno: int = node.lineno

        # W002 — function too long
        end_line: int | None = getattr(node, "end_lineno", None)
        if end_line is not None:
            length = end_line - lineno + 1
            if length > max_lines:
                findings.append(Finding(
                    severity="WARNING",
                    line=lineno,
                    code="W002",
                    message=(
                        f"function '{name}' is {length} lines long "
                        f"(limit: {max_lines})"
                    ),
                ))

        # W003 — too many parameters
        # Count positional-only + regular + keyword-only; exclude *args / **kwargs
        total_args = (
            len(node.args.posonlyargs)
            + len(node.args.args)
            + len(node.args.kwonlyargs)
        )
        if total_args > max_args:
            findings.append(Finding(
                severity="WARNING",
                line=lineno,
                code="W003",
                message=(
                    f"function '{name}' has {total_args} parameters "
                    f"(limit: {max_args})"
                ),
            ))

        # I001 — missing docstring
        if not _has_docstring(node):
            findings.append(Finding(
                severity="INFO",
                line=lineno,
                code="I001",
                message=f"function '{name}' is missing a docstring",
            ))

    return findings


def check_module_docstring(tree: ast.AST) -> list[Finding]:
    """I002 — module-level docstring absent."""
    if not _has_docstring(tree):
        return [Finding(
            severity="INFO",
            line=1,
            code="I002",
            message="module is missing a docstring",
        )]
    return []


def check_class_docstrings(tree: ast.AST) -> list[Finding]:
    """I003 — class body does not begin with a docstring."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and not _has_docstring(node):
            findings.append(Finding(
                severity="INFO",
                line=node.lineno,
                code="I003",
                message=f"class '{node.name}' is missing a docstring",
            ))
    return findings


# ---------------------------------------------------------------------------
# Table formatting
# ---------------------------------------------------------------------------

def _render_table(findings: list[Finding], source_path: Path) -> str:
    """Return a fixed-width table of findings as a string."""
    if not findings:
        return f"  {source_path}: no findings\n"

    headers = ("LINE", "SEVERITY", "CODE", "MESSAGE")
    rows: list[tuple[str, str, str, str]] = [
        (str(f.line), f.severity, f.code, f.message) for f in findings
    ]

    # Column widths — at least as wide as the header
    col_widths = [
        max(len(headers[i]), max(len(r[i]) for r in rows))
        for i in range(len(headers))
    ]
    # MESSAGE column needs no right-pad; just use header width as minimum
    col_widths[3] = max(len(headers[3]), max(len(r[3]) for r in rows))

    def _row(cells: tuple[str, ...], right_align_first: bool = False) -> str:
        parts = []
        for i, (cell, width) in enumerate(zip(cells, col_widths)):
            if i == 0 and right_align_first:
                parts.append(cell.rjust(width))
            else:
                parts.append(cell.ljust(width))
        return "  ".join(parts).rstrip()

    separator = "  ".join("-" * w for w in col_widths).rstrip()

    lines = [
        "",
        f"  lint report — {source_path}",
        "",
        "  " + _row(headers),
        "  " + separator,
    ]
    for row in rows:
        lines.append("  " + _row(row, right_align_first=True))
    lines.append("")

    # Summary counts
    counts = {s: sum(1 for f in findings if f.severity == s) for s in _SEVERITY_RANK}
    lines.append(
        f"  {counts['ERROR']} error(s)  "
        f"{counts['WARNING']} warning(s)  "
        f"{counts['INFO']} info"
    )
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Static lint report: unused imports, long functions, "
                    "excess arguments, missing docstrings, bare excepts.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "file",
        type=Path,
        help="Python source file to analyse",
    )
    parser.add_argument(
        "--max-lines",
        type=int,
        default=50,
        metavar="N",
        help="flag functions longer than N lines",
    )
    parser.add_argument(
        "--max-args",
        type=int,
        default=5,
        metavar="M",
        help="flag functions with more than M parameters",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 if any finding is present",
    )
    args = parser.parse_args()

    path: Path = args.file
    if not path.exists():
        print(f"error: file not found: {path}", file=sys.stderr)
        sys.exit(2)

    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"error: cannot read {path}: {exc}", file=sys.stderr)
        sys.exit(2)

    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as exc:
        print(f"error: syntax error in {path}: {exc}", file=sys.stderr)
        sys.exit(2)

    findings: list[Finding] = []
    findings += check_bare_excepts(tree)
    findings += check_unused_imports(tree)
    findings += check_functions(tree, args.max_lines, args.max_args)
    findings += check_module_docstring(tree)
    findings += check_class_docstrings(tree)

    # Sort by severity (ERROR first) then by line number
    findings.sort(key=lambda f: (_SEVERITY_RANK[f.severity], f.line))

    print(_render_table(findings, path), end="")

    if args.strict and findings:
        sys.exit(1)


if __name__ == "__main__":
    main()
