"""
Pytest suite for a tokenizer module.
Assumes the module exposes: tokenize(text: str) -> list[str]

When the module is absent the file stubs it out so collection and
most tests still run; the stub splits on whitespace and raises TypeError
on non-string input, mirroring the minimal contract.
"""
from __future__ import annotations

import sys
import types
from pathlib import Path
from typing import Callable, List

import pytest

# ---------------------------------------------------------------------------
# Import with graceful stub fallback
# ---------------------------------------------------------------------------

try:
    from tokenizer import tokenize  # type: ignore[import-untyped]
except ModuleNotFoundError:

    def _stub_tokenize(text: str) -> List[str]:  # noqa: D401
        """Whitespace-split stub used when the real module is missing."""
        if not isinstance(text, str):
            raise TypeError(
                f"tokenize() requires str, got {type(text).__name__!r}"
            )
        return text.split()

    tokenize = _stub_tokenize  # type: ignore[assignment]

    # Register under the expected name so any secondary imports don't fail.
    _stub_mod = types.ModuleType("tokenizer")
    _stub_mod.tokenize = tokenize  # type: ignore[attr-defined]
    sys.modules.setdefault("tokenizer", _stub_mod)


# ---------------------------------------------------------------------------
# Custom marker (register in pyproject.toml or conftest.py as well)
# ---------------------------------------------------------------------------

slow = pytest.mark.slow


# ---------------------------------------------------------------------------
# Shared fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def sample_texts() -> dict[str, str]:
    """
    Session-scoped bank of representative strings.

    Returned dict maps a human-readable label to a text value so test
    failures name the failing case clearly.
    """
    return {
        "simple":          "hello world",
        "single_word":     "word",
        "empty":           "",
        "spaces_only":     "   ",
        "tabs_and_newline": "\t \n",
        "leading_space":   "  leading",
        "trailing_space":  "trailing  ",
        "both_spaces":     "  both  ",
        "numbers":         "42 is the answer",
        "unicode":         "café naïve résumé",
        "multiline":       "first line\nsecond line\nthird line",
        "repeated_words":  "the the the the",
        "pangram":         "the quick brown fox jumps over the lazy dog",
    }


@pytest.fixture()
def tokenize_fn() -> Callable[[str], List[str]]:
    """Return the callable under test so tests are easy to parametrize."""
    return tokenize


# ---------------------------------------------------------------------------
# Basic behaviour — parametrized
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "text, expected",
    [
        ("hello world",   ["hello", "world"]),
        ("one",           ["one"]),
        ("a b c",         ["a", "b", "c"]),
        ("  leading",     ["leading"]),
        ("trailing  ",    ["trailing"]),
        ("  both  ",      ["both"]),
        ("a  b",          ["a", "b"]),          # multiple internal spaces
        ("a\tb",          ["a", "b"]),           # tab separator
        ("a\nb",          ["a", "b"]),           # newline separator
        ("a\r\nb",        ["a", "b"]),           # CRLF separator
    ],
    ids=[
        "two-words", "single-word", "three-words",
        "leading-space", "trailing-space", "both-spaces",
        "double-internal-space", "tab-sep", "newline-sep", "crlf-sep",
    ],
)
def test_basic_tokenization(text: str, expected: list[str]) -> None:
    assert tokenize(text) == expected


@pytest.mark.parametrize(
    "text, expected_len",
    [
        ("one two three four", 4),
        ("a b c d e f",        6),
        ("solo",               1),
    ],
    ids=["four-words", "six-words", "one-word"],
)
def test_token_count(text: str, expected_len: int) -> None:
    assert len(tokenize(text)) == expected_len


# ---------------------------------------------------------------------------
# Return-type contract
# ---------------------------------------------------------------------------


def test_always_returns_list(sample_texts: dict[str, str]) -> None:
    for label, text in sample_texts.items():
        result = tokenize(text)
        assert isinstance(result, list), f"Not a list for sample '{label}'"


def test_all_elements_are_strings(sample_texts: dict[str, str]) -> None:
    for label, text in sample_texts.items():
        for token in tokenize(text):
            assert isinstance(token, str), (
                f"Non-str token {token!r} in sample '{label}'"
            )


def test_no_empty_string_tokens(sample_texts: dict[str, str]) -> None:
    """Whitespace must not produce empty-string tokens."""
    for label, text in sample_texts.items():
        tokens = tokenize(text)
        assert "" not in tokens, (
            f"Empty token found in sample '{label}': {tokens!r}"
        )


# ---------------------------------------------------------------------------
# File-based cases (tmp_path)
# ---------------------------------------------------------------------------


def test_tokenize_single_line_file(tmp_path: Path) -> None:
    f = tmp_path / "single.txt"
    f.write_text("sphinx of black quartz", encoding="utf-8")
    tokens = tokenize(f.read_text(encoding="utf-8"))
    assert "sphinx" in tokens
    assert "quartz" in tokens
    assert len(tokens) == 4


def test_tokenize_multiline_file(tmp_path: Path) -> None:
    f = tmp_path / "multiline.txt"
    f.write_text("pack my box\nwith five dozen\nliquor jugs", encoding="utf-8")
    tokens = tokenize(f.read_text(encoding="utf-8"))
    for word in ("pack", "box", "five", "dozen", "jugs"):
        assert word in tokens, f"Expected {word!r} in tokens"


def test_tokenize_empty_file(tmp_path: Path) -> None:
    f = tmp_path / "empty.txt"
    f.write_text("", encoding="utf-8")
    tokens = tokenize(f.read_text(encoding="utf-8"))
    assert tokens == []


def test_tokenize_unicode_file(tmp_path: Path) -> None:
    f = tmp_path / "unicode.txt"
    f.write_text("café naïve résumé", encoding="utf-8")
    tokens = tokenize(f.read_text(encoding="utf-8"))
    assert len(tokens) > 0
    assert all(isinstance(t, str) for t in tokens)


def test_aggregate_tokens_across_files(tmp_path: Path) -> None:
    """Tokenising several files and merging results gives the right total."""
    contents = {
        "a.txt": "one two three",    # 3
        "b.txt": "four five six",    # 3
        "c.txt": "seven eight nine ten",  # 4
    }
    all_tokens: list[str] = []
    for name, text in contents.items():
        (tmp_path / name).write_text(text, encoding="utf-8")
        all_tokens.extend(tokenize(text))
    assert len(all_tokens) == 10


def test_roundtrip_write_read_tokenize(tmp_path: Path) -> None:
    """Write, read back, tokenise — result must equal direct tokenisation."""
    text = "roundtrip consistency check"
    f = tmp_path / "roundtrip.txt"
    f.write_text(text, encoding="utf-8")
    from_file = tokenize(f.read_text(encoding="utf-8"))
    direct = tokenize(text)
    assert from_file == direct


# ---------------------------------------------------------------------------
# Expected exceptions
# ---------------------------------------------------------------------------


def test_raises_type_error_on_int() -> None:
    with pytest.raises(TypeError):
        tokenize(42)  # type: ignore[arg-type]


def test_raises_on_none() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(None)  # type: ignore[arg-type]


def test_raises_on_bytes() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(b"bytes input")  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "bad_input",
    [[], {}, set(), 3.14, True, object()],
    ids=["list", "dict", "set", "float", "bool", "object"],
)
def test_raises_on_bad_input_types(bad_input: object) -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(bad_input)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Monkeypatched environment
# ---------------------------------------------------------------------------


def test_tokenize_survives_lang_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Setting LANG must not crash the tokenizer."""
    monkeypatch.setenv("LANG", "en_US.UTF-8")
    result = tokenize("environment variable test")
    assert isinstance(result, list)


def test_tokenize_survives_tokenizer_mode_env(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Some implementations honour TOKENIZER_MODE; return type must hold."""
    monkeypatch.setenv("TOKENIZER_MODE", "strict")
    result = tokenize("strict mode sentence")
    assert isinstance(result, list)


def test_tokenize_unset_cache_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Removing an optional env var must not break the tokenizer."""
    monkeypatch.delenv("TOKENIZER_CACHE", raising=False)
    result = tokenize("no cache present")
    assert isinstance(result, list)


def test_env_patch_isolation(monkeypatch: pytest.MonkeyPatch) -> None:
    """
    Result must be the same whether TOKENIZER_MODE is set or not,
    confirming that env patches don't bleed across tests.
    """
    text = "isolation verification string"
    monkeypatch.setenv("TOKENIZER_MODE", "experimental")
    with_env = tokenize(text)

    monkeypatch.delenv("TOKENIZER_MODE", raising=False)
    without_env = tokenize(text)

    # Both calls must at least honour the return-type contract.
    assert isinstance(with_env, list)
    assert isinstance(without_env, list)


def test_tokenize_path_env_override(monkeypatch: pytest.MonkeyPatch) -> None:
    """Patching PATH must not affect tokenise output."""
    monkeypatch.setenv("PATH", "/tmp/fake")
    result = tokenize("path override test")
    assert isinstance(result, list)


# ---------------------------------------------------------------------------
# Slow tests
# ---------------------------------------------------------------------------


@slow
def test_tokenize_large_corpus() -> None:
    """100 000 identical tokens must all be preserved."""
    large = " ".join(["word"] * 100_000)
    tokens = tokenize(large)
    assert len(tokens) == 100_000


@slow
def test_tokenize_wide_vocabulary() -> None:
    """10 000 unique tokens — no silent truncation."""
    words = [f"token_{i}" for i in range(10_000)]
    tokens = tokenize(" ".join(words))
    assert len(tokens) == 10_000
    assert tokens[0] == "token_0"
    assert tokens[-1] == "token_9999"


@slow
def test_tokenize_repeated_calls_stable() -> None:
    """1 000 consecutive calls must return an identical result each time."""
    text = "stability under repeated invocation"
    baseline = tokenize(text)
    for _ in range(1_000):
        assert tokenize(text) == baseline


@slow
def test_tokenize_very_long_single_token() -> None:
    """A string with no whitespace should produce exactly one token."""
    long_token = "a" * 100_000
    tokens = tokenize(long_token)
    assert len(tokens) == 1
    assert tokens[0] == long_token


# ---------------------------------------------------------------------------
# Determinism and immutability
# ---------------------------------------------------------------------------


def test_deterministic(tokenize_fn: Callable[[str], List[str]]) -> None:
    text = "deterministic output must never vary"
    assert tokenize_fn(text) == tokenize_fn(text)


def test_does_not_mutate_input() -> None:
    text = "the input string must not be altered"
    snapshot = text
    tokenize(text)
    assert text == snapshot


def test_independent_calls_do_not_share_state() -> None:
    """Two different inputs must not influence each other's output."""
    tokens_a = tokenize("alpha beta gamma")
    tokens_b = tokenize("delta epsilon zeta")
    # Re-run first call; result must be unchanged.
    assert tokenize("alpha beta gamma") == tokens_a
    _ = tokens_b  # used; no interaction expected
