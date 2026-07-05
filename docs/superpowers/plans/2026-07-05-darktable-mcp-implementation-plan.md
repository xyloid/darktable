# darktable MCP — Implementation Plan

Date: 2026-07-05
Status: proposed
Depends on: `docs/superpowers/specs/2026-07-05-darktable-mcp-design.md`

## Objective

Implement the design as small, independently testable vertical slices. The
first usable milestone is a read-only MCP server that discovers a running
darktable instance and reports the active image, modules, schemas, and current
parameter values. Mutation and preview support follow only after the control
boundary and transport have integration coverage.

This plan identifies intended files and responsibilities. Exact helper names
may change during implementation to match nearby darktable conventions.

## Definition of done

The initial project is complete when a packaged Python MCP sidecar can connect
to an explicitly enabled darktable instance on Linux, macOS, and Windows and:

- inspect the active image and live darkroom modules;
- expose stable schemas derived from module introspection;
- apply a validated multi-field patch as one history/undo record;
- enable, disable, and reset a module through normal darktable state changes;
- create a new instance of a multi-instance module through the native
  new-instance lifecycle;
- detect stale writes through a process-local revision;
- retrieve history and undo an edit;
- return a bounded JPEG preview as MCP image content;
- return photographic scopes (numeric summaries and rendered images) that all
  derive from one buffer and carry one revision;
- reject unauthenticated, malformed, oversized, or unsupported requests;
- recover cleanly when darktable exits or restarts.

## Work breakdown

### 1. Establish the internal remote-edit boundary

Create:

```text
src/control/remote_edit.h
src/control/remote_edit.c
src/tests/unittests/control/CMakeLists.txt
src/tests/unittests/control/test_remote_edit.c
```

Modify:

```text
src/CMakeLists.txt
src/tests/unittests/CMakeLists.txt
```

`src/CMakeLists.txt` enumerates control source files explicitly, so both new C
files must be added to the `SOURCE_FILES`/`HEADER_FILES` inputs rather than
assuming the source glob will include them.

Start with data structures and synchronous functions; do not add sockets yet:

```c
typedef enum dt_remote_error_code_t { ... } dt_remote_error_code_t;
typedef struct dt_remote_error_t { ... } dt_remote_error_t;
typedef struct dt_remote_state_t { ... } dt_remote_state_t;
typedef struct dt_remote_module_t { ... } dt_remote_module_t;
typedef struct dt_remote_field_t { ... } dt_remote_field_t;

gboolean dt_remote_get_state(dt_remote_state_t **out,
                             dt_remote_error_t **error);
gboolean dt_remote_list_modules(GPtrArray **out,
                                dt_remote_error_t **error);
gboolean dt_remote_get_module_schema(const char *op,
                                     int instance,
                                     GPtrArray **out,
                                     dt_remote_error_t **error);
gboolean dt_remote_get_module_params(const char *op,
                                     int instance,
                                     GHashTable **out,
                                     dt_remote_error_t **error);
```

Implementation requirements:

- Fail with `not_in_darkroom` unless the current view is darkroom.
- Fail with `no_image_open` unless `darktable.develop` has a valid image ID.
- Walk `darktable.develop->iop` for live module instances.
- Address modules by `module->op` plus `module->multi_priority`.
- Return translated names only as metadata.
- Walk `module->so->get_introspection_linear()` until the `NONE` sentinel.
- Resolve current values with field offsets or `get_p()` without exporting raw
  pointers beyond the function call.
- Mark unsupported fields explicitly instead of serializing their memory.
- Own and free every returned object through paired destroy functions.

First tests:

- operation/instance lookup selects exactly one live module;
- missing darkroom/image/module/instance errors are stable;
- every supported scalar type converts to the expected neutral value type;
- scalar leaves in nested structs appear under their dotted introspection
  names (e.g. `random.damping`) and resolve to the correct offsets;
- unsupported introspection types are read-only or omitted;
- schema field names, ranges, defaults, and enum members are preserved;
- repeated calls do not retain pointers into module parameter storage.

Because constructing a full `dt_develop_t` in a unit test may be expensive,
separate pure introspection conversion from live-module traversal. Test the
converter with fixture fields and cover traversal in integration tests.

Acceptance gate: no networking code is added until the pure schema/value
conversion tests pass.

### 2. Add JSON serialization and protocol dispatch

Create:

```text
src/control/remote_protocol.h
src/control/remote_protocol.c
src/tests/unittests/control/test_remote_protocol.c
```

Use `json-glib`, already a darktable dependency. Keep JSON parsing and
serialization outside `remote_edit.c`.

Expose one dispatcher that accepts a parsed request and produces a response:

```c
JsonNode *dt_remote_protocol_dispatch(JsonObject *request,
                                      dt_remote_session_t *session);
```

The dispatcher owns:

- request ID validation;
- method lookup through a static allowlist;
- parameter shape and length checks;
- conversion between JSON and remote-edit types;
- the stable error envelope;
- protocol capability reporting.

It does not own authentication, framing, sockets, or GUI-thread scheduling.

Tests must cover missing IDs/methods, unknown methods, wrong JSON types,
overlong strings, non-finite numbers, and stable success/error envelopes.
Request/response fixtures are authored from the protocol reference
(`2026-07-05-darktable-mcp-protocol-reference.md`) and shared with the Python
client tests.

Acceptance gate: feed JSON fixtures directly to the dispatcher and validate
responses without creating a socket.

### 3. Implement framing as a standalone streaming parser

Create:

```text
src/control/remote_frame.h
src/control/remote_frame.c
src/tests/unittests/control/test_remote_frame.c
```

The parser accepts arbitrary byte chunks and emits complete payloads. It must
handle:

- a header split across reads;
- a body split across reads;
- multiple frames in one read;
- zero-length and oversized frames;
- connection close during a partial frame;
- a maximum 16 MiB payload before allocation;
- big-endian length conversion independent of host architecture.

The encoder produces a `GBytes` containing the header and payload. Framing has
no JSON or socket dependency, making fuzzing practical later.

Acceptance gate: framing tests include byte-at-a-time input and sanitizer runs.

### 4. Implement authenticated loopback server and discovery

Create:

```text
src/control/remote_server.h
src/control/remote_server.c
src/control/remote_discovery.h
src/control/remote_discovery.c
src/tests/unittests/control/test_remote_discovery.c
```

Modify:

```text
src/CMakeLists.txt
src/common/darktable.h
src/common/darktable.c
data/darktableconfig.xml.in
```

Add `struct dt_remote_server_t *remote_server` to `darktable_t`. Initialize it
only for the GUI application and only after control, signals, configuration,
and the GUI are ready. Stop it before GUI teardown and before the develop
context becomes invalid. Do not reuse the earlier D-Bus initialization point,
which occurs before the rest of the darkroom state is available.

Add an opt-in preference such as:

```text
security/enable_remote_control = false
```

Also add a test-only/core option that can enable the service with a temporary
config directory in integration tests. The implementation should avoid a
second, contradictory preference path.

Server behavior:

- generate 32 random bytes with a new `dt_crypto_random_bytes` wrapper
  (`src/common/crypto_random.[ch]`: `getrandom(2)` with `/dev/urandom`
  fallback, `BCryptGenRandom`, `SecRandomCopyBytes`) — nothing in GLib at
  darktable's 2.56 floor is specified as CSPRNG-backed (see the remote-edit
  internals document §7);
- bind `GSocketService` to `127.0.0.1` and port 0;
- accept asynchronously on the GLib main context;
- require `hello` as the first frame;
- compare the token without early exit;
- attach an authenticated session object to the connection;
- impose connection, pending-request, and authentication-failure limits;
- remove connection state on error or EOF.

Discovery behavior:

- use the active config directory returned by `dt_loc_get_user_config_dir()`;
- create `<config>/mcp` if needed;
- write `session-<pid>.json` to a temporary sibling, flush/close it, then
  atomically rename it;
- request user-only permissions before exposing the token;
- delete the record during normal shutdown;
- tolerate stale records left by a crash.

Do not put the token in debug output. Tests should assert that diagnostic
messages redact it.

Acceptance gate: a small test client can authenticate, call every read-only
method, and is rejected with a bad token. Binding is verified as loopback-only.

### 5. Build the Python protocol client and read-only MCP sidecar

Create:

```text
tools/mcp/pyproject.toml
tools/mcp/README.md
tools/mcp/src/darktable_mcp/__init__.py
tools/mcp/src/darktable_mcp/__main__.py
tools/mcp/src/darktable_mcp/discovery.py
tools/mcp/src/darktable_mcp/protocol.py
tools/mcp/src/darktable_mcp/server.py
tools/mcp/src/darktable_mcp/errors.py
tools/mcp/tests/test_discovery.py
tools/mcp/tests/test_protocol.py
tools/mcp/tests/test_tools.py
```

Responsibilities:

- `discovery.py`: locate records, validate their schema, reject stale sessions,
  and implement explicit-path/PID/newest-live selection;
- `protocol.py`: connect, frame messages, authenticate, correlate IDs, enforce
  timeouts, and reconnect after an unambiguous disconnect;
- `server.py`: expose MCP tools and convert private responses to compact model
  responses;
- `errors.py`: preserve private error codes and add actionable MCP hints.

Initial MCP tools:

```text
get_current_image
list_modules
get_module_schema
get_module_params
```

Use the official Python MCP SDK, pin a tested compatible version range, and
keep the transport client independent of SDK types. Unit tests use a fake
framed server; they do not require darktable.

Acceptance gate: an MCP inspector/client can invoke all four tools against a
real darktable instance on each target platform.

### 6. Add revision tracking

Extend the remote server/session state with a 64-bit process-local revision.
Subscribe to the established develop-history-change signal after the signal
system is initialized. Increment on every active-image history change,
including GUI/user changes.

Requirements:

- revision zero is valid at process start;
- increments are monotonic and thread-safe;
- image switches also change the observable revision/state identity;
- all state/module/history responses include the captured revision;
- reads capture values and revision coherently on the main context;
- the counter is never treated as a persistent database history number.

Tests cover user-originated change simulation, image switches, overflow policy,
and stale expected revisions.

### 7. Implement atomic parameter mutation

Extend `remote_edit.h/.c` with a neutral patch representation and:

```c
gboolean dt_remote_set_module_params(const dt_remote_module_ref_t *module,
                                     const dt_remote_patch_t *patch,
                                     const uint64_t *expected_revision,
                                     dt_remote_mutation_result_t **out,
                                     dt_remote_error_t **error);
```

Implement in this order:

1. Verify view/image and expected revision.
2. Locate the existing module instance.
3. Allocate and copy the entire parameter block.
4. Resolve all fields and convert all values into the temporary block.
5. Reject non-finite, out-of-range, unknown, read-only, duplicate, or
   unsupported values.
6. Validate the completed parameter block.
7. Start the normal undo/history lifecycle.
8. Copy the block to live params.
9. Synchronize the module GUI and add one history item.
10. Return values read back from live params and the resulting revision.

Do not clamp silently. The schema tells callers the range, and an invalid patch
returns `invalid_value` without changing live state.

Keep the transaction generic over a prepared params block rather than coupled
to scalar fields: the patch struct carries a reserved `semantic_values` slot
(see the curve-classes low-level design) so future semantic parameter classes
extend the same copy/validate/commit path instead of forking it.

Do not automatically enable a disabled module merely because parameters were
set. If combined enable-and-patch behavior is desired, add an explicit
`enable` member to the request and still create one history record.

Tests:

- one valid scalar field;
- multiple valid fields create one history item;
- enum by stable name and integer representation;
- any invalid field leaves every field unchanged;
- stale revision leaves state unchanged;
- disabled module behavior is explicit;
- returned values equal live state;
- GUI update, history signal, invalidation, and redraw occur once.

Acceptance gate: setting exposure through MCP visibly updates darkroom and one
undo restores the prior state.

### 8. Add enable, reset, instance creation, history, and undo

Add private methods and MCP tools:

```text
set_module_enabled
reset_module
create_module_instance
get_history
undo
```

Use existing darktable helpers rather than manipulating history list pointers.

`create_module_instance` wraps the darkroom's native duplicate/new-instance
helper (`dt_iop_gui_duplicate` in `src/develop/imageop.c`), which already
handles module construction, GUI expander placement, history recording, and
pixelpipe rebuild. The remote operation must:

- reject modules flagged single-instance with `instance_not_supported`;
- resolve the source instance and honor `copy_params`;
- run entirely on the GUI/main context like every other mutation;
- return the new instance's priority and generated name read back from the
  live module list, plus the resulting revision.

Instance-creation tests:

- creating an instance of a multi-instance module (e.g. exposure) yields a
  new live instance addressable by the returned priority;
- `copy_params: true` duplicates source values, `false` yields defaults;
- a single-instance module returns `instance_not_supported` and creates
  nothing;
- history and undo behavior matches the GUI new-instance button (undo removes
  the created instance);
- the revision advances and subsequent `list_modules` reflects the instance.

The undo contract is fixed by the protocol reference: compare-and-undo with a
required `expected_revision`, one transition per call, `revision_conflict` on
any mismatch. Broader multi-step undo is future work.

History responses should be model-oriented: sequence, module operation,
instance, display name, enabled state, and revision-related metadata. Do not
return binary parameter blobs.

Acceptance gate: enable/reset/undo each produce the same GUI, history, and
pixelpipe behavior as their native darktable equivalents.

### 9. Implement bounded preview rendering

The helper question is resolved in
`docs/superpowers/specs/2026-07-05-darktable-mcp-remote-edit-internals.md`
§8: `dt_imageio_export_with_flags` with a synthetic in-memory format sink
(precedents: `_preview_write_image`, the HDR-merge job), `display_byteorder
= FALSE`, `icc_type = DT_COLORSPACE_SRGB`, then `dt_imageio_jpeg_compress`,
on a `DT_JOB_QUEUE_SYSTEM_BG` job, with a mandatory `dt_dev_write_history`
flush on the main thread before queueing.

Add an asynchronous remote operation returning:

```text
mime_type, width, height, revision, JPEG bytes
```

Requirements:

- longest edge defaults to 1024 and is capped at 2048;
- JPEG quality is bounded;
- current history and normal output color management are applied;
- the preview is associated with the revision actually rendered;
- heavy processing does not block socket handling or the GUI main loop;
- cancellation on disconnect releases result buffers;
- the framed response remains below the configured maximum;
- MCP returns native image content, not base64 text to the model.

Acceptance gate: a known visible parameter edit changes preview pixels, undo
restores them within an image-comparison tolerance, and memory remains bounded.

### 10. Implement scopes computation

The design is resolved in the remote-edit internals document §9: this fork's
scopes plugins (`src/libs/scopes/*.c`) already separate `process` from
`draw_*`, so the kernels factor into a statically-linkable
`src/common/scopes.[ch]` with POD output structs, and the lib's `*_process`
functions become thin adapters over the shared implementation.

Create:

```text
src/common/scopes.h
src/common/scopes.c
src/tests/unittests/common/test_scopes.c
```

Modify:

```text
src/libs/scopes/histogram.c
src/libs/scopes/waveform.c
src/libs/scopes/vectorscope.c
src/libs/histogram.c
```

Remote capture and the `compute_scopes` method:

- register at the same preview-pixelpipe gamma hook the GUI uses and retain
  a copy of the latest pushed buffer plus the revision stamped at push time,
  behind a mutex;
- run requested kernels over the retained buffer on a background job, never
  on the GUI thread;
- all scopes in one response derive from that single buffer and carry its
  single revision by construction;
- rendered scope images are composited without cairo drawing code from the
  lib (new colorizers over the A8 rasters) and PNG-encoded via
  `cairo_surface_write_to_png_stream` into memory;
- an empty capture slot (fresh darkroom, no preview run yet) returns
  `scope_failed` with a retryable hint;
- the histogram color profile conversion is shared with the GUI path so both
  produce identical numbers.

Tests: kernel outputs over fixture buffers (histogram bins, waveform raster
dimensions and orientation, vectorscope hue positions for known primaries),
GUI-adapter equivalence on one fixture, and revision coherence across a
multi-scope response.

Acceptance gate: after a visible exposure edit, remote histogram statistics
shift in the expected direction, and the GUI scopes panel and
`compute_scopes` agree on the same buffer.

### 11. Cross-platform packaging and documentation

Document:

- how users enable remote control;
- how the MCP host launches `darktable-mcp` over stdio;
- where discovery records live for a custom/default config directory;
- how multiple instances are selected;
- security limitations and how to disable the server;
- supported darktable/private protocol versions.

Package the sidecar independently from the darktable process. Platform bundles
may ship it, but darktable must continue to run without Python or MCP packages.

Verify line endings, path handling, atomic rename behavior, user-only file
permissions, loopback binding, and process-liveness checks on Linux, macOS, and
Windows.

## Integration test harness

Create a harness under `tools/mcp/tests/integration/` that:

1. creates temporary config, cache, library, and output directories;
2. starts a GUI darktable build with remote control explicitly enabled;
3. imports/opens a small redistributable test image;
4. waits for a matching discovery record;
5. runs protocol and MCP assertions;
6. asks darktable to exit normally;
7. verifies discovery cleanup and process termination.

Linux CI can use Xvfb. macOS and Windows jobs should use their existing GUI
test execution environment rather than silently skipping the integration suite.

Keep test credentials/session tokens inside temporary directories and redact
them from captured logs.

## Commit sequence

Prefer reviewable commits that leave the tree buildable:

1. `remote_edit`: read-only module introspection and unit fixtures
2. `remote_protocol`: JSON envelopes and validation
3. `remote_frame`: bounded streaming framing
4. `remote_server`: authenticated loopback and discovery
5. `darktable-mcp`: read-only Python MCP sidecar
6. `remote_edit`: revisions and atomic mutations
7. `remote_edit`: enable/reset/instance-creation/history/undo
8. `remote_edit`: asynchronous preview
9. `scopes`: shared kernels in `src/common/scopes.c` and `compute_scopes`
10. packaging, platform documentation, and end-to-end CI

Do not combine the initial C control API, networking, Python sidecar, and
mutation code into one change. The read-only vertical slice is the architectural
checkpoint.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Module introspection is incomplete for complex controls | Limit v1 to explicitly supported scalars/enums and report writable capability per field |
| Main-thread mutation deadlocks with history or signals | Dispatch on the GUI context and follow existing helper lock/signal order; add integration tests under repeated edits |
| Preview path blocks the UI | Use existing job/pixelpipe infrastructure and asynchronous completion |
| Session token leaks into logs or permissive files | Redaction tests, atomic restrictive file creation, mandatory loopback binding |
| A timed-out mutation completes later | Do not auto-retry; mark completion uncertain and read state/revision before another mutation |
| Multiple instances target the wrong GUI | Explicit discovery path/PID support and session identity in every handshake |
| Protocol and MCP schemas drift | Keep private protocol fixtures and MCP adapter tests in the same repository |
| Windows lifecycle/path behavior differs | Exercise discovery, atomic replacement, stale cleanup, and shutdown in Windows CI from the first read-only milestone |

## Decisions required before mutation work

The read-only vertical slice can proceed without these decisions, but Phase 2
must resolve them:

1. ~~Undo policy~~ — resolved in the protocol reference: compare-and-undo
   with a required `expected_revision`; no multi-step undo in v1.
2. Whether setting parameters may explicitly enable a module in the same
   request. (The protocol reference specifies an explicit `enable` member;
   confirm during implementation.)
3. ~~Unsupported schema fields~~ — resolved in the protocol reference:
   always included with `writable: false`, no opt-in flag.
4. ~~Cryptographic random source~~ — resolved in the remote-edit internals
   document §7: new `dt_crypto_random_bytes` wrapper
   (getrandom/BCryptGenRandom/SecRandomCopyBytes); no suitable in-tree or
   GLib-floor API exists.
5. ~~Preview path~~ — resolved in the remote-edit internals document §8:
   `dt_imageio_export_with_flags` with a synthetic in-memory format sink,
   sRGB, `dt_imageio_jpeg_compress`, on a background job after a main-thread
   history flush.

## First implementation checkpoint

Stop and review the architecture after Step 5. At that point the following
must work end to end without any mutation capability:

```text
MCP client
  → Python sidecar
  → discovery and authenticated loopback socket
  → JSON dispatcher
  → remote-edit read API
  → live darkroom module introspection
```

This checkpoint proves the process boundary, cross-platform transport, stable
identifiers, schema conversion, error model, and packaging shape before code is
allowed to change an image.
