#!/usr/bin/env bash
# darktable-mcp development environment. Source this file -- do not run it:
#
#     . tools/mcp/env.sh        # from the repo root (any cwd works)
#
# Idempotent. On first use it creates tools/mcp/.venv and installs the
# sidecar with its [dev] extras; every use activates the venv and defines
# the dt-mcp-darktable helper that launches darktable with remote control
# enabled in a scratch config dir.
#
# Exported for the rest of the shell session -- the set
# docs/ubuntu-mcp-client-setup.md asks you to redefine in each new
# terminal, so sourcing this file covers that step:
#
#     REPO              repository root of this checkout
#     DARKTABLE_BIN     in-tree GUI build: build/bin, then build-mcp/bin
#     MCP_BIN           the sidecar launcher in the venv
#     DT_CONFIG         persistent dev profile (default ~/.config/darktable-mcp-dev)
#     DT_CACHE          its cache            (default ~/.cache/darktable-mcp-dev)
#
# Each keeps a value you exported before sourcing. The DT_CONFIG and
# DT_CACHE directories are not created here -- the guide's `mkdir -p`
# still does that.

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

# --- the setup guide's shell variables -------------------------------------
# (DARKTABLE_BIN is handled after the venv, below.)
export REPO="${REPO:-$_dt_mcp_root}"
export MCP_BIN="${MCP_BIN:-$_dt_mcp_venv/bin/darktable-mcp}"
export DT_CONFIG="${DT_CONFIG:-$HOME/.config/darktable-mcp-dev}"
export DT_CACHE="${DT_CACHE:-$HOME/.cache/darktable-mcp-dev}"

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
# before sourcing always wins. build-mcp/ is the build directory
# docs/ubuntu-mcp-client-setup.md tells you to use, checked second so an
# ordinary build/ still wins.)
if [ -z "${DARKTABLE_BIN:-}" ]; then
  for _dt_mcp_cand in "$_dt_mcp_root/build/bin/darktable" "$_dt_mcp_root/build-mcp/bin/darktable"; do
    if [ -x "$_dt_mcp_cand" ]; then
      export DARKTABLE_BIN="$_dt_mcp_cand"
      break
    fi
  done
  unset _dt_mcp_cand
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
echo "  REPO           $REPO"
echo "  DARKTABLE_BIN  ${DARKTABLE_BIN:-(none -- integration suite will look in PATH or skip)}"
echo "  MCP_BIN        $MCP_BIN"
echo "  DT_CONFIG      $DT_CONFIG"
echo "  DT_CACHE       $DT_CACHE"
echo "  unit tests     pytest            (fast, no darktable needed)"
echo "  integration    pytest -m integration"
echo "  manual run     dt-mcp-darktable [CONFIGDIR], then: darktable-mcp --config-dir CONFIGDIR"

unset _dt_mcp_dir _dt_mcp_venv _dt_mcp_root
