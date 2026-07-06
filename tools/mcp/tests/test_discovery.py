"""Tests for discovery.py: schema validation, staleness, and selection
order. No darktable process involved -- "live" is faked by using this
test process's own pid (guaranteed alive) or a definitely-dead pid
obtained by spawning and reaping a short-lived child process.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from darktable_mcp import discovery
from darktable_mcp.errors import DiscoveryError


def _write_record(path: Path, **overrides) -> Path:
    record = {
        "protocol": "org.darktable.remote-edit",
        "protocol_version": 1,
        "darktable_version": "5.x",
        "pid": os.getpid(),
        "host": "127.0.0.1",
        "port": 43127,
        "token": "dGVzdC10b2tlbg",
        "started_at": "2026-07-05T15:04:05Z",
    }
    record.update(overrides)
    path.write_text(json.dumps(record))
    return path


@pytest.fixture
def dead_pid() -> int:
    """A pid that is guaranteed to have exited and been reaped by the
    time this fixture returns, on any platform: spawn the cheapest
    possible child, wait for it, done. A recycled-pid collision within
    the lifetime of one test run is not something we try to guard
    against further -- the same caveat exists for a real stale darktable
    discovery record.
    """
    if sys.platform == "win32":
        proc = subprocess.Popen(["cmd", "/c", "exit 0"])
    else:
        proc = subprocess.Popen(["true"])
    proc.wait()
    return proc.pid


def test_is_process_alive_true_for_self():
    assert discovery.is_process_alive(os.getpid()) is True


def test_is_process_alive_false_for_dead_pid(dead_pid):
    assert discovery.is_process_alive(dead_pid) is False


def test_is_process_alive_false_for_nonpositive_pid():
    assert discovery.is_process_alive(0) is False
    assert discovery.is_process_alive(-1) is False


def test_list_records_finds_valid_live_record(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json")

    records = discovery.list_records(tmp_path)

    assert len(records) == 1
    assert records[0].pid == os.getpid()
    assert records[0].port == 43127
    assert records[0].token == "dGVzdC10b2tlbg"
    assert records[0].protocol == "org.darktable.remote-edit"


def test_list_records_empty_when_mcp_dir_missing(tmp_path):
    assert discovery.list_records(tmp_path) == []


def test_list_records_rejects_stale_pid(tmp_path, dead_pid):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / f"session-{dead_pid}.json", pid=dead_pid)

    assert discovery.list_records(tmp_path) == []


def test_list_records_rejects_wrong_protocol_name(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json", protocol="org.example.other")

    assert discovery.list_records(tmp_path) == []


def test_list_records_rejects_wrong_protocol_version(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json", protocol_version=2)

    assert discovery.list_records(tmp_path) == []


@pytest.mark.parametrize(
    "overrides",
    [
        {"pid": "not-a-number"},
        {"port": "not-a-number"},
        {"port": 0},
        {"port": 70000},
        {"token": ""},
        {"host": 12345},
    ],
)
def test_list_records_rejects_malformed_fields(tmp_path, overrides):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json", **overrides)

    assert discovery.list_records(tmp_path) == []


def test_list_records_tolerates_garbage_file_alongside_valid_one(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    (mcp_dir / "session-99999999.json").write_text("{not json")
    _write_record(mcp_dir / "session-1.json")

    records = discovery.list_records(tmp_path)

    assert len(records) == 1
    assert records[0].pid == os.getpid()


def test_list_records_sorted_newest_first(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json", pid=os.getpid(), started_at="2026-01-01T00:00:00Z", port=1)
    _write_record(mcp_dir / "session-2.json", pid=os.getpid(), started_at="2026-06-01T00:00:00Z", port=2)
    _write_record(mcp_dir / "session-3.json", pid=os.getpid(), started_at="2026-03-01T00:00:00Z", port=3)

    records = discovery.list_records(tmp_path)

    assert [r.port for r in records] == [2, 3, 1]


def test_select_record_explicit_path(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    path = _write_record(mcp_dir / "session-1.json", port=9999)

    record = discovery.select_record(discovery_path=path)

    assert record.port == 9999


def test_select_record_explicit_path_rejects_stale(tmp_path, dead_pid):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    path = _write_record(mcp_dir / f"session-{dead_pid}.json", pid=dead_pid)

    with pytest.raises(DiscoveryError):
        discovery.select_record(discovery_path=path)


def test_select_record_explicit_path_rejects_malformed(tmp_path):
    path = tmp_path / "session-1.json"
    path.write_text("not json")

    with pytest.raises(DiscoveryError):
        discovery.select_record(discovery_path=path)


def test_select_record_by_pid(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json", pid=os.getpid(), port=1111)

    record = discovery.select_record(pid=os.getpid(), config_dir=tmp_path)

    assert record.port == 1111


def test_select_record_by_pid_not_found_raises(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(mcp_dir / "session-1.json", pid=os.getpid())

    with pytest.raises(DiscoveryError):
        discovery.select_record(pid=999999999, config_dir=tmp_path)


def test_select_record_falls_back_to_newest_live(tmp_path):
    mcp_dir = tmp_path / "mcp"
    mcp_dir.mkdir()
    _write_record(
        mcp_dir / "session-1.json",
        pid=os.getpid(),
        started_at="2026-01-01T00:00:00Z",
        port=1,
    )
    _write_record(
        mcp_dir / "session-2.json",
        pid=os.getpid(),
        started_at="2026-06-01T00:00:00Z",
        port=2,
    )

    record = discovery.select_record(config_dir=tmp_path)

    assert record.port == 2


def test_select_record_raises_when_nothing_found(tmp_path):
    with pytest.raises(DiscoveryError):
        discovery.select_record(config_dir=tmp_path)


def test_default_darktable_config_dir_ends_in_darktable():
    assert discovery.default_darktable_config_dir().name == "darktable"
