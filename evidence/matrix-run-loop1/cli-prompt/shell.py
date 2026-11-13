#!/usr/bin/env python3
"""
tiny shell — interactive REPL with readline, tab completion, history, and
a small set of built-in commands. Config loaded from ~/.config/tiny/config.json.
"""

import json
import os
import readline
import shutil
import stat
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

CONFIG_PATH = Path.home() / ".config" / "tiny" / "config.json"

DEFAULTS: dict = {
    "prompt": "$ ",
    "history_file": str(Path.home() / ".tiny_history"),
    "history_size": 1000,
    "ls_show_hidden": False,
}


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    if CONFIG_PATH.exists():
        try:
            with CONFIG_PATH.open() as fh:
                user = json.load(fh)
            if isinstance(user, dict):
                cfg.update(user)
        except (json.JSONDecodeError, OSError) as exc:
            print(f"[tiny] warning: could not read config ({exc})", file=sys.stderr)
    return cfg


# ---------------------------------------------------------------------------
# Readline / completion
# ---------------------------------------------------------------------------

COMMANDS = ["cd", "ls", "cat", "mkdir", "rm", "exit", "help"]


def _path_completions(prefix: str) -> list[str]:
    """Return filesystem entries that match *prefix*."""
    if os.sep in prefix or prefix.startswith("./") or prefix.startswith("../"):
        directory, partial = os.path.split(prefix)
        base = directory or "."
    else:
        base = "."
        partial = prefix

    try:
        entries = os.listdir(base or ".")
    except OSError:
        return []

    matches = []
    for entry in entries:
        if entry.startswith(partial):
            full = os.path.join(base, entry) if base != "." else entry
            if os.path.isdir(full):
                full += os.sep
            matches.append(full)
    return matches


def make_completer(commands: list[str]):
    """Return a readline-compatible completer function."""
    _state_cache: list[str] = []

    def completer(text: str, state: int) -> str | None:
        if state == 0:
            # Decide whether we are completing a command or a path argument.
            line = readline.get_line_buffer()
            tokens = line.lstrip().split()
            # Completing the first token → command completion
            if len(tokens) == 0 or (len(tokens) == 1 and not line.endswith(" ")):
                _state_cache.clear()
                _state_cache.extend(c for c in commands if c.startswith(text))
            else:
                # Completing a path argument
                _state_cache.clear()
                _state_cache.extend(_path_completions(text))

        try:
            return _state_cache[state]
        except IndexError:
            return None

    return completer


def setup_readline(history_file: str, history_size: int) -> None:
    readline.set_completer(make_completer(COMMANDS))
    readline.set_completer_delims(" \t\n;")
    readline.parse_and_bind("tab: complete")

    if os.path.isfile(history_file):
        try:
            readline.read_history_file(history_file)
        except OSError:
            pass

    readline.set_history_length(history_size)


def save_history(history_file: str) -> None:
    try:
        Path(history_file).parent.mkdir(parents=True, exist_ok=True)
        readline.write_history_file(history_file)
    except OSError as exc:
        print(f"[tiny] warning: could not save history ({exc})", file=sys.stderr)


# ---------------------------------------------------------------------------
# Built-in commands
# ---------------------------------------------------------------------------

def _fmt_mode(mode: int) -> str:
    """Return a 10-character permission string like ls -l."""
    is_dir = "d" if stat.S_ISDIR(mode) else "-"
    bits = ""
    for shift in (6, 3, 0):
        m = (mode >> shift) & 0o7
        bits += ("r" if m & 4 else "-")
        bits += ("w" if m & 2 else "-")
        bits += ("x" if m & 1 else "-")
    return is_dir + bits


def cmd_cd(args: list[str]) -> None:
    target = args[0] if args else str(Path.home())
    try:
        os.chdir(target)
    except FileNotFoundError:
        print(f"cd: {target}: no such directory")
    except NotADirectoryError:
        print(f"cd: {target}: not a directory")
    except PermissionError:
        print(f"cd: {target}: permission denied")


def cmd_ls(args: list[str], show_hidden: bool = False) -> None:
    path = args[0] if args else "."
    try:
        entries = sorted(os.scandir(path), key=lambda e: e.name.lower())
    except FileNotFoundError:
        print(f"ls: {path}: no such file or directory")
        return
    except PermissionError:
        print(f"ls: {path}: permission denied")
        return

    for entry in entries:
        if not show_hidden and entry.name.startswith("."):
            continue
        try:
            st = entry.stat()
            mode_str = _fmt_mode(st.st_mode)
            size = st.st_size
            name = entry.name + ("/" if entry.is_dir() else "")
            print(f"{mode_str}  {size:>10}  {name}")
        except OSError:
            print(entry.name)


def cmd_cat(args: list[str]) -> None:
    if not args:
        print("cat: missing file operand")
        return
    for path in args:
        try:
            with open(path) as fh:
                sys.stdout.write(fh.read())
        except FileNotFoundError:
            print(f"cat: {path}: no such file or directory")
        except IsADirectoryError:
            print(f"cat: {path}: is a directory")
        except PermissionError:
            print(f"cat: {path}: permission denied")
        except UnicodeDecodeError:
            print(f"cat: {path}: binary file, cannot display")


def cmd_mkdir(args: list[str]) -> None:
    if not args:
        print("mkdir: missing operand")
        return
    for path in args:
        try:
            Path(path).mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            print(f"mkdir: {path}: already exists")
        except PermissionError:
            print(f"mkdir: {path}: permission denied")


def cmd_rm(args: list[str]) -> None:
    if not args:
        print("rm: missing operand")
        return
    for path in args:
        if not Path(path).exists():
            print(f"rm: {path}: no such file or directory")
            continue
        try:
            answer = input(f"rm: remove '{path}'? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            continue
        if answer in ("y", "yes"):
            try:
                if os.path.isdir(path):
                    shutil.rmtree(path)
                else:
                    os.remove(path)
            except PermissionError:
                print(f"rm: {path}: permission denied")
            except OSError as exc:
                print(f"rm: {path}: {exc}")


def cmd_help() -> None:
    print(
        "built-in commands:\n"
        "  cd [dir]        change directory (defaults to home)\n"
        "  ls [dir]        list directory contents\n"
        "  cat file ...    print file contents\n"
        "  mkdir dir ...   create directories\n"
        "  rm path ...     remove file or directory (with confirmation)\n"
        "  help            show this message\n"
        "  exit            quit"
    )


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def run_repl(cfg: dict) -> None:
    prompt: str = cfg.get("prompt", DEFAULTS["prompt"])
    show_hidden: bool = bool(cfg.get("ls_show_hidden", DEFAULTS["ls_show_hidden"]))

    while True:
        try:
            line = input(prompt)
        except KeyboardInterrupt:
            # Ctrl-C cancels the current line; keep the loop running.
            print()
            continue
        except EOFError:
            # Ctrl-D exits cleanly.
            print()
            break

        line = line.strip()
        if not line:
            continue

        tokens = line.split()
        command, *args = tokens

        if command == "exit":
            break
        elif command == "cd":
            cmd_cd(args)
        elif command == "ls":
            cmd_ls(args, show_hidden=show_hidden)
        elif command == "cat":
            cmd_cat(args)
        elif command == "mkdir":
            cmd_mkdir(args)
        elif command == "rm":
            cmd_rm(args)
        elif command == "help":
            cmd_help()
        else:
            print(f"tiny: {command}: command not found  (try 'help')")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    cfg = load_config()
    history_file: str = cfg.get("history_file", DEFAULTS["history_file"])
    history_size: int = int(cfg.get("history_size", DEFAULTS["history_size"]))

    setup_readline(history_file, history_size)

    try:
        run_repl(cfg)
    finally:
        save_history(history_file)


if __name__ == "__main__":
    main()
