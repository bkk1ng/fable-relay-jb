"""
expr.py — arithmetic expression evaluator (Python 3.12, stdlib only)

Supports:
  - Integer and float literals
  - Binary operators: +  -  *  /  //  %  **  (with standard precedence)
  - Unary minus
  - Parentheses
  - Variables  (passed as a dict to evaluate())
  - Function calls: sin, cos, sqrt, min, max  (with arity checks)

Public API
----------
tokenize(text: str) -> list[Token]
parse(text: str) -> ASTNode
evaluate(text: str, variables: dict | None = None) -> float

REPL
----
Run  `python expr.py`  for an interactive prompt.

Doctests
--------
Run  `python -m doctest expr.py -v`  to execute the self-checks embedded below.

>>> evaluate("1 + 2")
3.0
>>> evaluate("2 ** 10")
1024.0
>>> evaluate("-(3 + 4) * 2")
-14.0
>>> evaluate("10 // 3")
3.0
>>> evaluate("10 % 3")
1.0
>>> evaluate("sin(0)")
0.0
>>> evaluate("cos(0)")
1.0
>>> round(evaluate("sqrt(2)"), 10)
1.4142135624
>>> evaluate("min(3, 1, 2)")
1.0
>>> evaluate("max(3, 1, 2)")
3.0
>>> evaluate("x * 2 + 1", {"x": 5})
11.0
>>> evaluate("pi", {"pi": 3.141592653589793})
3.141592653589793
"""

from __future__ import annotations

import math
import re
import sys
from dataclasses import dataclass, field
from typing import Any

# ---------------------------------------------------------------------------
# Token types
# ---------------------------------------------------------------------------

TK_NUMBER = "NUMBER"
TK_NAME   = "NAME"
TK_OP     = "OP"
TK_LPAREN = "LPAREN"
TK_RPAREN = "RPAREN"
TK_COMMA  = "COMMA"
TK_EOF    = "EOF"


@dataclass
class Token:
    kind: str
    value: str
    pos: int   # 0-based character offset in the source string

    def __repr__(self) -> str:
        return f"Token({self.kind}, {self.value!r}, pos={self.pos})"


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"""
    (?P<NUMBER> \d+(?:\.\d*)?(?:[eE][+-]?\d+)? )   # int / float
  | (?P<NAME>   [A-Za-z_][A-Za-z_0-9]* )            # identifier
  | (?P<OP>     \*\*|//|[+\-*/%] )                  # operators (** and // first)
  | (?P<LPAREN> \( )
  | (?P<RPAREN> \) )
  | (?P<COMMA>  , )
  | (?P<SKIP>   [ \t\r\n]+ )                        # whitespace — ignored
  | (?P<MISMATCH> . )                               # anything else → error
    """,
    re.VERBOSE,
)


class ExprError(Exception):
    """Raised for tokenization, parse, or evaluation errors."""
    def __init__(self, message: str, pos: int | None = None) -> None:
        self.pos = pos
        snippet = f" (at position {pos})" if pos is not None else ""
        super().__init__(f"{message}{snippet}")


def tokenize(text: str) -> list[Token]:
    """
    Lex *text* and return a list of Tokens ending with EOF.

    >>> [t.kind for t in tokenize("1 + 2")]
    ['NUMBER', 'OP', 'NUMBER', 'EOF']
    >>> tokenize("3.14")[0].value
    '3.14'
    >>> tokenize("**")[0].value
    '**'
    """
    tokens: list[Token] = []
    for m in _TOKEN_RE.finditer(text):
        kind = m.lastgroup
        value = m.group()
        pos = m.start()
        if kind == "SKIP":
            continue
        if kind == "MISMATCH":
            raise ExprError(f"Unexpected character {value!r}", pos)
        tokens.append(Token(kind, value, pos))  # type: ignore[arg-type]
    tokens.append(Token(TK_EOF, "", len(text)))
    return tokens


# ---------------------------------------------------------------------------
# AST nodes
# ---------------------------------------------------------------------------

@dataclass
class NumberNode:
    value: float
    pos: int


@dataclass
class NameNode:
    name: str
    pos: int


@dataclass
class UnaryNode:
    op: str
    operand: Any   # ASTNode
    pos: int


@dataclass
class BinaryNode:
    op: str
    left: Any      # ASTNode
    right: Any     # ASTNode
    pos: int


@dataclass
class CallNode:
    func: str
    args: list[Any] = field(default_factory=list)   # list[ASTNode]
    pos: int = 0


ASTNode = NumberNode | NameNode | UnaryNode | BinaryNode | CallNode


# ---------------------------------------------------------------------------
# Recursive-descent parser
# ---------------------------------------------------------------------------
#
# Grammar (loosely):
#   expr      → term    { ('+' | '-')   term    }
#   term      → factor  { ('*' | '/' | '//' | '%')  factor }
#   factor    → power
#   power     → unary   { '**' unary }          (right-associative)
#   unary     → '-' unary | primary
#   primary   → NUMBER | NAME ['(' arglist ')'] | '(' expr ')'
#   arglist   → expr { ',' expr }

class _Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    # -- helpers -------------------------------------------------------------

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _consume(self) -> Token:
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
        return self._consume()

    def _match_op(self, *ops: str) -> Token | None:
        tok = self._peek()
        if tok.kind == TK_OP and tok.value in ops:
            return self._consume()
        return None

    # -- grammar rules -------------------------------------------------------

    def parse(self) -> ASTNode:
        node = self._expr()
        if self._peek().kind != TK_EOF:
            tok = self._peek()
            raise ExprError(f"Unexpected token {tok.value!r}", tok.pos)
        return node

    def _expr(self) -> ASTNode:
        node = self._term()
        while True:
            tok = self._match_op("+", "-")
            if tok is None:
                break
            right = self._term()
            node = BinaryNode(tok.value, node, right, tok.pos)
        return node

    def _term(self) -> ASTNode:
        node = self._factor()
        while True:
            tok = self._match_op("*", "/", "//", "%")
            if tok is None:
                break
            right = self._factor()
            node = BinaryNode(tok.value, node, right, tok.pos)
        return node

    def _factor(self) -> ASTNode:
        # Delegate to power; exists as its own level so '*' and '/' bind
        # tighter than '+'/'-' but looser than '**'.
        return self._power()

    def _power(self) -> ASTNode:
        # Right-associative: a ** b ** c == a ** (b ** c)
        base = self._unary()
        tok = self._match_op("**")
        if tok is None:
            return base
        exp = self._power()   # recurse for right-associativity
        return BinaryNode("**", base, exp, tok.pos)

    def _unary(self) -> ASTNode:
        tok = self._match_op("-")
        if tok is not None:
            operand = self._unary()
            return UnaryNode("-", operand, tok.pos)
        return self._primary()

    def _primary(self) -> ASTNode:
        tok = self._peek()

        # Parenthesised sub-expression
        if tok.kind == TK_LPAREN:
            self._consume()
            node = self._expr()
            self._expect(TK_RPAREN)
            return node

        # Number literal
        if tok.kind == TK_NUMBER:
            self._consume()
            return NumberNode(float(tok.value), tok.pos)

        # Name: variable or function call
        if tok.kind == TK_NAME:
            self._consume()
            if self._peek().kind == TK_LPAREN:
                # function call
                self._consume()
                args: list[ASTNode] = []
                if self._peek().kind != TK_RPAREN:
                    args.append(self._expr())
                    while self._peek().kind == TK_COMMA:
                        self._consume()
                        args.append(self._expr())
                self._expect(TK_RPAREN)
                return CallNode(tok.value, args, tok.pos)
            return NameNode(tok.value, tok.pos)

        raise ExprError(
            f"Unexpected token {tok.value!r} in expression",
            tok.pos,
        )


def parse(text: str) -> ASTNode:
    """
    Parse *text* into an AST.

    >>> isinstance(parse("1 + 2"), BinaryNode)
    True
    >>> parse("42").value
    42.0
    >>> parse("-x").op
    '-'
    """
    tokens = tokenize(text)
    return _Parser(tokens).parse()


# ---------------------------------------------------------------------------
# Evaluator
# ---------------------------------------------------------------------------

# Built-in functions: name → (callable, min_arity, max_arity | None)
_BUILTINS: dict[str, tuple[Any, int, int | None]] = {
    "sin":  (math.sin,  1, 1),
    "cos":  (math.cos,  1, 1),
    "sqrt": (math.sqrt, 1, 1),
    "min":  (min,       2, None),   # 2+ args
    "max":  (max,       2, None),
}


def _eval_node(node: ASTNode, variables: dict[str, float]) -> float:
    match node:
        case NumberNode(value=v):
            return v

        case NameNode(name=name, pos=pos):
            if name in variables:
                return float(variables[name])
            raise ExprError(f"Undefined variable {name!r}", pos)

        case UnaryNode(op="-", operand=operand):
            return -_eval_node(operand, variables)

        case BinaryNode(op=op, left=left, right=right, pos=pos):
            lv = _eval_node(left, variables)
            rv = _eval_node(right, variables)
            match op:
                case "+":  return lv + rv
                case "-":  return lv - rv
                case "*":  return lv * rv
                case "/":
                    if rv == 0:
                        raise ExprError("Division by zero", pos)
                    return lv / rv
                case "//":
                    if rv == 0:
                        raise ExprError("Integer division by zero", pos)
                    return float(lv // rv)
                case "%":
                    if rv == 0:
                        raise ExprError("Modulo by zero", pos)
                    return float(lv % rv)
                case "**": return lv ** rv
                case _:
                    raise ExprError(f"Unknown operator {op!r}", pos)

        case CallNode(func=func, args=args, pos=pos):
            if func not in _BUILTINS:
                raise ExprError(f"Unknown function {func!r}", pos)
            fn, min_a, max_a = _BUILTINS[func]
            n = len(args)
            if n < min_a:
                raise ExprError(
                    f"{func}() requires at least {min_a} argument(s), got {n}",
                    pos,
                )
            if max_a is not None and n > max_a:
                raise ExprError(
                    f"{func}() takes at most {max_a} argument(s), got {n}",
                    pos,
                )
            evaluated = [_eval_node(a, variables) for a in args]
            return float(fn(*evaluated))

        case _:
            raise ExprError("Unknown AST node type")


def evaluate(text: str, variables: dict[str, float] | None = None) -> float:
    """
    Tokenize, parse, and evaluate *text*.  Return the result as a float.

    *variables* maps name strings to numeric values.

    >>> evaluate("3 + 4 * 2")
    11.0
    >>> evaluate("(3 + 4) * 2")
    14.0
    >>> evaluate("2 ** 3 ** 2")   # right-associative: 2 ** (3 ** 2) = 512
    512.0
    >>> evaluate("10 / 4")
    2.5
    >>> evaluate("10 // 4")
    2.0
    >>> evaluate("10 % 3")
    1.0
    >>> evaluate("-(-5)")
    5.0
    >>> evaluate("sqrt(9)")
    3.0
    >>> evaluate("min(5, 3, 8, 1)")
    1.0
    >>> evaluate("max(5, 3, 8, 1)")
    8.0
    >>> evaluate("x + y", {"x": 10, "y": 32})
    42.0
    """
    vars_: dict[str, float] = {} if variables is None else variables
    ast = parse(text)
    return _eval_node(ast, vars_)


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

_HELP = """\
expr REPL — arithmetic evaluator
  Operators : +  -  *  /  //  %  **  (  )
  Functions : sin  cos  sqrt  min  max
  Variables : assign with  name = expr
  Commands  : help, vars, quit / exit / Ctrl-D
"""


def _repl() -> None:
    print(_HELP)
    variables: dict[str, float] = {}
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
        if line == "help":
            print(_HELP)
            continue
        if line == "vars":
            if variables:
                for k, v in sorted(variables.items()):
                    print(f"  {k} = {v}")
            else:
                print("  (no variables defined)")
            continue

        # Assignment:  name = expr
        assign_match = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.+)", line)
        if assign_match:
            name, expr_text = assign_match.groups()
            try:
                result = evaluate(expr_text, variables)
                variables[name] = result
                print(f"  {name} = {result}")
            except ExprError as e:
                print(f"  Error: {e}")
            continue

        # Plain expression
        try:
            result = evaluate(line, variables)
            print(f"  {result}")
        except ExprError as e:
            print(f"  Error: {e}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    if len(sys.argv) > 1:
        # Non-interactive: evaluate the first argument and print the result.
        expr_arg = " ".join(sys.argv[1:])
        try:
            print(evaluate(expr_arg))
        except ExprError as err:
            print(f"Error: {err}", file=sys.stderr)
            sys.exit(1)
    else:
        _repl()
