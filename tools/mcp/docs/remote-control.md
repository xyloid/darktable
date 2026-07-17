# darktable remote control (MCP) — user & host guide

darktable can expose the **currently open darkroom image** to an external
tool over a private, local, token-authenticated protocol. The companion
[`darktable-mcp`](../README.md) sidecar turns that protocol into
[Model Context Protocol](https://modelcontextprotocol.io) tools, so an MCP
host (Claude Desktop, Claude Code, the MCP inspector, …) — and through it an
LLM agent — can inspect and edit your darkroom session: read the image, the
module stack and each module's parameters, enable/reset modules, create
instances, walk history, undo, render a preview, and read photographic scopes.

This document is for **users** enabling the feature and **host authors**
wiring the sidecar up. It is not the protocol spec; the wire contract lives
in `docs/superpowers/specs/` in the darktable source tree.

For a complete source-build and Claude Code walkthrough on Ubuntu, see
[`ubuntu-claude-code-setup.md`](ubuntu-claude-code-setup.md).

> **Remote control is off by default and must be explicitly enabled.** When
> off, darktable opens no port and writes no discovery record — there is
> nothing to connect to.

---

## 1. Enable remote control

Remote control is a single preference: **`security/enable_remote_control`**
(*preferences → security → allow remote control (MCP)*), default **false**.
It takes effect on the next darktable start.

When enabled, at the end of startup darktable:

1. generates a fresh, cryptographically random 256-bit session token;
2. binds **IPv4 loopback only** (`127.0.0.1`) on an OS-assigned port;
3. writes an atomic discovery record `session-<pid>.json` (user-only `0600`
   where the platform supports it) under an `mcp/` directory in its active
   config directory;
4. removes that record on orderly shutdown.

Ways to turn it on:

* **GUI** — set the preference and restart darktable.
* **Config file** — put `security/enable_remote_control=TRUE` in the
  `darktablerc` of the config directory you launch with.
* **Command line (per run, good for a scratch profile)** —

  ```sh
  darktable --configdir /path/to/scratch-config \
            --conf security/enable_remote_control=TRUE
  ```

  Using a dedicated `--configdir` keeps the feature (and the discovery
  record) out of your real profile.

There is deliberately **no second opt-in and no “remote” CLI subcommand**:
the one preference (optionally overridden by `--conf`) is the whole switch.

---

## 2. Point an MCP host at darktable

The sidecar speaks MCP over **stdio**; the host launches it as a child
process. It needs to find darktable's discovery record — by default it looks
in darktable's default config directory, or you tell it which config
directory / record / PID to use (see §4).

Install the sidecar (independent of darktable itself — see §6):

```sh
cd tools/mcp
python3 -m venv .venv && . .venv/bin/activate
pip install -e .        # provides the `darktable-mcp` console script
```

Then register it with your host. A typical `mcpServers`-style entry (Claude
Desktop / Claude Code and compatible hosts):

```jsonc
{
  "mcpServers": {
    "darktable": {
      "command": "darktable-mcp",
      "args": []
    }
  }
}
```

If darktable runs with a non-default config directory, tell the sidecar:

```jsonc
{
  "mcpServers": {
    "darktable": {
      "command": "darktable-mcp",
      "args": ["--config-dir", "/path/to/scratch-config"]
    }
  }
}
```

If `darktable-mcp` is not on the host's `PATH`, use the venv's absolute path
(`/path/to/tools/mcp/.venv/bin/darktable-mcp`) or
`["/path/to/.venv/bin/python", "-m", "darktable_mcp", …]`.

The sidecar resolves discovery **lazily**, on the first tool call, so it
starts cleanly even if darktable is not running yet; the first call then
reports if no live instance is found.

---

## 3. Where discovery records live

Records are `session-<pid>.json` under an `mcp/` subdirectory of darktable's
**active config directory** (the one that darktable is using this run — the
default, or whatever `--configdir` selected).

| Platform | Default config directory | Discovery records |
|---|---|---|
| Linux | `$XDG_CONFIG_HOME/darktable` (usually `~/.config/darktable`) | `~/.config/darktable/mcp/session-<pid>.json` |
| macOS | `~/.config/darktable` (GLib's user-config dir; darktable does **not** use `~/Library` here) | `~/.config/darktable/mcp/session-<pid>.json` |
| Windows | `%LOCALAPPDATA%\darktable` | `%LOCALAPPDATA%\darktable\mcp\session-<pid>.json` |
| **Custom** `--configdir DIR` (any platform) | `DIR` | `DIR/mcp/session-<pid>.json` |

The sidecar's own default search matches darktable's default config
directory. Point it elsewhere with `--config-dir DIR` when you launched
darktable with `--configdir DIR`.

Each record contains the protocol id/version, darktable version, PID,
`127.0.0.1`, the port, the session token, and a start timestamp. It is
written atomically (temp file + rename) and, on POSIX, created `0600` before
the token is ever present at the final path.

---

## 4. Selecting among multiple running instances

Every darktable process with remote control on writes its own
`session-<pid>.json`. The sidecar selects one, in this order:

1. `--discovery-path /…/session-<pid>.json` — an explicit record;
2. `--pid <PID>` — the record for that process id;
3. otherwise the **newest live** session under the config directory.

A record is “live” only if its JSON is well-formed **and** its PID is a
running process; stale leftovers from a crashed instance are ignored. The
sidecar never scans arbitrary ports, and the token from the record is
verified by the handshake — a same-PID coincidence after a fast restart
surfaces as an authentication failure, not a wrong connection.

---

## 5. Security model and limitations

The threat model is *another process running as the same desktop user*. The
protections:

* **Loopback only.** darktable binds an explicit `127.0.0.1` socket on an
  OS-assigned port; it never binds a wildcard/all-interfaces address. Nothing
  off-host can reach it.
* **Mandatory session token.** A new 256-bit random token per darktable
  process; the first frame on every connection must present it (compared in
  constant time). It is **never logged** and is kept out of the record's
  string representation.
* **User-only record file.** The discovery record is created `0600` where the
  platform supports it, so only your account can read the token from disk.
  The token stays mandatory even if permissions are somehow permissive.
* **Narrow, allow-listed surface.** Only the darkroom-editing methods exist.
  There is **no raw Lua, no SQL, no shell, no arbitrary file read, and no
  caller-chosen export path**. Preview/scope output is bounded in size and
  encoded in memory.
* **Bounded resources.** Frames are size-capped (16 MiB) before allocation;
  connection and in-flight-request counts are limited; repeated auth failures
  close the connection.

Limitations to be aware of:

* It is **not** a defense against a process that can already read all of your
  memory and files — such a process can read the token anyway. The token
  stops accidental/opportunistic access through a guessed port, nothing more.
* Only the **darkroom** is exposed: no library management, culling, tagging,
  collections, export workflows, masks, or blending in v1.
* Editing is not autonomous — your eye stays the feedback loop; the model
  proposes edits and can request a preview/scopes, but you are driving.

### Disabling it

Set `security/enable_remote_control` back to **false** (GUI or `darktablerc`)
and restart darktable — the server does not start, no port is opened, and no
record is written. Because it is off by default, doing nothing also leaves it
off. Removing/stopping the sidecar from your MCP host removes the client end.

---

## 6. Versioning and supported versions

* **Private protocol version: `1`.** The handshake negotiates one integer
  protocol version and the server advertises a `capabilities` list. New
  *optional* response fields and new capabilities may appear without a
  version bump; removing or changing the meaning/requiredness/type of a field
  requires a new protocol version. The sidecar and the darktable server are
  **versioned together** and ship in the same source tree — run a sidecar
  from the same darktable checkout as the running darktable.
* Once a protocol version 2 exists, the sidecar is intended to support the
  current and one previous protocol version.
* **Semantic curve editing is capability-gated, not version-gated.** A
  darktable with curve support advertises `semantic_params` and
  `curve_params` in the hello `capabilities` list (protocol version stays
  `1`); curve-capable schema/value members and the `semantic_values`
  request member only flow when the capability is present. The sidecar
  never sends a curve patch to a darktable that does not advertise
  `curve_params`, so an older server can never silently drop the curve
  half of a mixed patch -- the tool call fails client-side with an
  upgrade message instead. Milestone 3 extended the same `curve_params`
  capability to `tonecurve`, `colorzones`, and `basecurve` (joining
  `rgbcurve` from milestone 2) with no version bump.
* **Semantic vector editing (milestone 4) is likewise capability-gated.** A
  darktable with vector support advertises `vector_params` in the hello
  `capabilities` list (protocol version stays `1`); the `semantic_values`
  request member's vector entries only flow when the capability is
  present, exactly like curves — an older server never receives a partial
  patch, the sidecar refuses client-side with an upgrade message instead.
  Five modules expose vector semantics: `colorbalance` (`lift`/`gamma`/
  `gain`, with mode-gated `offset`/`power`/`slope` aliases writable
  instead under the default `SLOPE_OFFSET_POWER` mode — **stored identity
  is `1.0`, which the GUI displays as `0.0`/`0%`**, so do not send a
  GUI-style `0.0` expecting to reset a component), `channelmixerrgb`
  (mixing rows), `rgblevels` (`levels.linked` when `autoscale` is linked,
  `levels.red`/`levels.green`/`levels.blue` when it is independent —
  writing one row never resets the others), `borders` (`color`/
  `frame_color`), and `watermark` (`color`). See the sidecar
  [`README`](../README.md#tools-exposed) for the full `vectors` argument
  shape and worked colorbalance/rgblevels examples.
* **Semantic band editing (milestone 5) is likewise capability-gated.** A
  darktable with band support advertises `band_params` in the hello
  `capabilities` list (protocol version stays `1`); the `semantic_values`
  request member's bands entries only flow when the capability is present,
  with the same client-side refusal against older servers. Four modules
  expose band semantics: `atrous` (five six-sample channels with movable
  interior x positions — `bands.luma` and `bands.chroma` share their x
  with their `*_threshold` twins, so an x write moves both),
  `denoiseprofile` (six seven-sample wavelet channels, fixed x),
  `rawdenoise` (four five-sample channels, fixed x), and `lowlight`
  (`bands.transition`, six samples, movable interior x). Each write
  replaces the whole named band set; y values stay within the schema's
  `y_range` and nothing is clamped. See the sidecar
  [`README`](../README.md#tools-exposed) for the full `bands` argument
  shape and worked atrous/denoiseprofile examples.
* **darktable:** this feature targets the darktable release it ships in
  (5.x and later) on Linux, macOS, and Windows. The server reports its
  `darktable_version` in the handshake; schema responses are cacheable per
  *(darktable version, module op)* because parameter metadata is build-stable.
* No ABI is promised for the in-process C interface; stability is only at the
  private JSON protocol and the MCP tool boundaries.

---

## 7. Verifying it works

1. Launch darktable with remote control on and an image open in the darkroom.
2. Confirm the record exists, e.g. on Linux:
   `ls -l ~/.config/darktable/mcp/` → a `session-<pid>.json`, mode `-rw-------`.
3. Run the sidecar against it and drive it from any MCP stdio client (the
   [MCP inspector](https://github.com/modelcontextprotocol/inspector) is the
   easiest), or point your host's config at it as in §2.

For an automated end-to-end check, the repository ships a live integration
harness under `tools/mcp/tests/integration/` (run with `pytest -m integration`;
it launches darktable under a display server, exercises the full flow, and
verifies clean shutdown). See that directory and the project CI for details.
