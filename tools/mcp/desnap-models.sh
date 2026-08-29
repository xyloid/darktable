#!/usr/bin/env bash
# desnap-models.sh -- undo snap-confined XDG_DATA_HOME leakage for darktable.
#
# Snap-confined editors (VS Code and friends) export
#
#     XDG_DATA_HOME=$HOME/snap/<snap>/<rev>/.local/share
#
# to every process started from their integrated terminal. darktable resolves
# its AI models directory as g_get_user_data_dir()/darktable/models
# (src/ai/backend_common.c), so a darktable launched from such a terminal
# downloads hundreds of MB of models into a snap *revision* directory. The
# next snap update bumps <rev>, the path moves, and the models vanish from
# darktable's view.
#
# This script moves any models found under those snap paths into the real
#
#     $HOME/.local/share/darktable/models
#
# and clears away the leftover snap-side darktable directories and the
# compatibility symlinks that point at the canonical location.
#
# It NEVER touches ~/snap/darktable -- that is the Snap Store darktable
# package's own data (config, styles, library.db) and is not leakage.
#
# Usage:
#   tools/mcp/desnap-models.sh              # dry run: report what would change
#   tools/mcp/desnap-models.sh --apply      # actually do it
#   tools/mcp/desnap-models.sh --apply --snaps "code code-insiders cursor"
#
# Exit status: 0 clean, 1 usage error, 2 finished with something unresolved.

set -euo pipefail
shopt -s nullglob

# snaps whose XDG_DATA_HOME redirect we clean up. deliberately a list, not a
# wildcard: ~/snap/darktable must never be swept up by this.
SNAPS=(code code-insiders)
APPLY=0

CANON="$HOME/.local/share/darktable/models"

warned=0
seen=()

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --apply)  APPLY=1; shift ;;
    --snaps)  [ $# -ge 2 ] || { echo "--snaps needs an argument" >&2; exit 1; }
              read -r -a SNAPS <<<"$2"; shift 2 ;;
    -h|--help) usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage 1 ;;
  esac
done

say()  { printf '%s\n' "$*"; }
act()  { if [ "$APPLY" = 1 ]; then printf '  %s\n' "$*"; else printf '  [dry-run] %s\n' "$*"; fi; }
warn() { printf 'warning: %s\n' "$*" >&2; warned=1; }

run() { if [ "$APPLY" = 1 ]; then "$@"; fi; }

# --- refuse to run against a live darktable -------------------------------
# moving a model directory out from under a loaded ONNX session is asking for
# trouble; the inode survives but the next scan will not find the id.
if [ "$APPLY" = 1 ] && pgrep -x darktable >/dev/null 2>&1; then
  warn "darktable is running -- quit it first, then re-run with --apply"
  exit 2
fi

say "canonical models directory: $CANON"
if [ "$APPLY" = 1 ]; then
  mkdir -p "$CANON"
elif [ ! -d "$CANON" ]; then
  say "  (would be created)"
fi
say

found_any=0

for snap in "${SNAPS[@]}"; do
  # guard: never sweep the snap darktable package's own data
  if [ "$snap" = "darktable" ]; then
    warn "refusing to process ~/snap/darktable -- that is the darktable snap's own config/library, not leakage"
    continue
  fi

  for dtdir in "$HOME/snap/$snap"/*/.local/share/darktable; do
    # ~/snap/<snap>/current is a symlink to the active revision -- resolve and
    # skip repeats so the same directory is not processed (or reported) twice
    real=$(readlink -f "$dtdir")
    case " ${seen[*]-} " in *" $real "*) continue ;; esac
    seen+=("$real")

    found_any=1
    say "found: $dtdir"

    mdir="$dtdir/models"
    removed_models=0

    if [ -L "$mdir" ]; then
      # a compatibility symlink (possibly one this script created earlier)
      target=$(readlink -f "$mdir" || true)
      if [ "$target" = "$(readlink -f "$CANON" 2>/dev/null || echo "$CANON")" ]; then
        act "remove symlink models -> $target"
        run rm "$mdir"
        removed_models=1
      else
        warn "$mdir is a symlink to $target (not the canonical dir) -- leaving it alone"
        continue
      fi

    elif [ -d "$mdir" ]; then
      # real model payload: migrate each model id that the canonical dir lacks
      for model in "$mdir"/*; do
        [ -d "$model" ] || continue
        id=$(basename "$model")
        if [ -e "$CANON/$id" ]; then
          warn "$CANON/$id already exists -- leaving $model in place, resolve by hand"
          continue
        fi
        act "move $id -> $CANON/$id"
        run mv "$model" "$CANON/$id"
      done

      # anything left (stray files, unresolved models) keeps the dir alive
      leftovers=("$mdir"/* "$mdir"/.[!.]*)
      if [ ${#leftovers[@]} -eq 0 ]; then
        act "rmdir $mdir"
        run rmdir "$mdir"
        removed_models=1
      else
        warn "$mdir still has ${#leftovers[@]} entries -- not removing it"
        continue
      fi
    fi

    # drop the now-empty snap-side darktable dir, and any empty parents up to
    # .local/share -- rmdir refuses non-empty dirs, which is exactly the
    # safety we want here
    if [ -d "$dtdir" ]; then
      # in a dry run models/ is still on disk although we reported removing
      # it; discount it so the count reflects the post-apply state
      remaining=()
      for e in "$dtdir"/* "$dtdir"/.[!.]*; do
        [ "$removed_models" = 1 ] && [ "$e" = "$mdir" ] && continue
        remaining+=("$e")
      done
      if [ ${#remaining[@]} -eq 0 ]; then
        act "rmdir $dtdir"
        run rmdir "$dtdir"
      else
        say "  keeping $dtdir (${#remaining[@]} other entries, not models)"
      fi
    fi
    say
  done
done

if [ "$found_any" = 0 ]; then
  say "nothing to do: no snap-side darktable data dirs for: ${SNAPS[*]}"
fi

# --- durable fix reminder -------------------------------------------------
cat <<EOF

The cleanup above only removes what leaked. To stop it recurring, pin the
path in darktable's config so XDG_DATA_HOME cannot move it (set this with
darktable CLOSED -- it rewrites darktablerc on exit), in each profile you
use:

    plugins/ai/models_path=$CANON

or set it in preferences -> AI.
EOF

if [ "$APPLY" != 1 ]; then
  say
  say "this was a dry run -- re-run with --apply to make the changes"
fi

exit $(( warned ? 2 : 0 ))
