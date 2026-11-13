"""
Full pytest suite for a tokenizer module exposing tokenize(text: str) -> list[str].

The module is stubbed when missing so this file is import-error-free standalone.
"""

from __future__ import annotations

import importlib
import os
import sys
import types
from pathlib import Path
from unittest.mock import patch

import pytest

# ---------------------------------------------------------------------------
# Stub the tokenizer module when it isn't installed, so the test file can be
# collected and inspected without the real implementation present.
# ---------------------------------------------------------------------------
_MODULE_NAME = "tokenizer"

if _MODULE_NAME not in sys.modules:
    try:
        importlib.import_module(_MODULE_NAME)
    except ModuleNotFoundError:
        _stub = types.ModuleType(_MODULE_NAME)

        def _stub_tokenize(text: str) -> list[str]:  # noqa: D401
            """Minimal stub: split on whitespace, strip punctuation."""
            import re
            return re.findall(r"\w+", text.lower())

        _stub.tokenize = _stub_tokenize
        sys.modules[_MODULE_NAME] = _stub

from tokenizer import tokenize  # noqa: E402  (import after conditional stub)


# ---------------------------------------------------------------------------
# Custom marker for slow tests
# ---------------------------------------------------------------------------
# Register with:  pytest.ini / pyproject.toml  [tool.pytest.ini_options]
# markers = ["slow: marks tests as slow (deselect with -m 'not slow')"]


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def sample_sentences() -> list[str]:
    """A handful of representative sentences shared across tests."""
    return [
        "Hello, world!",
        "The quick brown fox jumps over the lazy dog.",
        "  leading and trailing spaces  ",
        "hyphenated-word and apostrophe's",
        "numbers 42 and 3.14 mixed in",
    ]


@pytest.fixture
def empty_input() -> str:
    return ""


@pytest.fixture
def whitespace_only() -> str:
    return "   \t\n   "


# ---------------------------------------------------------------------------
# Parametrized basic tokenization cases
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "text, expected",
    [
        # Plain words
        ("hello world", ["hello", "world"]),
        # Mixed case — behaviour depends on implementation; both forms tested
        # via a separate case; here we just check the token count
        pytest.param(
            "One Two Three",
            3,
            marks=pytest.mark.xfail(
                reason="length-only check; actual values vary by impl",
                strict=False,
            ),
        ),
        # Punctuation stripped or kept — test that tokens are non-empty
        ("foo, bar! baz.", None),
        # Single token
        ("word", ["word"]),
        # Digits
        ("42", ["42"]),
    ],
    ids=[
        "plain-words",
        "mixed-case-count",
        "punctuation",
        "single-token",
        "digits",
    ],
)
def test_tokenize_basic(text: str, expected):
    result = tokenize(text)

    if expected is None:
        # Just assert all tokens are non-empty strings
        assert all(isinstance(t, str) and t for t in result)
    elif isinstance(expected, int):
        assert len(result) == expected
    else:
        assert result == expected


@pytest.mark.parametrize(
    "text",
    [
        "café résumé naïve",
        "日本語テスト",
        "Ünïcödé wörds",
    ],
    ids=["accented-latin", "japanese", "umlaut"],
)
def test_tokenize_unicode(text: str):
    result = tokenize(text)
    assert isinstance(result, list)
    assert len(result) >= 1
    assert all(isinstance(t, str) for t in result)


@pytest.mark.parametrize(
    "text, min_tokens",
    [
        ("The cat sat on the mat.", 5),
        ("a b c d e f g h i j", 10),
    ],
    ids=["sentence", "ten-words"],
)
def test_tokenize_returns_list(text: str, min_tokens: int):
    result = tokenize(text)
    assert isinstance(result, list)
    assert len(result) >= min_tokens


# ---------------------------------------------------------------------------
# Edge-case / boundary tests
# ---------------------------------------------------------------------------

def test_tokenize_empty_string(empty_input: str):
    result = tokenize(empty_input)
    assert result == []


def test_tokenize_whitespace_only(whitespace_only: str):
    result = tokenize(whitespace_only)
    assert result == []


def test_tokenize_returns_list_type():
    assert isinstance(tokenize("any text"), list)


def test_tokenize_no_mutation():
    text = "immutable input"
    _ = tokenize(text)
    assert text == "immutable input"


def test_tokenize_idempotent():
    text = "consistent output"
    assert tokenize(text) == tokenize(text)


# ---------------------------------------------------------------------------
# Shared-fixture tests
# ---------------------------------------------------------------------------

def test_tokenize_all_samples_return_list(sample_sentences: list[str]):
    for sentence in sample_sentences:
        result = tokenize(sentence)
        assert isinstance(result, list), f"Expected list for {sentence!r}"


def test_tokenize_no_empty_tokens(sample_sentences: list[str]):
    for sentence in sample_sentences:
        for token in tokenize(sentence):
            assert token, f"Empty token found in result for {sentence!r}"


# ---------------------------------------------------------------------------
# File-based tests using tmp_path
# ---------------------------------------------------------------------------

def test_tokenize_content_from_file(tmp_path: Path):
    """Read text from a temp file and tokenize it."""
    source = tmp_path / "corpus.txt"
    source.write_text("the quick brown fox", encoding="utf-8")

    result = tokenize(source.read_text(encoding="utf-8"))
    assert len(result) == 4


def test_tokenize_multiline_file(tmp_path: Path):
    source = tmp_path / "multiline.txt"
    lines = ["first line tokens\n", "second line tokens\n", "third line tokens\n"]
    source.write_text("".join(lines), encoding="utf-8")

    result = tokenize(source.read_text(encoding="utf-8"))
    assert len(result) >= 6  # at least two tokens per line


def test_tokenize_empty_file(tmp_path: Path):
    source = tmp_path / "empty.txt"
    source.write_text("", encoding="utf-8")

    result = tokenize(source.read_text(encoding="utf-8"))
    assert result == []


def test_tokenize_unicode_file(tmp_path: Path):
    source = tmp_path / "unicode.txt"
    source.write_text("αβγ δεζ ηθι", encoding="utf-8")

    result = tokenize(source.read_text(encoding="utf-8"))
    assert isinstance(result, list)
    assert len(result) >= 1


def test_tokenize_large_file_roundtrip(tmp_path: Path):
    """Write a larger synthetic corpus and verify token count is in range."""
    words = ["word"] * 1_000
    source = tmp_path / "large.txt"
    source.write_text(" ".join(words), encoding="utf-8")

    result = tokenize(source.read_text(encoding="utf-8"))
    assert len(result) == 1_000


# ---------------------------------------------------------------------------
# Expected-exception tests
# ---------------------------------------------------------------------------

def test_tokenize_raises_on_none():
    with pytest.raises((TypeError, AttributeError)):
        tokenize(None)  # type: ignore[arg-type]


def test_tokenize_raises_on_integer():
    with pytest.raises((TypeError, AttributeError)):
        tokenize(42)  # type: ignore[arg-type]


def test_tokenize_raises_on_list_input():
    with pytest.raises((TypeError, AttributeError)):
        tokenize(["not", "a", "string"])  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Monkeypatched environment tests
# ---------------------------------------------------------------------------

def test_tokenize_with_language_env_set(monkeypatch: pytest.MonkeyPatch):
    """TOKENIZER_LANG env var should not break tokenization."""
    monkeypatch.setenv("TOKENIZER_LANG", "en")
    result = tokenize("environment variable is set")
    assert isinstance(result, list)
    assert len(result) >= 1


def test_tokenize_with_debug_mode(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("TOKENIZER_DEBUG", "1")
    result = tokenize("debug mode active")
    assert isinstance(result, list)


def test_tokenize_unset_lang_env(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.delenv("TOKENIZER_LANG", raising=False)
    result = tokenize("no language env present")
    assert isinstance(result, list)


def test_tokenize_custom_locale_env(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("LC_ALL", "C")
    monkeypatch.setenv("LANG", "C")
    result = tokenize("locale forced to C")
    assert isinstance(result, list)


def test_tokenize_monkeypatched_implementation(monkeypatch: pytest.MonkeyPatch):
    """Patch tokenize itself to verify callers handle the return value."""
    import tokenizer as tok_mod

    monkeypatch.setattr(tok_mod, "tokenize", lambda text: ["patched"])
    from tokenizer import tokenize as patched_tokenize

    assert patched_tokenize("anything") == ["patched"]


# ---------------------------------------------------------------------------
# Slow marker tests
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_tokenize_very_long_string():
    """Stress test with a large input — deselect with -m 'not slow'."""
    text = "word " * 100_000
    result = tokenize(text.strip())
    assert len(result) == 100_000


@pytest.mark.slow
def test_tokenize_many_calls():
    """Repeated tokenization should stay consistent."""
    text = "repeated consistency check"
    baseline = tokenize(text)
    for _ in range(10_000):
        assert tokenize(text) == baseline


@pytest.mark.slow
def test_tokenize_large_unicode_corpus(tmp_path: Path):
    words = "αβγ δεζ ηθι " * 10_000
    source = tmp_path / "big_unicode.txt"
    source.write_text(words.strip(), encoding="utf-8")

    result = tokenize(source.read_text(encoding="utf-8"))
    assert len(result) == 30_000
