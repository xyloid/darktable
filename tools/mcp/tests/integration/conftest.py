"""Fixtures and prerequisite gating for the darktable MCP integration suite.

Every test in this package is a *live* test: it launches a real darktable
GUI build under a display server, talks to it over the authenticated
loopback protocol, and shuts it down. That is expensive and needs a GUI
build, a display (Xvfb or an inherited ``$DISPLAY``), and the ``darktable_mcp``
package importable -- so the whole package is:

* marked ``integration`` (see ``pyproject.toml``); the fast unit suite runs
  by default and this suite only runs under ``pytest -m integration``;
* skipped **loudly, with a reason** when a prerequisite is missing -- never
  silently passed. A missing GUI build, missing display, or a darktable that
  fails to start all produce a visible skip or failure, per the plan's
  "skip with a reason, never a silent pass" rule.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from . import harness


@pytest.fixture(scope="session")
def darktable_binary() -> Path:
    binary = harness.find_darktable_binary()
    if binary is None:
        pytest.skip(
            "no darktable GUI build found (set DARKTABLE_BIN or build "
            "build/bin/darktable); integration suite needs a GUI binary"
        )
    return binary


@pytest.fixture(scope="session")
def display_server(tmp_path_factory) -> harness.DisplayServer:
    workdir = tmp_path_factory.mktemp("display")
    try:
        server = harness.DisplayServer(workdir).start()
    except harness.PrerequisiteMissing as exc:
        pytest.skip(str(exc))
    yield server
    server.stop()


def _spawn(binary: Path, display: str, workdir: Path) -> harness.DarktableInstance:
    bus = harness.PrivateSessionBus().start()
    instance = harness.DarktableInstance(
        binary=binary, workdir=workdir, display=display, dbus_address=bus.address
    )
    instance._bus = bus  # type: ignore[attr-defined]  # keep it alive + tear down
    try:
        instance.launch()
        instance.wait_for_discovery()
    except harness.DarktableLaunchError as exc:
        instance.close()
        bus.stop()
        pytest.fail(f"darktable failed to start for the integration suite:\n{exc}")
    return instance


def _teardown(instance: harness.DarktableInstance) -> None:
    instance.close()
    bus = getattr(instance, "_bus", None)
    if bus is not None:
        bus.stop()


@pytest.fixture(scope="session")
def darktable_session(darktable_binary, display_server, tmp_path_factory):
    """One long-lived darktable instance shared by the read / mutate /
    preview / scope gates (which never shut it down). Torn down at the end
    of the session."""
    workdir = tmp_path_factory.mktemp("dt-session")
    instance = _spawn(darktable_binary, display_server.display, workdir)
    yield instance
    _teardown(instance)


@pytest.fixture
def instance_factory(darktable_binary, display_server, tmp_path_factory):
    """Factory for fresh, function-scoped instances that a test intends to
    shut down itself (the M33/M42 lifecycle gates). Each gets its own temp
    tree and private D-Bus bus; all are force-torn-down at test end."""
    created: list[harness.DarktableInstance] = []

    def _make() -> harness.DarktableInstance:
        workdir = tmp_path_factory.mktemp("dt-instance")
        instance = _spawn(darktable_binary, display_server.display, workdir)
        created.append(instance)
        return instance

    yield _make

    for instance in created:
        _teardown(instance)
