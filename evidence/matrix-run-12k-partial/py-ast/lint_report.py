#!/usr/bin/env python3
"""lint_report.py — static analysis reporter for Python source files.

Checks performed
----------------
  unused-import   : imported name never referenced in the module body
  too-long        : function body exceeds --max-lines lines
  too-many-args   : function signature exceeds --max-args parameters
  missing-docstring: module / class / function has no docstring
  bare-except     : bare ``except:`` with no exception type specified

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

SEVERITY_RANK: dict[str, int] = {"ERROR": 0, "WARNING": 1, "INFO": 2}


class Finding(NamedTuple):
    line: int
    severity: str   # "ERROR" | "WARNING" | "INFO"
    rule: str
    message: str


# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------

def _collect_imported_names(tree: ast.Module) -> dict[str, int]:
    """Map each name bound by an import statement to its line number.

    ``import a.b.c``        binds ``a``
    ``import a.b.c as x``   binds ``x``
    ``from m import y``     binds ``y``
    ``from m import y as z``binds ``z``
    ``from m import *``     skipped — star imports are assumed used
    """
    names: dict[str, int] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                bound = alias.asname if alias.asname else alias.name.split(".")[0]
                names[bound] = node.lineno
        elif isinstance(node, ast.ImportFrom):
            if any(a.name == "*" for a in node.names):
                continue
            for alias in node.names:
                bound = alias.asname if alias.asname else alias.name
                names[bound] = node.lineno
    return names


def _collect_used_names(tree: ast.Module) -> set[str]:
    """Return every name referenced in a Load context anywhere in the tree.

    Also extracts string literals from ``__all__`` assignments so that
    names exported via ``__all__`` are not reported as unused.
    """
    used: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load):
            used.add(node.id)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "__all__":
                    if isinstance(node.value, (ast.List, ast.Tuple)):
                        for elt in node.value.elts:
                            if isinstance(elt, ast.Constant) and isinstance(elt.value, str):
                                used.add(elt.value)
    return used


def check_unused_imports(tree: ast.Module) -> list[Finding]:
    """Warn about imported names that are never referenced.

    Note: imports used only in string-form type annotations (e.g. under
    ``from __future__ import annotations``) may be reported as unused.
    """
    imported = _collect_imported_names(tree)
    used = _collect_used_names(tree)
    findings: list[Finding] = []
    for name, line in sorted(imported.items(), key=lambda kv: kv[1]):
        if name not in used:
            findings.append(Finding(
                line=line,
                severity="WARNING",
                rule="unused-import",
                message=f"'{name}' is imported but never used",
            ))
    return findings


def _has_docstring(node: ast.FunctionDef | ast.AsyncFunctionDef
                        | ast.ClassDef | ast.Module) -> bool:
    """Return True when the node's first statement is a string literal."""
    return bool(
        node.body
        and isinstance(node.body[0], ast.Expr)
        and isinstance(node.body[0].value, ast.Constant)
        and isinstance(node.body[0].value.value, str)
    )


def check_missing_docstrings(tree: ast.Module) -> list[Finding]:
    """Report modules, classes, and functions that lack a docstring."""
    findings: list[Finding] = []

    if not _has_docstring(tree):
        findings.append(Finding(
            line=1,
            severity="INFO",
            rule="missing-docstring",
            message="module is missing a docstring",
        ))

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if not _has_docstring(node):
                kind = "async function" if isinstance(node, ast.AsyncFunctionDef) else "function"
                findings.append(Finding(
                    line=node.lineno,
                    severity="INFO",
                    rule="missing-docstring",
                    message=f"{kind} '{node.name}' is missing a docstring",
                ))
        elif isinstance(node, ast.ClassDef):
            if not _has_docstring(node):
                findings.append(Finding(
                    line=node.lineno,
                    severity="INFO",
                    rule="missing-docstring",
                    message=f"class '{node.name}' is missing a docstring",
                ))

    return findings


def check_long_functions(tree: ast.Module, max_lines: int) -> list[Finding]:
    """Warn when a function body spans more than *max_lines* lines."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            length = node.end_lineno - node.lineno + 1
            if length > max_lines:
                kind = "async function" if isinstance(node, ast.AsyncFunctionDef) else "function"
                findings.append(Finding(
                    line=node.lineno,
                    severity="WARNING",
                    rule="too-long",
                    message=(
                        f"{kind} '{node.name}' is {length} lines "
                        f"(max {max_lines})"
                    ),
                ))
    return findings


def check_too_many_args(tree: ast.Module, max_args: int) -> list[Finding]:
    """Warn when a function signature exceeds *max_args* parameters.

    The count includes positional-only, regular, keyword-only, *args,
    and **kwargs parameters, but excludes implicit ``self`` / ``cls``
    when the function is defined inside a class body.
    """
    findings: list[Finding] = []

    # Build a set of line numbers for functions defined inside a class so we
    # can subtract the implicit self/cls parameter from the count.
    method_lines: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef):
            for item in ast.walk(node):
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    method_lines.add(item.lineno)

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            a = node.args
            count = (
                len(a.posonlyargs)
                + len(a.args)
                + len(a.kwonlyargs)
                + (1 if a.vararg else 0)
                + (1 if a.kwarg else 0)
            )
            # Subtract one for self/cls in methods
            if node.lineno in method_lines and count > 0:
                count -= 1

            if count > max_args:
                kind = "async function" if isinstance(node, ast.AsyncFunctionDef) else "function"
                findings.append(Finding(
                    line=node.lineno,
                    severity="WARNING",
                    rule="too-many-args",
                    message=(
                        f"{kind} '{node.name}' has {count} parameter(s) "
                        f"(max {max_args})"
                    ),
                ))
    return findings


def check_bare_excepts(tree: ast.Module) -> list[Finding]:
    """Error on bare ``except:`` clauses (no exception type specified)."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ExceptHandler) and node.type is None:
            findings.append(Finding(
                line=node.lineno,
                severity="ERROR",
                rule="bare-except",
                message=(
                    "bare 'except:' catches all exceptions including "
                    "KeyboardInterrupt and SystemExit"
                ),
            ))
    return findings


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

_ANSI: dict[str, str] = {
    "ERROR":   "\033[31m",  # red
    "WARNING": "\033[33m",  # yellow
    "INFO":    "\033[36m",  # cyan
    "RESET":   "\033[0m",
    "GREEN":   "\033[32m",
    "BOLD":    "\033[1m",
}


def _severity_sort_key(f: Finding) -> tuple[int, int]:
    return (SEVERITY_RANK.get(f.severity, 99), f.line)


def _colorize(text: str, code: str, use_color: bool) -> str:
    if not use_color:
        return text
    return f"{code}{text}{_ANSI['RESET']}"


def print_table(findings: list[Finding], *, use_color: bool) -> None:
    """Render findings as a fixed-width ASCII table to stdout."""
    if not findings:
        msg = "No findings — all checks passed."
        print(_colorize(msg, _ANSI["GREEN"], use_color))
        return

    # Column widths — at least as wide as the header label
    w_line = max(4,  max(len(str(f.line))     for f in findings))
    w_sev  = max(8,  max(len(f.severity)       for f in findings))
    w_rule = max(4,  max(len(f.rule)           for f in findings))
    w_msg  = max(7,  max(len(f.message)        for f in findings))

    sep = (
        f"+-{'-' * w_line}-"
        f"+-{'-' * w_sev}-"
        f"+-{'-' * w_rule}-"
        f"+-{'-' * w_msg}-+"
    )
    header = (
        f"| {_colorize('Line',     _ANSI['BOLD'], use_color):<{w_line + (len(_ANSI['BOLD']) + len(_ANSI['RESET']) if use_color else 0)}} "
        f"| {_colorize('Severity', _ANSI['BOLD'], use_color):<{w_sev  + (len(_ANSI['BOLD']) + len(_ANSI['RESET']) if use_color else 0)}} "
        f"| {_colorize('Rule',     _ANSI['BOLD'], use_color):<{w_rule + (len(_ANSI['BOLD']) + len(_ANSI['RESET']) if use_color else 0)}} "
        f"| {_colorize('Message',  _ANSI['BOLD'], use_color):<{w_msg  + (len(_ANSI['BOLD']) + len(_ANSI['RESET']) if use_color else 0)}} |"
    )

    print(sep)
    print(header)
    print(sep)

    for f in findings:
        sev_color = _ANSI.get(f.severity, "")
        colored_sev = _colorize(f"{f.severity:<{w_sev}}", sev_color, use_color)
        # Pad accounting for invisible escape bytes when color is on
        sev_pad = w_sev + (len(sev_color) + len(_ANSI["RESET"]) if use_color else 0)
        row = (
            f"| {str(f.line):<{w_line}} "
            f"| {colored_sev:<{sev_pad}} "
            f"| {f.rule:<{w_rule}} "
            f"| {f.message:<{w_msg}} |"
        )
        print(row)

    print(sep)

    # Summary line
    counts: dict[str, int] = {}
    for f in findings:
        counts[f.severity] = counts.get(f.severity, 0) + 1

    parts = [
        f"{v} {_colorize(k, _ANSI.get(k, ''), use_color)}"
        for k, v in sorted(counts.items(), key=lambda kv: SEVERITY_RANK.get(kv[0], 99))
    ]
    total = len(findings)
    print(f"\n{total} finding(s): {', '.join(parts)}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="lint_report.py",
        description="Static analysis report for a Python source file.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "file",
        type=Path,
        help="path to the Python file to analyse",
    )
    p.add_argument(
        "--max-lines",
        type=int,
        default=50,
        metavar="N",
        help="flag functions with more than N lines",
    )
    p.add_argument(
        "--max-args",
        type=int,
        default=5,
        metavar="M",
        help="flag functions with more than M parameters",
    )
    p.add_argument(
        "--strict",
        action="store_true",
        help="exit with status 1 if any finding is present",
    )
    p.add_argument(
        "--no-color",
        action="store_true",
        help="disable ANSI color output",
    )
    return p


def main() -> int:
    """Parse arguments, run all checks, print the report, return exit code."""
    args = _build_parser().parse_args()
    path: Path = args.file

    if not path.exists():
        print(f"error: file not found: {path}", file=sys.stderr)
        return 2
    if not path.is_file():
        print(f"error: not a regular file: {path}", file=sys.stderr)
        return 2

    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"error: cannot read {path}: {exc}", file=sys.stderr)
        return 2

    try:
        tree = ast.parse(source, filename=str(path), type_comments=False)
    except SyntaxError as exc:
        print(f"error: syntax error in {path}: {exc}", file=sys.stderr)
        return 2

    findings: list[Finding] = []
    findings.extend(check_unused_imports(tree))
    findings.extend(check_long_functions(tree, args.max_lines))
    findings.extend(check_too_many_args(tree, args.max_args))
    findings.extend(check_missing_docstrings(tree))
    findings.extend(check_bare_excepts(tree))

    findings.sort(key=_severity_sort_key)

    use_color = sys.stdout.isatty() and not args.no_color
    print(f"\nAnalysing: {path}\n")
    print_table(findings, use_color=use_color)
    print()

    if args.strict and findings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
