#!/usr/bin/env bash
# backup.sh — backup utility with create/restore/list/prune subcommands
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEST=""
SRC=""
KEEP=10
DRY_RUN=0
VERBOSE=0
LOCKFILE="/tmp/backup.sh.lock"
ARCHIVE_PREFIX="backup"
TIMESTAMP_FMT="%Y%m%dT%H%M%S"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
vlog() { [[ "$VERBOSE" -eq 1 ]] && log "$@" || true; }
die()  { log "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Cleanup / lockfile
# ---------------------------------------------------------------------------
cleanup() {
    local exit_code=$?
    if [[ -f "$LOCKFILE" ]] && [[ "$(cat "$LOCKFILE" 2>/dev/null)" == "$$" ]]; then
        vlog "Removing lockfile $LOCKFILE"
        rm -f "$LOCKFILE"
    fi
    exit "$exit_code"
}
trap cleanup EXIT
trap 'die "Interrupted"' INT TERM

acquire_lock() {
    if [[ -f "$LOCKFILE" ]]; then
        local existing_pid
        existing_pid=$(cat "$LOCKFILE" 2>/dev/null || true)
        if [[ -n "$existing_pid" ]] && kill -0 "$existing_pid" 2>/dev/null; then
            die "Another backup is already running (PID $existing_pid). Lockfile: $LOCKFILE"
        else
            log "Stale lockfile found (PID $existing_pid gone). Removing."
            rm -f "$LOCKFILE"
        fi
    fi
    printf '%s' "$$" > "$LOCKFILE"
    vlog "Lock acquired (PID $$, $LOCKFILE)"
}

# ---------------------------------------------------------------------------
# Argument parsing
# Supports both --flag value and --flag=value forms.
# ---------------------------------------------------------------------------
parse_args() {
    local subcommand="${1:-}"
    [[ -z "$subcommand" ]] && { usage; exit 1; }
    case "$subcommand" in
        create|restore|list|prune) SUBCOMMAND="$subcommand" ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown subcommand: $subcommand. Use create, restore, list, or prune." ;;
    esac
    shift

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dest)         DEST="${2:?'--dest requires a value'}"; shift 2 ;;
            --dest=*)       DEST="${1#*=}"; shift ;;
            --src)          SRC="${2:?'--src requires a value'}"; shift 2 ;;
            --src=*)        SRC="${1#*=}"; shift ;;
            --keep)         KEEP="${2:?'--keep requires a value'}"; shift 2 ;;
            --keep=*)       KEEP="${1#*=}"; shift ;;
            --dry-run)      DRY_RUN=1; shift ;;
            --verbose|-v)   VERBOSE=1; shift ;;
            -h|--help)      usage; exit 0 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
}

usage() {
    cat <<EOF
Usage: backup.sh <subcommand> [options]

Subcommands:
  create   Create a timestamped tar.gz archive of --src into --dest
  restore  Extract a backup archive from --src into --dest
  list     List archives in --dest, newest first
  prune    Remove old archives in --dest, keeping the --keep most recent

Options:
  --dest <dir>     Destination directory (required for create/list/prune)
  --src  <path>    Source path (required for create) or archive path (restore)
  --keep <n>       Number of archives to keep during prune (default: $KEEP)
  --dry-run        Show what would be done without making changes
  --verbose, -v    Print extra detail
  -h, --help       Show this message

Examples:
  backup.sh create  --src /home/alice/projects --dest /mnt/backups
  backup.sh restore --src /mnt/backups/backup_20240501T120000.tar.gz --dest /tmp/restore
  backup.sh list    --dest /mnt/backups
  backup.sh prune   --dest /mnt/backups --keep 5
EOF
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
require_dest() { [[ -n "$DEST" ]] || die "--dest is required for this subcommand"; }
require_src()  { [[ -n "$SRC"  ]] || die "--src is required for this subcommand"; }

list_archives() {
    # Print archive paths sorted newest-first (relies on timestamp in name).
    find "$DEST" -maxdepth 1 -name "${ARCHIVE_PREFIX}_*.tar.gz" -type f \
        | sort -r
}

dry_run_prefix() {
    [[ "$DRY_RUN" -eq 1 ]] && printf '[DRY-RUN] ' || true
}

# ---------------------------------------------------------------------------
# Subcommand: create
# ---------------------------------------------------------------------------
cmd_create() {
    require_src
    require_dest

    [[ -e "$SRC" ]] || die "Source does not exist: $SRC"
    [[ -d "$DEST" ]] || {
        log "Destination directory does not exist; creating: $DEST"
        if [[ "$DRY_RUN" -eq 0 ]]; then
            mkdir -p "$DEST"
        else
            log "[DRY-RUN] Would create directory: $DEST"
        fi
    }

    local timestamp archive_name archive_path
    timestamp=$(date +"$TIMESTAMP_FMT")
    archive_name="${ARCHIVE_PREFIX}_${timestamp}.tar.gz"
    archive_path="${DEST%/}/$archive_name"

    log "$(dry_run_prefix)Creating archive: $archive_path"
    vlog "  Source: $SRC"

    if [[ "$DRY_RUN" -eq 0 ]]; then
        local tar_opts="-czf"
        [[ "$VERBOSE" -eq 1 ]] && tar_opts="-czvf"
        tar "$tar_opts" "$archive_path" -C "$(dirname "$SRC")" "$(basename "$SRC")"
        local size
        size=$(du -sh "$archive_path" 2>/dev/null | cut -f1 || printf "?")
        log "Archive created: $archive_path ($size)"
    else
        log "[DRY-RUN] Would run: tar -czf $archive_path -C $(dirname "$SRC") $(basename "$SRC")"
    fi
}

# ---------------------------------------------------------------------------
# Subcommand: restore
# ---------------------------------------------------------------------------
cmd_restore() {
    require_src
    require_dest

    [[ -f "$SRC" ]] || die "Archive not found: $SRC"

    if [[ "$DRY_RUN" -eq 0 ]]; then
        mkdir -p "$DEST"
    fi

    log "$(dry_run_prefix)Restoring archive: $SRC -> $DEST"

    if [[ "$DRY_RUN" -eq 0 ]]; then
        local tar_opts="-xzf"
        [[ "$VERBOSE" -eq 1 ]] && tar_opts="-xzvf"
        tar "$tar_opts" "$SRC" -C "$DEST"
        log "Restore complete: $DEST"
    else
        log "[DRY-RUN] Would run: tar -xzf $SRC -C $DEST"
    fi
}

# ---------------------------------------------------------------------------
# Subcommand: list
# ---------------------------------------------------------------------------
cmd_list() {
    require_dest
    [[ -d "$DEST" ]] || die "Destination directory does not exist: $DEST"

    local archives
    mapfile -t archives < <(list_archives)

    if [[ "${#archives[@]}" -eq 0 ]]; then
        log "No archives found in $DEST"
        return 0
    fi

    printf '%-6s  %-30s  %s\n' "INDEX" "ARCHIVE" "SIZE"
    printf '%-6s  %-30s  %s\n' "-----" "-------" "----"
    local idx=1
    for archive in "${archives[@]}"; do
        local name size
        name=$(basename "$archive")
        size=$(du -sh "$archive" 2>/dev/null | cut -f1 || printf "?")
        printf '%-6s  %-30s  %s\n' "$idx" "$name" "$size"
        (( idx++ ))
    done
    log "Total: ${#archives[@]} archive(s) in $DEST"
}

# ---------------------------------------------------------------------------
# Subcommand: prune
# ---------------------------------------------------------------------------
cmd_prune() {
    require_dest
    [[ -d "$DEST" ]] || die "Destination directory does not exist: $DEST"

    # Validate --keep is a positive integer.
    [[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || die "--keep must be a positive integer, got: $KEEP"

    local archives
    mapfile -t archives < <(list_archives)
    local total="${#archives[@]}"

    if [[ "$total" -le "$KEEP" ]]; then
        log "Nothing to prune: $total archive(s) present, keeping $KEEP"
        return 0
    fi

    local to_delete=(  )
    local idx=0
    for archive in "${archives[@]}"; do
        (( idx++ ))
        if [[ "$idx" -gt "$KEEP" ]]; then
            to_delete+=("$archive")
        fi
    done

    log "$(dry_run_prefix)Pruning ${#to_delete[@]} archive(s) (keeping newest $KEEP of $total)"
    for archive in "${to_delete[@]}"; do
        log "$(dry_run_prefix)Deleting: $(basename "$archive")"
        if [[ "$DRY_RUN" -eq 0 ]]; then
            rm -f "$archive"
        fi
    done

    if [[ "$DRY_RUN" -eq 0 ]]; then
        log "Prune complete."
    fi
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
main() {
    [[ $# -ge 1 ]] || { usage; exit 1; }
    parse_args "$@"
    acquire_lock

    case "$SUBCOMMAND" in
        create)  cmd_create  ;;
        restore) cmd_restore ;;
        list)    cmd_list    ;;
        prune)   cmd_prune   ;;
    esac
}

main "$@"
