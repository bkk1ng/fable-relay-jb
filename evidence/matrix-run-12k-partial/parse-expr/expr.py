"""
expr.py — arithmetic expression tokenizer and recursive-descent evaluator.

Supports
--------
* Integer and floating-point literals  (including scientific notation)
* Binary operators  + - * /  with standard precedence
* Right-associative exponentiation  ^
* Unary minus (arbitrarily nested:  --x  is x)
* Parentheses
* Named variables supplied as a mapping
* Built-in functions with arity checks:
    sin(x)  cos(x)  sqrt(x)  min(a, b, …)  max(a, b, …)
* Clear error messages that include the source position

Public API
----------
    evaluate(source, variables=None) -> float

Run the interactive REPL::

    python expr.py

Run the built-in doctests::

    python -m doctest expr.py -v
"""

from __future__ import annotations

import enum
import math
import re
import sys
from typing import NamedTuple


# ---------------------------------------------------------------------------
# Token types
# ---------------------------------------------------------------------------

class TT(enum.Enum):
    NUMBER = "NUMBER"
    IDENT  = "IDENT"
    PLUS   = "+"
    MINUS  = "-"
    STAR   = "*"
    SLASH  = "/"
    CARET  = "^"
    LPAREN = "("
    RPAREN = ")"
    COMMA  = ","
    EOF    = "EOF"


class Token(NamedTuple):
    type:  TT
    value: str
    pos:   int   # 0-based index of the first character in the source string


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class ExprError(Exception):
    """Base class for all expression errors; carries the source position."""

    def __init__(self, msg: str, pos: int) -> None:
        super().__init__(msg)
        self.pos = pos

    def pretty(self, source: str) -> str:
        """Return a two-line string with a caret pointing at *pos*.

        >>> e = ParseError("oops", 4)
        >>> print(e.pretty("1 + ^ 2"))
        1 + ^ 2
            ^
        oops
        """
        col = max(0, min(self.pos, len(source)))
        return f"{source}\n{' ' * col}^\n{self}"


class TokenizeError(ExprError):
    """Raised when the tokenizer encounters an unexpected character."""


class ParseError(ExprError):
    """Raised when the parser encounters a structural or semantic error."""


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

_SINGLE_CHAR: dict[str, TT] = {
    "+": TT.PLUS,
    "-": TT.MINUS,
    "*": TT.STAR,
    "/": TT.SLASH,
    "^": TT.CARET,
    "(": TT.LPAREN,
    ")": TT.RPAREN,
    ",": TT.COMMA,
}

# Matches a floating-point literal (integers, decimals, scientific notation).
_NUMBER_RE = re.compile(r"\d+\.?\d*([eE][+-]?\d+)?|\.?\d+([eE][+-]?\d+)?")

# Matches an identifier (function name or variable name).
_IDENT_RE = re.compile(r"[A-Za-z_]\w*")


def tokenize(source: str) -> list[Token]:
    """Convert *source* into a list of :class:`Token` objects.

    The list always ends with a single ``Token(TT.EOF, '', len(source))``.
    Raises :class:`TokenizeError` on unknown characters.

    >>> toks = tokenize("1 + 2.5")
    >>> [(t.type, t.value) for t in toks]
    [(<TT.NUMBER: 'NUMBER'>, '1'), (<TT.PLUS: '+'>, '+'), (<TT.NUMBER: 'NUMBER'>, '2.5'), (<TT.EOF: 'EOF'>, '')]

    >>> toks = tokenize("sin(x)")
    >>> [t.type for t in toks]
    [<TT.IDENT: 'IDENT'>, <TT.LPAREN: '('>, <TT.IDENT: 'IDENT'>, <TT.RPAREN: ')'>, <TT.EOF: 'EOF'>]

    >>> toks = tokenize("1e3")
    >>> toks[0].value
    '1e3'

    >>> try:
    ...     tokenize("1 @ 2")
    ... except TokenizeError as exc:
    ...     "position 2" in str(exc)
    True
    """
    tokens: list[Token] = []
    i = 0
    n = len(source)

    while i < n:
        ch = source[i]

        # Skip whitespace.
        if ch.isspace():
            i += 1
            continue

        # Single-character operators / delimiters.
        if ch in _SINGLE_CHAR:
            tokens.append(Token(_SINGLE_CHAR[ch], ch, i))
            i += 1
            continue

        # Numeric literals.
        m = _NUMBER_RE.match(source, i)
        if m:
            tokens.append(Token(TT.NUMBER, m.group(), i))
            i = m.end()
            continue

        # Identifiers.
        m = _IDENT_RE.match(source, i)
        if m:
            tokens.append(Token(TT.IDENT, m.group(), i))
            i = m.end()
            continue

        raise TokenizeError(
            f"Unknown character {ch!r} at position {i}", i
        )

    tokens.append(Token(TT.EOF, "", n))
    return tokens


# ---------------------------------------------------------------------------
# Built-in functions
# ---------------------------------------------------------------------------

# (min_arity, max_arity_or_None, callable)
# max_arity=None means variadic (any number ≥ min).
_BUILTINS: dict[str, tuple[int, int | None, object]] = {
    "sin":  (1, 1, math.sin),
    "cos":  (1, 1, math.cos),
    "sqrt": (1, 1, math.sqrt),
    "min":  (2, None, min),
    "max":  (2, None, max),
}


# ---------------------------------------------------------------------------
# Recursive-descent parser / evaluator
# ---------------------------------------------------------------------------

class _Parser:
    """
    Internal recursive-descent parser that evaluates on the fly.

    Grammar (tightest binding first)
    ---------------------------------
        expr           := additive
        additive       := multiplicative ( ('+' | '-') multiplicative )*
        multiplicative := unary ( ('*' | '/') unary )*
        unary          := '-' unary | power
        power          := primary ( '^' unary )*     # right-associative
        primary        := NUMBER
                        | '(' expr ')'
                        | IDENT '(' arglist ')'
                        | IDENT
        arglist        := expr ( ',' expr )*
    """

    def __init__(
        self,
        tokens: list[Token],
        source: str,
        variables: dict[str, float],
    ) -> None:
        self._tokens   = tokens
        self._source   = source
        self._vars     = variables
        self._pos      = 0

    # -- helpers -----------------------------------------------------------

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _advance(self) -> Token:
        tok = self._tokens[self._pos]
        self._pos += 1
        return tok

    def _expect(self, tt: TT) -> Token:
        tok = self._peek()
        if tok.type != tt:
            raise ParseError(
                f"Expected '{tt.value}' but got "
                f"{tok.value!r} at position {tok.pos}",
                tok.pos,
            )
        return self._advance()

    # -- grammar rules -----------------------------------------------------

    def parse(self) -> float:
        """Parse and evaluate the entire source; raise on trailing garbage."""
        value = self._expr()
        tok = self._peek()
        if tok.type != TT.EOF:
            raise ParseError(
                f"Unexpected token {tok.value!r} at position {tok.pos}",
                tok.pos,
            )
        return value

    def _expr(self) -> float:
        return self._additive()

    def _additive(self) -> float:
        left = self._multiplicative()
        while self._peek().type in (TT.PLUS, TT.MINUS):
            op = self._advance()
            right = self._multiplicative()
            left = left + right if op.type == TT.PLUS else left - right
        return left

    def _multiplicative(self) -> float:
        left = self._unary()
        while self._peek().type in (TT.STAR, TT.SLASH):
            op = self._advance()
            right = self._unary()
            if op.type == TT.STAR:
                left *= right
            else:
                if right == 0.0:
                    raise ParseError(
                        f"Division by zero at position {op.pos}", op.pos
                    )
                left /= right
        return left

    def _unary(self) -> float:
        if self._peek().type == TT.MINUS:
            self._advance()
            return -self._unary()
        return self._power()

    def _power(self) -> float:
        # Right-associative: recurse into _unary for the exponent so that
        # 2^3^2 == 2^(3^2) == 512 (not (2^3)^2 == 64).
        base = self._primary()
        if self._peek().type == TT.CARET:
            self._advance()
            exp = self._unary()
            return base ** exp
        return base

    def _primary(self) -> float:
        tok = self._peek()

        if tok.type == TT.LPAREN:
            self._advance()
            value = self._expr()
            self._expect(TT.RPAREN)
            return value

        if tok.type == TT.NUMBER:
            self._advance()
            try:
                return float(tok.value)
            except ValueError:
                raise ParseError(
                    f"Malformed number {tok.value!r} at position {tok.pos}",
                    tok.pos,
                )

        if tok.type == TT.IDENT:
            self._advance()
            name = tok.value

            # Function call.
            if self._peek().type == TT.LPAREN:
                return self._call(name, tok.pos)

            # Variable.
            if name in self._vars:
                return float(self._vars[name])

            raise ParseError(
                f"Undefined variable {name!r} at position {tok.pos}",
                tok.pos,
            )

        raise ParseError(
            f"Unexpected token {tok.value!r} at position {tok.pos}",
            tok.pos,
        )

    def _call(self, name: str, name_pos: int) -> float:
        """Parse a parenthesised argument list, check arity, and call *name*."""
        self._expect(TT.LPAREN)
        args: list[float] = []

        if self._peek().type != TT.RPAREN:
            args.append(self._expr())
            while self._peek().type == TT.COMMA:
                self._advance()
                args.append(self._expr())

        self._expect(TT.RPAREN)

        if name not in _BUILTINS:
            raise ParseError(
                f"Unknown function {name!r} at position {name_pos}",
                name_pos,
            )

        min_arity, max_arity, fn = _BUILTINS[name]
        n = len(args)

        if n < min_arity:
            raise ParseError(
                f"{name}() requires at least {min_arity} argument(s), "
                f"got {n} at position {name_pos}",
                name_pos,
            )
        if max_arity is not None and n > max_arity:
            raise ParseError(
                f"{name}() takes at most {max_arity} argument(s), "
                f"got {n} at position {name_pos}",
                name_pos,
            )

        try:
            return float(fn(*args))  # type: ignore[operator]
        except (ValueError, TypeError) as exc:
            raise ParseError(
                f"{name}() error: {exc} at position {name_pos}",
                name_pos,
            ) from exc


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def evaluate(source: str, variables: dict[str, float] | None = None) -> float:
    """Tokenize and evaluate the arithmetic expression in *source*.

    Parameters
    ----------
    source:
        The expression string.
    variables:
        Optional mapping of name → value.  Unknown names raise
        :class:`ParseError`.

    Returns the result as a ``float``.

    Raises :class:`TokenizeError` for unexpected characters and
    :class:`ParseError` for structural or semantic errors; both carry a
    ``pos`` attribute (0-based character index) and a :meth:`~ExprError.pretty`
    method that renders a caret-annotated error string.

    Basic arithmetic and precedence
    --------------------------------
    >>> evaluate("2 + 3 * 4")
    14.0
    >>> evaluate("(2 + 3) * 4")
    20.0
    >>> evaluate("10 / 4")
    2.5
    >>> evaluate("1 + 2 - 3 + 4")
    4.0
    >>> evaluate("12 / 3 / 2")
    2.0

    Exponentiation (right-associative)
    ------------------------------------
    >>> evaluate("2 ^ 10")
    1024.0
    >>> evaluate("2 ^ 3 ^ 2")   # 2^(3^2) = 2^9
    512.0
    >>> evaluate("4 ^ 0.5")
    2.0

    Unary minus
    -----------
    >>> evaluate("-3")
    -3.0
    >>> evaluate("--3")
    3.0
    >>> evaluate("2 ^ -1")
    0.5
    >>> evaluate("-(2 + 3)")
    -5.0

    Variables
    ---------
    >>> evaluate("x + 1", {"x": 7})
    8.0
    >>> evaluate("a * b", {"a": 3, "b": 4})
    12.0
    >>> evaluate("pi", {"pi": 3.141592653589793})  # doctest: +ELLIPSIS
    3.14159...

    Built-in functions
    ------------------
    >>> import math
    >>> abs(evaluate("sin(0)")) < 1e-12
    True
    >>> abs(evaluate("cos(0)") - 1.0) < 1e-12
    True
    >>> evaluate("sqrt(9)")
    3.0
    >>> evaluate("sqrt(2)")  # doctest: +ELLIPSIS
    1.41421...
    >>> evaluate("min(3, 1, 2)")
    1.0
    >>> evaluate("max(3, 1, 2)")
    3.0
    >>> evaluate("max(min(5, 3), 2)")
    3.0

    Error cases
    -----------
    Division by zero:

    >>> try:
    ...     evaluate("1 / 0")
    ... except ParseError as exc:
    ...     "zero" in str(exc)
    True

    Too many arguments:

    >>> try:
    ...     evaluate("sin(1, 2)")
    ... except ParseError as exc:
    ...     "at most 1" in str(exc)
    True

    Too few arguments:

    >>> try:
    ...     evaluate("max(1)")
    ... except ParseError as exc:
    ...     "at least 2" in str(exc)
    True

    Unknown function:

    >>> try:
    ...     evaluate("tan(0)")
    ... except ParseError as exc:
    ...     "Unknown function" in str(exc)
    True

    Undefined variable:

    >>> try:
    ...     evaluate("x")
    ... except ParseError as exc:
    ...     "Undefined variable" in str(exc)
    True

    Trailing garbage:

    >>> try:
    ...     evaluate("1 + 2 )")
    ... except ParseError as exc:
    ...     "Unexpected" in str(exc)
    True

    Missing operand:

    >>> try:
    ...     evaluate("1 + ")
    ... except ParseError as exc:
    ...     "Unexpected" in str(exc)
    True

    Unknown character:

    >>> try:
    ...     evaluate("1 @ 2")
    ... except TokenizeError as exc:
    ...     exc.pos
    2
    """
    tokens = tokenize(source)
    return _Parser(tokens, source, variables or {}).parse()


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

# Pattern for optional assignment:  identifier = expression
_ASSIGN_RE = re.compile(r"^([A-Za-z_]\w*)\s*=(.*)", re.DOTALL)


def _repl() -> None:
    """Interactive read-eval-print loop with persistent variables."""
    try:
        import readline  # noqa: F401 — line editing / history on Unix/macOS
    except ImportError:
        pass

    print("expr REPL  —  type 'quit' or Ctrl-D to exit")
    print("Pre-defined: pi, e, tau, inf")
    print("Assignment:  radius = 5")
    print("Example:     2 * pi * radius")
    print()

    variables: dict[str, float] = {
        "pi":  math.pi,
        "e":   math.e,
        "tau": math.tau,
        "inf": math.inf,
    }

    while True:
        try:
            line = input(">>> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue
        if line.lower() in ("quit", "exit", "q"):
            break

        # Detect optional  name = rhs  assignment syntax.
        m = _ASSIGN_RE.match(line)
        assign_name: str | None = None
        expr_source = line

        if m:
            lhs, rhs = m.group(1), m.group(2).strip()
            # Only treat as assignment when rhs is non-empty.
            if rhs:
                assign_name = lhs
                expr_source = rhs

        try:
            result = evaluate(expr_source, variables)
        except (TokenizeError, ParseError) as exc:
            print(exc.pretty(expr_source))
            continue

        if assign_name is not None:
            variables[assign_name] = result
            print(f"{assign_name} = {result:g}")
        else:
            print(f"{result:g}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    if len(sys.argv) > 1:
        # Evaluate a one-shot expression passed on the command line.
        source = " ".join(sys.argv[1:])
        try:
            print(evaluate(source))
        except (TokenizeError, ParseError) as exc:
            print(exc.pretty(source), file=sys.stderr)
            sys.exit(1)
    else:
        _repl()
