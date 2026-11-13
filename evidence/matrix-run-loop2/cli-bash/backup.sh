#!/usr/bin/env bash
# backup.sh — backup utility with create/restore/list/prune subcommands
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEST=""
SRC=""
KEEP=7
DRY_RUN=0
VERBOSE=0
LOCKFILE="/tmp/backup.sh.lock"
ARCHIVE_PREFIX="backup"
TIMESTAMP_FMT="%Y%m%dT%H%M%S"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log()     { printf '%s [INFO]  %s\n'  "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2; }
verbose() { [[ "$VERBOSE" -eq 1 ]] && printf '%s [DEBUG] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2 || true; }
warn()    { printf '%s [WARN]  %s\n'  "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2; }
die()     { printf '%s [ERROR] %s\n'  "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Trap-based cleanup
# ---------------------------------------------------------------------------
_cleanup() {
    local exit_code=$?
    verbose "Cleanup triggered (exit code: $exit_code)"
    _release_lock
    exit "$exit_code"
}
trap _cleanup EXIT
trap 'die "Interrupted."' INT TERM

# ---------------------------------------------------------------------------
# Lockfile helpers
# ---------------------------------------------------------------------------
_acquire_lock() {
    if [[ -e "$LOCKFILE" ]]; then
        local pid
        pid=$(cat "$LOCKFILE" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            die "Another backup process is running (PID $pid). Lockfile: $LOCKFILE"
        else
            warn "Stale lockfile found (PID $pid). Removing."
            rm -f "$LOCKFILE"
        fi
    fi
    printf '%d\n' "$$" > "$LOCKFILE"
    verbose "Lock acquired: $LOCKFILE (PID $$)"
}

_release_lock() {
    if [[ -e "$LOCKFILE" ]]; then
        local pid
        pid=$(cat "$LOCKFILE" 2>/dev/null || true)
        if [[ "$pid" == "$$" ]]; then
            rm -f "$LOCKFILE"
            verbose "Lock released: $LOCKFILE"
        fi
    fi
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    cat >&2 <<EOF
Usage: $(basename "$0") [OPTIONS] COMMAND

Commands:
  create   Create a timestamped tar.gz archive of SRC in DEST
  restore  Restore an archive from DEST to SRC
  list     List archives in DEST
  prune    Remove archives in DEST beyond the KEEP count (oldest first)

Options:
  --dest DIR      Destination directory for archives (required for all commands)
  --src DIR       Source directory to back up / restore to
  --keep N        Number of archives to keep during prune (default: $KEEP)
  --dry-run       Print actions without executing them
  --verbose       Enable verbose/debug output

Examples:
  $(basename "$0") --src /home/user/data --dest /mnt/backups create
  $(basename "$0") --dest /mnt/backups list
  $(basename "$0") --dest /mnt/backups --keep 5 prune
  $(basename "$0") --dest /mnt/backups --src /home/user/data restore
EOF
    exit 1
}

# ---------------------------------------------------------------------------
# Argument parsing  (manual long-opt getopts-style, POSIX-safe)
# ---------------------------------------------------------------------------
parse_args() {
    local cmd=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dest)
                [[ $# -lt 2 ]] && die "--dest requires an argument."
                DEST="$2"; shift 2 ;;
            --dest=*)
                DEST="${1#--dest=}"; shift ;;
            --src)
                [[ $# -lt 2 ]] && die "--src requires an argument."
                SRC="$2"; shift 2 ;;
            --src=*)
                SRC="${1#--src=}"; shift ;;
            --keep)
                [[ $# -lt 2 ]] && die "--keep requires an argument."
                KEEP="$2"; shift 2 ;;
            --keep=*)
                KEEP="${1#--keep=}"; shift ;;
            --dry-run)
                DRY_RUN=1; shift ;;
            --verbose)
                VERBOSE=1; shift ;;
            -h|--help)
                usage ;;
            --)
                shift; cmd="$*"; break ;;
            -*)
                die "Unknown option: $1" ;;
            *)
                cmd="$1"; shift; break ;;
        esac
    done

    # Remaining positional after the command are ignored for now
    [[ -z "$cmd" ]] && usage
    printf '%s\n' "$cmd"
}

# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
require_dest() {
    [[ -z "$DEST" ]] && die "--dest is required."
    if [[ ! -d "$DEST" ]]; then
        if [[ "$DRY_RUN" -eq 1 ]]; then
            warn "[dry-run] Destination '$DEST' does not exist — would create it."
        else
            log "Creating destination directory: $DEST"
            mkdir -p "$DEST"
        fi
    fi
}

require_src() {
    [[ -z "$SRC" ]] && die "--src is required for this command."
    [[ ! -e "$SRC" ]] && die "Source path does not exist: $SRC"
}

validate_keep() {
    [[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || die "--keep must be a positive integer, got: $KEEP"
}

# ---------------------------------------------------------------------------
# Archive name helper
# ---------------------------------------------------------------------------
_archive_name() {
    local ts
    ts=$(date +"$TIMESTAMP_FMT")
    printf '%s_%s.tar.gz\n' "$ARCHIVE_PREFIX" "$ts"
}

# ---------------------------------------------------------------------------
# Command: create
# ---------------------------------------------------------------------------
cmd_create() {
    require_dest
    require_src
    local archive_name archive_path
    archive_name=$(_archive_name)
    archive_path="${DEST%/}/${archive_name}"

    log "Creating archive: $archive_path"
    verbose "Source: $SRC"
    verbose "Destination: $DEST"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "[dry-run] Would run: tar -czf '$archive_path' -C '$(dirname "$SRC")' '$(basename "$SRC")'"
        return 0
    fi

    tar -czf "$archive_path" -C "$(dirname "$SRC")" "$(basename "$SRC")"
    log "Archive created: $archive_path"
}

# ---------------------------------------------------------------------------
# Command: list
# ---------------------------------------------------------------------------
cmd_list() {
    require_dest
    log "Archives in '$DEST':"
    local count=0
    # Use a while-read loop so we handle spaces in paths correctly
    while IFS= read -r -d '' f; do
        printf '  %s  (%s)\n' "$(basename "$f")" "$(du -sh "$f" 2>/dev/null | cut -f1)"
        (( count++ )) || true
    done < <(find "$DEST" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -print0 | sort -z)

    if [[ "$count" -eq 0 ]]; then
        log "No archives found."
    else
        log "Total: $count archive(s)."
    fi
}

# ---------------------------------------------------------------------------
# Command: prune
# ---------------------------------------------------------------------------
cmd_prune() {
    require_dest
    validate_keep

    # Collect archives sorted oldest-first
    local archives=()
    while IFS= read -r -d '' f; do
        archives+=("$f")
    done < <(find "$DEST" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -print0 | sort -z)

    local total=${#archives[@]}
    local to_delete=$(( total - KEEP ))

    if [[ "$to_delete" -le 0 ]]; then
        log "Nothing to prune ($total archive(s) present, keeping $KEEP)."
        return 0
    fi

    log "Pruning $to_delete archive(s) (keeping $KEEP of $total)."
    local i=0
    while [[ "$i" -lt "$to_delete" ]]; do
        local target="${archives[$i]}"
        if [[ "$DRY_RUN" -eq 1 ]]; then
            log "[dry-run] Would delete: $(basename "$target")"
        else
            verbose "Deleting: $target"
            rm -f "$target"
            log "Deleted: $(basename "$target")"
        fi
        (( i++ )) || true
    done
}

# ---------------------------------------------------------------------------
# Command: restore
# ---------------------------------------------------------------------------
cmd_restore() {
    require_dest
    require_src

    # Find the most recent archive
    local latest=""
    while IFS= read -r -d '' f; do
        latest="$f"
    done < <(find "$DEST" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -print0 | sort -z)

    [[ -z "$latest" ]] && die "No archives found in '$DEST'."

    log "Restoring from: $(basename "$latest")"
    log "Restore target: $SRC"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "[dry-run] Would run: tar -xzf '$latest' -C '$SRC'"
        return 0
    fi

    if [[ ! -d "$SRC" ]]; then
        log "Creating restore target directory: $SRC"
        mkdir -p "$SRC"
    fi

    tar -xzf "$latest" -C "$SRC"
    log "Restore complete."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    local cmd
    cmd=$(parse_args "$@")

    _acquire_lock

    case "$cmd" in
        create)  cmd_create  ;;
        restore) cmd_restore ;;
        list)    cmd_list    ;;
        prune)   cmd_prune   ;;
        *)       die "Unknown command: $cmd. Use create, restore, list, or prune." ;;
    esac
}

main "$@"
