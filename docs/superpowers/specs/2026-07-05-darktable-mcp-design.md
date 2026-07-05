# darktable Conversational Companion MCP — Design

Date: 2026-07-05
Status: draft for review

## Goal

Build a cross-platform MCP server that lets an LLM operate the darkroom in a
running darktable GUI. A user can ask for an edit such as “brighten the
shadows and warm the image”; the model inspects the available module schema,
applies a small parameter patch, and the open darkroom updates normally.

The user's eye remains the primary feedback loop. The model can request a
rendered preview and photographic scopes when visual or quantitative analysis
is useful.

The first release targets Linux, macOS, and Windows. Its scope is darkroom
editing only: module parameters, module state, history, undo, preview, and
read-only scope analysis.

## Non-goals for v1

- Library management, culling, tagging, collections, or export workflows
- Drawn masks, parametric masks, or blending parameters
- Arbitrary Lua execution, SQL execution, or filesystem access
- Headless/server operation
- Fully autonomous editing without user direction
- Support for opaque or complex module parameter fields

## Design principles

1. **darktable owns editing state.** Only the running darktable process reads
   or mutates live modules, history, the pixelpipe, and GUI state.
2. **Transport is replaceable.** Editing operations do not know about TCP,
   JSON, or MCP.
3. **Stable identifiers, not labels.** Modules use their internal `op` name;
   parameters use generated introspection field names. Translated GUI labels
   are descriptive metadata only.
4. **Every mutation is validated and undoable.** A parameter patch is applied
   atomically and creates one normal darktable history/undo record.
5. **The remote surface is narrow.** There is no general-purpose remote code
   execution facility.
6. **Concurrent user edits are expected.** Reads are live, mutations return
   resulting state, and optional revisions allow stale writes to be rejected.

## Architecture

```text
MCP host (Claude Desktop, Codex, or another client)
             │
             │ MCP JSON-RPC over stdio
             ▼
darktable-mcp sidecar (Python)
             │
             │ authenticated framed JSON over loopback TCP
             ▼
remote server in the running darktable process
             │
             ▼
transport-neutral remote-edit service
             │
             ├── develop/module state
             ├── generated parameter introspection
             ├── history and undo
             ├── pixelpipe preview rendering
             └── histogram, waveform, parade, and vectorscope analysis
```

Proposed source layout:

```text
src/control/remote_edit.h       operation and result types
src/control/remote_edit.c       darkroom reads and mutations
src/control/remote_server.h     server lifecycle
src/control/remote_server.c     TCP, authentication, framing, JSON dispatch
tools/mcp/pyproject.toml
tools/mcp/src/darktable_mcp/    MCP tools, discovery, transport, error mapping
tools/mcp/tests/
```

`remote_edit` must not include socket or MCP concerns. `remote_server` is a
thin adapter around it. The Python sidecar translates between MCP content and
the private darktable protocol; darktable does not implement MCP itself.

## Why loopback TCP

Loopback TCP has consistent behavior on Linux, macOS, and Windows. GLib's
`GSocketService` and asynchronous stream APIs provide a common implementation
without introducing a new networking dependency. The Python sidecar also
needs no platform-specific IPC implementation.

Unix-domain sockets would require a separate Windows transport. Named pipes
would require a separate Unix transport. D-Bus is already present on Linux,
but is not a native darktable control plane on macOS or Windows. Embedding an
MCP server in darktable would put MCP protocol lifecycle and dependencies in
the GUI process without improving the editing API.

TCP is only a transport choice. The remote-edit interface remains usable from
a future platform-native transport if needed.

## Server lifecycle and discovery

Remote control is opt-in and disabled by default. It can be enabled with a
preference and, for testing, a command-line option. When enabled, darktable:

1. generates a cryptographically random 256-bit session token;
2. binds IPv4 loopback (`127.0.0.1`) on an OS-assigned port;
3. writes an atomic discovery record named `session-<pid>.json` beneath an
   `mcp` directory in the active darktable config directory;
4. removes the record during orderly shutdown.

Example discovery record:

```json
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
```

The file is created with user-only permissions where the platform supports
them. The token remains mandatory even when file permissions are restrictive.
The sidecar considers a record stale if the process is gone or the authenticated
handshake fails.

Multiple darktable instances produce multiple records. Selection order is:

1. an explicit discovery path from sidecar configuration;
2. an explicit PID;
3. the newest live session.

The sidecar never scans arbitrary ports.

## Security model

- Listen only on IPv4 loopback. Do not bind wildcard interfaces.
- Authenticate before dispatching any operation. Use constant-time token
  comparison and close the connection after repeated authentication failure.
- Generate a new token for every darktable process and never log it.
- Cap the frame size before allocation. The v1 maximum is 16 MiB, allowing a
  compressed preview while bounding memory use.
- Validate JSON types, required fields, string lengths, batch sizes, and
  numeric finiteness.
- Apply an allowlist of remote methods. Do not expose raw Lua, SQL, shell,
  arbitrary file reads, or arbitrary export destinations.
- Limit preview and scope dimensions and encode image results in memory; do
  not accept an output path from the caller.
- Keep remote control disabled unless the user explicitly enables it.

The threat model is another process running as the same desktop user. The
session token prevents accidental or opportunistic access through a guessed
port; it is not intended to defend against a process that can read all of the
user's memory and files.

## Wire protocol

Each message is a four-byte unsigned big-endian payload length followed by one
UTF-8 JSON object. Length-prefixing permits arbitrary JSON strings and avoids
newline framing ambiguity.

The client opens a persistent connection and sends a handshake first:

```json
{
  "id": 1,
  "method": "hello",
  "params": {
    "protocol_version": 1,
    "token": "base64url-encoded-random-token",
    "client": "darktable-mcp/0.1.0"
  }
}
```

Successful response:

```json
{
  "id": 1,
  "ok": true,
  "result": {
    "protocol_version": 1,
    "darktable_version": "5.x",
    "capabilities": ["params", "history", "preview"]
  }
}
```

Subsequent requests omit the token. Request IDs are unsigned integers unique
among requests in flight on that connection.

```json
{
  "id": 12,
  "method": "set_module_params",
  "params": {
    "module": "exposure",
    "instance": 0,
    "values": { "exposure": 0.7, "black": -0.002 },
    "expected_revision": 31
  }
}
```

```json
{
  "id": 12,
  "ok": true,
  "result": {
    "module": "exposure",
    "instance": 0,
    "enabled": true,
    "values": { "exposure": 0.7, "black": -0.002 },
    "revision": 32
  }
}
```

Errors have stable machine-readable codes:

```json
{
  "id": 12,
  "ok": false,
  "error": {
    "code": "revision_conflict",
    "message": "the darkroom changed after revision 31",
    "details": { "current_revision": 33 },
    "retryable": true
  }
}
```

Initial error codes are `not_in_darkroom`, `no_image_open`, `unknown_module`,
`unknown_instance`, `unknown_field`, `unsupported_field`, `invalid_value`,
`instance_not_supported`, `revision_conflict`, `preview_failed`,
`scope_failed`, `request_too_large`, `unauthorized`, `busy`, and `internal`.

Protocol v1 permits multiple requests in flight, but responses may arrive out
of order. Mutation execution is serialized on darktable's GUI/main context.
The sidecar initially issues one mutation at a time.

## Remote-edit operations

The private protocol exposes these methods:

| Method | Purpose |
|---|---|
| `get_state` | Current view, image identity and metadata, revision, capabilities |
| `list_modules` | Module `op`, display name, instances, enabled state |
| `get_module_schema` | Supported fields, types, ranges, defaults, enum choices |
| `get_module_params` | Current supported parameter values for one instance |
| `set_module_params` | Atomically validate and apply a field-value patch |
| `set_module_enabled` | Enable or disable one module instance |
| `reset_module` | Restore module parameters to defaults |
| `create_module_instance` | Create a new instance of a multi-instance module |
| `get_history` | Recent visible history entries and current revision |
| `undo` | Undo one or more remote/user edits through darktable's undo system |
| `render_preview` | Return a bounded JPEG preview with current history applied |
| `compute_scopes` | Return numeric summaries and/or rendered photographic scopes |

There is deliberately no generic `call`, `eval`, or `run_lua` method.

## Module and parameter addressing

A module is addressed by its stable internal operation name and instance
priority:

```json
{ "module": "colorbalancergb", "instance": 0 }
```

Responses also include translated display labels and instance names for model
and user presentation, but labels are never identifiers.

Parameter fields come from each module's generated introspection callbacks:
`get_introspection_linear()`, `get_f()`, and `get_p()`. Schema entries include:

```json
{
  "name": "exposure",
  "description": "exposure correction",
  "type": "float",
  "minimum": -18.0,
  "maximum": 18.0,
  "default": 0.0,
  "writable": true
}
```

v1 supports finite scalar `float`, `double`, signed/unsigned integer, Boolean,
and enum fields. Enum values use stable introspection names plus their integer
representation. Arrays, unions, strings, coordinates, curves, blobs, opaque
fields, and complex numbers are returned as `writable: false` or omitted.

Introspection ranges describe stored parameter values, not necessarily the
GUI's presentation units. A later metadata layer may add presentation units
and semantic aliases. The v1 MCP server should prefer modules and fields for
which stored values have clear meanings.

## Mutation semantics

`set_module_params` implements a transaction over one module instance:

1. Verify that darkroom is active and capture the current revision.
2. Reject the call if `expected_revision` is present and stale.
3. Resolve the module and instance without creating a new instance.
4. Copy `module->params` into temporary storage.
5. Resolve every requested field through introspection.
6. Reject the whole patch if any field is unsupported or any value is invalid.
7. Write validated values into the temporary parameter block.
8. Re-check the completed block against introspection ranges. This check
   belongs to the remote-edit service: darktable's own `_iop_validate_params`
   is compiled for debug diagnostics only and reports without rejecting, so
   it cannot serve as the enforcement gate.
9. Copy the block to `module->params`; enable the module when requested by the
   operation's explicit `enable` option, not as an implicit side effect.
10. Synchronize the GUI, add one normal history item, invalidate the pixelpipe,
    and request redraw using established darktable helpers.
11. Return the values read back from live state and the new revision.

No partially applied patch is observable. A patch produces one history entry
and one undo record even when it changes several fields.

Module enable/reset operations follow the same history and redraw lifecycle.
Direct database writes are forbidden while operating on the active image.

`set_module_params` never creates an instance implicitly. Instance creation is
its own explicit operation:

```json
{
  "id": 14,
  "method": "create_module_instance",
  "params": {
    "module": "exposure",
    "source_instance": 0,
    "copy_params": false
  }
}
```

`create_module_instance` follows the darkroom's native new-instance lifecycle
(the same code path as the module's "new instance" and "duplicate instance"
menu entries), which creates the module, places its GUI expander, rebuilds the
pixelpipe, and records history. The native helper always duplicates from an
existing base instance, so `source_instance` is optional and defaults to 0.
`copy_params: true` duplicates the source instance's parameters; `false`
starts from defaults. The operation fails with
`instance_not_supported` for modules that permit only one instance, and with
`unknown_instance` if the source instance does not exist.

The native lifecycle records a history entry for the source instance followed
by one for the new instance — identical to pressing the GUI button. The
response returns the new instance's identity (`instance`, generated instance
name) and the resulting revision; the caller addresses the instance by that
returned priority in subsequent calls.

The revision is a monotonically increasing, process-local counter incremented
for every observed darkroom history change, including user edits. It is not a
database history number and does not persist across sessions.

## Preview rendering

`render_preview` accepts a longest-edge limit, clamped to a server maximum
(2048 pixels in v1), and JPEG quality within a bounded range. It renders the
active image through the normal pixelpipe with current history applied.

The result contains MIME type, pixel dimensions, revision, and base64-encoded
JPEG bytes:

```json
{
  "mime_type": "image/jpeg",
  "width": 1536,
  "height": 1024,
  "revision": 32,
  "data": "...base64..."
}
```

Rendering is asynchronous so socket handling and the GUI remain responsive.
Only completion and serialization return to the main context. The response's
revision tells the model which edit state was rendered. The implementation
must use an in-memory destination and must not expose a caller-selected path.

## Scope analysis

`compute_scopes` gives the model the same classes of diagnostic information
available in darktable's scopes panel:

- RGB histogram
- waveform
- RGB parade
- vectorscope

Split scope is a presentation mode composed from waveform and vectorscope and
does not require a separate computation result.

A request selects one or more scopes and whether it needs compact statistics,
full histogram bins, rendered scope images, or a combination:

```json
{
  "scopes": ["histogram", "waveform", "vectorscope"],
  "include_summary": true,
  "include_bins": false,
  "include_images": true,
  "image_size": 512
}
```

The default response favors information density. Histogram analysis returns
numeric values useful for reasoning without requiring the model to interpret a
plot:

```json
{
  "revision": 32,
  "source": "final_preview",
  "color_profile": "sRGB",
  "roi": "full_image",
  "histogram": {
    "bins": 256,
    "black_clip_fraction": 0.0012,
    "white_clip_fraction": 0.0041,
    "luminance_percentiles": {
      "p01": 0.02,
      "p50": 0.41,
      "p99": 0.98
    },
    "channel_means": {
      "red": 0.46,
      "green": 0.42,
      "blue": 0.37
    }
  }
}
```

When requested, full histogram data contains 256 normalized bins for each RGB
channel. Raw pixel counts are not included by default because they are verbose
and depend on preview resolution. Every numeric response states its binning,
normalization, clipping thresholds, color profile, and region of interest.

Waveform, RGB parade, and vectorscope are inherently two-dimensional. They are
returned as bounded PNG images for a vision-capable model, accompanied by
compact summaries where useful. The private protocol carries base64-encoded
PNG bytes; the sidecar converts them to native MCP image content rather than
presenting base64 text to the model.

Scope computation must not scrape screenshots or directly expose the GTK
scope widget's private buffers. The current GUI computes only its selected
scope mode and may cache data from an earlier pixelpipe update. Instead, the
scope algorithms should be refactored behind a reusable computation API that
accepts one captured final-preview buffer and computes every requested result.
The GUI and remote-edit service should share that implementation.

All scopes in one response must derive from the same preview buffer and carry
the revision represented by that buffer. If darkroom state changes during
computation, the result remains valid for its reported revision; callers may
request another result rather than combining it with newer state. Computation
is asynchronous and uses bounded resolution, buffers, and output dimensions.

The scope color profile is explicit. By default, remote scopes use the same
histogram profile and final-preview stage as the GUI. A future protocol may
allow another supported profile, but v1 does not accept arbitrary ICC paths.

## MCP tool surface

The Python sidecar exposes a deliberately small tool vocabulary:

| MCP tool | Private operation |
|---|---|
| `get_current_image` | `get_state` |
| `list_modules` | `list_modules` |
| `get_module_schema` | `get_module_schema` |
| `get_module_params` | `get_module_params` |
| `set_module_params` | `set_module_params` |
| `set_module_enabled` | `set_module_enabled` |
| `reset_module` | `reset_module` |
| `create_module_instance` | `create_module_instance` |
| `get_history` | `get_history` |
| `undo` | `undo` |
| `look_at_image` | `render_preview`, returned as MCP image content |
| `get_scopes` | `compute_scopes`, returned as summaries and MCP image content |

Every mutating MCP tool returns resulting state and revision. The sidecar maps
private errors to concise MCP errors with recovery guidance. It reconnects and
re-runs discovery when darktable restarts, but never automatically retries a
mutation whose completion is uncertain.

Schema responses may be cached by `(darktable version, module op)` because
field metadata is build-stable. Module instances, values, enabled state,
history, and revisions are always read live.

## Why Lua is not the primary control layer

The existing D-Bus `Lua` method and `darktable.gui.action()` are useful for a
prototype with a known action path. They are not a sufficient foundation for
the production server:

- Lua does not expose a complete enumeration of live darkroom modules and
  their parameter schema.
- GUI action paths and effects are presentation-oriented and may contain
  translated labels.
- Not every stored parameter is represented by a widget action.
- Widget ranges, quantization, and presentation units can differ from the
  stored parameter representation.
- A raw remote Lua method is much broader than the intended editing surface.

The generated C introspection data is already the authoritative, build-synced
description of module parameter structures, so the remote-edit layer should
consume it directly.

## Threading and responsiveness

- Accept, authenticate, read, and write sockets asynchronously through GLib.
- Parse and size-check frames before dispatch.
- Execute all access to active develop/module/GUI state on the GUI/main
  context.
- Never add a history item while a GUI-update guard is active: darktable's
  history entry point returns silently without recording when the guard
  counter is raised. Synchronize widgets with the standard module GUI update
  helper (which manages the guard itself) strictly before or after the
  history call, never around it.
- Serialize mutations; read operations may be queued behind an active
  mutation to give each response a coherent revision.
- Run expensive preview and scope processing through darktable's existing
  job/pixelpipe mechanisms and complete the pending response asynchronously.
- Attach a deadline to each request in the sidecar. A timeout does not imply
  cancellation; the sidecar reports uncertain mutation completion and reads
  state before accepting another mutation.
- Apply connection and pending-request limits to prevent unbounded resource
  use.

## Compatibility and versioning

The handshake negotiates a single integer protocol version. New optional
response fields and capabilities may be added without changing the version.
Removing or changing field meaning requires a new protocol version.

The server reports capabilities so builds lacking preview, scope computation,
or a particular field type can degrade explicitly. The MCP sidecar should
support the current protocol and one previous protocol once version 2 exists.

No ABI is promised for `remote_edit.h` outside the darktable build. Stability
is provided at the private JSON protocol and MCP tool boundaries.

## Testing strategy

### Unit tests

- Frame parsing across partial reads and writes
- Oversized, malformed, and non-UTF-8 payload rejection
- Authentication and token comparison
- JSON request validation and stable error mapping
- Introspection schema conversion for every supported scalar type
- Enum lookup, numeric range validation, NaN/infinity rejection
- Atomic patch behavior when one field is invalid
- Histogram normalization, clipping fractions, and percentile calculations
- Scope output size limits and image serialization
- Discovery selection and stale-record handling on all platforms
- Python protocol client and MCP tool serialization

### darktable integration tests

Launch darktable with a temporary config directory, in-memory or temporary
library, a known test image, and remote control explicitly enabled. On Linux,
run GUI tests under Xvfb; use the existing platform GUI test environment on
macOS and Windows.

For each supported platform, verify:

1. discovery and authenticated handshake;
2. current image and module enumeration;
3. schema/current-value agreement;
4. a multi-field patch creates exactly one history entry;
5. returned values match live module parameters;
6. the pixelpipe updates and preview differs after a visible edit;
7. histogram, waveform, and vectorscope share the preview's revision;
8. a known exposure/color edit changes the expected scope measurements;
9. undo restores parameters, preview, and scope measurements;
10. a concurrent user/history change causes revision conflict;
11. malformed and unauthenticated clients cannot mutate state;
12. shutdown removes discovery state and disconnects clients.

### Manual release check

Run a golden conversation:

```text
get image → inspect modules/schema → inspect scopes → set exposure
→ inspect preview/scopes → undo
```

Confirm that the visible GUI, history panel, returned values, and preview all
agree at every step.

## Delivery phases

### Phase 1 — read-only vertical slice

- Internal remote-edit interface
- Authenticated TCP server, discovery, and handshake
- `get_state`, `list_modules`, `get_module_schema`, `get_module_params`
- Python protocol client and corresponding MCP tools

### Phase 2 — safe editing

- Atomic scalar/enum parameter patches
- Enable, reset, revisions, history, and undo
- Explicit instance creation for multi-instance modules
- Integration tests for history and concurrent edits

### Phase 3 — visual feedback and packaging

- Asynchronous in-memory preview
- MCP image content
- Numeric histogram summaries and optional bins
- Waveform, RGB parade, and vectorscope MCP image content
- Linux, macOS, and Windows packaging/configuration documentation
- End-to-end CI coverage

### Later work

- Library and export tools as separately permissioned capabilities
- Structured support for selected curve/vector parameter types
- Blending and masks with purpose-built schemas
- Event notifications for image/history changes
- Headless operation, if a concrete server workflow requires it

## Open implementation questions

- Whether the preference should enable the server persistently or require
  confirmation for each darktable session
- Which existing export/pixelpipe helper provides the cleanest bounded
  in-memory JPEG path on all platforms
- How much of the existing GUI scope implementations should move into a shared
  computation layer versus introducing parallel data-only implementations
- Whether `undo(steps)` should allow undoing user edits or only expose a
  compare-and-undo operation for the most recent remote revision
- Whether schema presentation metadata should be generated from Bauhaus widget
  bindings in v1.x to add GUI units and recommended increments

These questions do not change the core architecture: a transport-neutral C
editing service, an authenticated loopback transport, and an external MCP
sidecar.
