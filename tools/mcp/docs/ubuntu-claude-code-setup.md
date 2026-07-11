# Build and use darktable MCP with Claude Code on Ubuntu

This guide builds the `worktree-mcp-remote-edit` branch, runs its darktable
remote-control server with an isolated profile, installs the Python MCP
sidecar, and registers that sidecar with Claude Code.

The darktable binary and the sidecar must come from the same checkout. The
JSON protocol between them is private and versioned with this source tree.

## 1. Understand the process layout

Three processes participate:

```text
Claude Code
  |  MCP over stdio
  v
darktable-mcp Python sidecar
  |  authenticated framed JSON over 127.0.0.1
  v
darktable GUI from this branch
```

Claude Code starts the sidecar. You start darktable separately and keep it
running with an image open in the darkroom. Darktable writes a short-lived
discovery record containing its loopback port and session token; the sidecar
uses that record to connect.

Remote control is disabled by default. It does not listen on a network-facing
address and does not provide shell, SQL, Lua, arbitrary file access, library
management, or export-path control.

## 2. Install Ubuntu prerequisites

Install the basic build and Python tooling:

```sh
sudo apt update
sudo apt install \
  build-essential cmake ninja-build git pkg-config \
  python3 python3-venv python3-pip \
  libjson-glib-dev
```

Install darktable's normal build dependencies with Ubuntu's source-package
metadata:

```sh
sudo apt-get build-dep darktable
```

Recent Ubuntu releases require source repositories to be enabled before
`apt build-dep` works. Open **Software & Updates**, enable **Source code**, run
`sudo apt update`, and retry the command. The root `README.md` contains the
upstream dependency guidance if the packaged darktable version is too old to
describe a newly added dependency.

For the optional live integration suite, also install:

```sh
sudo apt install xvfb xauth dbus-x11 libglib2.0-bin
```

## 3. Obtain the feature branch

For a fresh checkout:

```sh
REPO="$HOME/src/darktable-mcp"
git clone --branch worktree-mcp-remote-edit --recurse-submodules \
  https://github.com/xyloid/darktable.git "$REPO"
cd "$REPO"
```

For an existing checkout of the fork:

```sh
cd /absolute/path/to/darktable
git fetch origin
git switch worktree-mcp-remote-edit
git pull --ff-only
git submodule update --init
REPO="$PWD"
```

Use an absolute path for `REPO`. Later Claude Code configuration stores the
sidecar executable path, so moving the checkout invalidates that entry.
The commands below assume the variables remain in the same shell; define
`REPO`, `DARKTABLE_BIN`, `MCP_BIN`, `DT_CONFIG`, and `DT_CACHE` again after
opening a new terminal.

## 4. Build the darktable GUI

Build into a feature-specific directory without overwriting a system
darktable installation:

```sh
cd "$REPO"
./build.sh \
  --build-dir "$REPO/build-mcp" \
  --build-type Release \
  --build-generator Ninja \
  --disable-ai \
  --disable-gmic
```

The MCP feature does not require AI or GMIC. Disabling them reduces optional
dependency requirements; omit those flags if you need those features and have
their dependencies installed.

Confirm that the expected binary was produced:

```sh
DARKTABLE_BIN="$REPO/build-mcp/bin/darktable"
test -x "$DARKTABLE_BIN"
"$DARKTABLE_BIN" --version
```

Do not substitute an Ubuntu-packaged darktable binary. Released packages do
not contain the unmerged remote-control implementation from this branch.

## 5. Install the MCP sidecar

Create a virtual environment inside the sidecar directory and install it in
editable mode:

```sh
cd "$REPO/tools/mcp"
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/pip install -e '.[dev]'
```

Record and verify the absolute launcher path:

```sh
MCP_BIN="$REPO/tools/mcp/.venv/bin/darktable-mcp"
test -x "$MCP_BIN"
"$MCP_BIN" --help
```

Do not run `darktable-mcp` by itself and expect an interactive prompt. It is a
stdio MCP server; Claude Code launches it and communicates through its
standard input and output.

## 6. Create an isolated darktable profile

Use a separate config, cache, and library database while testing this branch.
This avoids opening a stable darktable database with a development build and
keeps remote-control discovery records separate from your normal profile.

```sh
DT_CONFIG="$HOME/.config/darktable-mcp-dev"
DT_CACHE="$HOME/.cache/darktable-mcp-dev"
mkdir -p "$DT_CONFIG" "$DT_CACHE"
```

Start darktable from a terminal:

```sh
"$DARKTABLE_BIN" \
  --configdir "$DT_CONFIG" \
  --cachedir "$DT_CACHE" \
  --library "$DT_CONFIG/library.db" \
  --conf security/enable_remote_control=TRUE \
  --conf write_sidecar_files=never
```

To open a photograph immediately, append its absolute path:

```sh
PHOTO="/absolute/path/to/photo.raw"
"$DARKTABLE_BIN" \
  --configdir "$DT_CONFIG" \
  --cachedir "$DT_CACHE" \
  --library "$DT_CONFIG/library.db" \
  --conf security/enable_remote_control=TRUE \
  --conf write_sidecar_files=never \
  "$PHOTO"
```

If darktable shows the lighttable, open the photograph in the darkroom before
using darkroom-scoped tools. Leave this darktable process running.

The command-line setting applies only to this launch. To persist it in this
isolated profile, enable **preferences > security > allow remote control
(MCP)** and restart darktable.

## 7. Verify discovery before configuring Claude Code

After darktable finishes starting, it should create one record:

```sh
find "$DT_CONFIG/mcp" -maxdepth 1 -name 'session-*.json' -ls
stat -c '%a %n' "$DT_CONFIG"/mcp/session-*.json
```

On Ubuntu the mode should be `600`. Do not paste or commit the record: it
contains the live session token. The record is deleted during an orderly
darktable shutdown and stale records are ignored when their PID is no longer
running.

If there is no record, confirm all of the following:

1. You launched `build-mcp/bin/darktable`, not `/usr/bin/darktable`.
2. `security/enable_remote_control=TRUE` was passed exactly as shown.
3. The GUI completed startup and is still running.
4. You are inspecting the same directory passed through `--configdir`.

## 8. Register the sidecar with Claude Code

Run the registration command from the project in which you use Claude Code.
Local scope keeps this machine-specific absolute path out of the repository:

```sh
claude mcp add --scope local darktable -- \
  "$MCP_BIN" \
  --config-dir "$DT_CONFIG" \
  --request-timeout 30
```

The longer request timeout gives RAW preview rendering room to finish. Verify
the saved entry and connection status:

```sh
claude mcp get darktable
claude mcp list
```

Use `--scope user` instead of `--scope local` if the same sidecar should be
available from every Claude Code project. Use `--scope project` only if you
intend to share and review a `.mcp.json`; it will contain checkout-specific
absolute paths unless you deliberately make the command portable.

If the sidecar was registered previously, remove and recreate the entry:

```sh
claude mcp remove darktable
```

Then rerun `claude mcp add`.

## 9. Verify the tools from Claude Code

Start Claude Code in the project where the local-scoped server was registered:

```sh
claude
```

Run `/mcp` inside Claude Code. Confirm that `darktable` is connected and allow
the tool when prompted. Useful first requests are:

```text
Use darktable MCP to report the currently open image and revision.
```

```text
List the active darkroom modules, then show the current exposure parameters.
Do not change anything.
```

```text
Render a 1024 pixel preview of the current darkroom image and describe what
you see.
```

```text
Get histogram and waveform scopes with summaries for the current image.
```

Claude Code receives previews and rendered scopes as native MCP image content,
not base64 text. The preview is a developed sRGB JPEG, not the original RAW
payload. Keep `max_px` at 1024 or lower in image-heavy conversations to reduce
context usage.

## 10. Current MCP operations

The sidecar currently exposes these Claude Code tools:

| Category | Tools |
|---|---|
| state and introspection | `get_current_image`, `list_modules`, `get_module_schema`, `get_module_params` |
| module lifecycle | `set_module_enabled`, `reset_module`, `create_module_instance` |
| history | `get_history`, `undo` |
| visual feedback | `render_preview`, `get_scopes` |

Mutating calls create normal darktable history and advance the process-local
revision. Prefer compare-and-swap with `expected_revision` so a stale request
cannot overwrite a newer GUI or MCP edit. `undo` always requires the revision
that the caller most recently observed.

Important current limitation: the private darktable protocol implements
`set_module_params`, but the Python sidecar does not yet register it as an MCP
tool. Claude Code can inspect numeric module parameters and can enable, reset,
duplicate, or undo modules, but it cannot currently set an arbitrary numeric
parameter such as exposure through the MCP surface.

## 11. Run the automated tests

Run the Python unit suite:

```sh
cd "$REPO/tools/mcp"
.venv/bin/pytest -q
```

Run the live suite against the binary built above:

```sh
cd "$REPO/tools/mcp"
DARKTABLE_BIN="$REPO/build-mcp/bin/darktable" \
  .venv/bin/pytest -m integration -v
```

The live suite launches its own isolated darktable process, Xvfb display,
private D-Bus session, config, cache, and database. It uses the repository RAW
fixture at `img/DSC07350.ARW`; it does not use your normal profile or photos.

## 12. Everyday startup sequence

After the one-time build and registration, a normal session is:

1. Start the branch's darktable binary with the same `DT_CONFIG` and remote
   control enabled.
2. Open a photograph in the darkroom.
3. Start Claude Code from a project where the MCP registration is in scope.
4. Check `/mcp` if darktable tools are missing.
5. Ask Claude to inspect state before making any mutating call.
6. Close darktable normally when finished so its discovery record is removed.

The sidecar discovers darktable lazily on the first tool call. It is therefore
fine to start Claude Code before darktable, but the first darktable tool call
will fail until a live discovery record exists.

## 13. Troubleshooting

### `no live darktable instance found`

Check the discovery record and ensure the `--config-dir` passed to the sidecar
exactly matches darktable's `--configdir`. Restart darktable if the record was
created by an older binary or session.

### `not_in_darkroom` or `no_image_open`

The connection works, but the GUI is not displaying an open image in the
darkroom. Open a photograph and retry.

### Claude Code does not show the tools

Run `claude mcp get darktable`, then inspect `/mcp` inside Claude Code. Recreate
the registration if the checkout or virtual environment moved. Restart Claude
Code after replacing the sidecar environment.

### Preview or scopes time out

Keep `--request-timeout 30` in the MCP registration, wait for the darkroom
preview to finish, and retry. Request a smaller preview, such as
`render_preview(max_px=512)`, if image processing or MCP output is too large.

### Authentication failure

The token changes every time darktable starts. Do not pin or copy tokens into
Claude configuration. Let the sidecar re-read the current discovery record;
restart the MCP connection from `/mcp` if it retained a connection to the old
process.

### darktable database is locked

Do not run two darktable processes against the same config directory or
library database. Keep the development profile separate from the stable
profile.

## 14. Disable or remove the setup

Stop exposing remote control by closing darktable, or set
`security/enable_remote_control=FALSE` and restart it. Remove Claude Code's
sidecar registration with:

```sh
claude mcp remove darktable
```

The build tree, virtual environment, isolated config, and isolated cache can
then be removed independently when they are no longer needed.
