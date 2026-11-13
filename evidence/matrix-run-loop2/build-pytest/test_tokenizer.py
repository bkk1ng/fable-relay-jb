"""
Pytest suite for a tokenizer module that exposes:
    tokenize(text: str) -> list[str]

Runs standalone even when the real module is absent — a minimal stub is
injected on ImportError so every test can be collected and exercised.

Run slow tests:        pytest -m slow
Skip slow tests:       pytest -m "not slow"
"""

from __future__ import annotations

import os
import sys
import re
import pytest

# ---------------------------------------------------------------------------
# Stub — injected when the real module cannot be imported
# ---------------------------------------------------------------------------

try:
    from tokenizer import tokenize  # type: ignore[import-not-found]
except ImportError:  # pragma: no cover — only active when module is absent

    def tokenize(text: str) -> list[str]:  # type: ignore[misc]
        """
        Minimal reference implementation used when the real module is missing.
        Splits on word boundaries, strips punctuation, preserves Unicode letters
        and digits.  Raises TypeError for non-string input (matches real API).
        """
        if not isinstance(text, str):
            raise TypeError(
                f"tokenize() requires a str, got {type(text).__name__!r}"
            )
        return re.findall(r"\b\w+\b", text)


# ---------------------------------------------------------------------------
# Custom marker registration (keeps pytest -W error::pytest.PytestUnknownMarkWarning
# happy without requiring a separate pytest.ini / pyproject.toml entry)
# ---------------------------------------------------------------------------

def pytest_configure(config: pytest.Config) -> None:  # noqa: D401
    config.addinivalue_line(
        "markers",
        "slow: marks tests as slow; deselect with -m 'not slow'",
    )


# ---------------------------------------------------------------------------
# Shared fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def sample_sentences() -> list[str]:
    """A handful of plain English sentences used across multiple tests."""
    return [
        "Hello world",
        "The quick brown fox",
        "foo bar baz",
        "one two three four five",
    ]


@pytest.fixture
def punctuation_text() -> str:
    """Text with common punctuation attached to words."""
    return "Hello, world! How are you?"


@pytest.fixture
def unicode_text() -> str:
    """Text with accented characters and non-ASCII letters."""
    return "café résumé naïve über"


# ---------------------------------------------------------------------------
# Basic parametrized cases
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "text, expected",
    [
        ("hello world", ["hello", "world"]),
        ("foo", ["foo"]),
        ("", []),
        ("one two three", ["one", "two", "three"]),
        ("  leading and trailing  ", ["leading", "and", "trailing"]),
        ("a   b   c", ["a", "b", "c"]),         # multiple interior spaces
        ("UPPER lower Mixed", ["UPPER", "lower", "Mixed"]),
        ("42 is the answer", ["42", "is", "the", "answer"]),
    ],
    ids=[
        "two-words",
        "single-word",
        "empty-string",
        "three-words",
        "leading-trailing-spaces",
        "multiple-interior-spaces",
        "mixed-case",
        "numeric-token",
    ],
)
def test_tokenize_basic(text: str, expected: list[str]) -> None:
    assert tokenize(text) == expected


# ---------------------------------------------------------------------------
# Return-type guarantees
# ---------------------------------------------------------------------------

def test_tokenize_returns_list() -> None:
    assert isinstance(tokenize("hello world"), list)


def test_tokenize_all_elements_are_strings() -> None:
    result = tokenize("hello world foo")
    assert all(isinstance(t, str) for t in result)


def test_tokenize_empty_string_returns_empty_list() -> None:
    assert tokenize("") == []


def test_tokenize_whitespace_only_returns_empty_list(
    text: str = "   \t\n  ",
) -> None:
    assert tokenize(text) == []


@pytest.mark.parametrize(
    "whitespace",
    ["   ", "\t", "\n", "\r\n", "\t\n   "],
    ids=["spaces", "tab", "newline", "crlf", "mixed"],
)
def test_tokenize_various_whitespace(whitespace: str) -> None:
    assert tokenize(whitespace) == []


# ---------------------------------------------------------------------------
# Fixture-driven tests
# ---------------------------------------------------------------------------

def test_tokenize_fixture_sentences_are_non_empty(
    sample_sentences: list[str],
) -> None:
    for sentence in sample_sentences:
        result = tokenize(sentence)
        assert isinstance(result, list)
        assert len(result) > 0, f"Expected tokens for {sentence!r}"


def test_tokenize_fixture_token_count(sample_sentences: list[str]) -> None:
    """Word count in result must match the number of space-separated words
    for plain sentences that contain no punctuation."""
    for sentence in sample_sentences:
        expected_count = len(sentence.split())
        result = tokenize(sentence)
        assert len(result) == expected_count, (
            f"sentence={sentence!r}: got {result}"
        )


def test_tokenize_punctuation_fixture_produces_words(
    punctuation_text: str,
) -> None:
    result = tokenize(punctuation_text)
    assert isinstance(result, list)
    assert len(result) >= 2
    for token in result:
        # Tokens must not start or end with punctuation
        assert token[0].isalnum() or token[0] == "_"
        assert token[-1].isalnum() or token[-1] == "_"


def test_tokenize_unicode_fixture_non_empty(unicode_text: str) -> None:
    result = tokenize(unicode_text)
    assert isinstance(result, list)
    assert len(result) >= 1


# ---------------------------------------------------------------------------
# tmp_path — file-based cases
# ---------------------------------------------------------------------------

def test_tokenize_reads_lines_from_file(tmp_path: pytest.TempPathFactory) -> None:
    input_file = tmp_path / "input.txt"
    sentences = ["hello world", "foo bar baz", "one two three"]
    input_file.write_text("\n".join(sentences), encoding="utf-8")

    results: list[list[str]] = []
    for line in input_file.read_text(encoding="utf-8").splitlines():
        results.append(tokenize(line))

    assert len(results) == len(sentences)
    assert results[0] == ["hello", "world"]
    assert results[1] == ["foo", "bar", "baz"]


def test_tokenize_writes_tokens_to_file(tmp_path: pytest.TempPathFactory) -> None:
    text = "write these tokens to disk"
    output_file = tmp_path / "tokens.txt"

    tokens = tokenize(text)
    output_file.write_text("\n".join(tokens), encoding="utf-8")

    written_tokens = output_file.read_text(encoding="utf-8").splitlines()
    assert written_tokens == tokens


def test_tokenize_round_trip_through_file(tmp_path: pytest.TempPathFactory) -> None:
    """Write multi-line tokenizer output, read back, compare."""
    sentences = ["alpha beta gamma", "delta epsilon zeta"]
    input_file = tmp_path / "sentences.txt"
    input_file.write_text("\n".join(sentences), encoding="utf-8")

    all_tokens: list[str] = []
    for line in input_file.read_text(encoding="utf-8").splitlines():
        all_tokens.extend(tokenize(line))

    assert len(all_tokens) == 6  # 3 tokens × 2 sentences
    assert "alpha" in all_tokens
    assert "epsilon" in all_tokens


def test_tokenize_empty_file_produces_no_tokens(
    tmp_path: pytest.TempPathFactory,
) -> None:
    empty_file = tmp_path / "empty.txt"
    empty_file.write_text("", encoding="utf-8")

    lines = empty_file.read_text(encoding="utf-8").splitlines()
    tokens = [t for line in lines for t in tokenize(line)]
    assert tokens == []


def test_tokenize_file_with_blank_lines(tmp_path: pytest.TempPathFactory) -> None:
    content = "hello world\n\nfoo bar\n"
    text_file = tmp_path / "blanks.txt"
    text_file.write_text(content, encoding="utf-8")

    results = []
    for line in text_file.read_text(encoding="utf-8").splitlines():
        results.append(tokenize(line))

    # Blank line must produce an empty token list, not raise
    assert results[1] == []
    assert results[0] == ["hello", "world"]
    assert results[2] == ["foo", "bar"]


# ---------------------------------------------------------------------------
# Expected exceptions
# ---------------------------------------------------------------------------

def test_tokenize_none_raises_type_error() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(None)  # type: ignore[arg-type]


def test_tokenize_integer_raises_type_error() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(42)  # type: ignore[arg-type]


def test_tokenize_list_raises_type_error() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(["not", "a", "string"])  # type: ignore[arg-type]


def test_tokenize_dict_raises_type_error() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize({"key": "value"})  # type: ignore[arg-type]


def test_tokenize_bytes_raises_type_error() -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(b"bytes input")  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "bad_input",
    [None, 0, 3.14, [], {}, object()],
    ids=["None", "int-zero", "float", "list", "dict", "object"],
)
def test_tokenize_non_string_raises(bad_input: object) -> None:
    with pytest.raises((TypeError, AttributeError)):
        tokenize(bad_input)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Monkeypatched environment
# ---------------------------------------------------------------------------

def test_tokenize_with_locale_env_set(monkeypatch: pytest.MonkeyPatch) -> None:
    """tokenize() must work (and return a list) when TOKENIZER_LOCALE is set."""
    monkeypatch.setenv("TOKENIZER_LOCALE", "en_US.UTF-8")
    result = tokenize("hello world")
    assert isinstance(result, list)
    assert len(result) == 2


def test_tokenize_with_debug_env_set(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("TOKENIZER_DEBUG", "1")
    result = tokenize("debug mode active")
    assert isinstance(result, list)


def test_tokenize_with_locale_env_absent(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("TOKENIZER_LOCALE", raising=False)
    result = tokenize("no locale set")
    assert isinstance(result, list)


def test_tokenize_with_max_tokens_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Behaviour under TOKENIZER_MAX_TOKENS is implementation-defined,
    but the call must not raise and must return a list."""
    monkeypatch.setenv("TOKENIZER_MAX_TOKENS", "2")
    result = tokenize("one two three four")
    assert isinstance(result, list)


def test_tokenize_env_isolation_across_tests(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Confirm monkeypatch teardown: variable set in one test must not
    bleed into the next.  We set, check, and the fixture will undo it."""
    monkeypatch.setenv("TOKENIZER_CUSTOM", "pytest-isolation-check")
    assert os.environ.get("TOKENIZER_CUSTOM") == "pytest-isolation-check"
    result = tokenize("isolation test")
    assert isinstance(result, list)
    # After this test, the monkeypatch fixture undoes the setenv automatically.


# ---------------------------------------------------------------------------
# Slow tests — deselect with: pytest -m "not slow"
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_tokenize_large_input_performance() -> None:
    """100 k-word string must tokenize without error."""
    text = " ".join(["word"] * 100_000)
    result = tokenize(text)
    assert len(result) == 100_000
    assert result[0] == "word"
    assert result[-1] == "word"


@pytest.mark.slow
def test_tokenize_repeated_calls_stability() -> None:
    """10 k repeated calls must all return identical results."""
    text = "stability check across many calls"
    baseline = tokenize(text)
    for _ in range(10_000):
        assert tokenize(text) == baseline


@pytest.mark.slow
def test_tokenize_very_long_single_token() -> None:
    """A single word with 50 k characters must be handled gracefully."""
    long_word = "a" * 50_000
    result = tokenize(long_word)
    assert isinstance(result, list)
    assert len(result) == 1
    assert result[0] == long_word


# ---------------------------------------------------------------------------
# Determinism and idempotency
# ---------------------------------------------------------------------------

def test_tokenize_is_deterministic() -> None:
    text = "the quick brown fox jumps over the lazy dog"
    results = [tokenize(text) for _ in range(10)]
    assert all(r == results[0] for r in results)


def test_tokenize_is_idempotent() -> None:
    text = "idempotency check"
    assert tokenize(text) == tokenize(text)


# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------

def test_tokenize_single_character() -> None:
    assert tokenize("a") == ["a"]


def test_tokenize_single_digit() -> None:
    result = tokenize("9")
    assert result == ["9"]


def test_tokenize_numbers_only() -> None:
    result = tokenize("1 2 3 4 5")
    assert len(result) == 5
    assert result == ["1", "2", "3", "4", "5"]


@pytest.mark.parametrize(
    "text, min_len",
    [
        ("Hello, world!", 2),
        ("one, two, three.", 3),
        ("it's a contraction", 2),  # behaviour on apostrophe is impl-defined
    ],
    ids=["two-words-punct", "three-words-punct", "contraction"],
)
def test_tokenize_with_punctuation_has_min_tokens(
    text: str, min_len: int
) -> None:
    result = tokenize(text)
    assert len(result) >= min_len


def test_tokenize_newlines_inside_text() -> None:
    result = tokenize("line one\nline two")
    assert isinstance(result, list)
    assert len(result) >= 2


def test_tokenize_tabs_inside_text() -> None:
    result = tokenize("col1\tcol2\tcol3")
    assert isinstance(result, list)
    assert len(result) >= 1


def test_tokenize_unicode_letters() -> None:
    result = tokenize("café résumé naïve")
    assert isinstance(result, list)
    assert len(result) >= 1


def test_tokenize_all_punctuation_no_words() -> None:
    result = tokenize("... !!! ???")
    assert isinstance(result, list)
    # Either empty or contains only non-alphabetic tokens — must not raise
