"""
expr.py — arithmetic expression tokenizer and evaluator (Python 3.12, stdlib only)

Supports:
  - Integer and float literals
  - Operators: + - * / // % ** with correct precedence
  - Unary minus
  - Parentheses
  - Variables (assigned in the environment dict)
  - Function calls: sin, cos, sqrt, min, max (with arity checks)
  - Clear error messages that include the position in the input string

Usage:
  result = evaluate("2 + 3 * sin(0)")
  result = evaluate("x ** 2 + y", env={"x": 3, "y": 4})

Doctests:
  >>> evaluate("1 + 2")
  3
  >>> evaluate("2 ** 10")
  1024
  >>> evaluate("10 // 3")
  3
  >>> evaluate("10 % 3")
  1
  >>> evaluate("-(-5)")
  5
  >>> evaluate("2 + 3 * 4")
  14
  >>> evaluate("(2 + 3) * 4")
  20
  >>> evaluate("min(3, 1, 2)")
  1
  >>> evaluate("max(10, 20)")
  20
  >>> import math
  >>> abs(evaluate("sin(0)") - 0.0) < 1e-9
  True
  >>> abs(evaluate("cos(0)") - 1.0) < 1e-9
  True
  >>> abs(evaluate("sqrt(4)") - 2.0) < 1e-9
  True
  >>> evaluate("x + 1", env={"x": 41})
  42
  >>> evaluate("1 / 0")
  Traceback (most recent call last):
      ...
  expr.EvalError: division by zero at position 2
  >>> evaluate("foo(1)")
  Traceback (most recent call last):
      ...
  expr.EvalError: unknown function 'foo' at position 0
  >>> evaluate("sin(1, 2)")
  Traceback (most recent call last):
      ...
  expr.EvalError: 'sin' expects 1 argument, got 2 at position 0
  >>> evaluate("unknownVar")
  Traceback (most recent call last):
      ...
  expr.EvalError: undefined variable 'unknownVar' at position 0
  >>> evaluate("1 +")
  Traceback (most recent call last):
      ...
  expr.ParseError: unexpected end of input at position 3
  >>> evaluate("(1 + 2")
  Traceback (most recent call last):
      ...
  expr.ParseError: expected ')' at position 6
"""

import math
import re
from typing import NamedTuple


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class ExprError(Exception):
    """Base class for all errors raised by this module."""

class ParseError(ExprError):
    """Raised when the input cannot be tokenized or parsed."""

class EvalError(ExprError):
    """Raised when a valid expression cannot be evaluated (e.g. div-by-zero)."""


# ---------------------------------------------------------------------------
# Tokens
# ---------------------------------------------------------------------------

class TokenKind:
    NUMBER  = "NUMBER"
    NAME    = "NAME"
    PLUS    = "+"
    MINUS   = "-"
    STAR    = "*"
    SLASH   = "/"
    DSLASH  = "//"
    PERCENT = "%"
    DSTAR   = "**"
    LPAREN  = "("
    RPAREN  = ")"
    COMMA   = ","
    EOF     = "EOF"


class Token(NamedTuple):
    kind: str
    value: str | float | int
    pos: int  # byte offset in the original source string


# Token patterns ordered so longer operators match before shorter ones.
_TOKEN_RE = re.compile(
    r"""
    (?P<NUMBER>  \d+(?:\.\d*)?(?:[eE][+-]?\d+)? )   # numeric literal
  | (?P<NAME>    [A-Za-z_][A-Za-z_0-9]* )            # identifier or keyword
  | (?P<DSTAR>   \*\* )                              # ** before *
  | (?P<DSLASH>  // )                                # // before /
  | (?P<PLUS>    \+ )
  | (?P<MINUS>   - )
  | (?P<STAR>    \* )
  | (?P<SLASH>   / )
  | (?P<PERCENT> % )
  | (?P<LPAREN>  \( )
  | (?P<RPAREN>  \) )
  | (?P<COMMA>   , )
  | (?P<SKIP>    [ \t\n\r]+ )                        # whitespace — ignored
  | (?P<MISMATCH> . )                                # anything else → error
    """,
    re.VERBOSE,
)


def tokenize(source: str) -> list[Token]:
    """
    Convert *source* into a flat list of :class:`Token` objects followed by
    a sentinel ``EOF`` token.

    Raises :class:`ParseError` on any unrecognised character.

    >>> tokenize("1 + 2")  # doctest: +NORMALIZE_WHITESPACE
    [Token(kind='NUMBER', value=1, pos=0),
     Token(kind='+', value='+', pos=2),
     Token(kind='NUMBER', value=2, pos=4),
     Token(kind='EOF', value='', pos=5)]
    """
    tokens: list[Token] = []
    for m in _TOKEN_RE.finditer(source):
        kind = m.lastgroup
        raw  = m.group()
        pos  = m.start()
        if kind == "SKIP":
            continue
        if kind == "MISMATCH":
            raise ParseError(f"unexpected character {raw!r} at position {pos}")
        if kind == "NUMBER":
            value: str | float | int = float(raw) if ("." in raw or "e" in raw.lower()) else int(raw)
        else:
            value = raw
        tokens.append(Token(kind, value, pos))  # type: ignore[arg-type]
    tokens.append(Token(TokenKind.EOF, "", len(source)))
    return tokens


# ---------------------------------------------------------------------------
# Parser  (recursive descent)
# ---------------------------------------------------------------------------
# Grammar (highest precedence first, right-associative ** noted):
#
#   expr      → additive
#   additive  → multiplicative ( ('+' | '-') multiplicative )*
#   multip    → unary ( ('*' | '/' | '//' | '%') unary )*
#   power     → unary ( '**' power )?          ← right-assoc
#   unary     → '-' unary | primary
#   primary   → NUMBER | NAME '(' arglist ')' | NAME | '(' expr ')'
#   arglist   → expr (',' expr)*

class _Parser:
    """Internal recursive-descent parser.  Not part of the public API."""

    def __init__(self, tokens: list[Token]) -> None:
        self._tokens = tokens
        self._pos    = 0

    # --- helpers ---

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _advance(self) -> Token:
        tok = self._tokens[self._pos]
        self._pos += 1
        return tok

    def _expect(self, kind: str) -> Token:
        tok = self._peek()
        if tok.kind != kind:
            if tok.kind == TokenKind.EOF:
                raise ParseError(f"unexpected end of input at position {tok.pos}")
            raise ParseError(f"expected {kind!r} but got {tok.value!r} at position {tok.pos}")
        return self._advance()

    # --- grammar rules ---

    def parse(self):
        """Parse a full expression and verify no trailing tokens remain."""
        node = self._additive()
        eof  = self._peek()
        if eof.kind != TokenKind.EOF:
            raise ParseError(f"unexpected token {eof.value!r} at position {eof.pos}")
        return node

    def _additive(self):
        left = self._multiplicative()
        while self._peek().kind in (TokenKind.PLUS, TokenKind.MINUS):
            op  = self._advance()
            rhs = self._multiplicative()
            left = ("binop", op.kind, op.pos, left, rhs)
        return left

    def _multiplicative(self):
        left = self._power()
        while self._peek().kind in (TokenKind.STAR, TokenKind.SLASH,
                                    TokenKind.DSLASH, TokenKind.PERCENT):
            op  = self._advance()
            rhs = self._power()
            left = ("binop", op.kind, op.pos, left, rhs)
        return left

    def _power(self):
        """Right-associative exponentiation."""
        base = self._unary()
        if self._peek().kind == TokenKind.DSTAR:
            op  = self._advance()
            exp = self._power()           # right-recursive for right-assoc
            return ("binop", op.kind, op.pos, base, exp)
        return base

    def _unary(self):
        if self._peek().kind == TokenKind.MINUS:
            op  = self._advance()
            operand = self._unary()       # allow chained unary, e.g. --x
            return ("unary", op.pos, operand)
        return self._primary()

    def _primary(self):
        tok = self._peek()

        # Numeric literal
        if tok.kind == TokenKind.NUMBER:
            self._advance()
            return ("num", tok.pos, tok.value)

        # Identifier: either a function call or a variable reference
        if tok.kind == TokenKind.NAME:
            self._advance()
            name = tok.value
            pos  = tok.pos
            if self._peek().kind == TokenKind.LPAREN:
                self._advance()          # consume '('
                args: list = []
                if self._peek().kind != TokenKind.RPAREN:
                    args.append(self._additive())
                    while self._peek().kind == TokenKind.COMMA:
                        self._advance()
                        args.append(self._additive())
                self._expect(TokenKind.RPAREN)
                return ("call", pos, name, args)
            return ("var", pos, name)

        # Parenthesised sub-expression
        if tok.kind == TokenKind.LPAREN:
            self._advance()
            node = self._additive()
            close = self._peek()
            if close.kind != TokenKind.RPAREN:
                raise ParseError(f"expected ')' at position {close.pos}")
            self._advance()
            return node

        if tok.kind == TokenKind.EOF:
            raise ParseError(f"unexpected end of input at position {tok.pos}")
        raise ParseError(f"unexpected token {tok.value!r} at position {tok.pos}")


# ---------------------------------------------------------------------------
# Evaluator
# ---------------------------------------------------------------------------

# Built-in functions: name → (callable, min_arity, max_arity | None)
# None for max_arity means variadic (no upper bound).
_BUILTINS: dict[str, tuple] = {
    "sin":  (math.sin,  1, 1),
    "cos":  (math.cos,  1, 1),
    "sqrt": (math.sqrt, 1, 1),
    "min":  (min,       2, None),
    "max":  (max,       2, None),
}


def _eval_node(node, env: dict[str, float | int]) -> float | int:
    """Recursively evaluate an AST node produced by :class:`_Parser`."""
    match node:
        case ("num", _, value):
            return value

        case ("var", pos, name):
            if name in env:
                return env[name]
            raise EvalError(f"undefined variable {name!r} at position {pos}")

        case ("unary", _, operand):
            return -_eval_node(operand, env)

        case ("binop", op, pos, left, right):
            lv = _eval_node(left,  env)
            rv = _eval_node(right, env)
            match op:
                case "+":  return lv + rv
                case "-":  return lv - rv
                case "*":  return lv * rv
                case "/":
                    if rv == 0:
                        raise EvalError(f"division by zero at position {pos}")
                    return lv / rv
                case "//":
                    if rv == 0:
                        raise EvalError(f"division by zero at position {pos}")
                    return lv // rv
                case "%":
                    if rv == 0:
                        raise EvalError(f"division by zero at position {pos}")
                    return lv % rv
                case "**": return lv ** rv
                case _:    raise EvalError(f"unknown operator {op!r} at position {pos}")

        case ("call", pos, name, args):
            if name not in _BUILTINS:
                raise EvalError(f"unknown function {name!r} at position {pos}")
            fn, min_a, max_a = _BUILTINS[name]
            n = len(args)
            if n < min_a or (max_a is not None and n > max_a):
                if min_a == max_a:
                    raise EvalError(
                        f"{name!r} expects {min_a} argument{'s' if min_a != 1 else ''}, "
                        f"got {n} at position {pos}"
                    )
                bound = f"at least {min_a}"
                raise EvalError(
                    f"{name!r} expects {bound} argument{'s' if min_a != 1 else ''}, "
                    f"got {n} at position {pos}"
                )
            evaluated = [_eval_node(a, env) for a in args]
            try:
                return fn(*evaluated)
            except Exception as exc:
                raise EvalError(f"error in {name!r} at position {pos}: {exc}") from exc

        case _:
            raise EvalError(f"unknown AST node: {node!r}")


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def evaluate(source: str, *, env: dict[str, float | int] | None = None) -> float | int:
    """
    Tokenize, parse, and evaluate an arithmetic expression string.

    Parameters
    ----------
    source : str
        The expression to evaluate.
    env : dict, optional
        A mapping of variable names to numeric values.  Defaults to an empty
        dict, so any variable reference raises :class:`EvalError`.

    Returns
    -------
    int or float

    Raises
    ------
    ParseError
        If the input cannot be tokenized or parsed.
    EvalError
        If the expression is syntactically valid but cannot be evaluated
        (division by zero, unknown variable, bad arity, etc.).

    Examples
    --------
    >>> evaluate("2 + 3")
    5
    >>> evaluate("2 ** 8")
    256
    >>> evaluate("sqrt(9)")
    3.0
    >>> evaluate("pi * r**2", env={"pi": 3.14159265, "r": 5})
    78.53981625
    """
    if env is None:
        env = {}
    tokens = tokenize(source)
    parser = _Parser(tokens)
    ast    = parser.parse()
    return _eval_node(ast, env)


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def _repl() -> None:
    """
    Interactive read-eval-print loop.

    Special commands:
      vars          — list all currently defined variables
      <name> = <expr>  — assign a variable
      quit / exit   — exit the REPL
    """
    print("expr REPL  (type 'quit' or 'exit' to leave, 'vars' to list variables)")
    env: dict[str, float | int] = {}

    while True:
        try:
            line = input(">>> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue

        if line in ("quit", "exit"):
            break

        if line == "vars":
            if env:
                for k, v in sorted(env.items()):
                    print(f"  {k} = {v}")
            else:
                print("  (no variables defined)")
            continue

        # Assignment:  name = expr
        assign_match = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.+)", line)
        if assign_match:
            name, expr_src = assign_match.group(1), assign_match.group(2)
            try:
                result = evaluate(expr_src, env=env)
                env[name] = result
                print(f"  {name} = {result}")
            except ExprError as exc:
                print(f"  Error: {exc}")
            continue

        # Plain expression
        try:
            result = evaluate(line, env=env)
            print(f"  {result}")
        except ExprError as exc:
            print(f"  Error: {exc}")


# ---------------------------------------------------------------------------
# Self-checks (run with: python -m doctest expr.py -v)
# ---------------------------------------------------------------------------

def _run_self_checks() -> None:
    """Run a small suite of inline assertions to smoke-test the module."""
    def approx(a, b, tol=1e-9):
        return abs(a - b) < tol

    # Basic arithmetic
    assert evaluate("1 + 2")           == 3
    assert evaluate("10 - 3")          == 7
    assert evaluate("4 * 5")           == 20
    assert evaluate("7 / 2")           == 3.5
    assert evaluate("7 // 2")          == 3
    assert evaluate("7 % 3")           == 1
    assert evaluate("2 ** 10")         == 1024

    # Precedence
    assert evaluate("2 + 3 * 4")       == 14
    assert evaluate("(2 + 3) * 4")     == 20
    assert evaluate("2 ** 3 ** 2")     == 512   # right-assoc: 2**(3**2)

    # Unary minus
    assert evaluate("-3")              == -3
    assert evaluate("--3")             == 3
    assert evaluate("-(-5)")           == 5
    assert evaluate("2 * -3")          == -6

    # Variables
    assert evaluate("x + 1",   env={"x": 41})     == 42
    assert evaluate("x * y",   env={"x": 6, "y": 7}) == 42

    # Functions
    assert approx(evaluate("sin(0)"),    0.0)
    assert approx(evaluate("cos(0)"),    1.0)
    assert approx(evaluate("sqrt(16)"),  4.0)
    assert evaluate("min(3, 1, 2)")    == 1
    assert evaluate("max(1, 100, 50)") == 100

    # Error cases
    import traceback

    def raises(kind, expr, env=None):
        try:
            evaluate(expr, env=env or {})
            return False
        except kind:
            return True
        except Exception:
            return False

    assert raises(EvalError,  "1 / 0")
    assert raises(EvalError,  "1 // 0")
    assert raises(EvalError,  "1 % 0")
    assert raises(EvalError,  "foo(1)")
    assert raises(EvalError,  "sin(1, 2)")
    assert raises(EvalError,  "min(1)")
    assert raises(EvalError,  "unknownVar")
    assert raises(ParseError, "1 +")
    assert raises(ParseError, "(1 + 2")
    assert raises(ParseError, "1 2")
    assert raises(ParseError, "@")

    print("All self-checks passed.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        _run_self_checks()
    elif "--doctest" in sys.argv:
        import doctest
        results = doctest.testmod(verbose=True)
        sys.exit(0 if results.failed == 0 else 1)
    else:
        _repl()
