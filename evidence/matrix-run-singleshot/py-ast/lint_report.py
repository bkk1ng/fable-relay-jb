"""
lint_report.py — static analysis report for a Python file.

Checks:
  - Unused imports
  - Functions longer than N lines (default 50)
  - Functions with more than M arguments (default 5)
  - Missing docstrings (functions, classes, modules)
  - Bare except clauses

Usage:
  python lint_report.py path/to/file.py [--max-lines N] [--max-args M] [--strict]
"""

import ast
import argparse
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Severity ordering for sorting
# ---------------------------------------------------------------------------
SEVERITY_ORDER = {"error": 0, "warning": 1, "info": 2}


# ---------------------------------------------------------------------------
# Data class (plain tuple-based to stay stdlib-only)
# ---------------------------------------------------------------------------
class Finding:
    __slots__ = ("severity", "line", "code", "message")

    def __init__(self, severity: str, line: int, code: str, message: str) -> None:
        self.severity = severity
        self.line = line
        self.code = code
        self.message = message


# ---------------------------------------------------------------------------
# Visitors
# ---------------------------------------------------------------------------
class LintVisitor(ast.NodeVisitor):
    """Single-pass AST visitor collecting all findings."""

    def __init__(self, source_lines: list[str], max_lines: int, max_args: int) -> None:
        self._source_lines = source_lines
        self._max_lines = max_lines
        self._max_args = max_args
        self.findings: list[Finding] = []

        # Track imported names → set of alias nodes seen; value = False means unused.
        # Key: local name string, value: (alias_node, used: bool)
        self._imports: dict[str, ast.alias] = {}
        self._used_names: set[str] = set()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------
    def _add(self, severity: str, line: int, code: str, message: str) -> None:
        self.findings.append(Finding(severity, line, code, message))

    def _function_body_lines(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
        """Count source lines occupied by the function body (end - start)."""
        if hasattr(node, "end_lineno") and node.end_lineno is not None:
            return node.end_lineno - node.lineno
        # Fallback: count non-blank body lines
        return sum(
            1
            for child in ast.walk(node)
            if hasattr(child, "lineno") and child.lineno > node.lineno
        )

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
            and isinstance(first.value, (ast.Constant, ast.Str))
        )

    # ------------------------------------------------------------------
    # Import tracking
    # ------------------------------------------------------------------
    def visit_Import(self, node: ast.Import) -> None:  # noqa: N802
        for alias in node.names:
            local_name = alias.asname if alias.asname else alias.name.split(".")[0]
            self._imports[local_name] = alias
        self.generic_visit(node)

    def visit_ImportFrom(self, node: ast.ImportFrom) -> None:  # noqa: N802
        for alias in node.names:
            if alias.name == "*":
                continue  # star imports — cannot track usage
            local_name = alias.asname if alias.asname else alias.name
            self._imports[local_name] = alias
        self.generic_visit(node)

    def visit_Name(self, node: ast.Name) -> None:  # noqa: N802
        self._used_names.add(node.id)
        self.generic_visit(node)

    def visit_Attribute(self, node: ast.Attribute) -> None:  # noqa: N802
        # Capture the root name of attribute chains (e.g. `os.path` → `os`)
        root = node
        while isinstance(root, ast.Attribute):
            root = root.value
        if isinstance(root, ast.Name):
            self._used_names.add(root.id)
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Function / class checks
    # ------------------------------------------------------------------
    def _check_function(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> None:
        # Docstring
        if not self._has_docstring(node):
            self._add(
                "warning",
                node.lineno,
                "W001",
                f"Function '{node.name}' is missing a docstring",
            )

        # Length
        body_lines = self._function_body_lines(node)
        if body_lines > self._max_lines:
            self._add(
                "warning",
                node.lineno,
                "W002",
                f"Function '{node.name}' is {body_lines} lines long (limit {self._max_lines})",
            )

        # Argument count
        nargs = self._arg_count(node)
        if nargs > self._max_args:
            self._add(
                "warning",
                node.lineno,
                "W003",
                f"Function '{node.name}' has {nargs} arguments (limit {self._max_args})",
            )

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:  # noqa: N802
        self._check_function(node)
        self.generic_visit(node)

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:  # noqa: N802
        self._check_function(node)
        self.generic_visit(node)

    def visit_ClassDef(self, node: ast.ClassDef) -> None:  # noqa: N802
        if not self._has_docstring(node):
            self._add(
                "info",
                node.lineno,
                "I001",
                f"Class '{node.name}' is missing a docstring",
            )
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Bare except
    # ------------------------------------------------------------------
    def visit_ExceptHandler(self, node: ast.ExceptHandler) -> None:  # noqa: N802
        if node.type is None:
            self._add(
                "error",
                node.lineno,
                "E001",
                "Bare 'except:' clause catches everything including SystemExit and KeyboardInterrupt",
            )
        self.generic_visit(node)

    # ------------------------------------------------------------------
    # Post-pass: resolve unused imports
    # ------------------------------------------------------------------
    def finalize(self, module_node: ast.Module) -> None:
        """Must be called after visit() to emit unused-import findings."""
        # Module docstring
        if not self._has_docstring(module_node):
            self._add("info", 1, "I002", "Module is missing a docstring")

        for name, alias in self._imports.items():
            if name not in self._used_names:
                display = alias.name
                if alias.asname:
                    display = f"{alias.name} as {alias.asname}"
                self._add(
                    "warning",
                    getattr(alias, "lineno", 0),
                    "W004",
                    f"Imported name '{display}' appears unused",
                )


# ---------------------------------------------------------------------------
# Report rendering
# ---------------------------------------------------------------------------
_SEVERITY_COLOR = {
    "error":   "\033[31m",   # red
    "warning": "\033[33m",   # yellow
    "info":    "\033[36m",   # cyan
}
_RESET = "\033[0m"
_BOLD  = "\033[1m"


def _supports_color() -> bool:
    return sys.stdout.isatty()


def render_table(findings: list[Finding], source_path: str) -> None:
    """Print a formatted table of findings to stdout."""
    use_color = _supports_color()

    if not findings:
        msg = f"No findings in {source_path}"
        if use_color:
            msg = f"\033[32m{msg}{_RESET}"
        print(msg)
        return

    sorted_findings = sorted(
        findings,
        key=lambda f: (SEVERITY_ORDER.get(f.severity, 99), f.line),
    )

    # Column widths
    col_sev  = max(len("SEVERITY"),  max(len(f.severity)   for f in sorted_findings))
    col_line = max(len("LINE"),      max(len(str(f.line))  for f in sorted_findings))
    col_code = max(len("CODE"),      max(len(f.code)       for f in sorted_findings))
    col_msg  = max(len("MESSAGE"),   max(len(f.message)    for f in sorted_findings))

    sep_sev  = "-" * col_sev
    sep_line = "-" * col_line
    sep_code = "-" * col_code
    sep_msg  = "-" * col_msg

    header_fmt = f"{{:<{col_sev}}}  {{:>{col_line}}}  {{:<{col_code}}}  {{:<{col_msg}}}"
    row_fmt    = header_fmt

    header = header_fmt.format("SEVERITY", "LINE", "CODE", "MESSAGE")
    sep    = header_fmt.format(sep_sev, sep_line, sep_code, sep_msg)

    title = f"Lint report: {source_path}  ({len(sorted_findings)} finding(s))"
    if use_color:
        title = f"{_BOLD}{title}{_RESET}"

    print()
    print(title)
    print()

    if use_color:
        print(f"{_BOLD}{header}{_RESET}")
    else:
        print(header)
    print(sep)

    for f in sorted_findings:
        row = row_fmt.format(f.severity.upper(), f.line, f.code, f.message)
        if use_color:
            color = _SEVERITY_COLOR.get(f.severity, "")
            print(f"{color}{row}{_RESET}")
        else:
            print(row)

    print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Static lint report for a Python source file.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("file", type=Path, help="Python source file to analyse")
    parser.add_argument(
        "--max-lines",
        type=int,
        default=50,
        metavar="N",
        help="Maximum allowed lines per function body",
    )
    parser.add_argument(
        "--max-args",
        type=int,
        default=5,
        metavar="M",
        help="Maximum allowed arguments per function",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit with code 1 if any finding is present",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    path: Path = args.file

    if not path.exists():
        print(f"error: file not found: {path}", file=sys.stderr)
        sys.exit(2)

    if path.suffix.lower() != ".py":
        print(f"warning: {path} does not have a .py extension, proceeding anyway", file=sys.stderr)

    source = path.read_text(encoding="utf-8")

    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as exc:
        print(f"error: cannot parse {path}: {exc}", file=sys.stderr)
        sys.exit(2)

    source_lines = source.splitlines()

    # Attach line numbers to import alias nodes (Python doesn't do this by default)
    # We walk the tree and copy lineno from Import/ImportFrom to each alias child.
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                alias.lineno = node.lineno  # type: ignore[attr-defined]

    visitor = LintVisitor(source_lines, args.max_lines, args.max_args)
    visitor.visit(tree)
    visitor.finalize(tree)

    render_table(visitor.findings, str(path))

    if args.strict and visitor.findings:
        sys.exit(1)


if __name__ == "__main__":
    main()
