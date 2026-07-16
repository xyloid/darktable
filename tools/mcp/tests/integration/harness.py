"""Process harness for the darktable MCP integration suite.

This module owns everything about *driving a real darktable GUI process*
from a test: locating the build, launching it headless under a display
server with remote control enabled, waiting for its discovery record,
handing back an authenticated protocol client, quitting it the way a user
would, and tearing everything down deterministically so no darktable or
Xvfb or dbus-daemon process is ever leaked -- on the happy path or any
failure path.

It has three hard rules, each load-bearing for this task's security and
determinism constraints:

* **Everything lives in a temp dir.** Config, cache, library, output, the
  copied test image, the private D-Bus socket, and the Xvfb auth file are
  all created under a pytest ``tmp_path``. The session token darktable
  generates therefore only ever exists under that temp tree (in the
  ``mcp/session-<pid>.json`` record, 0600) -- never in the developer's real
  ``~/.config/darktable``.

* **Captured logs are redacted.** ``DarktableInstance.log_text()`` returns
  darktable's stderr with the live session token blanked out, so a test
  that prints a failing instance's log cannot leak the token. Teardown
  additionally persists a token-redacted copy to disk
  (``darktable.redacted.log``, written by ``close()``) so a CI job uploads
  the redacted file rather than the raw ``darktable.log``.
  :func:`assert_token_absent` is the standing check.

* **Teardown always runs.** ``close()`` escalates SIGTERM -> SIGKILL under a
  bounded deadline and always reaps the private dbus-daemon and Xvfb, from
  ``__exit__`` and from fixture finalizers, regardless of how the test
  exited.

No part of this module imports the ``mcp`` SDK; it uses the SDK-free
``darktable_mcp.discovery`` / ``darktable_mcp.protocol`` layers, exactly as
a non-MCP script would.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import signal
import subprocess
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path

from darktable_mcp import discovery
from darktable_mcp.errors import ProtocolError
from darktable_mcp.protocol import ProtocolClient

# --------------------------------------------------------------------------
# environment scrubbing
# --------------------------------------------------------------------------

# Variables that a host desktop (notably a snap-packaged terminal/editor,
# e.g. VS Code) injects to redirect GTK/GdkPixbuf/GIO module loading into a
# confined runtime. Inherited into a *system* darktable they make it dlopen
# foreign modules against a foreign libc and abort with a GLIBC_PRIVATE
# symbol-lookup error before it ever starts. A real end user launching
# darktable from a normal shell never has these set; scrubbing them makes
# the harness behave like that clean launch. (Discovered the hard way while
# bringing this harness up -- see task 11 report.)
_GTK_CONTAMINATION = (
    "GTK_PATH",
    "GTK_EXE_PREFIX",
    "GTK_DATA_PREFIX",
    "GTK_IM_MODULE_FILE",
    "GIO_MODULE_DIR",
    "GDK_PIXBUF_MODULE_FILE",
    "GDK_PIXBUF_MODULEDIR",
    "LOCPATH",
    "LD_LIBRARY_PATH",
    "LD_PRELOAD",
)

# Variables worth forwarding when present (a real display/runtime the
# harness is reusing rather than creating).
_ENV_PASSTHROUGH = ("XDG_RUNTIME_DIR", "XAUTHORITY")


def scrubbed_env(*, home: str, display: str, dbus_address: str | None) -> dict[str, str]:
    """Build a minimal, contamination-free environment for launching
    darktable. Starts from almost nothing rather than ``os.environ`` so no
    host GTK/GIO redirection can leak in."""
    env: dict[str, str] = {
        "HOME": home,
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "DISPLAY": display,
    }
    for name in _ENV_PASSTHROUGH:
        value = os.environ.get(name)
        if value:
            env[name] = value
    if dbus_address:
        env["DBUS_SESSION_BUS_ADDRESS"] = dbus_address
    return env


# --------------------------------------------------------------------------
# prerequisite discovery (drives skip-loudly behaviour in conftest)
# --------------------------------------------------------------------------


def find_repo_root(start: Path | None = None) -> Path:
    """Walk up from this file to the repository root (the checkout that
    contains ``tools/mcp`` and ``data/pixmaps``)."""
    here = (start or Path(__file__)).resolve()
    for parent in here.parents:
        if (parent / "tools" / "mcp").is_dir() and (parent / "data").is_dir():
            return parent
    raise RuntimeError("could not locate the darktable repository root from " + str(here))


def find_darktable_binary() -> Path | None:
    """Locate the *GUI* darktable executable to exercise.

    Order: an explicit ``DARKTABLE_BIN`` override, then the in-tree build
    output, then ``PATH``. Returns ``None`` when no GUI build exists so the
    caller can skip with a clear reason (a source checkout without a build,
    or a build configured ``-DBUILD_GUI=OFF``)."""
    override = os.environ.get("DARKTABLE_BIN")
    if override:
        p = Path(override)
        return p if p.is_file() and os.access(p, os.X_OK) else None
    root = find_repo_root()
    candidates = [
        root / "build" / "bin" / "darktable",
        root / "build" / "bin" / "darktable.exe",
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    found = shutil.which("darktable")
    return Path(found) if found else None


def test_image() -> Path:
    """The in-repo RAW photograph used by the live darkroom tests.

    Keeping this as a camera RAW, rather than the application logo, exercises
    the same decode and develop pipeline that remote editing is intended to
    control.
    """
    return find_repo_root() / "img" / "DSC07350.ARW"


def _display_is_reachable(display: str) -> bool:
    exe = shutil.which("xdpyinfo")
    if not exe:
        # Can't probe; assume a set DISPLAY is usable and let launch fail
        # loudly if it isn't.
        return True
    env = dict(os.environ, DISPLAY=display)
    try:
        return subprocess.run([exe], env=env, capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


# --------------------------------------------------------------------------
# display server (isolated Xvfb when available, else an existing $DISPLAY)
# --------------------------------------------------------------------------


class DisplayServer:
    """A usable X display for the GUI process.

    Prefers a private Xvfb (full isolation from any human's desktop); falls
    back to an existing, reachable ``$DISPLAY`` when Xvfb is not installed
    (e.g. a developer box) so the suite is still runnable there. Raises
    :class:`PrerequisiteMissing` when neither is available so the caller
    skips rather than hangs."""

    def __init__(self, workdir: Path):
        self._workdir = workdir
        self._proc: subprocess.Popen | None = None
        self.display: str = ""
        self.kind: str = ""

    def start(self) -> "DisplayServer":
        xvfb = shutil.which("Xvfb")
        if xvfb:
            self._start_xvfb(xvfb)
            self.kind = "xvfb"
            return self
        existing = os.environ.get("DISPLAY")
        if existing and _display_is_reachable(existing):
            self.display = existing
            self.kind = "inherited"
            return self
        raise PrerequisiteMissing(
            "no usable X display: install Xvfb (recommended, isolated) or run "
            "with a reachable $DISPLAY set"
        )

    def _start_xvfb(self, xvfb: str) -> None:
        # Pick a free display number by probing the X abstract/unix sockets.
        for num in range(99, 130):
            if not Path(f"/tmp/.X11-unix/X{num}").exists():
                display = f":{num}"
                break
        else:
            raise PrerequisiteMissing("no free X display number for Xvfb")
        authfile = self._workdir / "Xauthority"
        authfile.touch()
        self._proc = subprocess.Popen(
            [xvfb, display, "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,  # own process group -> _terminate's killpg is targeted
        )
        # Wait for it to come up.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self._proc.poll() is not None:
                raise PrerequisiteMissing("Xvfb exited immediately")
            if Path(f"/tmp/.X11-unix/X{num}").exists() and _display_is_reachable(display):
                self.display = display
                return
            time.sleep(0.2)
        self.stop()
        raise PrerequisiteMissing("Xvfb did not become ready in time")

    def stop(self) -> None:
        _terminate(self._proc)
        self._proc = None


# --------------------------------------------------------------------------
# private D-Bus session bus (per instance -> unique well-known name owner)
# --------------------------------------------------------------------------


class PrivateSessionBus:
    """A private D-Bus session bus for exactly one darktable instance.

    darktable owns the *well-known* name ``org.darktable.service`` and its
    ``Remote.Quit`` method is the orderly-shutdown entry point (it calls the
    same ``dt_control_quit()`` as closing the window). But only one process
    can own that name per bus, and darktable keeps running after losing the
    name rather than exiting -- so on a *shared* bus a ``Quit`` addressed to
    the well-known name would hit whichever instance won the race, not
    necessarily the one under test. Giving each instance its own bus makes
    it the sole owner, so ``Quit`` is unambiguous. It also means the suite
    needs no pre-existing session bus at all (important for a bare CI
    container).

    ``None``-safe by construction: if ``dbus-daemon`` is unavailable the bus
    is simply absent and D-Bus-dependent tests skip."""

    def __init__(self) -> None:
        self._proc: subprocess.Popen | None = None
        self.address: str | None = None

    def start(self) -> "PrivateSessionBus":
        exe = shutil.which("dbus-daemon")
        if not exe:
            return self
        proc = subprocess.Popen(
            [exe, "--session", "--nofork", "--print-address"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            start_new_session=True,  # own process group -> _terminate's killpg is targeted
        )
        assert proc.stdout is not None
        line = proc.stdout.readline().strip()
        if not line or proc.poll() is not None:
            _terminate(proc)
            return self
        self._proc = proc
        self.address = line
        return self

    def stop(self) -> None:
        _terminate(self._proc)
        self._proc = None


# --------------------------------------------------------------------------
# the darktable instance
# --------------------------------------------------------------------------


class PrerequisiteMissing(Exception):
    """Raised when the environment cannot support a live run; the caller
    turns it into a loud pytest skip (never a silent pass)."""


class DarktableLaunchError(Exception):
    """darktable started but never produced a valid discovery record."""


@dataclass
class DarktableInstance:
    binary: Path
    workdir: Path
    display: str
    dbus_address: str | None
    startup_timeout: float = 90.0
    _proc: subprocess.Popen | None = field(default=None, init=False)
    _record: discovery.DiscoveryRecord | None = field(default=None, init=False)
    config_dir: Path = field(init=False)
    log_path: Path = field(init=False)
    redacted_log_path: Path = field(init=False)

    def __post_init__(self) -> None:
        self.config_dir = self.workdir / "config"
        self.log_path = self.workdir / "darktable.log"
        # A token-redacted copy of the raw log, persisted to disk at teardown
        # so a CI job can upload *this* file instead of the raw darktable.log
        # (which contains darktable's untouched stderr). See close().
        self.redacted_log_path = self.workdir / "darktable.redacted.log"

    # -- lifecycle ---------------------------------------------------------

    def launch(self, *, sidecar_files: dict[str, bytes] | None = None) -> "DarktableInstance":
        for sub in ("config", "cache", "output"):
            (self.workdir / sub).mkdir(parents=True, exist_ok=True)
        source_image = test_image()
        image = self.workdir / f"test{source_image.suffix}"
        shutil.copyfile(source_image, image)

        # Optional launch-time fixture drop (e.g. a pre-seeded .xmp sidecar
        # a test wants darktable to import history from): written into
        # self.workdir next to the copied image, before the process starts.
        # Default empty -- every other caller is unaffected.
        for name, content in (sidecar_files or {}).items():
            (self.workdir / name).write_bytes(content)

        env = scrubbed_env(
            home=str(self.workdir / "home"),
            display=self.display,
            dbus_address=self.dbus_address,
        )
        (self.workdir / "home").mkdir(parents=True, exist_ok=True)

        argv = [
            str(self.binary),
            "--configdir", str(self.config_dir),
            "--cachedir", str(self.workdir / "cache"),
            "--library", str(self.workdir / "library.db"),
            # The single opt-in preference (design spec / remote_server.h).
            "--conf", "security/enable_remote_control=TRUE",
            # Skip the first-run modal welcome screen, which otherwise blocks
            # startup *before* the remote server starts and would hang the
            # discovery wait forever.
            "--conf", "ui/show_welcome_screen=FALSE",
            # Keep the run hermetic and deterministic.
            "--conf", "write_sidecar_files=never",
            "--conf", "opencl=FALSE",
            str(image),
        ]
        log = open(self.log_path, "wb")
        try:
            self._proc = subprocess.Popen(
                argv, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True
            )
        finally:
            log.close()
        return self

    def wait_for_discovery(self) -> discovery.DiscoveryRecord:
        assert self._proc is not None
        mcp_dir = self.config_dir / "mcp"
        deadline = time.monotonic() + self.startup_timeout
        while time.monotonic() < deadline:
            rc = self._proc.poll()
            if rc is not None:
                raise DarktableLaunchError(
                    f"darktable exited (code {rc}) before writing a discovery record.\n"
                    + self._log_tail()
                )
            records = discovery.list_records(self.config_dir)
            mine = [r for r in records if r.pid == self._proc.pid]
            if mine:
                self._record = mine[0]
                return self._record
            # Also accept a record whose pid is the process even before the
            # liveness list has it (defensive against slow pid propagation).
            if mcp_dir.is_dir():
                for candidate in mcp_dir.glob("session-*.json"):
                    rec = discovery._parse_record(candidate)
                    if rec and rec.pid == self._proc.pid:
                        self._record = rec
                        return rec
            time.sleep(0.25)
        raise DarktableLaunchError(
            f"no discovery record after {self.startup_timeout}s.\n" + self._log_tail()
        )

    @property
    def pid(self) -> int:
        assert self._proc is not None
        return self._proc.pid

    @property
    def record(self) -> discovery.DiscoveryRecord:
        assert self._record is not None, "call wait_for_discovery() first"
        return self._record

    def client(self, *, connect_timeout: float = 15.0, request_timeout: float = 90.0) -> ProtocolClient:
        return ProtocolClient(
            self.record, connect_timeout=connect_timeout, request_timeout=request_timeout
        )

    def is_alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def discovery_record_exists(self) -> bool:
        return bool(list((self.config_dir / "mcp").glob("session-*.json")))

    # -- shutdown ----------------------------------------------------------

    def can_quit_cleanly(self) -> bool:
        return bool(self.dbus_address) and shutil.which("gdbus") is not None

    def quit_via_dbus(self, timeout: float = 60.0) -> int:
        """Ask darktable to quit the way a user would (D-Bus ``Remote.Quit``
        -> ``dt_control_quit()``), then wait for the process and return its
        exit code. Raises if D-Bus quit is unavailable -- callers gate on
        :meth:`can_quit_cleanly`."""
        assert self._proc is not None
        if not self.can_quit_cleanly():
            raise PrerequisiteMissing("clean quit needs a private dbus session and gdbus")
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=self.dbus_address or "")
        subprocess.run(
            [
                "gdbus", "call", "--session",
                "--dest", "org.darktable.service",
                "--object-path", "/darktable",
                "--method", "org.darktable.service.Remote.Quit",
            ],
            env=env,
            capture_output=True,
            timeout=30,
            check=True,
        )
        return self._proc.wait(timeout=timeout)

    def wait(self, timeout: float) -> int:
        assert self._proc is not None
        return self._proc.wait(timeout=timeout)

    def close(self) -> None:
        """Deterministic teardown: SIGTERM -> SIGKILL the process group under
        a bounded deadline, then persist a token-redacted copy of the log.
        Safe to call more than once."""
        _terminate(self._proc)
        self._proc = None
        self._persist_redacted_log()

    # -- logs (token-redacted) --------------------------------------------

    def _persist_redacted_log(self) -> None:
        """Write a token-redacted copy of the raw log to disk so it can be
        safely captured as a CI artifact. Best-effort and idempotent: the
        process has stopped writing by the time close() calls this, and a
        missing/unreadable log simply yields no redacted copy.

        Redaction sources: the record the harness parsed, plus a sweep of
        any ``session-*.json`` files still on disk -- launch can fail after
        darktable generated a token but before the harness read the record,
        and the ``.redacted.log`` filename must never over-promise. If no
        token can be found anywhere, none was ever knowable to the harness;
        the copy is prefixed with a marker saying so rather than silently
        posing as redacted."""
        try:
            raw = self.log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return
        tokens: set[str] = set()
        if self._record and self._record.token:
            tokens.add(self._record.token)
        try:
            for candidate in (self.config_dir / "mcp").glob("session-*.json"):
                rec = discovery._parse_record(candidate)
                if rec and rec.token:
                    tokens.add(rec.token)
        except OSError:
            pass
        redacted = raw
        for token in tokens:
            redacted = redact_token(redacted, token)
        if not tokens:
            redacted = (
                "# NOTE: the harness learned no session token for this instance and\n"
                "# found no discovery record on disk to recover one from (launch\n"
                "# likely failed before token generation), so there was no token to\n"
                "# redact from the raw log below.\n"
            ) + redacted
        try:
            self.redacted_log_path.write_text(redacted, encoding="utf-8")
        except OSError:
            pass

    def log_text(self) -> str:
        try:
            raw = self.log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""
        return redact_token(raw, self._record.token if self._record else None)

    def _log_tail(self, lines: int = 25) -> str:
        return "\n".join(self.log_text().splitlines()[-lines:])

    # -- context manager ---------------------------------------------------

    def __enter__(self) -> "DarktableInstance":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------


@asynccontextmanager
async def connected_client(instance: "DarktableInstance", **kwargs):
    """Async context manager yielding an authenticated protocol client for
    ``instance``, always closed on exit. Created fresh per use so it lives
    on the caller's event loop (safe across the session-scoped instance)."""
    client = instance.client(**kwargs)
    try:
        # The first call performs the lazy connect + hello handshake.
        await client.call("get_state")
        yield client
    finally:
        await client.close()


async def wait_for_scopes(
    client: ProtocolClient,
    expected_rev: int,
    *,
    scopes=("histogram",),
    include_images: bool = False,
    timeout: float = 45.0,
) -> dict:
    """Poll ``compute_scopes`` until it succeeds for ``expected_rev``.

    The scope capture is pushed asynchronously from the darkroom *preview*
    pipe (not the export pipe ``render_preview`` uses), so immediately after
    a mutation the buffer may still be for the previous revision or absent
    entirely (a retryable ``scope_failed``). This retries until the captured
    buffer's revision catches up to the mutation we just made. "Caught up"
    is ``>= expected_rev``, not ``==``: trailing DEVELOP_HISTORY_CHANGE
    signals can advance the revision past the mutation echo's value (see
    :func:`undo_latest`), in which case the buffer is stamped with the
    newer revision and strict equality would never be reached (the same
    drift race in another guise, surfacing as a poll timeout)."""
    deadline = time.monotonic() + timeout
    last_exc: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = await client.call(
                "compute_scopes",
                {
                    "scopes": list(scopes),
                    "include_summary": True,
                    "include_bins": False,
                    "include_images": include_images,
                    "image_size": 256,
                },
            )
            if result.get("revision", -1) >= expected_rev:
                return result
            last_exc = AssertionError(
                f"scopes revision {result.get('revision')} < expected {expected_rev}"
            )
        except ProtocolError as exc:
            if not exc.retryable:
                raise
            last_exc = exc
        await asyncio.sleep(0.5)
    raise AssertionError(f"scopes did not reach revision {expected_rev} in {timeout}s: {last_exc}")


async def wait_for_stable_revision(
    client: ProtocolClient,
    *,
    consecutive_reads: int = 5,
    poll_interval: float = 0.2,
    timeout: float = 4.0,
) -> int:
    """Return the revision after a sustained run of identical reads.

    A single quiet polling interval is not enough to prove that deferred
    DEVELOP_HISTORY_CHANGE deliveries have drained. Requiring several equal
    reads gives those deliveries a bounded window to arrive; failure to become
    quiescent is reported explicitly instead of silently using the last value.
    """
    if consecutive_reads < 2:
        raise ValueError("consecutive_reads must be at least 2")

    deadline = time.monotonic() + timeout
    current: int | None = None
    matching_reads = 0
    while time.monotonic() < deadline:
        revision = (await client.call("get_state"))["revision"]
        if revision == current:
            matching_reads += 1
        else:
            current = revision
            matching_reads = 1
        if matching_reads >= consecutive_reads:
            return revision
        await asyncio.sleep(poll_interval)

    raise AssertionError(
        f"revision did not stabilize after {timeout}s "
        f"(last revision {current}, {matching_reads}/{consecutive_reads} matching reads)"
    )


async def undo_latest(client: ProtocolClient, *, retries: int = 6) -> dict:
    """Compare-and-undo the latest transition, tolerating benign revision
    drift.

    ``undo`` requires ``expected_revision`` to equal the *live* revision. But
    the process-local revision can keep advancing asynchronously for a beat
    after a mutation's response returns (darktable emits the
    DEVELOP_HISTORY_CHANGE signal that bumps the counter slightly after the
    mutation call completes), so an undo keyed on a separately-read
    ``get_state`` revision can spuriously ``revision_conflict``. The correct
    client pattern (and what a well-behaved caller does) is to re-read the
    revision and retry the compare-and-undo; this helper does exactly that a
    bounded number of times."""
    from darktable_mcp.errors import ProtocolError

    last: Exception | None = None
    for _ in range(retries):
        state = await client.call("get_state")
        try:
            return await client.call("undo", {"expected_revision": state["revision"]})
        except ProtocolError as exc:
            if exc.code != "revision_conflict":
                raise
            last = exc
            await asyncio.sleep(0.2)
    raise AssertionError(f"undo kept conflicting on revision drift: {last}")


def redact_token(text: str, token: str | None) -> str:
    if token:
        text = text.replace(token, "<REDACTED-SESSION-TOKEN>")
    return text


def assert_token_absent(text: str, token: str) -> None:
    """Standing security check: a captured/rendered log must never contain
    the live session token."""
    assert token, "no token to check for"
    assert token not in text, "session token leaked into captured output"


def _terminate(proc: subprocess.Popen | None, *, deadline: float = 15.0) -> None:
    """Escalating, bounded teardown of a subprocess and its process group."""
    if proc is None or proc.poll() is not None:
        return
    # Prefer signalling the whole group (darktable spawns worker threads /
    # possibly helpers); the process was started with start_new_session.
    def _signal(sig: int) -> None:
        try:
            os.killpg(os.getpgid(proc.pid), sig)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                proc.send_signal(sig)
            except (ProcessLookupError, OSError):
                pass

    _signal(signal.SIGTERM)
    try:
        proc.wait(timeout=deadline)
        return
    except subprocess.TimeoutExpired:
        pass
    _signal(signal.SIGKILL)
    try:
        proc.wait(timeout=deadline)
    except subprocess.TimeoutExpired:
        pass
