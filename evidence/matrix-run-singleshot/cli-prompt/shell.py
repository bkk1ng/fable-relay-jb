#!/usr/bin/env python3
"""
tiny shell — interactive command REPL with readline history, tab completion,
and a small set of built-in commands. Config loaded from
~/.config/tiny/config.json with sensible defaults.
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

DEFAULTS: dict = {
    "prompt": "tiny> ",
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
            print(f"[warn] could not read config: {exc}", file=sys.stderr)
    return cfg


# ---------------------------------------------------------------------------
# Readline history + tab completion
# ---------------------------------------------------------------------------

COMMANDS = ["cd", "ls", "cat", "mkdir", "rm", "exit", "help"]


def _complete(text: str, state: int) -> str | None:
    """Tab-completion: commands first, then filesystem paths."""
    buffer = readline.get_line_buffer()
    tokens = buffer.lstrip().split()

    # Still on the first token — complete a command name
    if len(tokens) == 0 or (len(tokens) == 1 and not buffer.endswith(" ")):
        matches = [c for c in COMMANDS if c.startswith(text)]
        return matches[state] if state < len(matches) else None

    # Completing a path argument
    if text.startswith("~"):
        expanded = os.path.expanduser(text)
    else:
        expanded = text

    dir_part = os.path.dirname(expanded) or "."
    base_part = os.path.basename(expanded)

    try:
        entries = os.listdir(dir_part)
    except OSError:
        return None

    matches = []
    for entry in entries:
        if entry.startswith(base_part):
            full = os.path.join(dir_part, entry) if dir_part != "." else entry
            # Reconstruct with original ~ prefix if the user typed it
            if text.startswith("~"):
                full = "~" + full[len(os.path.expanduser("~")):]
            if os.path.isdir(os.path.join(dir_part, entry)):
                full += "/"
            matches.append(full)

    return matches[state] if state < len(matches) else None


def setup_readline(history_file: str, history_size: int) -> None:
    readline.set_completer(_complete)
    readline.set_completer_delims(" \t\n;")
    readline.parse_and_bind("tab: complete")

    hpath = Path(history_file)
    hpath.parent.mkdir(parents=True, exist_ok=True)
    if hpath.exists():
        try:
            readline.read_history_file(str(hpath))
        except OSError:
            pass
    readline.set_history_length(history_size)


def save_history(history_file: str) -> None:
    try:
        readline.write_history_file(history_file)
    except OSError as exc:
        print(f"[warn] could not save history: {exc}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Built-in commands
# ---------------------------------------------------------------------------

def cmd_cd(args: list[str]) -> None:
    target = args[0] if args else str(Path.home())
    try:
        os.chdir(os.path.expanduser(target))
    except FileNotFoundError:
        print(f"cd: no such directory: {target}")
    except NotADirectoryError:
        print(f"cd: not a directory: {target}")
    except PermissionError:
        print(f"cd: permission denied: {target}")


def cmd_ls(args: list[str], show_hidden: bool) -> None:
    path = os.path.expanduser(args[0]) if args else "."
    try:
        entries = sorted(os.listdir(path))
    except FileNotFoundError:
        print(f"ls: no such file or directory: {path}")
        return
    except PermissionError:
        print(f"ls: permission denied: {path}")
        return

    if not show_hidden:
        entries = [e for e in entries if not e.startswith(".")]

    # Simple columnar display using terminal width
    width = shutil.get_terminal_size(fallback=(80, 24)).columns
    if not entries:
        return

    col_w = max(len(e) for e in entries) + 2
    cols = max(1, width // col_w)
    for i, entry in enumerate(entries):
        full = os.path.join(path, entry)
        display = entry + "/" if os.path.isdir(full) else entry
        end = "\n" if (i + 1) % cols == 0 or i == len(entries) - 1 else ""
        print(f"{display:<{col_w}}", end=end)


def cmd_cat(args: list[str]) -> None:
    if not args:
        print("cat: missing filename")
        return
    for name in args:
        path = os.path.expanduser(name)
        try:
            with open(path) as fh:
                sys.stdout.write(fh.read())
        except FileNotFoundError:
            print(f"cat: {name}: no such file or directory")
        except IsADirectoryError:
            print(f"cat: {name}: is a directory")
        except PermissionError:
            print(f"cat: {name}: permission denied")
        except UnicodeDecodeError:
            print(f"cat: {name}: binary file, cannot display")


def cmd_mkdir(args: list[str]) -> None:
    if not args:
        print("mkdir: missing operand")
        return
    for name in args:
        path = os.path.expanduser(name)
        try:
            Path(path).mkdir(parents=True, exist_ok=False)
            print(f"created: {path}")
        except FileExistsError:
            print(f"mkdir: {name}: already exists")
        except PermissionError:
            print(f"mkdir: {name}: permission denied")


def cmd_rm(args: list[str]) -> None:
    if not args:
        print("rm: missing operand")
        return
    for name in args:
        path = os.path.expanduser(name)
        if not os.path.exists(path):
            print(f"rm: {name}: no such file or directory")
            continue
        try:
            answer = input(f"rm: remove '{name}'? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            continue
        if answer not in ("y", "yes"):
            print(f"rm: skipped '{name}'")
            continue
        try:
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
            print(f"removed: {path}")
        except PermissionError:
            print(f"rm: {name}: permission denied")
        except OSError as exc:
            print(f"rm: {name}: {exc}")


def cmd_help() -> None:
    print(
        "commands:\n"
        "  cd [dir]       change directory (default: home)\n"
        "  ls [dir]       list directory contents\n"
        "  cat <file...>  print file contents\n"
        "  mkdir <dir...> create directories\n"
        "  rm <path...>   remove files or directories (with confirmation)\n"
        "  help           show this message\n"
        "  exit           quit"
    )


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def run(cfg: dict) -> None:
    prompt: str = cfg["prompt"]
    show_hidden: bool = bool(cfg.get("ls_show_hidden", False))

    while True:
        try:
            line = input(prompt)
        except KeyboardInterrupt:
            # Ctrl-C clears the current line; stay in the loop
            print()
            continue
        except EOFError:
            # Ctrl-D exits cleanly
            print()
            break

        line = line.strip()
        if not line:
            continue

        parts = line.split()
        command, args = parts[0], parts[1:]

        if command == "exit":
            break
        elif command == "cd":
            cmd_cd(args)
        elif command == "ls":
            cmd_ls(args, show_hidden)
        elif command == "cat":
            cmd_cat(args)
        elif command == "mkdir":
            cmd_mkdir(args)
        elif command == "rm":
            cmd_rm(args)
        elif command == "help":
            cmd_help()
        else:
            print(f"unknown command: {command!r}  (type 'help' for a list)")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    cfg = load_config()
    setup_readline(cfg["history_file"], int(cfg["history_size"]))
    try:
        run(cfg)
    finally:
        save_history(cfg["history_file"])


if __name__ == "__main__":
    main()
