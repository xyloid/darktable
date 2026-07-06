"""Discovery of darktable remote-edit sessions.

Mirrors the writer in `src/control/remote_discovery.c`: darktable writes one
record per running process to `<configdir>/mcp/session-<pid>.json`, 0600,
containing:

    {
      "protocol": "org.darktable.remote-edit",
      "protocol_version": 1,
      "darktable_version": "5.x",
      "pid": 12345,
      "host": "127.0.0.1",
      "port": 43127,
      "token": "base64url-encoded-random-token",
      "started_at": "2026-07-05T15:04:05Z"
    }

Selection order (per the design spec's "Server lifecycle and discovery"
section):

    1. an explicit discovery path from sidecar configuration;
    2. an explicit PID;
    3. the newest live session.

A record is "live" if its schema is well-formed and its `pid` is a running
process. This module never scans arbitrary ports and never opens a
connection -- it only reads and validates JSON files on disk. Whether the
token is actually accepted by that process is discovered on the first
`hello` (see `protocol.py`); a record can be schema-valid, its pid alive,
and still turn out to be stale if the process is a same-pid coincidence
after a fast restart (astronomically unlikely, but see `protocol.py`'s
handling of an unauthorized/refused hello for the fallback).

This module has no dependency on the MCP SDK (see the package README).
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .errors import DiscoveryError

EXPECTED_PROTOCOL = "org.darktable.remote-edit"

_REQUIRED_STRING_FIELDS = ("protocol", "darktable_version", "host", "token", "started_at")
_REQUIRED_INT_FIELDS = ("protocol_version", "pid", "port")


@dataclass(frozen=True)
class DiscoveryRecord:
    """One parsed, schema-valid `session-<pid>.json` record."""

    path: Path
    protocol: str
    protocol_version: int
    darktable_version: str
    pid: int
    host: str
    port: int
    # repr=False so a stray `print(record)`/log line/traceback never leaks
    # the session token; the attribute itself is unchanged (still a plain
    # required `str`, still assigned positionally/by-keyword like any
    # other field -- `field()` without a `default` does not turn this into
    # an optional field or require reordering the fields around it).
    token: str = field(repr=False)
    started_at: str


def default_darktable_config_dir() -> Path:
    """Best-effort match for `g_get_user_config_dir()/darktable`, the
    default darktable uses for `--configdir` (see
    `src/common/file_location.c`). GLib does not special-case macOS: the
    same XDG-style logic applies on Linux and macOS; only Windows differs.

    This is only ever a *default* -- an explicit `config_dir`/
    `discovery_path` argument always wins, and the live acceptance check
    for this task points `--configdir` at a private temp directory rather
    than relying on this function at all.
    """
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or str(Path.home())
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "darktable"


def is_process_alive(pid: int) -> bool:
    """True if `pid` refers to a running process, best-effort.

    On POSIX this is `os.kill(pid, 0)` (raises without actually sending a
    signal): `ProcessLookupError` -> dead, `PermissionError` -> alive but
    owned by someone else (same-desktop-user threat model makes this rare
    for our own discovery records, but still "alive" for this check), any
    other `OSError` -> treat as dead rather than raise.

    Windows caveat: `os.kill(pid, sig)` on Windows is NOT signal-0-safe --
    CPython's Windows implementation calls `TerminateProcess(handle, sig)`
    for any `sig` other than the two `CTRL_*_EVENT` constants, so
    `os.kill(pid, 0)` would actually terminate the target process with
    exit code 0. That would be catastrophic here (it could kill the very
    darktable instance being probed), so this function never calls
    `os.kill` on Windows; it instead opens the process with
    `PROCESS_QUERY_LIMITED_INFORMATION` and checks `GetExitCodeProcess`
    for `STILL_ACTIVE`, via ctypes, with no extra dependency.
    """
    if pid <= 0:
        return False
    if sys.platform == "win32":
        return _is_process_alive_windows(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def _is_process_alive_windows(pid: int) -> bool:  # pragma: no cover - exercised only on Windows
    import ctypes

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259

    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        exit_code = ctypes.c_ulong()
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
            return False
        return exit_code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def _parse_record(path: Path) -> DiscoveryRecord | None:
    """Parses and schema-validates one record file. Returns `None` (never
    raises) for anything malformed -- a directory full of records may
    contain leftovers from a crashed process with a different pid
    (`remote_discovery.c` explicitly tolerates those on the write side; we
    tolerate them here on the read side, symmetrically), so a single bad
    file must not break discovery for every other live session.
    """
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None

    if not isinstance(raw, dict):
        return None

    for key in _REQUIRED_STRING_FIELDS:
        if not isinstance(raw.get(key), str):
            return None
    for key in _REQUIRED_INT_FIELDS:
        value = raw.get(key)
        # bool is a subclass of int in Python; reject it explicitly so a
        # literal `true`/`false` in the JSON does not pass as a port/pid.
        if isinstance(value, bool) or not isinstance(value, int):
            return None

    if raw["protocol"] != EXPECTED_PROTOCOL:
        return None
    if raw["protocol_version"] != 1:
        return None
    if raw["pid"] <= 0:
        return None
    if not (0 < raw["port"] <= 65535):
        return None
    if not raw["token"]:
        return None

    return DiscoveryRecord(
        path=path,
        protocol=raw["protocol"],
        protocol_version=raw["protocol_version"],
        darktable_version=raw["darktable_version"],
        pid=raw["pid"],
        host=raw["host"],
        port=raw["port"],
        token=raw["token"],
        started_at=raw["started_at"],
    )


def list_records(config_dir: str | Path | None = None) -> list[DiscoveryRecord]:
    """Returns every schema-valid, live record under
    `<config_dir>/mcp/session-*.json`, newest first (by `started_at`,
    which is an ISO-8601 UTC string and therefore sorts correctly as
    plain text). Never raises; a missing/unreadable `mcp` directory just
    yields an empty list.
    """
    base = Path(config_dir) if config_dir is not None else default_darktable_config_dir()
    mcp_dir = base / "mcp"
    if not mcp_dir.is_dir():
        return []

    records: list[DiscoveryRecord] = []
    for candidate in sorted(mcp_dir.glob("session-*.json")):
        record = _parse_record(candidate)
        if record is None:
            continue
        if not is_process_alive(record.pid):
            continue
        records.append(record)

    records.sort(key=lambda r: r.started_at, reverse=True)
    return records


def select_record(
    *,
    discovery_path: str | Path | None = None,
    pid: int | None = None,
    config_dir: str | Path | None = None,
) -> DiscoveryRecord:
    """Resolves one discovery record per the design spec's selection
    order: explicit path, then explicit pid, then the newest live
    session. Raises `DiscoveryError` if the requested record cannot be
    found or is stale/invalid.
    """
    if discovery_path is not None:
        path = Path(discovery_path)
        record = _parse_record(path)
        if record is None:
            raise DiscoveryError(f"not a valid darktable discovery record: {path}")
        if not is_process_alive(record.pid):
            raise DiscoveryError(
                f"stale discovery record (pid {record.pid} is not running): {path}"
            )
        return record

    records = list_records(config_dir)

    if pid is not None:
        for record in records:
            if record.pid == pid:
                return record
        raise DiscoveryError(f"no live darktable discovery record for pid {pid}")

    if not records:
        raise DiscoveryError(
            "no live darktable discovery records found; is a darktable instance "
            "running with security/enable_remote_control set?"
        )
    return records[0]
