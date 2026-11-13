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
ARCHIVE_EXT=".tar.gz"
TIMESTAMP_FMT="%Y%m%dT%H%M%S"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log()     { printf '%s [INFO]  %s\n'  "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >&2; }
verbose() { [[ "$VERBOSE" -eq 1 ]] && printf '%s [DEBUG] %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >&2 || true; }
warn()    { printf '%s [WARN]  %s\n'  "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >&2; }
die()     { printf '%s [ERROR] %s\n'  "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Trap-based cleanup
# ---------------------------------------------------------------------------
_cleanup() {
    local exit_code=$?
    verbose "Cleaning up (exit code: $exit_code)"
    if [[ -f "$LOCKFILE" ]]; then
        local lock_pid
        lock_pid=$(cat "$LOCKFILE" 2>/dev/null || true)
        if [[ "$lock_pid" == "$$" ]]; then
            verbose "Removing lockfile: $LOCKFILE"
            rm -f "$LOCKFILE"
        fi
    fi
    exit "$exit_code"
}
trap '_cleanup' EXIT
trap 'die "Interrupted by signal"' INT TERM

# ---------------------------------------------------------------------------
# Lockfile: prevent concurrent runs
# ---------------------------------------------------------------------------
acquire_lock() {
    if [[ -f "$LOCKFILE" ]]; then
        local lock_pid
        lock_pid=$(cat "$LOCKFILE" 2>/dev/null || true)
        if [[ -n "$lock_pid" ]] && kill -0 "$lock_pid" 2>/dev/null; then
            die "Another instance is running (PID $lock_pid, lockfile: $LOCKFILE)"
        else
            warn "Stale lockfile found (PID $lock_pid); removing and continuing"
            rm -f "$LOCKFILE"
        fi
    fi
    printf '%d' "$$" > "$LOCKFILE"
    verbose "Lockfile acquired: $LOCKFILE (PID $$)"
}

# ---------------------------------------------------------------------------
# Argument parser — long options via manual loop, short via getopts
# ---------------------------------------------------------------------------
parse_args() {
    # Normalise --foo=bar  ->  --foo bar
    local args=()
    for arg in "$@"; do
        case "$arg" in
            --*=*) args+=("${arg%%=*}" "${arg#*=}") ;;
            *)     args+=("$arg") ;;
        esac
    done
    set -- "${args[@]+"${args[@]}"}"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dest)     shift; DEST="${1:-}";     shift ;;
            --src)      shift; SRC="${1:-}";      shift ;;
            --keep)     shift; KEEP="${1:-}";     shift ;;
            --dry-run)  DRY_RUN=1;                shift ;;
            --verbose)  VERBOSE=1;                shift ;;
            -d)         shift; DEST="${1:-}";     shift ;;
            -s)         shift; SRC="${1:-}";      shift ;;
            -k)         shift; KEEP="${1:-}";     shift ;;
            -n)         DRY_RUN=1;                shift ;;
            -v)         VERBOSE=1;                shift ;;
            --)         shift; break ;;
            -*)         die "Unknown option: $1" ;;
            *)          break ;;
        esac
    done

    # Remaining positional: subcommand
    SUBCOMMAND="${1:-}"
    [[ -n "${2:-}" ]] && warn "Extra positional arguments ignored: ${*:2}"
}

# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
require_dest() {
    [[ -n "$DEST" ]] || die "--dest is required for this subcommand"
    if [[ "$DRY_RUN" -eq 0 ]]; then
        mkdir -p "$DEST"
    fi
}

require_src() {
    [[ -n "$SRC" ]] || die "--src is required for this subcommand"
    [[ -e "$SRC" ]] || die "Source does not exist: $SRC"
}

validate_keep() {
    [[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || die "--keep must be a positive integer (got: $KEEP)"
}

# ---------------------------------------------------------------------------
# Archive naming
# ---------------------------------------------------------------------------
archive_name() {
    local ts
    ts=$(date +"$TIMESTAMP_FMT")
    # Base name from last component of SRC, with non-alphanumeric chars replaced
    local base
    base=$(basename "$SRC" | tr -cs '[:alnum:]_-' '_')
    printf '%s_%s%s' "$base" "$ts" "$ARCHIVE_EXT"
}

# ---------------------------------------------------------------------------
# Subcommand: create
# ---------------------------------------------------------------------------
cmd_create() {
    require_src
    require_dest
    validate_keep

    local archive
    archive="$DEST/$(archive_name)"

    log "Creating backup: $SRC -> $archive"
    verbose "Dry-run: $DRY_RUN"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "[dry-run] Would create: $archive"
        return 0
    fi

    tar \
        --create \
        --gzip \
        ${VERBOSE:+--verbose} \
        --file="$archive" \
        -- \
        "$SRC"

    log "Backup created: $archive"

    # Auto-prune after creating
    cmd_prune
}

# ---------------------------------------------------------------------------
# Subcommand: restore
# ---------------------------------------------------------------------------
cmd_restore() {
    require_dest   # DEST is the restore target directory

    # If --src points directly to an archive, use it; otherwise find the latest
    local archive=""
    if [[ -f "$SRC" ]]; then
        archive="$SRC"
    elif [[ -n "$DEST" && -d "$DEST" ]]; then
        # Pick the most recent archive in DEST
        archive=$(
            find "$DEST" -maxdepth 1 -name "*${ARCHIVE_EXT}" -type f \
                | sort \
                | tail -n 1
        )
        [[ -n "$archive" ]] || die "No archives found in: $DEST"
        verbose "No --src given; using latest archive: $archive"
    else
        die "--src must be a path to an archive, or --dest must contain archives"
    fi

    [[ -f "$archive" ]] || die "Archive not found: $archive"

    local restore_dir="${SRC_RESTORE:-$DEST}"
    log "Restoring: $archive -> $restore_dir"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "[dry-run] Would restore: $archive -> $restore_dir"
        verbose "Archive contents:"
        tar --list --gzip --file="$archive" >&2
        return 0
    fi

    mkdir -p "$restore_dir"
    tar \
        --extract \
        --gzip \
        ${VERBOSE:+--verbose} \
        --file="$archive" \
        --directory="$restore_dir"

    log "Restore complete: $restore_dir"
}

# ---------------------------------------------------------------------------
# Subcommand: list
# ---------------------------------------------------------------------------
cmd_list() {
    require_dest

    local archives=()
    while IFS= read -r -d '' f; do
        archives+=("$f")
    done < <(find "$DEST" -maxdepth 1 -name "*${ARCHIVE_EXT}" -type f -print0 | sort -z)

    if [[ "${#archives[@]}" -eq 0 ]]; then
        log "No archives found in: $DEST"
        return 0
    fi

    printf '%-6s  %-12s  %s\n' "SIZE" "MODIFIED" "ARCHIVE"
    printf '%-6s  %-12s  %s\n' "------" "------------" "-------"
    for f in "${archives[@]}"; do
        local size modified
        size=$(du -sh "$f" 2>/dev/null | cut -f1)
        # Portable mtime: stat differs between macOS and Linux
        if stat --version 2>/dev/null | grep -q GNU; then
            modified=$(stat -c '%y' "$f" | cut -c1-19)
        else
            modified=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$f" 2>/dev/null || echo "unknown")
        fi
        printf '%-6s  %-12s  %s\n' "$size" "$modified" "$(basename "$f")"
    done
    printf '\nTotal: %d archive(s) in %s\n' "${#archives[@]}" "$DEST"
}

# ---------------------------------------------------------------------------
# Subcommand: prune
# ---------------------------------------------------------------------------
cmd_prune() {
    require_dest
    validate_keep

    local archives=()
    while IFS= read -r -d '' f; do
        archives+=("$f")
    done < <(find "$DEST" -maxdepth 1 -name "*${ARCHIVE_EXT}" -type f -print0 | sort -z)

    local total="${#archives[@]}"
    local excess=$(( total - KEEP ))

    if [[ "$excess" -le 0 ]]; then
        verbose "Prune: $total archive(s) present, keeping $KEEP — nothing to remove"
        return 0
    fi

    log "Prune: $total archive(s) found, keeping $KEEP, removing $excess"

    local i=0
    for f in "${archives[@]}"; do
        if [[ "$i" -ge "$excess" ]]; then
            break
        fi
        if [[ "$DRY_RUN" -eq 1 ]]; then
            log "[dry-run] Would remove: $f"
        else
            verbose "Removing: $f"
            rm -f -- "$f"
            log "Removed: $f"
        fi
        (( i++ )) || true
    done
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    cat >&2 <<EOF
Usage: $(basename "$0") [OPTIONS] SUBCOMMAND

Subcommands:
  create   Create a timestamped .tar.gz archive of --src in --dest
  restore  Extract the latest (or specified --src) archive into --dest
  list     List archives in --dest
  prune    Remove oldest archives in --dest, keeping --keep most recent

Options:
  -d, --dest DIR        Destination directory for archives
  -s, --src PATH        Source path (create/restore) or archive path (restore)
  -k, --keep N          Number of archives to retain during prune (default: $KEEP)
  -n, --dry-run         Print what would be done without making changes
  -v, --verbose         Enable verbose output

Examples:
  $(basename "$0") --src /data/db --dest /backups create
  $(basename "$0") --dest /backups list
  $(basename "$0") --dest /backups --keep 5 prune
  $(basename "$0") --src /backups/db_20240101T120000.tar.gz --dest /restore restore
EOF
    exit 1
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
main() {
    parse_args "$@"

    [[ -n "$SUBCOMMAND" ]] || usage

    acquire_lock

    case "$SUBCOMMAND" in
        create)  cmd_create  ;;
        restore) cmd_restore ;;
        list)    cmd_list    ;;
        prune)   cmd_prune   ;;
        help|-h|--help) usage ;;
        *) die "Unknown subcommand: $SUBCOMMAND" ;;
    esac
}

main "$@"
