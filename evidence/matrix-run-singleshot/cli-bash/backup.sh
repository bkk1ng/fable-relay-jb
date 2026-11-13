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

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log()  { printf '[%s] %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*"; }
info() { log "INFO  $*"; }
warn() { log "WARN  $*" >&2; }
err()  { log "ERROR $*" >&2; }

verbose() {
    [[ "$VERBOSE" -eq 1 ]] && log "DEBUG $*" || true
}

# ---------------------------------------------------------------------------
# Trap-based cleanup
# ---------------------------------------------------------------------------
cleanup() {
    local exit_code=$?
    verbose "cleanup: removing lockfile $LOCKFILE"
    rm -f "$LOCKFILE"
    if [[ $exit_code -ne 0 ]]; then
        err "Exiting with status $exit_code"
    fi
}
trap cleanup EXIT
trap 'err "Interrupted"; exit 130' INT TERM

# ---------------------------------------------------------------------------
# Lockfile — prevent concurrent runs
# ---------------------------------------------------------------------------
acquire_lock() {
    if [[ -e "$LOCKFILE" ]]; then
        local pid
        pid=$(cat "$LOCKFILE" 2>/dev/null || echo "unknown")
        err "Another instance is running (PID $pid). Lock: $LOCKFILE"
        exit 1
    fi
    printf '%s\n' "$$" > "$LOCKFILE"
    verbose "Lock acquired: $LOCKFILE (PID $$)"
}

# ---------------------------------------------------------------------------
# Argument parsing  (manual long-opt via while/case; getopts handles short)
# ---------------------------------------------------------------------------
parse_args() {
    # Normalise --foo=bar to --foo bar so the loop is uniform
    local args=()
    for arg in "$@"; do
        case "$arg" in
            --*=*) args+=("${arg%%=*}" "${arg#*=}") ;;
            *)     args+=("$arg") ;;
        esac
    done
    set -- "${args[@]}"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dest)    DEST="$2";    shift 2 ;;
            --src)     SRC="$2";     shift 2 ;;
            --keep)    KEEP="$2";    shift 2 ;;
            --dry-run) DRY_RUN=1;    shift   ;;
            --verbose) VERBOSE=1;    shift   ;;
            -d)        DEST="$2";    shift 2 ;;
            -s)        SRC="$2";     shift 2 ;;
            -k)        KEEP="$2";    shift 2 ;;
            -n)        DRY_RUN=1;    shift   ;;
            -v)        VERBOSE=1;    shift   ;;
            --) shift; break ;;
            -*) err "Unknown option: $1"; usage; exit 1 ;;
            *)  break ;;
        esac
    done
    # Remaining positional args are available to subcommand handlers via "$@"
    SUBCOMMAND_ARGS=("$@")
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    cat <<'EOF'
Usage: backup.sh <subcommand> [options]

Subcommands:
  create   Create a timestamped tar.gz archive of SRC in DEST
  restore  Extract a named archive back to a target directory
  list     List archives in DEST
  prune    Remove oldest archives, keeping the N most recent

Options:
  -d, --dest <dir>     Destination directory for archives
  -s, --src  <dir>     Source directory to back up (create/restore)
  -k, --keep <n>       Number of archives to keep during prune  (default: 7)
  -n, --dry-run        Print actions without executing them
  -v, --verbose        Enable verbose output

Examples:
  backup.sh create  --src /var/data  --dest /mnt/backups
  backup.sh restore --dest /var/data --src /mnt/backups/backup_20240101_120000.tar.gz
  backup.sh list    --dest /mnt/backups
  backup.sh prune   --dest /mnt/backups --keep 5
EOF
}

# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------
require_dest() {
    [[ -n "$DEST" ]] || { err "--dest is required for this subcommand"; exit 1; }
}

require_src() {
    [[ -n "$SRC" ]] || { err "--src is required for this subcommand"; exit 1; }
}

timestamp() {
    date '+%Y%m%d_%H%M%S'
}

# Run or dry-run a command
run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "[dry-run] $*"
    else
        verbose "exec: $*"
        "$@"
    fi
}

# List archives sorted oldest-first
list_archives() {
    local dest="$1"
    # Use find + sort for POSIX portability; avoid ls parsing
    find "$dest" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -type f \
        | sort
}

# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------

cmd_create() {
    require_src
    require_dest

    [[ -d "$SRC" ]]  || { err "Source does not exist or is not a directory: $SRC";  exit 1; }
    [[ -d "$DEST" ]] || { err "Destination does not exist or is not a directory: $DEST"; exit 1; }

    local ts archive_name archive_path
    ts=$(timestamp)
    archive_name="${ARCHIVE_PREFIX}_${ts}.tar.gz"
    archive_path="${DEST}/${archive_name}"

    info "Creating archive: $archive_path"
    info "  Source : $SRC"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "[dry-run] tar -czf $archive_path -C $(dirname "$SRC") $(basename "$SRC")"
    else
        tar -czf "$archive_path" -C "$(dirname "$SRC")" "$(basename "$SRC")"
        local size
        size=$(du -sh "$archive_path" | cut -f1)
        info "Archive created: $archive_path ($size)"
    fi
}

cmd_restore() {
    # --src holds the archive path; --dest holds the extraction target
    require_src
    require_dest

    [[ -f "$SRC" ]]  || { err "Archive not found: $SRC"; exit 1; }
    [[ -d "$DEST" ]] || { err "Restore target does not exist or is not a directory: $DEST"; exit 1; }

    info "Restoring $SRC -> $DEST"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        info "[dry-run] tar -xzf $SRC -C $DEST"
    else
        tar -xzf "$SRC" -C "$DEST"
        info "Restore complete."
    fi
}

cmd_list() {
    require_dest

    [[ -d "$DEST" ]] || { err "Destination does not exist: $DEST"; exit 1; }

    local archives
    mapfile -t archives < <(list_archives "$DEST")

    if [[ ${#archives[@]} -eq 0 ]]; then
        info "No archives found in $DEST"
        return
    fi

    info "Archives in $DEST (${#archives[@]} total):"
    local f size mtime
    for f in "${archives[@]}"; do
        size=$(du -sh "$f" | cut -f1)
        # stat is not fully POSIX, but both GNU and BSD support -c/%z variants
        if stat --version &>/dev/null 2>&1; then
            # GNU stat
            mtime=$(stat -c '%y' "$f" | cut -d'.' -f1)
        else
            # BSD/macOS stat
            mtime=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$f")
        fi
        printf '  %-50s  %6s  %s\n' "$(basename "$f")" "$size" "$mtime"
    done
}

cmd_prune() {
    require_dest

    [[ -d "$DEST" ]] || { err "Destination does not exist: $DEST"; exit 1; }

    # Validate --keep is a positive integer
    case "$KEEP" in
        ''|*[!0-9]*) err "--keep must be a positive integer, got: $KEEP"; exit 1 ;;
    esac
    [[ "$KEEP" -gt 0 ]] || { err "--keep must be >= 1"; exit 1; }

    local archives
    mapfile -t archives < <(list_archives "$DEST")

    local total=${#archives[@]}
    if [[ $total -le $KEEP ]]; then
        info "Nothing to prune: $total archive(s) present, keeping $KEEP."
        return
    fi

    local delete_count=$(( total - KEEP ))
    info "Pruning $delete_count archive(s) from $DEST (keeping $KEEP most recent)..."

    local i=0
    for f in "${archives[@]}"; do
        if [[ $i -ge $delete_count ]]; then
            break
        fi
        info "  Removing: $(basename "$f")"
        run rm -f "$f"
        (( i++ )) || true
    done

    info "Prune complete. Removed $delete_count archive(s)."
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
main() {
    if [[ $# -eq 0 ]]; then
        usage
        exit 1
    fi

    local subcommand="$1"
    shift

    parse_args "$@"

    acquire_lock

    case "$subcommand" in
        create)  cmd_create  ;;
        restore) cmd_restore ;;
        list)    cmd_list    ;;
        prune)   cmd_prune   ;;
        help|--help|-h) usage; exit 0 ;;
        *) err "Unknown subcommand: $subcommand"; usage; exit 1 ;;
    esac
}

main "$@"
