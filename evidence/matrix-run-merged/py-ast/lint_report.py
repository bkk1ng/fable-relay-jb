"""
lint_report.py — static analysis report for a Python file.

Checks:
  - Unused imports
  - Functions longer than N lines (default: 50)
  - Functions with more than M arguments (default: 5)
  - Missing docstrings (module, class, function/method)
  - Bare except clauses

Usage:
  python lint_report.py <file> [--max-lines N] [--max-args M] [--strict]
"""

import ast
import argparse
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Severity ordering (higher index = higher severity)
# ---------------------------------------------------------------------------

SEVERITY_ORDER = {"low": 0, "medium": 1, "high": 2}


# ---------------------------------------------------------------------------
# Finding dataclass (plain tuple for stdlib-only)
# ---------------------------------------------------------------------------

def make_finding(severity: str, line: int, code: str, message: str) -> dict:
    return {"severity": severity, "line": line, "code": code, "message": message}


# ---------------------------------------------------------------------------
# Visitors
# ---------------------------------------------------------------------------

class LintVisitor(ast.NodeVisitor):
    """Walk the AST and collect all findings."""

    def __init__(self, source_lines: list[str], max_lines: int, max_args: int):
        self.source_lines = source_lines
        self.max_lines = max_lines
        self.max_args = max_args
        self.findings: list[dict] = []

        # Tracks names bound by import statements
        self._imported_names: dict[str, int] = {}  # name -> line
        # Tracks all Name loads seen anywhere in the tree
        self._used_names: set[str] = set()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _add(self, severity: str, line: int, code: str, message: str) -> None:
        self.findings.append(make_finding(severity, line, code, message))

    def _function_body_lines(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
        """Count source lines occupied by the function body."""
        if not node.body:
            return 0
        first = node.body[0].lineno
        # end_lineno is available from Python 3.8+
        last = getattr(node, "end_lineno", node.body[-1].lineno)
        return last - first + 1

    def _arg_count(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
        args = node.args
        total = (
            len(args.args)
            + len(args.posonlyargs)
            + len(args.kwonlyargs)
            + (1 if args.vararg else 0)
            + (1 if args.kwarg else 0)
        )
        return total

    def _has_docstring(self, node: ast.AST) -> bool:
        body = getattr(node, "body", [])
        if not body:
            return False
        first = body[0]
        return (
            isinstance(first, ast.Expr)
            and isinstance(first.value, ast.Constant)
            and isinstance(first.value.value, str)
        )

    # ------------------------------------------------------------------
    # Import tracking
    # ------------------------------------------------------------------

    def visit_Import(self, node: ast.Import) -> None:
        for alias in node.names:
            bound_name = alias.asname if alias.asname else alias.name.split(".")[0]
            self._imported_names[bound_name] = node.lineno
        self.generic_visit(node)

    def visit_ImportFrom(self, node: ast.ImportFrom) -> None:
        for alias in node.names:
            if alias.name == "*":
                # Wildcard import — we can't track usage; skip unused check
                continue
            bound_name = alias.asname if alias.asname else alias.name
            self._imported_names[bound_name] = node.lineno
        self.generic_visit(node)

    def visit_Name(self, node: ast.Name) -> None:
        if isinstance(node.ctx, ast.Load):
            self._used_names.add(node.id)
        self.generic_visit(node)

    def visit_Attribute(self, node: ast.Attribute) -> None:
        # Mark the root name of attribute chains as used (e.g. `os.path` marks `os`)
        root = node
        while isinstance(root, ast.Attribute):
            root = root.value
        if isinstance(root, ast.Name):
            self._used_names.add(root.id)
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Function / method checks
    # ------------------------------------------------------------------

    def _check_function(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> None:
        body_lines = self._function_body_lines(node)
        if body_lines > self.max_lines:
            self._add(
                "medium",
                node.lineno,
                "F001",
                f"Function '{node.name}' is {body_lines} lines long (limit: {self.max_lines})",
            )

        arg_count = self._arg_count(node)
        if arg_count > self.max_args:
            self._add(
                "medium",
                node.lineno,
                "F002",
                f"Function '{node.name}' has {arg_count} arguments (limit: {self.max_args})",
            )

        if not self._has_docstring(node):
            self._add(
                "low",
                node.lineno,
                "D001",
                f"Function '{node.name}' is missing a docstring",
            )

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        self._check_function(node)
        self.generic_visit(node)

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self._check_function(node)
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Class checks
    # ------------------------------------------------------------------

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        if not self._has_docstring(node):
            self._add(
                "low",
                node.lineno,
                "D002",
                f"Class '{node.name}' is missing a docstring",
            )
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Bare except
    # ------------------------------------------------------------------

    def visit_ExceptHandler(self, node: ast.ExceptHandler) -> None:
        if node.type is None:
            self._add(
                "high",
                node.lineno,
                "E001",
                "Bare 'except' clause catches all exceptions including SystemExit and KeyboardInterrupt",
            )
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Post-walk: unused imports
    # ------------------------------------------------------------------

    def report_unused_imports(self) -> None:
        for name, lineno in self._imported_names.items():
            if name not in self._used_names:
                self._add(
                    "medium",
                    lineno,
                    "I001",
                    f"Imported name '{name}' is never used",
                )


# ---------------------------------------------------------------------------
# Module-level docstring check (outside the visitor)
# ---------------------------------------------------------------------------

def check_module_docstring(tree: ast.Module) -> list[dict]:
    findings = []
    body = tree.body
    has_doc = (
        body
        and isinstance(body[0], ast.Expr)
        and isinstance(body[0].value, ast.Constant)
        and isinstance(body[0].value.value, str)
    )
    if not has_doc:
        findings.append(
            make_finding("low", 1, "D003", "Module is missing a top-level docstring")
        )
    return findings


# ---------------------------------------------------------------------------
# Table rendering
# ---------------------------------------------------------------------------

_SEV_LABEL = {"high": "HIGH  ", "medium": "MEDIUM", "low": "LOW   "}
_SEV_MARKER = {"high": "!!!", "medium": "! ", "low": "  "}


def render_table(findings: list[dict], path: str) -> str:
    if not findings:
        return f"No findings in {path}.\n"

    # Column widths
    col_sev = max(len("SEVERITY"), max(len(_SEV_LABEL[f["severity"]]) for f in findings))
    col_line = max(len("LINE"), max(len(str(f["line"])) for f in findings))
    col_code = max(len("CODE"), max(len(f["code"]) for f in findings))
    col_msg = max(len("MESSAGE"), max(len(f["message"]) for f in findings))

    sep = (
        "+-"
        + "-" * col_sev
        + "-+-"
        + "-" * col_line
        + "-+-"
        + "-" * col_code
        + "-+-"
        + "-" * col_msg
        + "-+"
    )

    def row(sev: str, line: str, code: str, msg: str) -> str:
        return (
            "| "
            + sev.ljust(col_sev)
            + " | "
            + line.rjust(col_line)
            + " | "
            + code.ljust(col_code)
            + " | "
            + msg.ljust(col_msg)
            + " |"
        )

    lines = [
        f"Findings for: {path}",
        sep,
        row("SEVERITY", "LINE", "CODE", "MESSAGE"),
        sep,
    ]

    for f in findings:
        marker = _SEV_MARKER[f["severity"]]
        label = _SEV_LABEL[f["severity"]]
        lines.append(row(marker + label.strip(), str(f["line"]), f["code"], f["message"]))

    lines.append(sep)

    counts = {s: sum(1 for f in findings if f["severity"] == s) for s in SEVERITY_ORDER}
    lines.append(
        f"  Total: {len(findings)} finding(s) — "
        f"high: {counts['high']}, medium: {counts['medium']}, low: {counts['low']}"
    )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def analyze(path: Path, max_lines: int, max_args: int) -> list[dict]:
    source = path.read_text(encoding="utf-8")
    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as exc:
        return [make_finding("high", exc.lineno or 1, "S001", f"SyntaxError: {exc.msg}")]

    source_lines = source.splitlines()

    visitor = LintVisitor(source_lines, max_lines, max_args)
    visitor.visit(tree)
    visitor.report_unused_imports()

    findings = check_module_docstring(tree) + visitor.findings

    # Sort: severity descending, then line ascending
    findings.sort(key=lambda f: (-SEVERITY_ORDER[f["severity"]], f["line"]))
    return findings


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Static lint report for a Python file.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Codes:
  E001  Bare except clause
  F001  Function body too long
  F002  Too many arguments
  D001  Function/method missing docstring
  D002  Class missing docstring
  D003  Module missing docstring
  I001  Unused import
  S001  Syntax error (file could not be parsed)
""",
    )
    parser.add_argument("file", type=Path, help="Python file to analyse")
    parser.add_argument(
        "--max-lines",
        type=int,
        default=50,
        metavar="N",
        help="Maximum allowed lines in a function body (default: 50)",
    )
    parser.add_argument(
        "--max-args",
        type=int,
        default=5,
        metavar="M",
        help="Maximum allowed function arguments (default: 5)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit with code 1 if any finding is present",
    )
    args = parser.parse_args()

    if not args.file.exists():
        print(f"error: file not found: {args.file}", file=sys.stderr)
        sys.exit(2)

    if not args.file.is_file():
        print(f"error: not a file: {args.file}", file=sys.stderr)
        sys.exit(2)

    findings = analyze(args.file, args.max_lines, args.max_args)
    print(render_table(findings, str(args.file)), end="")

    if args.strict and findings:
        sys.exit(1)


if __name__ == "__main__":
    main()
