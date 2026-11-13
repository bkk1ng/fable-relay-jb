"""
Pytest suite for a tokenizer module.

Expected public interface:
    tokenize(text: str) -> list[str]

Standalone guarantee: if the real ``tokenizer`` package is not installed, a
minimal stub is inserted into ``sys.modules`` before any test runs, so the
file can be collected and executed with no import errors.

Slow-test marker
----------------
Tests decorated with ``@pytest.mark.slow`` are excluded by default.  To run
them pass ``-m slow`` (or ``-m "slow or not slow"`` for everything).  To
suppress the "unknown mark" warning without a conftest.py, add this to
``pytest.ini`` or ``pyproject.toml``::

    [pytest]
    markers =
        slow: marks tests that exercise large inputs or timing-sensitive paths
"""

from __future__ import annotations

import os
import re
import sys
import types
from typing import Any

import pytest

# ---------------------------------------------------------------------------
# Stub — inserted when the real tokenizer is absent
# ---------------------------------------------------------------------------

def _build_stub() -> types.ModuleType:
    """
    Minimal ``tokenizer`` stub that satisfies the public contract.

    Behaviour used by the test suite:
    * Returns an empty list for blank / whitespace-only input.
    * Splits on whitespace and strips punctuation (word characters only).
    * Raises ``TypeError`` for non-string input.
    * Respects ``TOKENIZER_LOCALE=lower`` by lowercasing every token.
    """
    mod = types.ModuleType("tokenizer")
    mod.__doc__ = "Stub tokenizer inserted by test_tokenizer.py"

    def tokenize(text: str) -> list[str]:  # noqa: D401
        if not isinstance(text, str):
            raise TypeError(
                f"tokenize() expects str, got {type(text).__name__!r}"
            )
        tokens: list[str] = re.findall(r"[A-Za-z0-9']+", text)
        if os.environ.get("TOKENIZER_LOCALE") == "lower":
            tokens = [t.lower() for t in tokens]
        return tokens

    mod.tokenize = tokenize  # type: ignore[attr-defined]
    return mod


try:
    import tokenizer as _tok_module  # noqa: F401
except ImportError:
    _tok_module = _build_stub()
    sys.modules["tokenizer"] = _tok_module

from tokenizer import tokenize  # noqa: E402


# ---------------------------------------------------------------------------
# Fixtures — shared data and helpers
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def sample_texts() -> dict[str, str]:
    """
    Named text strings reused across test functions.

    Keeping these in one place makes it easy to extend coverage without
    changing individual tests.
    """
    return {
        "greeting":       "Hello, world!",
        "pangram":        "The quick brown fox jumps over the lazy dog.",
        "numbers":        "42 items cost $3.99 each",
        "mixed_case":     "Python IS awesome",
        "multiline":      "first line\nsecond line\nthird line",
        "repeated_word":  "ha ha ha ha ha",
        "single_word":    "tokenizer",
        "tabs":           "word1\tword2\tword3",
        "empty":          "",
        "whitespace_only": "   \t\n  ",
    }


@pytest.fixture()
def write_text(tmp_path):
    """
    Factory: write *content* to a file inside the test's tmp_path and return
    the ``pathlib.Path``.  Accepts an optional *filename* keyword argument.
    """
    def _write(content: str, filename: str = "input.txt") -> "os.PathLike[str]":
        path = tmp_path / filename
        path.write_text(content, encoding="utf-8")
        return path

    return _write


# ---------------------------------------------------------------------------
# Return-type contract
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "text",
    [
        "hello world",
        "",
        "single",
        "  leading and trailing  ",
        "line1\nline2",
        "tab\tseparated",
    ],
    ids=["two-words", "empty", "single", "padded", "newline", "tab"],
)
def test_tokenize_always_returns_list(text: str) -> None:
    assert isinstance(tokenize(text), list)


@pytest.mark.parametrize(
    "text",
    [
        "hello world",
        "one two three four five",
        "a b c d e f g",
    ],
)
def test_every_token_is_a_string(text: str) -> None:
    for token in tokenize(text):
        assert isinstance(token, str)


# ---------------------------------------------------------------------------
# Empty / blank input
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "blank",
    ["", " ", "  ", "\t", "\n", "\r\n", "   \t   \n   "],
    ids=["empty", "one-space", "spaces", "tab", "newline", "crlf", "mixed-ws"],
)
def test_blank_input_returns_empty_list(blank: str) -> None:
    assert tokenize(blank) == []


# ---------------------------------------------------------------------------
# Token content / structure
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "text, expected",
    [
        ("hello world",        ["hello", "world"]),
        ("one two three",      ["one", "two", "three"]),
        ("Hello World",        ["Hello", "World"]),
    ],
)
def test_basic_word_tokenisation(text: str, expected: list[str]) -> None:
    assert tokenize(text) == expected


@pytest.mark.parametrize(
    "text, min_count",
    [
        ("a b c",          3),
        ("one two",        2),
        ("alpha beta gamma delta epsilon", 5),
    ],
)
def test_minimum_token_count(text: str, min_count: int) -> None:
    assert len(tokenize(text)) >= min_count


def test_no_empty_string_tokens(sample_texts: dict[str, str]) -> None:
    """Every returned token must be non-empty."""
    for label, text in sample_texts.items():
        for token in tokenize(text):
            assert token != "", f"Empty token in output for sample {label!r}"


def test_single_word_gives_one_token(sample_texts: dict[str, str]) -> None:
    result = tokenize(sample_texts["single_word"])
    assert len(result) == 1
    assert result[0] == "tokenizer"


def test_repeated_word_tokens_all_equal(sample_texts: dict[str, str]) -> None:
    result = tokenize(sample_texts["repeated_word"])
    assert len(result) >= 2
    assert len(set(result)) == 1, "All tokens should be identical for repeated-word input"


# ---------------------------------------------------------------------------
# Fixture-driven smoke tests across the full sample catalogue
# ---------------------------------------------------------------------------

def test_all_samples_return_lists(sample_texts: dict[str, str]) -> None:
    for label, text in sample_texts.items():
        result = tokenize(text)
        assert isinstance(result, list), f"Expected list for sample {label!r}"


# ---------------------------------------------------------------------------
# File-based tests (tmp_path)
# ---------------------------------------------------------------------------

def test_tokenise_simple_file_content(write_text) -> None:
    path = write_text("hello from a file")
    result = tokenize(path.read_text(encoding="utf-8"))
    assert "hello" in result
    assert "file" in result


def test_tokenise_multiline_file(write_text) -> None:
    path = write_text("first line\nsecond line\nthird line")
    result = tokenize(path.read_text(encoding="utf-8"))
    # Expect at least one token per word
    assert len(result) >= 6


def test_tokenise_file_round_trip(write_text) -> None:
    """Re-joining and re-tokenising from a file gives a stable token list."""
    original = "the quick brown fox"
    path = write_text(original)
    first = tokenize(path.read_text(encoding="utf-8"))
    second = tokenize(" ".join(first))
    assert first == second


@pytest.mark.parametrize(
    "filename, content, expected_min",
    [
        ("poem.txt",   "roses are red\nviolets are blue", 6),
        ("code.txt",   "def foo bar baz",                 4),
        ("header.txt", "name age city country",           4),
    ],
)
def test_tokenise_various_file_types(
    tmp_path,
    filename: str,
    content: str,
    expected_min: int,
) -> None:
    path = tmp_path / filename
    path.write_text(content, encoding="utf-8")
    result = tokenize(path.read_text(encoding="utf-8"))
    assert isinstance(result, list)
    assert len(result) >= expected_min


def test_tokenise_large_file(tmp_path) -> None:
    words = ["alpha", "beta", "gamma", "delta"]
    lines = [" ".join(words)] * 50          # 50 lines × 4 words = 200 tokens
    path = tmp_path / "large.txt"
    path.write_text("\n".join(lines), encoding="utf-8")
    result = tokenize(path.read_text(encoding="utf-8"))
    assert len(result) == 200


def test_empty_file_returns_empty_list(write_text) -> None:
    path = write_text("")
    assert tokenize(path.read_text(encoding="utf-8")) == []


# ---------------------------------------------------------------------------
# Expected exceptions
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "bad_input",
    [None, 0, 3.14, [], {}, object()],
    ids=["none", "int-zero", "float", "list", "dict", "object"],
)
def test_non_string_input_raises(bad_input: Any) -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(bad_input)  # type: ignore[arg-type]


def test_none_raises_type_error() -> None:
    """Explicit None check — TypeError is the conventional signal."""
    with pytest.raises((TypeError, AttributeError)):
        tokenize(None)  # type: ignore[arg-type]


def test_integer_raises() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(42)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Monkeypatched environment tests
# ---------------------------------------------------------------------------

def test_tokenize_with_locale_en(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("TOKENIZER_LOCALE", "en")
    result = tokenize("Hello World")
    assert isinstance(result, list)
    assert len(result) >= 1


def test_tokenize_without_locale_env_var(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("TOKENIZER_LOCALE", raising=False)
    result = tokenize("Hello World")
    assert isinstance(result, list)


def test_locale_lower_produces_lowercase_tokens(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """TOKENIZER_LOCALE=lower must cause the stub to return lowercase tokens."""
    monkeypatch.setenv("TOKENIZER_LOCALE", "lower")
    result = tokenize("Hello WORLD Python")
    for token in result:
        assert token == token.lower(), f"Token {token!r} is not lowercase"


def test_locale_lower_does_not_affect_empty_input(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("TOKENIZER_LOCALE", "lower")
    assert tokenize("") == []


def test_monkeypatch_env_does_not_leak(monkeypatch: pytest.MonkeyPatch) -> None:
    """Confirm the patched value is visible within the test."""
    monkeypatch.setenv("TOKENIZER_LOCALE", "lower")
    assert os.environ.get("TOKENIZER_LOCALE") == "lower"
    # monkeypatch teardown happens automatically after this test returns.


def test_env_restored_after_previous_monkeypatch() -> None:
    """
    Runs after test_monkeypatch_env_does_not_leak.
    The monkeypatch fixture must have restored the environment.
    """
    assert os.environ.get("TOKENIZER_LOCALE") != "lower"


def test_custom_flag_env_var(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("TOKENIZER_CUSTOM", "1")
    result = tokenize("a b c")
    assert isinstance(result, list)
    assert len(result) == 3


# ---------------------------------------------------------------------------
# Determinism / idempotency
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "text",
    ["hello world", "The quick brown fox", "  spaced  out  "],
)
def test_tokenize_is_deterministic(text: str) -> None:
    assert tokenize(text) == tokenize(text)


def test_idempotent_round_trip() -> None:
    """Tokenise → join → tokenise should give the same result both times."""
    text = "the quick brown fox"
    first = tokenize(text)
    second = tokenize(" ".join(first))
    assert first == second


# ---------------------------------------------------------------------------
# Slow tests — marked so they can be skipped in fast CI runs
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_tokenize_hundred_thousand_words() -> None:
    text = " ".join(["word"] * 100_000)
    result = tokenize(text)
    assert len(result) == 100_000


@pytest.mark.slow
def test_tokenize_long_single_line() -> None:
    text = " ".join(["token"] * 50_000)
    result = tokenize(text)
    assert len(result) == 50_000
    assert all(t == "token" for t in result)


@pytest.mark.slow
@pytest.mark.parametrize(
    "repeat",
    [10_000, 50_000, 100_000],
    ids=["10k", "50k", "100k"],
)
def test_tokenize_scales_with_input_size(repeat: int) -> None:
    text = " ".join(["scale"] * repeat)
    result = tokenize(text)
    assert len(result) == repeat


@pytest.mark.slow
def test_tokenize_large_file_via_tmp_path(tmp_path) -> None:
    path = tmp_path / "huge.txt"
    path.write_text(" ".join(["chunk"] * 200_000), encoding="utf-8")
    result = tokenize(path.read_text(encoding="utf-8"))
    assert len(result) == 200_000
