#!/usr/bin/env python3
"""
tiny shell — interactive command REPL with readline history, tab completion,
and a small set of built-in commands. Config loaded from
~/.config/tiny/config.json with sane defaults.
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
            print(f"[tiny] warning: could not read config: {exc}", file=sys.stderr)
    return cfg


# ---------------------------------------------------------------------------
# Readline history + completion
# ---------------------------------------------------------------------------

COMMANDS = ["cd", "ls", "cat", "mkdir", "rm", "exit", "help"]


def _path_completions(prefix: str) -> list[str]:
    """Return filesystem entries that start with *prefix*."""
    if not prefix:
        base_dir, partial = ".", ""
    elif prefix.endswith(os.sep):
        base_dir, partial = prefix, ""
    else:
        base_dir = os.path.dirname(prefix) or "."
        partial = os.path.basename(prefix)

    try:
        entries = os.listdir(base_dir)
    except OSError:
        return []

    matches = []
    for entry in entries:
        if entry.startswith(partial):
            full = os.path.join(base_dir, entry) if base_dir != "." else entry
            if prefix.startswith("./") or "/" in prefix:
                full = os.path.join(base_dir, entry)
            if os.path.isdir(full):
                matches.append(full + os.sep)
            else:
                matches.append(full)
    return matches


def make_completer():
    """Return a readline-compatible completer function."""
    def completer(text: str, state: int) -> str | None:
        # Build the full line to decide context.
        line = readline.get_line_buffer()
        tokens = line.split()

        # First token → complete command names.
        if not tokens or (len(tokens) == 1 and not line.endswith(" ")):
            matches = [c for c in COMMANDS if c.startswith(text)]
        else:
            # Subsequent tokens → complete paths.
            matches = _path_completions(text)

        return matches[state] if state < len(matches) else None

    return completer


def setup_readline(history_file: str, history_size: int) -> None:
    readline.set_completer(make_completer())
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
        print(f"[tiny] warning: could not save history: {exc}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Built-in command implementations
# ---------------------------------------------------------------------------

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


def _format_mode(mode: int) -> str:
    """Return a 10-character permission string like drwxr-xr-x."""
    kinds = {
        stat.S_IFDIR:  "d",
        stat.S_IFLNK:  "l",
        stat.S_IFREG:  "-",
        stat.S_IFIFO:  "p",
        stat.S_IFSOCK: "s",
        stat.S_IFBLK:  "b",
        stat.S_IFCHR:  "c",
    }
    file_type = kinds.get(stat.S_IFMT(mode), "?")
    bits = ""
    for shift in (6, 3, 0):
        bits += "r" if mode >> shift & 0o4 else "-"
        bits += "w" if mode >> shift & 0o2 else "-"
        bits += "x" if mode >> shift & 0o1 else "-"
    return file_type + bits


def cmd_ls(args: list[str], show_hidden: bool) -> None:
    path = args[0] if args else "."
    try:
        entries = sorted(os.listdir(path))
    except FileNotFoundError:
        print(f"ls: {path}: no such file or directory")
        return
    except NotADirectoryError:
        print(f"ls: {path}: not a directory")
        return
    except PermissionError:
        print(f"ls: {path}: permission denied")
        return

    for name in entries:
        if not show_hidden and name.startswith("."):
            continue
        full = os.path.join(path, name)
        try:
            st = os.lstat(full)
            size = st.st_size
            mode_str = _format_mode(st.st_mode)
        except OSError:
            mode_str = "??????????"
            size = 0
        display = name + ("/" if os.path.isdir(full) else "")
        print(f"{mode_str}  {size:>10}  {display}")


def cmd_cat(args: list[str]) -> None:
    if not args:
        print("cat: missing file operand")
        return
    for filepath in args:
        try:
            with open(filepath, "r", errors="replace") as fh:
                sys.stdout.write(fh.read())
        except FileNotFoundError:
            print(f"cat: {filepath}: no such file or directory")
        except IsADirectoryError:
            print(f"cat: {filepath}: is a directory")
        except PermissionError:
            print(f"cat: {filepath}: permission denied")
    # Ensure the prompt starts on a new line if the file had no trailing newline.
    if sys.stdout.tell() if hasattr(sys.stdout, "tell") else True:
        pass  # let the REPL print its own newline via input()


def cmd_mkdir(args: list[str]) -> None:
    if not args:
        print("mkdir: missing operand")
        return
    for dirpath in args:
        try:
            Path(dirpath).mkdir(parents=True, exist_ok=False)
            print(f"mkdir: created '{dirpath}'")
        except FileExistsError:
            print(f"mkdir: {dirpath}: already exists")
        except PermissionError:
            print(f"mkdir: {dirpath}: permission denied")
        except OSError as exc:
            print(f"mkdir: {dirpath}: {exc}")


def cmd_rm(args: list[str]) -> None:
    if not args:
        print("rm: missing operand")
        return
    for target in args:
        p = Path(target)
        if not p.exists() and not p.is_symlink():
            print(f"rm: {target}: no such file or directory")
            continue
        is_dir = p.is_dir() and not p.is_symlink()
        kind = "directory" if is_dir else "file"
        try:
            answer = input(f"rm: remove {kind} '{target}'? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            continue
        if answer in ("y", "yes"):
            try:
                if is_dir:
                    shutil.rmtree(target)
                else:
                    p.unlink()
                print(f"rm: removed '{target}'")
            except PermissionError:
                print(f"rm: {target}: permission denied")
            except OSError as exc:
                print(f"rm: {target}: {exc}")
        else:
            print(f"rm: '{target}' not removed")


def cmd_help() -> None:
    print(
        "built-in commands:\n"
        "  cd [dir]        change directory (default: home)\n"
        "  ls [dir]        list directory contents\n"
        "  cat <file>...   print file contents\n"
        "  mkdir <dir>...  create directories\n"
        "  rm <path>...    remove files or directories (with confirmation)\n"
        "  help            show this message\n"
        "  exit            quit (also Ctrl-D)\n"
    )


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def dispatch(line: str, cfg: dict) -> bool:
    """Parse *line* and run the matching command. Return False to exit."""
    tokens = line.split()
    if not tokens:
        return True

    cmd, *args = tokens

    if cmd == "exit":
        return False
    elif cmd == "cd":
        cmd_cd(args)
    elif cmd == "ls":
        cmd_ls(args, show_hidden=cfg.get("ls_show_hidden", False))
    elif cmd == "cat":
        cmd_cat(args)
    elif cmd == "mkdir":
        cmd_mkdir(args)
    elif cmd == "rm":
        cmd_rm(args)
    elif cmd == "help":
        cmd_help()
    else:
        print(f"tiny: {cmd}: command not found (try 'help')")

    return True


def repl(cfg: dict) -> None:
    prompt = cfg.get("prompt", DEFAULTS["prompt"])
    history_file = cfg.get("history_file", DEFAULTS["history_file"])

    while True:
        try:
            line = input(prompt)
        except KeyboardInterrupt:
            # Ctrl-C: cancel current line, keep running.
            print()
            continue
        except EOFError:
            # Ctrl-D: clean exit.
            print()
            break

        line = line.strip()
        if not line:
            continue

        try:
            keep_going = dispatch(line, cfg)
        except Exception as exc:  # noqa: BLE001 — surface unexpected errors
            print(f"[tiny] error: {exc}", file=sys.stderr)
            keep_going = True

        if not keep_going:
            break

    save_history(history_file)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    cfg = load_config()
    setup_readline(
        history_file=cfg.get("history_file", DEFAULTS["history_file"]),
        history_size=int(cfg.get("history_size", DEFAULTS["history_size"])),
    )
    print("tiny shell  •  type 'help' for commands, Ctrl-D or 'exit' to quit")
    repl(cfg)


if __name__ == "__main__":
    main()
