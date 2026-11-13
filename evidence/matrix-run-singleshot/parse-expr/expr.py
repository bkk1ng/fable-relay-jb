"""
expr.py — arithmetic expression evaluator (Python 3.12, stdlib only)

Supports:
  - Integer and float literals
  - Infix operators: +  -  *  /  //  %  **  (with standard precedence)
  - Unary minus and unary plus
  - Parentheses for grouping
  - Variables (set via Evaluator.env)
  - Function calls: sin, cos, sqrt, min, max  (arity-checked)
  - Clear error messages that include the position of the problem

Quickstart
----------
>>> ev = Evaluator()
>>> ev.eval("2 + 3 * 4")
14.0
>>> ev.env["x"] = 7
>>> ev.eval("x ** 2 - 1")
48.0
>>> ev.eval("sqrt(2)")
1.4142135623730951
>>> ev.eval("min(3, 1, 2)")
1.0

REPL: run this file directly.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Callable


# ---------------------------------------------------------------------------
# Token types
# ---------------------------------------------------------------------------

class TT(Enum):
    NUMBER   = auto()
    IDENT    = auto()   # variable or function name
    PLUS     = auto()
    MINUS    = auto()
    STAR     = auto()
    SLASH    = auto()
    DSLASH   = auto()   # //
    PERCENT  = auto()
    DSTAR    = auto()   # **
    LPAREN   = auto()
    RPAREN   = auto()
    COMMA    = auto()
    EOF      = auto()


@dataclass
class Token:
    kind: TT
    text: str
    pos: int   # 0-based index in the source string


# ---------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"""
    (?P<NUMBER>  \d+(?:\.\d*)?(?:[eE][+-]?\d+)? )
  | (?P<IDENT>   [A-Za-z_][A-Za-z0-9_]* )
  | (?P<DSTAR>   \*\* )
  | (?P<DSLASH>  // )
  | (?P<OP>      [+\-*/%(),] )
  | (?P<SKIP>    \s+ )
  | (?P<BAD>     . )
    """,
    re.VERBOSE,
)

_OP_MAP: dict[str, TT] = {
    "+": TT.PLUS,
    "-": TT.MINUS,
    "*": TT.STAR,
    "/": TT.SLASH,
    "%": TT.PERCENT,
    "(": TT.LPAREN,
    ")": TT.RPAREN,
    ",": TT.COMMA,
}


def tokenize(source: str) -> list[Token]:
    """
    Convert *source* into a flat list of tokens ending with EOF.

    >>> [t.kind for t in tokenize("1 + 2")]
    [<TT.NUMBER: 1>, <TT.PLUS: 3>, <TT.NUMBER: 1>, <TT.EOF: 14>]
    >>> tokenize("1 @")[1].kind
    Traceback (most recent call last):
        ...
    SyntaxError: unexpected character '@' at position 2
    """
    tokens: list[Token] = []
    for m in _TOKEN_RE.finditer(source):
        kind_name = m.lastgroup
        text = m.group()
        pos = m.start()
        if kind_name == "SKIP":
            continue
        if kind_name == "BAD":
            raise SyntaxError(
                f"unexpected character {text!r} at position {pos}"
            )
        if kind_name == "NUMBER":
            tokens.append(Token(TT.NUMBER, text, pos))
        elif kind_name == "IDENT":
            tokens.append(Token(TT.IDENT, text, pos))
        elif kind_name == "DSTAR":
            tokens.append(Token(TT.DSTAR, text, pos))
        elif kind_name == "DSLASH":
            tokens.append(Token(TT.DSLASH, text, pos))
        elif kind_name == "OP":
            tokens.append(Token(_OP_MAP[text], text, pos))
    tokens.append(Token(TT.EOF, "", len(source)))
    return tokens


# ---------------------------------------------------------------------------
# AST nodes  (simple dataclasses — no visitor boilerplate needed here)
# ---------------------------------------------------------------------------

@dataclass
class NumberNode:
    value: float
    pos: int

@dataclass
class VarNode:
    name: str
    pos: int

@dataclass
class UnaryNode:
    op: str          # "+" or "-"
    operand: object
    pos: int

@dataclass
class BinaryNode:
    op: str
    left: object
    right: object
    pos: int

@dataclass
class CallNode:
    name: str
    args: list
    pos: int


# ---------------------------------------------------------------------------
# Parser  (recursive descent)
# ---------------------------------------------------------------------------

class Parser:
    """
    Grammar (highest precedence last in the rule list):

        expr    : add_expr
        add_expr: mul_expr  (( '+' | '-' ) mul_expr)*
        mul_expr: exp_expr  (( '*' | '/' | '//' | '%' ) exp_expr)*
        exp_expr: unary     ( '**' exp_expr )?          # right-associative
        unary   : ( '+' | '-' ) unary
                | primary
        primary : NUMBER
                | IDENT '(' arg_list ')'
                | IDENT
                | '(' expr ')'
        arg_list: expr ( ',' expr )*
                | (empty)
    """

    def __init__(self, tokens: list[Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    # ---- helpers ----------------------------------------------------------

    @property
    def _cur(self) -> Token:
        return self._tokens[self._pos]

    def _peek(self, offset: int = 0) -> Token:
        idx = self._pos + offset
        if idx >= len(self._tokens):
            return self._tokens[-1]   # EOF
        return self._tokens[idx]

    def _eat(self, kind: TT) -> Token:
        tok = self._cur
        if tok.kind != kind:
            raise SyntaxError(
                f"expected {kind.name}, got {tok.text!r} at position {tok.pos}"
            )
        self._pos += 1
        return tok

    def _advance(self) -> Token:
        tok = self._cur
        self._pos += 1
        return tok

    # ---- grammar rules ----------------------------------------------------

    def parse(self) -> object:
        """Entry point; returns AST root and asserts the whole input was consumed."""
        node = self._expr()
        if self._cur.kind != TT.EOF:
            raise SyntaxError(
                f"unexpected token {self._cur.text!r} at position {self._cur.pos}"
            )
        return node

    def _expr(self) -> object:
        return self._add_expr()

    def _add_expr(self) -> object:
        left = self._mul_expr()
        while self._cur.kind in (TT.PLUS, TT.MINUS):
            op_tok = self._advance()
            right = self._mul_expr()
            left = BinaryNode(op_tok.text, left, right, op_tok.pos)
        return left

    def _mul_expr(self) -> object:
        left = self._exp_expr()
        while self._cur.kind in (TT.STAR, TT.SLASH, TT.DSLASH, TT.PERCENT):
            op_tok = self._advance()
            right = self._exp_expr()
            left = BinaryNode(op_tok.text, left, right, op_tok.pos)
        return left

    def _exp_expr(self) -> object:
        base = self._unary()
        if self._cur.kind == TT.DSTAR:
            op_tok = self._advance()
            exp = self._exp_expr()   # right-associative: recurse
            return BinaryNode(op_tok.text, base, exp, op_tok.pos)
        return base

    def _unary(self) -> object:
        if self._cur.kind in (TT.PLUS, TT.MINUS):
            op_tok = self._advance()
            operand = self._unary()
            return UnaryNode(op_tok.text, operand, op_tok.pos)
        return self._primary()

    def _primary(self) -> object:
        tok = self._cur

        if tok.kind == TT.NUMBER:
            self._advance()
            return NumberNode(float(tok.text), tok.pos)

        if tok.kind == TT.IDENT:
            self._advance()
            if self._cur.kind == TT.LPAREN:   # function call
                self._eat(TT.LPAREN)
                args: list = []
                if self._cur.kind != TT.RPAREN:
                    args.append(self._expr())
                    while self._cur.kind == TT.COMMA:
                        self._advance()
                        args.append(self._expr())
                self._eat(TT.RPAREN)
                return CallNode(tok.text, args, tok.pos)
            return VarNode(tok.text, tok.pos)

        if tok.kind == TT.LPAREN:
            self._advance()
            node = self._expr()
            self._eat(TT.RPAREN)
            return node

        if tok.kind == TT.EOF:
            raise SyntaxError(
                f"unexpected end of expression at position {tok.pos}"
            )
        raise SyntaxError(
            f"unexpected token {tok.text!r} at position {tok.pos}"
        )


# ---------------------------------------------------------------------------
# Built-in functions  (name -> (min_arity, max_arity, callable))
# ---------------------------------------------------------------------------

def _builtin_min(*args: float) -> float:
    return float(min(args))

def _builtin_max(*args: float) -> float:
    return float(max(args))

# (min_arity, max_arity)  — None means unlimited
_BUILTINS: dict[str, tuple[int, int | None, Callable]] = {
    "sin":  (1, 1, lambda x: math.sin(x)),
    "cos":  (1, 1, lambda x: math.cos(x)),
    "sqrt": (1, 1, lambda x: math.sqrt(x)),
    "min":  (2, None, _builtin_min),
    "max":  (2, None, _builtin_max),
}


# ---------------------------------------------------------------------------
# Evaluator
# ---------------------------------------------------------------------------

class Evaluator:
    """
    Evaluate arithmetic expression strings.

    Parameters
    ----------
    env : dict[str, float] | None
        Initial variable bindings.  Defaults to an empty dict.

    Examples
    --------
    >>> ev = Evaluator()
    >>> ev.eval("1 + 2")
    3.0
    >>> ev.eval("3 ** 2 ** 2")  # right-assoc: 3 ** (2**2) = 81
    81.0
    >>> ev.eval("10 // 3")
    3.0
    >>> ev.eval("10 % 3")
    1.0
    >>> ev.eval("-(-5)")
    5.0
    >>> ev.env["r"] = 3.0
    >>> import math
    >>> abs(ev.eval("sin(0)")) < 1e-12
    True
    >>> ev.eval("cos(0)")
    1.0
    >>> abs(ev.eval("sqrt(2)") - math.sqrt(2)) < 1e-12
    True
    >>> ev.eval("min(4, 2, 7)")
    2.0
    >>> ev.eval("max(4, 2, 7)")
    7.0
    """

    def __init__(self, env: dict[str, float] | None = None) -> None:
        self.env: dict[str, float] = env if env is not None else {}

    def eval(self, source: str) -> float:
        """
        Parse and evaluate *source*, returning a float.

        Raises
        ------
        SyntaxError
            Tokenisation or parse errors, with the position of the problem.
        NameError
            Reference to an undefined variable, with its position.
        TypeError
            Wrong number of arguments to a built-in, with position info.
        ValueError
            Domain error (e.g. sqrt of a negative), with position info.
        ZeroDivisionError
            Division by zero, with position info.
        """
        tokens = tokenize(source)
        parser = Parser(tokens)
        ast = parser.parse()
        return self._visit(ast)

    # ---- tree walk --------------------------------------------------------

    def _visit(self, node: object) -> float:
        match node:
            case NumberNode(value=v):
                return v

            case VarNode(name=name, pos=pos):
                if name not in self.env:
                    raise NameError(
                        f"undefined variable {name!r} at position {pos}"
                    )
                return float(self.env[name])

            case UnaryNode(op=op, operand=operand):
                v = self._visit(operand)
                return v if op == "+" else -v

            case BinaryNode(op=op, left=left, right=right, pos=pos):
                lv = self._visit(left)
                rv = self._visit(right)
                return self._apply_binary(op, lv, rv, pos)

            case CallNode(name=name, args=args, pos=pos):
                return self._call(name, args, pos)

            case _:
                raise RuntimeError(f"unknown AST node: {node!r}")

    @staticmethod
    def _apply_binary(op: str, lv: float, rv: float, pos: int) -> float:
        try:
            match op:
                case "+":  return lv + rv
                case "-":  return lv - rv
                case "*":  return lv * rv
                case "/":
                    if rv == 0:
                        raise ZeroDivisionError(
                            f"division by zero at position {pos}"
                        )
                    return lv / rv
                case "//":
                    if rv == 0:
                        raise ZeroDivisionError(
                            f"floor division by zero at position {pos}"
                        )
                    return float(int(lv // rv))
                case "%":
                    if rv == 0:
                        raise ZeroDivisionError(
                            f"modulo by zero at position {pos}"
                        )
                    return lv % rv
                case "**": return lv ** rv
                case _:
                    raise RuntimeError(f"unknown operator {op!r}")
        except ZeroDivisionError:
            raise
        except OverflowError:
            raise ValueError(
                f"result overflow for '{op}' at position {pos}"
            )

    def _call(self, name: str, raw_args: list, pos: int) -> float:
        if name not in _BUILTINS:
            raise NameError(
                f"unknown function {name!r} at position {pos}"
            )
        min_arity, max_arity, fn = _BUILTINS[name]
        n = len(raw_args)
        if n < min_arity:
            raise TypeError(
                f"{name}() requires at least {min_arity} argument(s), "
                f"got {n} at position {pos}"
            )
        if max_arity is not None and n > max_arity:
            raise TypeError(
                f"{name}() accepts at most {max_arity} argument(s), "
                f"got {n} at position {pos}"
            )
        args = [self._visit(a) for a in raw_args]
        try:
            result = fn(*args)
        except ValueError as exc:
            raise ValueError(
                f"{name}() domain error at position {pos}: {exc}"
            ) from exc
        return float(result)


# ---------------------------------------------------------------------------
# Self-checks (doctest-style, run via  python -m doctest expr.py -v)
# ---------------------------------------------------------------------------

def _self_checks() -> None:
    """
    Standalone self-checks exercising edge cases beyond the class docstrings.

    >>> ev = Evaluator()

    # --- precedence and associativity ---
    >>> ev.eval("2 + 3 * 4")
    14.0
    >>> ev.eval("(2 + 3) * 4")
    20.0
    >>> ev.eval("2 ** 3 ** 2")   # right-assoc → 2 ** 9 = 512
    512.0
    >>> ev.eval("(2 ** 3) ** 2") # left-forced  → 8 ** 2 = 64
    64.0

    # --- unary minus ---
    >>> ev.eval("-3 ** 2")       # unary minus applies AFTER ** : -(3**2) = -9
    -9.0
    >>> ev.eval("(-3) ** 2")
    9.0

    # --- floor div and modulo ---
    >>> ev.eval("7 // 2")
    3.0
    >>> ev.eval("7 % 2")
    1.0

    # --- variables ---
    >>> ev.env["a"] = 5
    >>> ev.eval("a * a")
    25.0

    # --- nested function calls ---
    >>> import math
    >>> abs(ev.eval("sqrt(sin(0) + cos(0))") - 1.0) < 1e-12
    True

    # --- min / max with many args ---
    >>> ev.eval("min(9, 3, 7, 1, 5)")
    1.0
    >>> ev.eval("max(9, 3, 7, 1, 5)")
    9.0

    # --- error: undefined variable ---
    >>> ev.eval("z + 1")
    Traceback (most recent call last):
        ...
    NameError: undefined variable 'z' at position 0

    # --- error: unknown function ---
    >>> ev.eval("log(10)")
    Traceback (most recent call last):
        ...
    NameError: unknown function 'log' at position 0

    # --- error: arity too low ---
    >>> ev.eval("min(1)")
    Traceback (most recent call last):
        ...
    TypeError: min() requires at least 2 argument(s), got 1 at position 0

    # --- error: arity too high for sin ---
    >>> ev.eval("sin(1, 2)")
    Traceback (most recent call last):
        ...
    TypeError: sin() accepts at most 1 argument(s), got 2 at position 0

    # --- error: division by zero ---
    >>> ev.eval("1 / 0")
    Traceback (most recent call last):
        ...
    ZeroDivisionError: division by zero at position 2

    # --- error: sqrt of negative ---
    >>> ev.eval("sqrt(-1)")
    Traceback (most recent call last):
        ...
    ValueError: sqrt() domain error at position 0: math domain error

    # --- error: bad character ---
    >>> ev.eval("1 $ 2")
    Traceback (most recent call last):
        ...
    SyntaxError: unexpected character '$' at position 2

    # --- error: unmatched paren ---
    >>> ev.eval("(1 + 2")
    Traceback (most recent call last):
        ...
    SyntaxError: expected RPAREN, got '' at position 6
    """


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def _repl() -> None:
    """Interactive read-eval-print loop."""
    import sys

    ev = Evaluator()
    print("expr REPL  —  type 'quit' or Ctrl-D to exit, 'vars' to list variables")
    print("            assign with  name = expression")

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
        if line.lower() == "vars":
            if ev.env:
                for k, v in sorted(ev.env.items()):
                    print(f"  {k} = {v}")
            else:
                print("  (no variables defined)")
            continue

        # Simple assignment:  name = expr
        if "=" in line and re.match(r"^[A-Za-z_]\w*\s*=", line):
            name, _, rhs = line.partition("=")
            name = name.strip()
            try:
                value = ev.eval(rhs.strip())
                ev.env[name] = value
                print(f"  {name} = {value}")
            except (SyntaxError, NameError, TypeError, ValueError, ZeroDivisionError) as exc:
                print(f"  error: {exc}", file=sys.stderr)
            continue

        try:
            result = ev.eval(line)
            print(f"  {result}")
        except (SyntaxError, NameError, TypeError, ValueError, ZeroDivisionError) as exc:
            print(f"  error: {exc}", file=sys.stderr)


if __name__ == "__main__":
    import sys

    if "--test" in sys.argv:
        import doctest
        results = doctest.testmod(verbose=True)
        sys.exit(0 if results.failed == 0 else 1)

    _repl()
