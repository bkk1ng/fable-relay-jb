"""
expr.py — arithmetic expression tokenizer and recursive-descent evaluator.

Supports:
  - Integer and float literals
  - Operators: + - * / // % ** (with correct precedence)
  - Unary minus and unary plus
  - Parentheses for grouping
  - Variables (looked up in a user-supplied env dict)
  - Function calls: sin, cos, sqrt, min, max (arity-checked)
  - Clear error messages that include the position of the problem

Usage as a library::

    from expr import evaluate
    print(evaluate("2 + 3 * 4"))          # 14
    print(evaluate("x ** 2", {"x": 5}))   # 25

REPL::

    $ python expr.py

Doctests are embedded in each public function and can be run with::

    $ python -m doctest expr.py -v

Grammar (EBNF)::

    expr       = additive
    additive   = multiplicative  (('+' | '-') multiplicative)*
    multiplicative = power  (('*' | '/' | '//' | '%') power)*
    power      = unary  ('**' unary)*          # right-associative
    unary      = ('-' | '+') unary | call
    call       = IDENT '(' arglist ')' | primary
    primary    = NUMBER | IDENT | '(' expr ')'
    arglist    = expr (',' expr)* | ε
"""

from __future__ import annotations

import math
import re
from typing import Any

# ---------------------------------------------------------------------------
# Token types
# ---------------------------------------------------------------------------

TK_NUMBER = "NUMBER"
TK_IDENT  = "IDENT"
TK_OP     = "OP"
TK_LPAREN = "LPAREN"
TK_RPAREN = "RPAREN"
TK_COMMA  = "COMMA"
TK_EOF    = "EOF"

# ---------------------------------------------------------------------------
# Built-in functions and their expected arities
# ---------------------------------------------------------------------------

_BUILTINS: dict[str, tuple[int, Any]] = {
    "sin":  (1, math.sin),
    "cos":  (1, math.cos),
    "sqrt": (1, math.sqrt),
    "min":  (-1, min),   # -1 means variadic (1 or more args)
    "max":  (-1, max),
}

# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class ExprError(ValueError):
    """Raised for any tokenizer or parser error.

    Attributes
    ----------
    message:
        Human-readable description.
    pos:
        0-based character position in the source string where the problem
        was detected, or -1 if not applicable.
    """

    def __init__(self, message: str, pos: int = -1) -> None:
        self.message = message
        self.pos = pos
        loc = f" (position {pos})" if pos >= 0 else ""
        super().__init__(f"{message}{loc}")


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"""
    (?P<NUMBER>  \d+(?:\.\d*)?(?:[eE][+-]?\d+)? )   # int / float / sci
    | (?P<IDENT>   [A-Za-z_][A-Za-z0-9_]*         )   # identifier
    | (?P<OP>      \*\*|//|[+\-*/%]               )   # operators (** and // first)
    | (?P<LPAREN>  \(                              )
    | (?P<RPAREN>  \)                              )
    | (?P<COMMA>   ,                               )
    | (?P<SKIP>    \s+                             )   # ignored whitespace
    | (?P<UNKNOWN> .                               )   # error catch-all
    """,
    re.VERBOSE,
)


class Token:
    __slots__ = ("kind", "value", "pos")

    def __init__(self, kind: str, value: str, pos: int) -> None:
        self.kind  = kind
        self.value = value
        self.pos   = pos

    def __repr__(self) -> str:
        return f"Token({self.kind!r}, {self.value!r}, pos={self.pos})"


def tokenize(source: str) -> list[Token]:
    """Convert *source* into a flat list of tokens ending with EOF.

    Raises :class:`ExprError` on any unrecognised character.

    >>> tokenize("1 + 2")
    [Token('NUMBER', '1', pos=0), Token('OP', '+', pos=2), Token('NUMBER', '2', pos=4), Token('EOF', '', pos=5)]
    >>> tokenize("sin(x)")  # doctest: +NORMALIZE_WHITESPACE
    [Token('IDENT', 'sin', pos=0), Token('LPAREN', '(', pos=3), Token('IDENT', 'x', pos=4), Token('RPAREN', ')', pos=5), Token('EOF', '', pos=6)]
    >>> tokenize("2 @")
    Traceback (most recent call last):
        ...
    expr.ExprError: Unexpected character '@' (position 2)
    """
    tokens: list[Token] = []
    for m in _TOKEN_RE.finditer(source):
        kind = m.lastgroup
        if kind == "SKIP":
            continue
        if kind == "UNKNOWN":
            raise ExprError(f"Unexpected character {m.group()!r}", m.start())
        tokens.append(Token(kind, m.group(), m.start()))  # type: ignore[arg-type]
    tokens.append(Token(TK_EOF, "", len(source)))
    return tokens


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


class Parser:
    """Recursive-descent parser over a token list produced by :func:`tokenize`.

    You rarely need to instantiate this directly — use :func:`evaluate` or
    :func:`parse` instead.
    """

    def __init__(self, tokens: list[Token]) -> None:
        self._tokens = tokens
        self._pos    = 0

    # --- internal helpers ---------------------------------------------------

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _advance(self) -> Token:
        tok = self._tokens[self._pos]
        self._pos += 1
        return tok

    def _expect(self, kind: str, value: str | None = None) -> Token:
        tok = self._peek()
        if tok.kind != kind:
            raise ExprError(
                f"Expected {kind!r} but got {tok.kind!r} ({tok.value!r})",
                tok.pos,
            )
        if value is not None and tok.value != value:
            raise ExprError(
                f"Expected {value!r} but got {tok.value!r}",
                tok.pos,
            )
        return self._advance()

    # --- grammar rules ------------------------------------------------------

    def parse(self) -> "_Node":
        """Parse and return an AST.  Raises ExprError on syntax problems."""
        node = self._additive()
        tok = self._peek()
        if tok.kind != TK_EOF:
            raise ExprError(
                f"Unexpected token {tok.value!r} after expression",
                tok.pos,
            )
        return node

    def _additive(self) -> "_Node":
        node = self._multiplicative()
        while self._peek().kind == TK_OP and self._peek().value in ("+", "-"):
            op  = self._advance()
            rhs = self._multiplicative()
            node = BinOp(op.value, node, rhs, op.pos)
        return node

    def _multiplicative(self) -> "_Node":
        node = self._power()
        while self._peek().kind == TK_OP and self._peek().value in ("*", "/", "//", "%"):
            op  = self._advance()
            rhs = self._power()
            node = BinOp(op.value, node, rhs, op.pos)
        return node

    def _power(self) -> "_Node":
        # Right-associative: a ** b ** c  =>  a ** (b ** c)
        base = self._unary()
        if self._peek().kind == TK_OP and self._peek().value == "**":
            op  = self._advance()
            exp = self._power()       # recursive for right-associativity
            return BinOp("**", base, exp, op.pos)
        return base

    def _unary(self) -> "_Node":
        tok = self._peek()
        if tok.kind == TK_OP and tok.value in ("-", "+"):
            self._advance()
            operand = self._unary()   # allow chaining: --x
            return UnaryOp(tok.value, operand, tok.pos)
        return self._call()

    def _call(self) -> "_Node":
        """Handle function calls; fall through to primary otherwise."""
        tok = self._peek()
        # Only treat as a call if IDENT immediately followed by '('
        if (
            tok.kind == TK_IDENT
            and self._pos + 1 < len(self._tokens)
            and self._tokens[self._pos + 1].kind == TK_LPAREN
        ):
            name_tok = self._advance()   # consume IDENT
            self._advance()              # consume '('
            args: list["_Node"] = []
            if self._peek().kind != TK_RPAREN:
                args.append(self._additive())
                while self._peek().kind == TK_COMMA:
                    self._advance()      # consume ','
                    args.append(self._additive())
            self._expect(TK_RPAREN)
            return FuncCall(name_tok.value, args, name_tok.pos)
        return self._primary()

    def _primary(self) -> "_Node":
        tok = self._peek()
        if tok.kind == TK_NUMBER:
            self._advance()
            # Preserve int type when possible
            value: float | int = float(tok.value) if ("." in tok.value or "e" in tok.value.lower()) else int(tok.value)
            return Literal(value, tok.pos)
        if tok.kind == TK_IDENT:
            self._advance()
            return Variable(tok.value, tok.pos)
        if tok.kind == TK_LPAREN:
            self._advance()
            node = self._additive()
            self._expect(TK_RPAREN)
            return node
        if tok.kind == TK_EOF:
            raise ExprError("Unexpected end of expression", tok.pos)
        raise ExprError(f"Unexpected token {tok.value!r}", tok.pos)


# ---------------------------------------------------------------------------
# AST nodes
# ---------------------------------------------------------------------------


class _Node:
    """Base class for all AST nodes."""

    pos: int

    def eval(self, env: dict[str, Any]) -> float | int:  # pragma: no cover
        raise NotImplementedError


class Literal(_Node):
    __slots__ = ("value", "pos")

    def __init__(self, value: float | int, pos: int) -> None:
        self.value = value
        self.pos   = pos

    def eval(self, env: dict[str, Any]) -> float | int:
        return self.value


class Variable(_Node):
    __slots__ = ("name", "pos")

    def __init__(self, name: str, pos: int) -> None:
        self.name = name
        self.pos  = pos

    def eval(self, env: dict[str, Any]) -> float | int:
        try:
            return env[self.name]
        except KeyError:
            raise ExprError(f"Undefined variable {self.name!r}", self.pos)


class BinOp(_Node):
    __slots__ = ("op", "left", "right", "pos")

    def __init__(self, op: str, left: _Node, right: _Node, pos: int) -> None:
        self.op    = op
        self.left  = left
        self.right = right
        self.pos   = pos

    def eval(self, env: dict[str, Any]) -> float | int:
        lv = self.left.eval(env)
        rv = self.right.eval(env)
        try:
            if self.op == "+":  return lv + rv
            if self.op == "-":  return lv - rv
            if self.op == "*":  return lv * rv
            if self.op == "/":
                if rv == 0:
                    raise ExprError("Division by zero", self.pos)
                return lv / rv
            if self.op == "//":
                if rv == 0:
                    raise ExprError("Floor division by zero", self.pos)
                return lv // rv
            if self.op == "%":
                if rv == 0:
                    raise ExprError("Modulo by zero", self.pos)
                return lv % rv
            if self.op == "**": return lv ** rv
        except ExprError:
            raise
        except Exception as exc:
            raise ExprError(str(exc), self.pos) from exc
        raise ExprError(f"Unknown operator {self.op!r}", self.pos)  # unreachable


class UnaryOp(_Node):
    __slots__ = ("op", "operand", "pos")

    def __init__(self, op: str, operand: _Node, pos: int) -> None:
        self.op      = op
        self.operand = operand
        self.pos     = pos

    def eval(self, env: dict[str, Any]) -> float | int:
        v = self.operand.eval(env)
        return -v if self.op == "-" else v


class FuncCall(_Node):
    __slots__ = ("name", "args", "pos")

    def __init__(self, name: str, args: list[_Node], pos: int) -> None:
        self.name = name
        self.args = args
        self.pos  = pos

    def eval(self, env: dict[str, Any]) -> float | int:
        # Allow user-supplied callables in env to shadow built-ins
        if self.name in env and callable(env[self.name]):
            fn = env[self.name]
            vals = [a.eval(env) for a in self.args]
            try:
                return fn(*vals)
            except Exception as exc:
                raise ExprError(str(exc), self.pos) from exc

        if self.name not in _BUILTINS:
            raise ExprError(f"Unknown function {self.name!r}", self.pos)

        expected_arity, fn = _BUILTINS[self.name]
        vals = [a.eval(env) for a in self.args]

        if expected_arity == -1:
            # variadic — require at least one argument
            if len(vals) == 0:
                raise ExprError(
                    f"{self.name}() requires at least 1 argument, got 0",
                    self.pos,
                )
        else:
            if len(vals) != expected_arity:
                raise ExprError(
                    f"{self.name}() takes {expected_arity} argument(s), got {len(vals)}",
                    self.pos,
                )

        try:
            return fn(*vals)
        except Exception as exc:
            raise ExprError(str(exc), self.pos) from exc


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def parse(source: str) -> _Node:
    """Tokenize and parse *source*, returning the root AST node.

    Raises :class:`ExprError` on any syntax error.

    >>> type(parse("1 + 2")).__name__
    'BinOp'
    >>> type(parse("42")).__name__
    'Literal'
    """
    tokens = tokenize(source)
    return Parser(tokens).parse()


def evaluate(source: str, env: dict[str, Any] | None = None) -> float | int:
    """Evaluate an arithmetic expression string and return its numeric result.

    Parameters
    ----------
    source:
        The expression to evaluate.
    env:
        Optional mapping of variable names to values (or callables for custom
        functions).  Built-in functions (sin, cos, sqrt, min, max) are always
        available and can be shadowed here.

    Returns
    -------
    int if the result is a whole integer with no floating-point operations in
    the path, float otherwise.

    Raises :class:`ExprError` on syntax or semantic errors.

    Basic arithmetic
    ----------------
    >>> evaluate("2 + 3")
    5
    >>> evaluate("10 - 4 * 2")
    2
    >>> evaluate("10 / 4")
    2.5
    >>> evaluate("10 // 3")
    3
    >>> evaluate("10 % 3")
    1
    >>> evaluate("2 ** 10")
    1024

    Precedence and associativity
    ----------------------------
    >>> evaluate("2 + 3 * 4")       # * before +
    14
    >>> evaluate("(2 + 3) * 4")     # parens override
    20
    >>> evaluate("2 ** 3 ** 2")     # right-assoc: 2**(3**2) = 512
    512
    >>> evaluate("-2 ** 2")         # unary minus on result of **
    -4

    Unary operators
    ---------------
    >>> evaluate("-5")
    -5
    >>> evaluate("--5")
    5
    >>> evaluate("+-5")
    -5

    Variables
    ---------
    >>> evaluate("x + y", {"x": 3, "y": 7})
    10
    >>> evaluate("pi * r ** 2", {"pi": 3.14159, "r": 2})
    12.56636

    Built-in functions
    ------------------
    >>> import math
    >>> abs(evaluate("sin(0)") - 0) < 1e-12
    True
    >>> abs(evaluate("cos(0)") - 1) < 1e-12
    True
    >>> evaluate("sqrt(9)")
    3.0
    >>> evaluate("min(3, 1, 4, 1, 5)")
    1
    >>> evaluate("max(3, 1, 4, 1, 5)")
    5

    Error cases
    -----------
    >>> evaluate("1 / 0")
    Traceback (most recent call last):
        ...
    expr.ExprError: Division by zero (position 2)
    >>> evaluate("x", {})
    Traceback (most recent call last):
        ...
    expr.ExprError: Undefined variable 'x' (position 0)
    >>> evaluate("sin(1, 2)")
    Traceback (most recent call last):
        ...
    expr.ExprError: sin() takes 1 argument(s), got 2 (position 0)
    >>> evaluate("max()")
    Traceback (most recent call last):
        ...
    expr.ExprError: max() requires at least 1 argument, got 0 (position 0)
    >>> evaluate("foo()")
    Traceback (most recent call last):
        ...
    expr.ExprError: Unknown function 'foo' (position 0)
    >>> evaluate("2 +")
    Traceback (most recent call last):
        ...
    expr.ExprError: Unexpected end of expression (position 3)
    >>> evaluate("(2 + 3")
    Traceback (most recent call last):
        ...
    expr.ExprError: Expected 'RPAREN' but got 'EOF' ('') (position 6)
    """
    if env is None:
        env = {}
    ast = parse(source)
    return ast.eval(env)


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------


def _repl() -> None:
    """Interactive read-eval-print loop.

    Type an expression and press Enter to evaluate it.
    Variables persist across lines: `x = expr` stores the result.
    Type  quit  or  exit  (or Ctrl-D / Ctrl-C) to leave.
    """
    print("expr REPL  —  type an expression to evaluate it.")
    print("            Assign with  name = expr  to store variables.")
    print("            quit / exit / Ctrl-D to leave.\n")

    env: dict[str, Any] = {}
    # Seed with common constants for convenience
    env["pi"] = math.pi
    env["e"]  = math.e

    _assign_re = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$")

    while True:
        try:
            line = input(">>> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue
        if line.lower() in ("quit", "exit"):
            break

        m = _assign_re.match(line)
        if m:
            var_name, expr_src = m.group(1), m.group(2)
            try:
                result = evaluate(expr_src, env)
                env[var_name] = result
                print(f"{var_name} = {result}")
            except ExprError as exc:
                print(f"Error: {exc}")
        else:
            try:
                result = evaluate(line, env)
                # Store last result as _
                env["_"] = result
                print(result)
            except ExprError as exc:
                # Show a caret under the problem position
                if exc.pos >= 0:
                    print(f"  {line}")
                    print(f"  {' ' * exc.pos}^")
                print(f"Error: {exc.message}")


if __name__ == "__main__":
    import sys

    if "--test" in sys.argv:
        import doctest
        results = doctest.testmod(verbose=True)
        sys.exit(0 if results.failed == 0 else 1)
    else:
        _repl()
