# darktable-mcp development environment. Source this file -- do not run it:
#
#     . tools/mcp/env.sh        # from the repo root (any cwd works)
#
# Idempotent. On first use it creates tools/mcp/.venv and installs the
# sidecar with its [dev] extras; every use activates the venv, exports
# DARKTABLE_BIN to the in-tree build when one exists (never overriding a
# value you already set), and defines the dt-mcp-darktable helper that
# launches darktable with remote control enabled in a scratch config dir.

# --- refuse execution; this file must be sourced (needs bash or zsh) -------
if [ -n "${BASH_SOURCE:-}" ]; then
  if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    echo "env.sh: source this file, don't execute it:  . ${BASH_SOURCE[0]}" >&2
    exit 1
  fi
  _dt_mcp_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
elif [ -n "${ZSH_VERSION:-}" ]; then
  case ":${ZSH_EVAL_CONTEXT:-}:" in
    *:file:*) ;;
    *) echo "env.sh: source this file, don't execute it:  . $0" >&2; exit 1 ;;
  esac
  _dt_mcp_dir=$(cd "$(dirname "${(%):-%N}")" && pwd)
else
  echo "env.sh: unsupported shell -- source from bash or zsh" >&2
  return 1 2>/dev/null || exit 1
fi

_dt_mcp_venv="$_dt_mcp_dir/.venv"
_dt_mcp_root=$(cd "$_dt_mcp_dir/../.." && pwd)

# --- venv: create on first use, repair a half-made one ---------------------
if [ ! -x "$_dt_mcp_venv/bin/python" ]; then
  echo "env.sh: creating venv at $_dt_mcp_venv" >&2
  python3 -m venv "$_dt_mcp_venv" || return 1
fi
if ! "$_dt_mcp_venv/bin/python" -c 'import darktable_mcp, pytest' 2>/dev/null; then
  echo "env.sh: installing darktable-mcp[dev] into the venv" >&2
  "$_dt_mcp_venv/bin/pip" install -q -e "$_dt_mcp_dir[dev]" || return 1
fi
. "$_dt_mcp_venv/bin/activate"

# --- DARKTABLE_BIN: point the integration suite at the in-tree build -------
# (tests/integration/harness.py finds build/bin/darktable by itself, so this
# is a convenience for out-of-tree builds and explicitness; a value you set
# before sourcing always wins.)
if [ -z "${DARKTABLE_BIN:-}" ] && [ -x "$_dt_mcp_root/build/bin/darktable" ]; then
  export DARKTABLE_BIN="$_dt_mcp_root/build/bin/darktable"
fi

# --- helper: launch darktable for manual sidecar runs ----------------------
dt-mcp-darktable() {
  # Usage: dt-mcp-darktable [CONFIGDIR]   (default /tmp/dt-mcp-scratch)
  # Enables security/enable_remote_control in a scratch config dir and
  # launches $DARKTABLE_BIN with it, keeping your real profile untouched.
  local cfg="${1:-/tmp/dt-mcp-scratch}"
  if [ -z "${DARKTABLE_BIN:-}" ] || [ ! -x "$DARKTABLE_BIN" ]; then
    echo "dt-mcp-darktable: no darktable binary -- build the repo (build/bin/darktable) or export DARKTABLE_BIN" >&2
    return 1
  fi
  mkdir -p "$cfg" || return 1
  grep -qs '^security/enable_remote_control=TRUE' "$cfg/darktablerc" \
    || echo 'security/enable_remote_control=TRUE' >> "$cfg/darktablerc"
  echo "dt-mcp-darktable: launching $DARKTABLE_BIN --configdir $cfg"
  echo "dt-mcp-darktable: discovery record will appear as $cfg/mcp/session-<pid>.json"
  echo "dt-mcp-darktable: connect with:  darktable-mcp --config-dir $cfg"
  "$DARKTABLE_BIN" --configdir "$cfg"
}

echo "darktable-mcp environment ready:"
echo "  venv           $VIRTUAL_ENV"
echo "  DARKTABLE_BIN  ${DARKTABLE_BIN:-(none -- integration suite will look in PATH or skip)}"
echo "  unit tests     pytest            (fast, no darktable needed)"
echo "  integration    pytest -m integration"
echo "  manual run     dt-mcp-darktable [CONFIGDIR], then: darktable-mcp --config-dir CONFIGDIR"

unset _dt_mcp_dir _dt_mcp_venv _dt_mcp_root
