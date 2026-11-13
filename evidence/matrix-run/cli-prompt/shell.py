#!/usr/bin/env python3
"""
tiny shell — interactive REPL with readline, history, tab completion,
and a small set of built-in commands. Config from ~/.config/tiny/config.json.
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
    "cat_max_bytes": 1_048_576,  # 1 MiB
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
            print(f"[warn] could not read config: {exc}", file=sys.stderr)
    return cfg


# ---------------------------------------------------------------------------
# Tab completion
# ---------------------------------------------------------------------------

COMMANDS = ["cd", "ls", "cat", "mkdir", "rm", "exit", "help"]


def _path_completions(text: str) -> list[str]:
    """Return filesystem completions for a partial path."""
    if not text:
        entries = list(Path(".").iterdir())
    else:
        p = Path(text)
        parent = p.parent if p.parent != Path("") else Path(".")
        try:
            entries = [
                child
                for child in parent.iterdir()
                if child.name.startswith(p.name)
            ]
        except (OSError, PermissionError):
            return []

    completions = []
    for entry in entries:
        # Reconstruct relative to the original partial text
        base = str(Path(text).parent) if "/" in text else ""
        name = str(entry) if not base else str(entry)
        suffix = "/" if entry.is_dir() else ""
        completions.append(name + suffix)
    return completions


class ShellCompleter:
    def __init__(self) -> None:
        self._matches: list[str] = []

    def complete(self, text: str, state: int) -> str | None:
        if state == 0:
            # Decide what we are completing
            buf = readline.get_line_buffer().lstrip()
            tokens = buf.split()

            if not tokens or (len(tokens) == 1 and not buf.endswith(" ")):
                # Completing the command name itself
                self._matches = [c for c in COMMANDS if c.startswith(text)]
            else:
                # Completing a path argument
                self._matches = _path_completions(text)

        return self._matches[state] if state < len(self._matches) else None


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_cd(args: list[str], _cfg: dict) -> None:
    target = Path(args[0]).expanduser() if args else Path.home()
    try:
        os.chdir(target)
    except FileNotFoundError:
        print(f"cd: {target}: no such directory")
    except NotADirectoryError:
        print(f"cd: {target}: not a directory")
    except PermissionError:
        print(f"cd: {target}: permission denied")


def cmd_ls(args: list[str], cfg: dict) -> None:
    target = Path(args[0]).expanduser() if args else Path(".")
    try:
        entries = sorted(target.iterdir(), key=lambda p: p.name.lower())
    except FileNotFoundError:
        print(f"ls: {target}: no such file or directory")
        return
    except NotADirectoryError:
        # target is a file — just show it
        print(target.name)
        return
    except PermissionError:
        print(f"ls: {target}: permission denied")
        return

    show_hidden: bool = cfg.get("ls_show_hidden", False)
    for entry in entries:
        if not show_hidden and entry.name.startswith("."):
            continue
        suffix = "/" if entry.is_dir() else ""
        print(entry.name + suffix)


def cmd_cat(args: list[str], cfg: dict) -> None:
    if not args:
        print("cat: missing filename")
        return
    max_bytes: int = cfg.get("cat_max_bytes", DEFAULTS["cat_max_bytes"])
    for name in args:
        path = Path(name).expanduser()
        try:
            size = path.stat().st_size
            if size > max_bytes:
                print(
                    f"cat: {path}: file too large "
                    f"({size} bytes, limit {max_bytes})"
                )
                continue
            print(path.read_text(errors="replace"), end="")
        except FileNotFoundError:
            print(f"cat: {path}: no such file or directory")
        except IsADirectoryError:
            print(f"cat: {path}: is a directory")
        except PermissionError:
            print(f"cat: {path}: permission denied")


def cmd_mkdir(args: list[str], _cfg: dict) -> None:
    if not args:
        print("mkdir: missing directory name")
        return
    for name in args:
        path = Path(name).expanduser()
        try:
            path.mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            print(f"mkdir: {path}: already exists")
        except PermissionError:
            print(f"mkdir: {path}: permission denied")


def cmd_rm(args: list[str], _cfg: dict) -> None:
    if not args:
        print("rm: missing path")
        return
    for name in args:
        path = Path(name).expanduser()
        try:
            is_dir = path.is_dir() and not path.is_symlink()
            kind = "directory" if is_dir else "file"
            answer = input(f"rm: remove {kind} '{path}'? [y/N] ").strip().lower()
            if answer not in ("y", "yes"):
                print(f"rm: skipped '{path}'")
                continue
            if is_dir:
                shutil.rmtree(path)
            else:
                path.unlink()
        except FileNotFoundError:
            print(f"rm: {path}: no such file or directory")
        except PermissionError:
            print(f"rm: {path}: permission denied")
        except KeyboardInterrupt:
            print()  # newline after ^C during prompt
            print(f"rm: skipped '{path}'")


def cmd_help(_args: list[str], _cfg: dict) -> None:
    print(
        "commands: cd [dir]  ls [dir]  cat <file>…  mkdir <dir>…  "
        "rm <path>…  exit  help"
    )


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

DISPATCH = {
    "cd": cmd_cd,
    "ls": cmd_ls,
    "cat": cmd_cat,
    "mkdir": cmd_mkdir,
    "rm": cmd_rm,
    "help": cmd_help,
}


# ---------------------------------------------------------------------------
# History
# ---------------------------------------------------------------------------

def setup_history(cfg: dict) -> None:
    readline.set_history_length(cfg.get("history_length", DEFAULTS["history_length"]))
    if HISTORY_PATH.exists():
        try:
            readline.read_history_file(str(HISTORY_PATH))
        except OSError:
            pass


def save_history() -> None:
    try:
        readline.write_history_file(str(HISTORY_PATH))
    except OSError as exc:
        print(f"[warn] could not save history: {exc}", file=sys.stderr)


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def repl(cfg: dict) -> None:
    prompt: str = cfg.get("prompt", DEFAULTS["prompt"])

    while True:
        try:
            line = input(prompt)
        except EOFError:
            # Ctrl-D — clean exit
            print()
            break
        except KeyboardInterrupt:
            # Ctrl-C — cancel current line, keep running
            print()
            continue

        line = line.strip()
        if not line:
            continue

        tokens = line.split()
        command, *args = tokens

        if command == "exit":
            break

        handler = DISPATCH.get(command)
        if handler is None:
            print(f"{command}: command not found")
        else:
            try:
                handler(args, cfg)
            except KeyboardInterrupt:
                # Allow Ctrl-C to abort a running command without killing the shell
                print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    cfg = load_config()
    setup_history(cfg)

    completer = ShellCompleter()
    readline.set_completer(completer.complete)
    # Use tab for completion; on macOS the default binding is different
    readline.parse_and_bind(
        "bind ^I rl_complete" if sys.platform == "darwin" else "tab: complete"
    )
    readline.set_completer_delims(" \t\n")

    try:
        repl(cfg)
    finally:
        save_history()


if __name__ == "__main__":
    main()
