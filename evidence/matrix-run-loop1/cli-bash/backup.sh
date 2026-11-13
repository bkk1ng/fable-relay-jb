#!/usr/bin/env bash
# backup.sh — backup utility with create/restore/list/prune subcommands
# Requires: bash 5, tar, gzip. POSIX-safe where practical.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEST=""
SRC=""
KEEP=7
DRY_RUN=0
VERBOSE=0
LOCKFILE="/tmp/backup_$$.lock"
ARCHIVE_PREFIX="backup"
TIMESTAMP_FMT="%Y%m%d_%H%M%S"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log()  { printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
info() { log "INFO  $*"; }
warn() { log "WARN  $*" >&2; }
die()  { log "ERROR $*" >&2; exit 1; }
verbose() { [[ $VERBOSE -eq 1 ]] && log "DEBUG $*" || true; }

# ---------------------------------------------------------------------------
# Trap-based cleanup
# ---------------------------------------------------------------------------
cleanup() {
    local exit_code=$?
    verbose "Cleaning up (exit ${exit_code})"
    [[ -f "$LOCKFILE" ]] && rm -f "$LOCKFILE"
    exit "$exit_code"
}
trap cleanup EXIT
trap 'die "Interrupted by signal"' INT TERM HUP

# ---------------------------------------------------------------------------
# Lockfile — prevent concurrent runs
# ---------------------------------------------------------------------------
acquire_lock() {
    local lock="${DEST:+${DEST}/}.backup.lock"
    LOCKFILE="$lock"

    if [[ -f "$lock" ]]; then
        local pid
        pid=$(<"$lock")
        if kill -0 "$pid" 2>/dev/null; then
            die "Another backup process is running (PID ${pid}, lockfile ${lock})"
        else
            warn "Stale lockfile found (PID ${pid}); removing"
            rm -f "$lock"
        fi
    fi

    printf '%d\n' "$$" > "$lock"
    verbose "Lock acquired: ${lock}"
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") <subcommand> [options]

Subcommands:
  create   Create a timestamped tar.gz archive
  restore  Restore an archive to a destination directory
  list     List available archives in DEST
  prune    Remove old archives, keeping the N most recent

Options:
  --dest  DIR    Destination directory for archives (required for all)
  --src   DIR    Source directory to back up (required for create)
  --keep  N      Number of archives to keep when pruning (default: ${KEEP})
  --dry-run      Show what would be done without making changes
  --verbose      Enable verbose/debug output

Examples:
  $(basename "$0") create  --src /data/myapp --dest /backups
  $(basename "$0") list    --dest /backups
  $(basename "$0") prune   --dest /backups --keep 5
  $(basename "$0") restore --src /backups/backup_20240101_120000.tar.gz --dest /data/myapp
EOF
    exit 1
}

# ---------------------------------------------------------------------------
# Argument parsing — manual long-option getopts emulation (POSIX-safe)
# ---------------------------------------------------------------------------
parse_args() {
    # First argument is the subcommand
    if [[ $# -lt 1 ]]; then
        usage
    fi

    SUBCOMMAND="$1"
    shift

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dest)
                [[ $# -lt 2 ]] && die "--dest requires a value"
                DEST="$2"; shift 2 ;;
            --dest=*)
                DEST="${1#--dest=}"; shift ;;
            --src)
                [[ $# -lt 2 ]] && die "--src requires a value"
                SRC="$2"; shift 2 ;;
            --src=*)
                SRC="${1#--src=}"; shift ;;
            --keep)
                [[ $# -lt 2 ]] && die "--keep requires a value"
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
                shift; break ;;
            -*)
                die "Unknown option: $1" ;;
            *)
                die "Unexpected argument: $1" ;;
        esac
    done

    # Validate KEEP is a positive integer
    if ! [[ "$KEEP" =~ ^[1-9][0-9]*$ ]]; then
        die "--keep must be a positive integer, got: ${KEEP}"
    fi
}

# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
require_dest() {
    [[ -n "$DEST" ]] || die "--dest is required for this subcommand"
}

require_src() {
    [[ -n "$SRC" ]] || die "--src is required for this subcommand"
}

ensure_dest_dir() {
    if [[ ! -d "$DEST" ]]; then
        if [[ $DRY_RUN -eq 1 ]]; then
            info "[dry-run] Would create destination directory: ${DEST}"
        else
            verbose "Creating destination directory: ${DEST}"
            mkdir -p "$DEST"
        fi
    fi
}

# ---------------------------------------------------------------------------
# Subcommand: create
# ---------------------------------------------------------------------------
cmd_create() {
    require_dest
    require_src
    ensure_dest_dir
    acquire_lock

    [[ -d "$SRC" ]] || die "Source directory does not exist: ${SRC}"

    local timestamp
    timestamp=$(date +"$TIMESTAMP_FMT")
    local archive_name="${ARCHIVE_PREFIX}_${timestamp}.tar.gz"
    local archive_path="${DEST}/${archive_name}"

    if [[ $DRY_RUN -eq 1 ]]; then
        info "[dry-run] Would create archive: ${archive_path}"
        info "[dry-run] Source: ${SRC}"
        return 0
    fi

    info "Creating archive: ${archive_path}"
    verbose "Source: ${SRC}"

    local tar_opts=(-czf "$archive_path")
    if [[ $VERBOSE -eq 1 ]]; then
        tar_opts=(-czvf "$archive_path")
    fi

    # Strip leading slash from SRC so archive paths are relative
    local src_parent src_basename
    src_parent="$(dirname "$SRC")"
    src_basename="$(basename "$SRC")"

    tar "${tar_opts[@]}" -C "$src_parent" "$src_basename"

    local size
    size=$(du -sh "$archive_path" 2>/dev/null | cut -f1 || echo "unknown")
    info "Archive created: ${archive_path} (${size})"
}

# ---------------------------------------------------------------------------
# Subcommand: restore
# ---------------------------------------------------------------------------
cmd_restore() {
    require_dest
    require_src
    ensure_dest_dir
    acquire_lock

    [[ -f "$SRC" ]] || die "Archive file does not exist: ${SRC}"

    # Basic sanity check — expect a .tar.gz
    case "$SRC" in
        *.tar.gz|*.tgz) ;;
        *) warn "File does not have a .tar.gz/.tgz extension: ${SRC}" ;;
    esac

    if [[ $DRY_RUN -eq 1 ]]; then
        info "[dry-run] Would restore archive: ${SRC}"
        info "[dry-run] Destination: ${DEST}"
        return 0
    fi

    info "Restoring archive: ${SRC}"
    verbose "Destination: ${DEST}"

    local tar_opts=(-xzf "$SRC" -C "$DEST")
    if [[ $VERBOSE -eq 1 ]]; then
        tar_opts=(-xzvf "$SRC" -C "$DEST")
    fi

    tar "${tar_opts[@]}"
    info "Restore complete: ${DEST}"
}

# ---------------------------------------------------------------------------
# Subcommand: list
# ---------------------------------------------------------------------------
cmd_list() {
    require_dest

    [[ -d "$DEST" ]] || die "Destination directory does not exist: ${DEST}"

    # Find archives matching our naming pattern, sorted newest-first
    local archives=()
    while IFS= read -r -d '' f; do
        archives+=("$f")
    done < <(find "$DEST" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -print0 \
             | sort -rz)

    if [[ ${#archives[@]} -eq 0 ]]; then
        info "No archives found in: ${DEST}"
        return 0
    fi

    info "Archives in ${DEST} (${#archives[@]} total):"
    local i=1
    for f in "${archives[@]}"; do
        local size
        size=$(du -sh "$f" 2>/dev/null | cut -f1 || echo "?")
        local mtime
        mtime=$(date -r "$f" '+%Y-%m-%d %H:%M:%S' 2>/dev/null \
                || stat -c '%y' "$f" 2>/dev/null | cut -d'.' -f1 \
                || echo "unknown")
        printf '  %3d. %-45s  %6s  %s\n' "$i" "$(basename "$f")" "$size" "$mtime"
        (( i++ )) || true
    done
}

# ---------------------------------------------------------------------------
# Subcommand: prune
# ---------------------------------------------------------------------------
cmd_prune() {
    require_dest

    [[ -d "$DEST" ]] || die "Destination directory does not exist: ${DEST}"

    acquire_lock

    # Collect archives sorted newest-first
    local archives=()
    while IFS= read -r -d '' f; do
        archives+=("$f")
    done < <(find "$DEST" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -print0 \
             | sort -rz)

    local total=${#archives[@]}

    if [[ $total -le $KEEP ]]; then
        info "Nothing to prune: ${total} archive(s) found, keeping ${KEEP}"
        return 0
    fi

    local to_delete=$(( total - KEEP ))
    info "Found ${total} archive(s); keeping ${KEEP}, pruning ${to_delete}"

    # Archives to delete are the oldest ones — tail of the sorted list
    local i=0
    for f in "${archives[@]}"; do
        (( i++ )) || true
        if [[ $i -le $KEEP ]]; then
            verbose "Keeping: $(basename "$f")"
            continue
        fi
        if [[ $DRY_RUN -eq 1 ]]; then
            info "[dry-run] Would delete: ${f}"
        else
            info "Deleting: ${f}"
            rm -f "$f"
        fi
    done
}

# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------
main() {
    parse_args "$@"

    verbose "Subcommand : ${SUBCOMMAND}"
    verbose "DEST       : ${DEST:-<unset>}"
    verbose "SRC        : ${SRC:-<unset>}"
    verbose "KEEP       : ${KEEP}"
    verbose "DRY_RUN    : ${DRY_RUN}"

    case "$SUBCOMMAND" in
        create)  cmd_create  ;;
        restore) cmd_restore ;;
        list)    cmd_list    ;;
        prune)   cmd_prune   ;;
        help|-h|--help) usage ;;
        *) die "Unknown subcommand '${SUBCOMMAND}'. Run with --help for usage." ;;
    esac
}

main "$@"
