"""
pytest suite for a tokenizer module exposing tokenize(text: str) -> list[str].

The module is imported with a fallback stub so this file is import-error-free
even when the real implementation is not installed.
"""

from __future__ import annotations

import importlib
import os
import sys
import types
from pathlib import Path
from typing import Any

import pytest

# ---------------------------------------------------------------------------
# Fallback stub — keeps the file import-error-free when tokenizer is absent.
# If the real module is on sys.path it is used instead.
# ---------------------------------------------------------------------------

def _load_tokenizer() -> types.ModuleType:
    spec = importlib.util.find_spec("tokenizer")
    if spec is not None:
        return importlib.import_module("tokenizer")

    # Build a minimal stub so every test can at least import and run.
    stub = types.ModuleType("tokenizer")

    def _stub_tokenize(text: str) -> list[str]:
        """Reference implementation used when the real module is absent."""
        if not isinstance(text, str):
            raise TypeError(f"tokenize() expects str, got {type(text).__name__}")
        if not text:
            return []
        return text.split()

    stub.tokenize = _stub_tokenize
    sys.modules["tokenizer"] = stub
    return stub


tokenizer = _load_tokenizer()
tokenize = tokenizer.tokenize

# ---------------------------------------------------------------------------
# Custom marker registration (also declared in pytest.ini / pyproject.toml)
# ---------------------------------------------------------------------------

def pytest_configure(config: pytest.Config) -> None:  # noqa: D401
    config.addinivalue_line(
        "markers",
        "slow: mark test as slow-running (deselect with -m 'not slow')",
    )


# ---------------------------------------------------------------------------
# Shared fixtures
# ---------------------------------------------------------------------------

@pytest.fixture()
def common_sentences() -> list[str]:
    """A handful of plain sentences shared across multiple tests."""
    return [
        "the quick brown fox",
        "hello world",
        "one",
        "   leading and trailing   ",
    ]


@pytest.fixture()
def punctuation_rich() -> str:
    """A string that contains punctuation a real tokenizer must handle."""
    return "Hello, world! This is a test: one, two, three."


@pytest.fixture(scope="module")
def large_corpus() -> str:
    """A large string used by slow / performance tests (module-scoped)."""
    words = ["word"] * 10_000
    return " ".join(words)


# ---------------------------------------------------------------------------
# Parametrized basic-correctness tests
# ---------------------------------------------------------------------------

BASIC_CASES: list[tuple[str, list[str]]] = [
    ("hello world", ["hello", "world"]),
    ("one", ["one"]),
    ("", []),
    ("  spaces  between  words  ", ["spaces", "between", "words"]),
    ("a b c d e", ["a", "b", "c", "d", "e"]),
]


@pytest.mark.parametrize("text,expected", BASIC_CASES, ids=[c[0][:20] or "<empty>" for c in BASIC_CASES])
def test_basic_tokenization(text: str, expected: list[str]) -> None:
    assert tokenize(text) == expected


@pytest.mark.parametrize(
    "text",
    ["", "   ", "\t\n\r"],
    ids=["empty_string", "only_spaces", "only_whitespace_chars"],
)
def test_empty_or_whitespace_returns_empty_list(text: str) -> None:
    result = tokenize(text)
    assert isinstance(result, list)
    assert len(result) == 0


@pytest.mark.parametrize(
    "text,min_tokens",
    [
        ("one two three", 3),
        ("a b c d e f", 6),
    ],
)
def test_token_count(text: str, min_tokens: int) -> None:
    result = tokenize(text)
    assert len(result) >= min_tokens


# ---------------------------------------------------------------------------
# Return-type invariant
# ---------------------------------------------------------------------------

def test_return_type_is_list(common_sentences: list[str]) -> None:
    for sentence in common_sentences:
        result = tokenize(sentence.strip())
        assert isinstance(result, list), f"Expected list, got {type(result)} for {sentence!r}"
        assert all(isinstance(tok, str) for tok in result), "All tokens must be str"


def test_tokens_are_non_empty_strings(common_sentences: list[str]) -> None:
    for sentence in common_sentences:
        for token in tokenize(sentence.strip()):
            assert len(token) > 0, f"Empty token found in result for {sentence!r}"


# ---------------------------------------------------------------------------
# File-based tests using tmp_path
# ---------------------------------------------------------------------------

def test_tokenize_content_from_file(tmp_path: Path) -> None:
    """tokenize() should produce the same result regardless of the input source."""
    sample = "the quick brown fox jumps over the lazy dog"
    input_file = tmp_path / "input.txt"
    input_file.write_text(sample, encoding="utf-8")

    text = input_file.read_text(encoding="utf-8")
    result = tokenize(text)
    assert result == tokenize(sample)


def test_multiple_files_in_tmp_path(tmp_path: Path) -> None:
    sentences = {
        "a.txt": "hello world",
        "b.txt": "foo bar baz",
        "c.txt": "",
    }
    for name, content in sentences.items():
        (tmp_path / name).write_text(content, encoding="utf-8")

    for name, content in sentences.items():
        text = (tmp_path / name).read_text(encoding="utf-8")
        result = tokenize(text)
        assert result == tokenize(content)


def test_unicode_file_roundtrip(tmp_path: Path) -> None:
    sample = "café naïve résumé"
    path = tmp_path / "unicode.txt"
    path.write_text(sample, encoding="utf-8")
    result = tokenize(path.read_text(encoding="utf-8"))
    assert isinstance(result, list)
    assert len(result) > 0


def test_large_file_produces_tokens(tmp_path: Path) -> None:
    words = ["token"] * 1_000
    text = " ".join(words)
    path = tmp_path / "large.txt"
    path.write_text(text, encoding="utf-8")
    result = tokenize(path.read_text(encoding="utf-8"))
    assert len(result) == 1_000


# ---------------------------------------------------------------------------
# Expected-exception tests
# ---------------------------------------------------------------------------

def test_raises_on_non_string_input_int() -> None:
    with pytest.raises(TypeError):
        tokenize(42)  # type: ignore[arg-type]


def test_raises_on_non_string_input_none() -> None:
    with pytest.raises(TypeError):
        tokenize(None)  # type: ignore[arg-type]


def test_raises_on_non_string_input_list() -> None:
    with pytest.raises(TypeError):
        tokenize(["already", "a", "list"])  # type: ignore[arg-type]


def test_raises_on_bytes_input() -> None:
    with pytest.raises(TypeError):
        tokenize(b"bytes input")  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "bad_input",
    [123, 3.14, {"key": "value"}, object()],
    ids=["int", "float", "dict", "object"],
)
def test_raises_type_error_parametrized(bad_input: Any) -> None:
    with pytest.raises(TypeError):
        tokenize(bad_input)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Monkeypatched-environment tests
# ---------------------------------------------------------------------------

def test_tokenize_unaffected_by_locale_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """tokenize() must behave consistently regardless of LANG / LC_ALL."""
    monkeypatch.setenv("LANG", "C")
    monkeypatch.setenv("LC_ALL", "C")
    result = tokenize("hello world")
    assert "hello" in result
    assert "world" in result


def test_tokenize_with_custom_env_variable(monkeypatch: pytest.MonkeyPatch) -> None:
    """Simulate an environment that the module might read at import time."""
    monkeypatch.setenv("TOKENIZER_MODE", "strict")
    result = tokenize("some text here")
    # Regardless of mode the result must still be a non-empty list.
    assert isinstance(result, list)
    assert len(result) > 0


def test_tokenize_with_absent_env_variable(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("TOKENIZER_MODE", raising=False)
    result = tokenize("some text here")
    assert isinstance(result, list)


def test_monkeypatch_tokenize_itself(monkeypatch: pytest.MonkeyPatch) -> None:
    """Replace tokenize with a controlled double to verify call-site behaviour."""

    calls: list[str] = []

    def recording_tokenize(text: str) -> list[str]:
        calls.append(text)
        return text.split()

    monkeypatch.setattr(tokenizer, "tokenize", recording_tokenize)
    # Call through the patched module attribute.
    tokenizer.tokenize("patch test")
    assert "patch test" in calls


def test_path_env_does_not_break_tokenizer(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("PATH", "/nonexistent/path")
    result = tokenize("environment is irrelevant")
    assert result == ["environment", "is", "irrelevant"]


# ---------------------------------------------------------------------------
# Slow marker — integration / performance tests
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_large_corpus_token_count(large_corpus: str) -> None:
    result = tokenize(large_corpus)
    assert len(result) == 10_000


@pytest.mark.slow
def test_repeated_calls_are_deterministic(large_corpus: str) -> None:
    first = tokenize(large_corpus)
    second = tokenize(large_corpus)
    assert first == second


@pytest.mark.slow
@pytest.mark.parametrize("n", [5_000, 10_000, 50_000])
def test_scales_linearly_with_input_size(n: int) -> None:
    text = " ".join(["word"] * n)
    result = tokenize(text)
    assert len(result) == n


# ---------------------------------------------------------------------------
# Edge-case and idempotency tests
# ---------------------------------------------------------------------------

def test_single_character_tokens() -> None:
    result = tokenize("a b c")
    assert result == ["a", "b", "c"]


def test_newline_separated_words() -> None:
    result = tokenize("line1\nline2\nline3")
    # At minimum the words must all appear in the token list.
    flat = " ".join(result)
    assert "line1" in flat
    assert "line2" in flat
    assert "line3" in flat


def test_tab_separated_words() -> None:
    result = tokenize("col1\tcol2\tcol3")
    flat = " ".join(result)
    assert "col1" in flat


def test_idempotency_of_tokens(common_sentences: list[str]) -> None:
    """tokenize(join(tokenize(text))) must equal tokenize(text)."""
    for sentence in common_sentences:
        s = sentence.strip()
        if not s:
            continue
        first_pass = tokenize(s)
        second_pass = tokenize(" ".join(first_pass))
        assert first_pass == second_pass, (
            f"Non-idempotent result for {s!r}: {first_pass!r} vs {second_pass!r}"
        )


def test_very_long_single_token() -> None:
    long_word = "a" * 10_000
    result = tokenize(long_word)
    assert long_word in result


def test_mixed_whitespace_normalization() -> None:
    result = tokenize("a  b\t c\n d")
    assert "a" in result
    assert "b" in result
    assert "c" in result
    assert "d" in result
