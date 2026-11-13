#!/usr/bin/env python3
"""lint_report.py — static analysis for Python files.

Checks: unused imports, long functions, too many arguments,
        missing docstrings, bare excepts.

Prints findings as a table sorted by severity (HIGH → MEDIUM → LOW).
Exits 1 when --strict is set and at least one finding exists.
"""

import ast
import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ── data ──────────────────────────────────────────────────────────────────────

SEVERITY_RANK: dict[str, int] = {"HIGH": 0, "MEDIUM": 1, "LOW": 2}

SEVERITY_COLOR: dict[str, str] = {
    "HIGH":   "\033[1;31m",   # bold red
    "MEDIUM": "\033[1;33m",   # bold yellow
    "LOW":    "\033[1;34m",   # bold blue
    "RESET":  "\033[0m",
}


@dataclass(order=False)
class Finding:
    line: int
    severity: str   # "HIGH" | "MEDIUM" | "LOW"
    rule: str
    message: str


# ── AST helpers ───────────────────────────────────────────────────────────────

def _has_docstring(node: ast.AST) -> bool:
    body = getattr(node, "body", [])
    if not body:
        return False
    first = body[0]
    return (
        isinstance(first, ast.Expr)
        and isinstance(first.value, ast.Constant)
        and isinstance(first.value.value, str)
    )


def _used_names(tree: ast.Module) -> set[str]:
    """Return every Name id that appears in a Load context outside import nodes."""
    skip = (ast.Import, ast.ImportFrom)
    used: set[str] = set()

    # Collect names exported via __all__ = [...] so they aren't flagged unused.
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "__all__":
                    if isinstance(node.value, (ast.List, ast.Tuple)):
                        for elt in node.value.elts:
                            if isinstance(elt, ast.Constant) and isinstance(elt.value, str):
                                used.add(elt.value)

    # Walk the whole tree; skip the import statement nodes themselves.
    def _walk_non_import(node: ast.AST) -> None:
        for child in ast.iter_child_nodes(node):
            if isinstance(child, skip):
                continue
            if isinstance(child, ast.Name) and isinstance(child.ctx, ast.Load):
                used.add(child.id)
            _walk_non_import(child)

    _walk_non_import(tree)
    return used


def _imported_aliases(tree: ast.Module) -> list[tuple[str, int]]:
    """Return (bound_name, lineno) for every imported symbol."""
    result: list[tuple[str, int]] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                bound = alias.asname if alias.asname else alias.name.split(".")[0]
                result.append((bound, node.lineno))
        elif isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if alias.name == "*":
                    continue
                bound = alias.asname if alias.asname else alias.name
                result.append((bound, node.lineno))
    return result


# ── visitors ──────────────────────────────────────────────────────────────────

class _LintVisitor(ast.NodeVisitor):
    """Single-pass visitor for function/class-level checks."""

    def __init__(self, max_lines: int, max_args: int) -> None:
        self.max_lines = max_lines
        self.max_args = max_args
        self.findings: list[Finding] = []
        self._class_depth = 0  # track nesting to detect methods vs. functions

    # ── classes ──

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        if not _has_docstring(node):
            self.findings.append(Finding(
                line=node.lineno,
                severity="LOW",
                rule="missing-docstring",
                message=f"class '{node.name}' has no docstring",
            ))
        self._class_depth += 1
        self.generic_visit(node)
        self._class_depth -= 1

    # ── functions ──

    def _visit_function(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef
    ) -> None:
        # long-function
        length = node.end_lineno - node.lineno + 1
        if length > self.max_lines:
            self.findings.append(Finding(
                line=node.lineno,
                severity="MEDIUM",
                rule="long-function",
                message=(
                    f"'{node.name}' is {length} lines"
                    f" (max {self.max_lines})"
                ),
            ))

        # too-many-args
        # For methods the first positional param (self/cls) is excluded.
        a = node.args
        count = (
            len(a.posonlyargs)
            + len(a.args)
            + len(a.kwonlyargs)
            + (1 if a.vararg else 0)
            + (1 if a.kwarg else 0)
        )
        effective = count - 1 if (self._class_depth > 0 and count > 0) else count
        if effective > self.max_args:
            self.findings.append(Finding(
                line=node.lineno,
                severity="MEDIUM",
                rule="too-many-args",
                message=(
                    f"'{node.name}' has {effective} parameter(s)"
                    f" (max {self.max_args})"
                ),
            ))

        # missing-docstring
        if not _has_docstring(node):
            self.findings.append(Finding(
                line=node.lineno,
                severity="LOW",
                rule="missing-docstring",
                message=f"'{node.name}' has no docstring",
            ))

        # Descend; a function defined inside a class method is still a function,
        # so we don't bump _class_depth here.
        self.generic_visit(node)

    visit_FunctionDef = _visit_function        # type: ignore[assignment]
    visit_AsyncFunctionDef = _visit_function   # type: ignore[assignment]

    # ── bare excepts ──

    def visit_ExceptHandler(self, node: ast.ExceptHandler) -> None:
        if node.type is None:
            self.findings.append(Finding(
                line=node.lineno,
                severity="HIGH",
                rule="bare-except",
                message=(
                    "bare 'except:' catches all exceptions including "
                    "SystemExit and KeyboardInterrupt"
                ),
            ))
        self.generic_visit(node)


# ── top-level analysis ────────────────────────────────────────────────────────

def analyse(
    source: str,
    max_lines: int,
    max_args: int,
) -> list[Finding]:
    """Parse *source* and return all findings."""
    try:
        tree = ast.parse(source)
    except SyntaxError as exc:
        return [Finding(
            line=exc.lineno or 0,
            severity="HIGH",
            rule="syntax-error",
            message=str(exc),
        )]

    findings: list[Finding] = []

    # unused-import (needs full-tree name scan before visiting)
    used = _used_names(tree)
    for name, lineno in _imported_aliases(tree):
        if name not in used:
            findings.append(Finding(
                line=lineno,
                severity="MEDIUM",
                rule="unused-import",
                message=f"'{name}' is imported but never used",
            ))

    # structural checks via visitor
    visitor = _LintVisitor(max_lines=max_lines, max_args=max_args)
    visitor.visit(tree)
    findings.extend(visitor.findings)

    return sorted(findings, key=lambda f: (SEVERITY_RANK[f.severity], f.line))


# ── rendering ─────────────────────────────────────────────────────────────────

def _color_supported() -> bool:
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty()


def _pad(text: str, width: int) -> str:
    """Left-justify *text* in a field of *width*, truncating with … if needed."""
    if len(text) > width:
        return text[: width - 1] + "…"
    return text.ljust(width)


def print_report(
    path: Path,
    findings: list[Finding],
    use_color: bool,
) -> None:
    if not findings:
        print(f"{path}: no findings.\n")
        return

    # column widths — driven by content, with hard minimums
    w_line = max(4, max(len(str(f.line))     for f in findings))
    w_sev  = max(8, max(len(f.severity)      for f in findings))
    w_rule = max(4, max(len(f.rule)          for f in findings))
    # cap message column so the table stays readable in an 80-col terminal
    w_msg  = min(72, max(7, max(len(f.message) for f in findings)))

    def sep() -> str:
        return (
            f"+{'-' * (w_line + 2)}"
            f"+{'-' * (w_sev  + 2)}"
            f"+{'-' * (w_rule + 2)}"
            f"+{'-' * (w_msg  + 2)}+"
        )

    def row(line: str, sev: str, rule: str, msg: str, color: str = "") -> str:
        sev_padded = _pad(sev, w_sev)
        if use_color and color:
            sev_cell = f"{color}{sev_padded}{SEVERITY_COLOR['RESET']}"
        else:
            sev_cell = sev_padded
        return (
            f"| {_pad(line, w_line)} "
            f"| {sev_cell} "
            f"| {_pad(rule, w_rule)} "
            f"| {_pad(msg,  w_msg )} |"
        )

    print(f"\n{path}")
    print(sep())
    print(row("LINE", "SEVERITY", "RULE", "MESSAGE"))
    print(sep())

    for f in findings:
        color = SEVERITY_COLOR.get(f.severity, "") if use_color else ""
        print(row(str(f.line), f.severity, f.rule, f.message, color))

    print(sep())

    # summary line
    counts: dict[str, int] = {}
    for f in findings:
        counts[f.severity] = counts.get(f.severity, 0) + 1

    parts: list[str] = []
    for sev in ("HIGH", "MEDIUM", "LOW"):
        n = counts.get(sev, 0)
        if n:
            label = f"{n} {sev}"
            if use_color:
                label = f"{SEVERITY_COLOR[sev]}{label}{SEVERITY_COLOR['RESET']}"
            parts.append(label)

    total = len(findings)
    plural = "s" if total != 1 else ""
    print(f"  {total} finding{plural}: {', '.join(parts)}\n")


# ── CLI ───────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="lint_report",
        description="Static lint report for Python files.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "files",
        metavar="FILE",
        nargs="+",
        type=Path,
        help="Python file(s) to analyse",
    )
    p.add_argument(
        "--max-lines",
        metavar="N",
        type=int,
        default=50,
        help="flag functions longer than N lines",
    )
    p.add_argument(
        "--max-args",
        metavar="M",
        type=int,
        default=5,
        help="flag functions with more than M arguments (self/cls excluded)",
    )
    p.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 if any finding is reported",
    )
    p.add_argument(
        "--no-color",
        action="store_true",
        help="disable ANSI colour output",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    use_color = not args.no_color and _color_supported()

    any_findings = False

    for path in args.files:
        if not path.exists():
            print(f"error: '{path}' does not exist", file=sys.stderr)
            return 2
        if not path.is_file():
            print(f"error: '{path}' is not a file", file=sys.stderr)
            return 2

        try:
            source = path.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

        findings = analyse(source, args.max_lines, args.max_args)
        print_report(path, findings, use_color)

        if findings:
            any_findings = True

    if args.strict and any_findings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
