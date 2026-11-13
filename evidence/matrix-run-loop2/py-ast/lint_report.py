#!/usr/bin/env python3
"""
lint_report.py — static analysis report for a Python source file.

Checks
------
- unused-import      : imported names never referenced in the file
- function-too-long  : function body exceeds --max-lines (default 50)
- too-many-arguments : function signature exceeds --max-args (default 5)
- missing-docstring  : module / class / function lacks a docstring
- bare-except        : bare ``except:`` with no exception type

Output
------
Findings are printed as a fixed-width table sorted by severity (HIGH first),
then by line number.  With --strict the process exits 1 when any finding exists.

Usage
-----
  python lint_report.py [--max-lines N] [--max-args M] [--strict] <file>
"""

import ast
import argparse
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

SEVERITY_RANK: dict[str, int] = {"HIGH": 0, "MEDIUM": 1, "LOW": 2}


class Finding:
    """One lint finding."""

    __slots__ = ("severity", "line", "check", "message")

    def __init__(self, severity: str, line: int, check: str, message: str) -> None:
        self.severity = severity
        self.line = line
        self.check = check
        self.message = message

    def sort_key(self) -> tuple[int, int]:
        return (SEVERITY_RANK[self.severity], self.line)


# ---------------------------------------------------------------------------
# AST helpers
# ---------------------------------------------------------------------------

def _collect_used_names(tree: ast.Module) -> set[str]:
    """
    Return every name identifier actually referenced in *tree*.

    Walks the full AST so decorators, annotations, comprehension targets,
    default values, and ``__all__`` string entries are all counted.
    """
    used: set[str] = set()

    for node in ast.walk(tree):
        # Direct name references
        if isinstance(node, ast.Name):
            used.add(node.id)
        # Attribute chains: collect the root Name (e.g. ``os`` in ``os.path.join``)
        elif isinstance(node, ast.Attribute):
            root: ast.expr = node
            while isinstance(root, ast.Attribute):
                root = root.value
            if isinstance(root, ast.Name):
                used.add(root.id)
        # ``__all__ = ["name", ...]`` marks names as publicly re-exported
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "__all__":
                    if isinstance(node.value, (ast.List, ast.Tuple)):
                        for elt in node.value.elts:
                            if isinstance(elt, ast.Constant) and isinstance(elt.value, str):
                                used.add(elt.value)

    return used


def _has_docstring(node: ast.AST) -> bool:
    """Return True when *node* opens with a string-literal docstring."""
    body = getattr(node, "body", [])
    if not body:
        return False
    first = body[0]
    return (
        isinstance(first, ast.Expr)
        and isinstance(first.value, ast.Constant)
        and isinstance(first.value.value, str)
    )


def _function_line_count(node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
    end = getattr(node, "end_lineno", node.lineno)
    return end - node.lineno + 1


def _argument_count(node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
    """Count all parameters including *args and **kwargs."""
    args = node.args
    total = (
        len(args.posonlyargs)
        + len(args.args)
        + len(args.kwonlyargs)
        + (1 if args.vararg else 0)
        + (1 if args.kwarg else 0)
    )
    return total


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_bare_excepts(tree: ast.Module) -> list[Finding]:
    """HIGH — bare ``except:`` clause with no exception type."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ExceptHandler) and node.type is None:
            findings.append(Finding(
                severity="HIGH",
                line=node.lineno,
                check="bare-except",
                message="bare 'except:' silently swallows every exception "
                        "including KeyboardInterrupt and SystemExit",
            ))
    return findings


def check_unused_imports(tree: ast.Module) -> list[Finding]:
    """
    MEDIUM — imported names never referenced elsewhere in the file.

    Skips the entire check when a ``from x import *`` is present because
    any imported name might be consumed via the star binding.
    """
    findings: list[Finding] = []

    # Bail on star imports — cannot know what they bind
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if alias.name == "*":
                    return []

    used = _collect_used_names(tree)

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                # ``import os.path`` binds ``os`` in the local namespace
                bound = alias.asname if alias.asname else alias.name.split(".")[0]
                if bound not in used:
                    display = alias.asname or alias.name
                    findings.append(Finding(
                        severity="MEDIUM",
                        line=node.lineno,
                        check="unused-import",
                        message=f"'{display}' imported but never used",
                    ))
        elif isinstance(node, ast.ImportFrom):
            for alias in node.names:
                bound = alias.asname if alias.asname else alias.name
                if bound not in used:
                    display = alias.asname or alias.name
                    module = node.module or "(unknown)"
                    findings.append(Finding(
                        severity="MEDIUM",
                        line=node.lineno,
                        check="unused-import",
                        message=f"'{display}' imported from '{module}' but never used",
                    ))

    return findings


def check_function_length(tree: ast.Module, max_lines: int) -> list[Finding]:
    """LOW — function body is longer than *max_lines*."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            length = _function_line_count(node)
            if length > max_lines:
                findings.append(Finding(
                    severity="LOW",
                    line=node.lineno,
                    check="function-too-long",
                    message=f"'{node.name}' is {length} lines (limit {max_lines})",
                ))
    return findings


def check_argument_count(tree: ast.Module, max_args: int) -> list[Finding]:
    """LOW — function has more parameters than *max_args*."""
    findings: list[Finding] = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            count = _argument_count(node)
            if count > max_args:
                findings.append(Finding(
                    severity="LOW",
                    line=node.lineno,
                    check="too-many-arguments",
                    message=f"'{node.name}' has {count} arguments (limit {max_args})",
                ))
    return findings


def check_missing_docstrings(tree: ast.Module) -> list[Finding]:
    """LOW — module, class, or function is missing a docstring."""
    findings: list[Finding] = []

    if not _has_docstring(tree):
        findings.append(Finding(
            severity="LOW",
            line=1,
            check="missing-docstring",
            message="module is missing a module-level docstring",
        ))

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if not _has_docstring(node):
                findings.append(Finding(
                    severity="LOW",
                    line=node.lineno,
                    check="missing-docstring",
                    message=f"function '{node.name}' is missing a docstring",
                ))
        elif isinstance(node, ast.ClassDef):
            if not _has_docstring(node):
                findings.append(Finding(
                    severity="LOW",
                    line=node.lineno,
                    check="missing-docstring",
                    message=f"class '{node.name}' is missing a docstring",
                ))

    return findings


# ---------------------------------------------------------------------------
# Table renderer
# ---------------------------------------------------------------------------

# Column widths (excluding the single-space separator)
_W_SEV   = 8
_W_LINE  = 6
_W_CHECK = 24
_W_MSG   = 56   # truncated with ellipsis if longer

_DIVIDER = (
    f"{'─' * _W_SEV}-{'─' * _W_LINE}-{'─' * _W_CHECK}-{'─' * _W_MSG}"
)
_HEADER  = (
    f"{'SEVERITY':<{_W_SEV}} {'LINE':>{_W_LINE}} {'CHECK':<{_W_CHECK}} MESSAGE"
)

_SEV_LABEL: dict[str, str] = {
    "HIGH":   "HIGH",
    "MEDIUM": "MEDIUM",
    "LOW":    "LOW",
}


def _truncate(text: str, width: int) -> str:
    if len(text) <= width:
        return text
    return text[: width - 1] + "…"


def render_table(findings: list[Finding], source_path: str) -> str:
    """Format *findings* as a fixed-width severity table."""
    if not findings:
        return f"✓  No findings in {source_path}.\n"

    ordered = sorted(findings, key=Finding.sort_key)

    rows: list[str] = [
        f"Findings: {source_path}",
        _DIVIDER,
        _HEADER,
        _DIVIDER,
    ]

    for f in ordered:
        sev   = f"{_SEV_LABEL[f.severity]:<{_W_SEV}}"
        line  = f"{f.line:>{_W_LINE}}"
        check = f"{f.check:<{_W_CHECK}}"
        msg   = _truncate(f.message, _W_MSG)
        rows.append(f"{sev} {line} {check} {msg}")

    rows.append(_DIVIDER)

    counts: dict[str, int] = {}
    for f in ordered:
        counts[f.severity] = counts.get(f.severity, 0) + 1

    parts: list[str] = []
    for sev in ("HIGH", "MEDIUM", "LOW"):
        if sev in counts:
            parts.append(f"{counts[sev]} {sev}")
    rows.append(f"Total: {', '.join(parts)}  ({len(ordered)} finding{'s' if len(ordered) != 1 else ''})")

    return "\n".join(rows) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="lint_report",
        description="Static analysis report for a Python source file.",
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
        help="max lines per function body before flagging",
    )
    parser.add_argument(
        "--max-args",
        type=int,
        default=5,
        metavar="M",
        help="max parameters per function before flagging",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 when any finding is present",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Entry point.  Returns the process exit code."""
    args = _build_parser().parse_args(argv)

    path: Path = args.file
    if not path.exists():
        print(f"lint_report: error: file not found: {path}", file=sys.stderr)
        return 2
    if not path.is_file():
        print(f"lint_report: error: not a regular file: {path}", file=sys.stderr)
        return 2

    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"lint_report: error: cannot read {path}: {exc}", file=sys.stderr)
        return 2

    try:
        tree = ast.parse(source, filename=str(path), type_comments=False)
    except SyntaxError as exc:
        print(
            f"lint_report: syntax error in {path}, line {exc.lineno}: {exc.msg}",
            file=sys.stderr,
        )
        return 2

    findings: list[Finding] = []
    findings.extend(check_bare_excepts(tree))
    findings.extend(check_unused_imports(tree))
    findings.extend(check_function_length(tree, args.max_lines))
    findings.extend(check_argument_count(tree, args.max_args))
    findings.extend(check_missing_docstrings(tree))

    print(render_table(findings, str(path)), end="")

    return 1 if (args.strict and findings) else 0


if __name__ == "__main__":
    sys.exit(main())
