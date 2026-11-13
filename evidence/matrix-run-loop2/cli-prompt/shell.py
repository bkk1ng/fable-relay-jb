#!/usr/bin/env python3
"""
tiny shell — interactive command REPL with readline, history, and tab completion.
Python 3.12, stdlib only.
"""

import json
import os
import readline
import shutil
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

CONFIG_PATH = Path.home() / ".config" / "tiny" / "config.json"
HISTORY_PATH = Path.home() / ".tiny_history"

DEFAULTS: dict = {
    "prompt": "tiny> ",
    "history_length": 1000,
    "ls_show_hidden": False,
    "cat_max_bytes": 1_048_576,  # 1 MiB guard
}


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    if CONFIG_PATH.exists():
        try:
            with CONFIG_PATH.open() as f:
                user = json.load(f)
            if isinstance(user, dict):
                cfg.update(user)
        except (json.JSONDecodeError, OSError) as exc:
            print(f"[warn] could not read config ({exc}); using defaults", file=sys.stderr)
    return cfg


# ---------------------------------------------------------------------------
# Readline history
# ---------------------------------------------------------------------------

def setup_history(history_path: Path, maxlen: int) -> None:
    readline.set_history_length(maxlen)
    if history_path.exists():
        try:
            readline.read_history_file(str(history_path))
        except OSError:
            pass


def save_history(history_path: Path) -> None:
    try:
        history_path.parent.mkdir(parents=True, exist_ok=True)
        readline.write_history_file(str(history_path))
    except OSError as exc:
        print(f"[warn] could not save history: {exc}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Tab completion
# ---------------------------------------------------------------------------

COMMANDS = ["cd", "ls", "cat", "mkdir", "rm", "exit", "help"]


class Completer:
    def __init__(self) -> None:
        self._matches: list[str] = []

    def complete(self, text: str, state: int) -> str | None:
        if state == 0:
            self._matches = self._build_matches(text)
        try:
            return self._matches[state]
        except IndexError:
            return None

    def _build_matches(self, text: str) -> list[str]:
        buf = readline.get_line_buffer()
        tokens = buf.lstrip().split()

        # first token — complete command names
        if not tokens or (len(tokens) == 1 and not buf.endswith(" ")):
            return [c + " " for c in COMMANDS if c.startswith(text)]

        # subsequent tokens — complete filesystem paths
        return self._path_matches(text)

    @staticmethod
    def _path_matches(text: str) -> list[str]:
        if text == "":
            base, prefix = Path("."), ""
        else:
            p = Path(text)
            if text.endswith("/"):
                base, prefix = p, ""
            else:
                base, prefix = p.parent, p.name

        try:
            entries = list(base.iterdir())
        except (PermissionError, FileNotFoundError):
            return []

        results = []
        for entry in entries:
            if entry.name.startswith(prefix):
                stem = str(base / entry.name) if str(base) != "." else entry.name
                # keep original leading ./ or ~/ if present
                if text.startswith("./"):
                    stem = "./" + stem.lstrip("./")
                suffix = "/" if entry.is_dir() else " "
                results.append(stem + suffix)
        return sorted(results)


def setup_completion(completer: Completer) -> None:
    readline.set_completer(completer.complete)
    readline.set_completer_delims(" \t\n;")
    # macOS ships libedit under the readline name; it uses a different bind syntax
    if "libedit" in readline.__doc__ or "":
        readline.parse_and_bind("bind ^I rl_complete")
    else:
        readline.parse_and_bind("tab: complete")


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_cd(args: list[str]) -> None:
    target = args[0] if args else str(Path.home())
    try:
        os.chdir(Path(target).expanduser())
    except FileNotFoundError:
        print(f"cd: no such directory: {target}")
    except NotADirectoryError:
        print(f"cd: not a directory: {target}")
    except PermissionError:
        print(f"cd: permission denied: {target}")


def cmd_ls(args: list[str], show_hidden: bool) -> None:
    target = Path(args[0]).expanduser() if args else Path(".")
    try:
        entries = sorted(target.iterdir(), key=lambda e: (e.is_file(), e.name.lower()))
    except FileNotFoundError:
        print(f"ls: no such directory: {target}")
        return
    except PermissionError:
        print(f"ls: permission denied: {target}")
        return

    rows = []
    for entry in entries:
        if not show_hidden and entry.name.startswith("."):
            continue
        indicator = "/" if entry.is_dir() else ("*" if os.access(entry, os.X_OK) else "")
        try:
            size = entry.stat().st_size if entry.is_file() else 0
        except OSError:
            size = 0
        rows.append((entry.name + indicator, size))

    if not rows:
        return

    name_w = max(len(r[0]) for r in rows)
    for name, size in rows:
        size_str = f"{size:>10}" if size or not name.endswith("/") else " " * 10
        print(f"{name:<{name_w}}  {size_str}")


def cmd_cat(args: list[str], max_bytes: int) -> None:
    if not args:
        print("cat: missing filename")
        return
    for raw in args:
        path = Path(raw).expanduser()
        try:
            size = path.stat().st_size
            if size > max_bytes:
                print(f"cat: {raw}: file too large ({size} bytes; limit {max_bytes})")
                continue
            print(path.read_text(errors="replace"), end="")
        except FileNotFoundError:
            print(f"cat: {raw}: no such file")
        except IsADirectoryError:
            print(f"cat: {raw}: is a directory")
        except PermissionError:
            print(f"cat: {raw}: permission denied")


def cmd_mkdir(args: list[str]) -> None:
    if not args:
        print("mkdir: missing directory name")
        return
    for raw in args:
        path = Path(raw).expanduser()
        try:
            path.mkdir(parents=True, exist_ok=True)
        except PermissionError:
            print(f"mkdir: {raw}: permission denied")
        except OSError as exc:
            print(f"mkdir: {raw}: {exc.strerror}")


def cmd_rm(args: list[str]) -> None:
    if not args:
        print("rm: missing path")
        return
    for raw in args:
        path = Path(raw).expanduser()
        try:
            kind = "directory" if path.is_dir() else "file"
            answer = input(f"rm: remove {kind} '{path}'? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if answer not in ("y", "yes"):
            print(f"rm: skipping '{path}'")
            continue
        try:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
        except FileNotFoundError:
            print(f"rm: {raw}: no such file or directory")
        except PermissionError:
            print(f"rm: {raw}: permission denied")
        except OSError as exc:
            print(f"rm: {raw}: {exc.strerror}")


def cmd_help() -> None:
    print(
        "commands:\n"
        "  cd [dir]         change directory (default: home)\n"
        "  ls [dir]         list directory contents\n"
        "  cat <file...>    print file contents\n"
        "  mkdir <dir...>   create directories (including parents)\n"
        "  rm <path...>     remove file or directory (with confirmation)\n"
        "  exit             quit\n"
        "  help             show this message"
    )


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def run_line(line: str, cfg: dict) -> bool:
    """Parse and dispatch one input line. Returns False to signal exit."""
    tokens = line.split()
    if not tokens:
        return True
    cmd, *args = tokens

    match cmd:
        case "exit":
            return False
        case "help":
            cmd_help()
        case "cd":
            cmd_cd(args)
        case "ls":
            cmd_ls(args, show_hidden=cfg.get("ls_show_hidden", False))
        case "cat":
            cmd_cat(args, max_bytes=cfg.get("cat_max_bytes", DEFAULTS["cat_max_bytes"]))
        case "mkdir":
            cmd_mkdir(args)
        case "rm":
            cmd_rm(args)
        case _:
            print(f"{cmd}: command not found")
    return True


def repl(cfg: dict) -> None:
    prompt = cfg.get("prompt", DEFAULTS["prompt"])
    while True:
        try:
            line = input(prompt).strip()
        except KeyboardInterrupt:
            # Ctrl-C clears the current line; stay in the loop
            print()
            continue
        except EOFError:
            # Ctrl-D exits cleanly
            print()
            break

        if not run_line(line, cfg):
            break


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    cfg = load_config()

    setup_history(HISTORY_PATH, maxlen=cfg.get("history_length", DEFAULTS["history_length"]))

    completer = Completer()
    setup_completion(completer)

    try:
        repl(cfg)
    finally:
        save_history(HISTORY_PATH)


if __name__ == "__main__":
    main()
