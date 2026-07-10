# darktable MCP — Remote-Edit Internals (Low-Level Design)

Date: 2026-07-05
Status: reference (companion to `2026-07-05-darktable-mcp-design.md` and
`2026-07-05-darktable-mcp-protocol-reference.md`)
Code evidence: this fork at commit `86a8e6bdc3`

## Purpose

C-level design for the in-process half of the MCP system: type layouts,
function signatures, threading placement, and the concrete darktable helpers
each operation is built on. This resolves the plan's remaining
code-archaeology questions (preview helper, scopes factoring, crypto random)
with evidence, so implementers work from decisions, not investigations.

## 1. Threading model (foundation for everything below)

`GSocketService` accept/read/write callbacks run on the GLib default main
context — which in darktable **is the GTK main thread**. This is the load-
bearing simplification:

- **Reads and mutations execute inline in the request handler.** No cross-
  thread marshalling: by the time the dispatcher runs, we are already on the
  only thread allowed to touch `darktable.develop`, module params, and
  history. Mutation cost is small — `dt_dev_add_history_item` invalidates
  the pipe and queues a redraw; the actual reprocess is already asynchronous.
- **Preview renders and scope computation go to background jobs**
  (`dt_control_add_job`), because they run a pixelpipe. The pending request
  is completed from the job's completion path; the connection stays
  responsive because socket I/O is async on the main loop.
- The Lua lock and `dt_lua_gtk_wrap` machinery are **not involved** — this
  subsystem never enters the Lua runtime.
- Two rules from earlier code review remain binding: never call
  `dt_dev_add_history_item` while a GUI-update guard is raised (it returns
  silently without recording — `src/develop/develop.c:1379`), and follow the
  preset-apply ordering: write params → `dt_iop_gui_update` → one history
  item (`src/gui/presets.c:1074-1115`).

## 2. `src/control/remote_edit.h` — types and ownership

Naming and placement follow `src/control/` conventions (flat snake_case
pairs; `dbus.c` is the IPC precedent).

```c
typedef enum dt_remote_error_code_t
{
  DT_REMOTE_OK = 0,
  DT_REMOTE_ERR_NOT_IN_DARKROOM,
  DT_REMOTE_ERR_NO_IMAGE_OPEN,
  DT_REMOTE_ERR_UNKNOWN_MODULE,
  DT_REMOTE_ERR_UNKNOWN_INSTANCE,
  DT_REMOTE_ERR_UNKNOWN_FIELD,
  DT_REMOTE_ERR_UNSUPPORTED_FIELD,
  DT_REMOTE_ERR_INVALID_VALUE,
  DT_REMOTE_ERR_INSTANCE_NOT_SUPPORTED,
  DT_REMOTE_ERR_REVISION_CONFLICT,
  DT_REMOTE_ERR_PREVIEW_FAILED,
  DT_REMOTE_ERR_SCOPE_FAILED,
  DT_REMOTE_ERR_INTERNAL,
} dt_remote_error_code_t;
// transport-only codes (unauthorized, request_too_large, busy) live in
// remote_server/remote_protocol, not here — remote_edit never sees them.

typedef struct dt_remote_error_t
{
  dt_remote_error_code_t code;
  char *message;        // owned; human-readable
  char *details_json;   // owned, nullable; pre-serialized details object
} dt_remote_error_t;

typedef enum dt_remote_value_type_t
{
  DT_REMOTE_VALUE_FLOAT,   // float and double fields
  DT_REMOTE_VALUE_INT,     // all signed/unsigned integer widths
  DT_REMOTE_VALUE_BOOL,
  DT_REMOTE_VALUE_ENUM,
} dt_remote_value_type_t;

typedef struct dt_remote_value_t
{
  dt_remote_value_type_t type;
  union
  {
    double f;
    int64_t i;
    gboolean b;
    struct { int value; char *name; } e;  // name owned
  } v;
} dt_remote_value_t;

typedef struct dt_remote_field_t
{
  char *name;           // introspection name, dotted for nested leaves
  char *description;    // $DESCRIPTION or ""
  char *type_name;      // "float", "int", "uint", "bool", "enum",
                        // "array", "string", "struct", "opaque"
  gboolean writable;
  gboolean has_range;
  double minimum, maximum;   // valid iff has_range
  gboolean has_default;
  dt_remote_value_t default_value;
  GPtrArray *enum_values;    // of dt_remote_enum_value_t {name,value,desc}
} dt_remote_field_t;

typedef struct dt_remote_module_ref_t
{
  const char *op;
  int instance;              // multi_priority; 0 = default
} dt_remote_module_ref_t;

// The result structs below mirror the wire shapes in the protocol
// reference field-for-field; the dispatcher serializes them 1:1.

typedef struct dt_remote_state_t   // ← get_state
{
  char *view;                   // owned; current view name
  gboolean has_image;           // FALSE → image fields undefined, wire null
  int32_t image_id;
  char *image_filename;         // owned; basename only
  int width, height;
  char *maker, *model, *lens;   // owned; "" when unknown
  float iso, aperture, exposure_time, focal_length;  // 0 when unknown
  uint64_t revision;
} dt_remote_state_t;

typedef struct dt_remote_module_t  // ← list_modules entries
{
  char *op;                     // owned; stable identifier
  int instance;                 // multi_priority
  char *instance_name;          // owned; translated, "" for default
  char *display_name;           // owned; translated, presentation only
  gboolean enabled;
  gboolean deprecated;
  gboolean supports_multiple_instances;
} dt_remote_module_t;

typedef struct dt_remote_module_schema_t  // ← get_module_schema
{
  char *op;                     // owned
  char *display_name;           // owned
  int params_version;           // module class version()
  gboolean deprecated;
  gboolean supports_multiple_instances;
  GPtrArray *fields;            // dt_remote_field_t
} dt_remote_module_schema_t;

typedef struct dt_remote_history_item_t  // ← get_history items
{
  int seq;                      // history stack position
  char *op;                     // owned
  int instance;
  char *display_name;           // owned
  char *instance_name;          // owned
  gboolean enabled;
} dt_remote_history_item_t;

// Shared by every mutation (set/enable/reset/create): all members are
// read back from live state after commit, inside the same main-context
// dispatch — the caller never echoes what it sent.
typedef struct dt_remote_mutation_result_t
{
  char *op;                     // owned
  int instance;                 // the new instance for create_module_instance
  char *instance_name;          // owned; meaningful after create
  gboolean enabled;
  GPtrArray *values;            // dt_remote_patch_entry_t, read back; NULL
                                // for enable/create (no values on the wire)
  uint64_t revision;
} dt_remote_mutation_result_t;

typedef struct dt_remote_patch_entry_t
{
  char *name;
  dt_remote_value_t value;
} dt_remote_patch_entry_t;

typedef struct dt_remote_patch_t
{
  GPtrArray *scalar_values;    // dt_remote_patch_entry_t
  GPtrArray *semantic_values;  // reserved: semantic-class patches (curves
                               // etc., see the curve-classes design); NULL
                               // and unused in v1, but the transaction is
                               // written against this struct so semantic
                               // support extends it without a rewrite
  gboolean has_enable;
  gboolean enable;
} dt_remote_patch_t;
// duplicates rejected at protocol layer
```

Ownership rules: every returned object has a paired `dt_remote_*_free()`;
errors are heap-allocated by the callee into `dt_remote_error_t **error`
out-params and freed by the caller; no function retains pointers into
`module->params` beyond its own call (values are copied out).

Read API (plan step 1) and mutation API (step 7) — these signatures are
normative; the plan carries excerpts:

```c
gboolean dt_remote_get_state(dt_remote_state_t **out, dt_remote_error_t **err);
gboolean dt_remote_list_modules(GPtrArray **out /* dt_remote_module_t */,
                                dt_remote_error_t **err);
gboolean dt_remote_get_module_schema(const char *op,
                                     dt_remote_module_schema_t **out,
                                     dt_remote_error_t **err);
gboolean dt_remote_get_module_params(const dt_remote_module_ref_t *ref,
                                     GPtrArray **out /* patch entries */,
                                     dt_remote_error_t **err);
gboolean dt_remote_set_module_params(const dt_remote_module_ref_t *ref,
                                     const dt_remote_patch_t *patch,
                                     const uint64_t *expected_revision,
                                     dt_remote_mutation_result_t **out,
                                     dt_remote_error_t **err);
// enable/disable travels inside dt_remote_patch_t (has_enable/enable);
// there is no separate enable argument.
gboolean dt_remote_set_module_enabled(const dt_remote_module_ref_t *ref,
                                      gboolean enabled,
                                      const uint64_t *expected_revision,
                                      dt_remote_mutation_result_t **out,
                                      dt_remote_error_t **err);
gboolean dt_remote_reset_module(const dt_remote_module_ref_t *ref,
                                const uint64_t *expected_revision,
                                dt_remote_mutation_result_t **out,
                                dt_remote_error_t **err);
gboolean dt_remote_create_module_instance(const char *op, int source_instance,
                                          gboolean copy_params,
                                          const uint64_t *expected_revision,
                                          dt_remote_mutation_result_t **out,
                                          dt_remote_error_t **err);
gboolean dt_remote_get_history(int limit,
                               GPtrArray **out /* dt_remote_history_item_t */,
                               dt_remote_error_t **err);
gboolean dt_remote_undo(uint64_t expected_revision, uint64_t *new_revision,
                        dt_remote_error_t **err);
```

### 2a. Pure conversion layer (the plan's testability requirement)

The introspection→schema conversion is a pure function over descriptor
data, testable with fixture `dt_introspection_field_t` values and no
`dt_develop_t`:

```c
// walks a get_introspection_linear()-style array; NONE-terminated
GPtrArray *dt_remote_schema_from_introspection(
    const dt_introspection_field_t *linear,
    const dt_remote_denylist_t *denylist);   // per-module writable:false names

// single-value conversions, both directions
gboolean dt_remote_value_from_field(const dt_introspection_field_t *f,
                                    const void *params_blob,
                                    dt_remote_value_t *out);
gboolean dt_remote_value_validate_and_write(const dt_introspection_field_t *f,
                                            const dt_remote_value_t *v,
                                            void *params_blob,
                                            dt_remote_error_t **err);
```

Type mapping (from `src/common/introspection.h:34-55`): FLOAT/DOUBLE →
float; CHAR..ULONG → int/uint; BOOL → bool; ENUM → enum with
entries from `f->Enum.values`; ARRAY/STRUCT-nonleaf/UNION/OPAQUE/
FLOATCOMPLEX → unsupported (`writable:false`). Field names come from
`header.name` (full dotted path); the walker template is
`dt_iop_default_init` (`src/develop/imageop.c:216-303`). Range metadata:
`f->Float.Min/Max`, `f->Int.Min/Max`, etc.; enum name↔value via
`dt_introspection_get_enum_name/value` (`introspection.h:312,330`).

Live traversal (not unit-tested; covered by integration tests): iterate
`darktable.develop->iop`, resolve instances with
`dt_iop_get_module_by_op_priority` (`src/develop/imageop.c:3652`), read
schema from `module->so->get_introspection_linear()`.

## 3. Mutation engine sequence (step 7, concretized)

All on the main thread, inside the request handler:

1. Check `dt_view_get_current()` is darkroom; `darktable.develop` has a
   valid image (`no_image_open` otherwise).
2. Revision check against the tracker (§5).
3. `dt_iop_get_module_by_op_priority(darktable.develop->iop, op, instance)`;
   NULL → `unknown_module`/`unknown_instance`.
4. `g_malloc(module->params_size)` + memcpy of `module->params` into the
   temp block.
5. For each patch entry: resolve the field against the schema (denylist
   applied), `dt_remote_value_validate_and_write` into the temp block.
   First failure → free and return; live params untouched.
6. memcpy temp block → `module->params`. If `enable` tri-state present,
   set `module->enabled` accordingly (same single history item).
7. `dt_iop_gui_update(module)` — syncs widgets; manages its own
   ENTER/LEAVE guard (`src/develop/imageop.c:2267` area).
8. `dt_dev_add_history_item(darktable.develop, module, FALSE)` — one item
   for the whole patch; internally invalidates pipes and queues redraw
   (`src/develop/develop.c:1413,1432`).
9. Read values back from `module->params` into the result; capture the new
   revision.

`reset_module` copies `module->default_params` in step 4-6.
`create_module_instance` delegates to `dt_iop_gui_duplicate(base,
copy_params)` (`src/develop/imageop.c:703-797`), which handles GUI expander
placement, both history entries, pipe rebuild, and focus; afterwards read
the new instance's `multi_priority`/`multi_name` from the returned module.
Reject when `module->flags() & IOP_FLAGS_ONE_INSTANCE`.

`undo` calls the same undo entry point as Ctrl+Z (`dt_undo_do_undo` on
`darktable.undo`, scoped to develop) after the revision equality check.

## 4. `src/control/remote_frame.[ch]` — framing parser

Pure streaming state machine, no GLib I/O or JSON dependency:

```c
typedef struct dt_remote_frame_parser_t
{
  uint8_t header[4];
  size_t header_filled;
  GByteArray *body;        // allocated only after header validates
  uint32_t body_expected;
  gboolean failed;
} dt_remote_frame_parser_t;

typedef gboolean (*dt_remote_frame_cb)(GBytes *payload, gpointer user_data);

gboolean dt_remote_frame_feed(dt_remote_frame_parser_t *p,
                              const uint8_t *data, size_t len,
                              dt_remote_frame_cb on_frame, gpointer ud,
                              GError **error);
GBytes *dt_remote_frame_encode(GBytes *payload);  // NULL if > max
```

Length is validated against `DT_REMOTE_MAX_FRAME` (16 MiB) **before** the
body array is allocated. Zero-length frames are an error. Big-endian
conversion via `GUINT32_FROM_BE`. `feed` handles any chunking: header split
across reads, multiple frames per read, EOF mid-frame (caller treats a
non-empty parser at EOF as a protocol error).

## 5. Revision tracker

```c
typedef struct dt_remote_revision_t
{
  guint64 counter;      // g_atomic access; 0 valid at start
  dt_imgid_t imgid;     // identity: revision N is meaningful only per image
} dt_remote_revision_t;
```

Wiring: `DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, ...)`
(`src/control/signal.h:173`; connect pattern `src/libs/history.c:165`)
increments the counter; `DT_SIGNAL_DEVELOP_IMAGE_CHANGED`
(`signal.h:192`) increments it *and* re-stamps `imgid`, so an image switch
is always observable as a state change. Signals are raised on the main
thread; reads happen there too — the atomic is belt-and-braces for the
job-completion paths. Responses carry `(revision)`; `expected_revision`
comparisons also require the stamped `imgid` to equal the live image.

## 6. `src/control/remote_protocol.[ch]` — dispatch

```c
typedef struct dt_remote_method_t
{
  const char *name;
  gboolean needs_darkroom;   // metadata only: the precondition is enforced
                             // inside the remote_edit functions (plan step 1
                             // mandates the checks there); dispatch does not
                             // duplicate it — kept for scheduling and
                             // documentation in later steps
  gboolean is_mutation;      // serialized; rejected while another runs
  gboolean is_async;         // render_preview, compute_scopes
  JsonNode *(*handler)(JsonObject *params, dt_remote_session_t *session,
                       dt_remote_pending_t *pending /* async only */);
} dt_remote_method_t;
```

The two transport types the handlers see (owned by `remote_server`, opaque
to `remote_edit`):

```c
typedef struct dt_remote_session_t   // one per accepted connection
{
  GSocketConnection *connection;     // ref held
  enum { DT_REMOTE_SESSION_AWAIT_HELLO,
         DT_REMOTE_SESSION_READY } auth_state;
  int auth_failures;                 // closes at MAX_AUTH_FAILURES
  int pending_requests;              // rejected busy at MAX_PENDING
  dt_remote_frame_parser_t frame;    // per-connection streaming parser
  struct dt_remote_server_t *server; // back-pointer: revision tracker, limits
} dt_remote_session_t;

typedef struct dt_remote_pending_t   // one per in-flight async request
{
  dt_remote_session_t *session;      // connection ref held via session
  gint64 request_id;
  GCancellable *cancellable;         // fired on disconnect; job checks it
} dt_remote_pending_t;
```

A static array of 13 entries is the allowlist; lookup by `g_str_equal`.
Validation is hand-rolled per handler with shared helpers
(`_require_string(obj, "module", &err)`, `_optional_int_default(...)`,
strict unknown-key scan via `json_object_foreach_member`), per the protocol
reference's strict-params rule. Responses are built with `JsonBuilder`;
json-glib is a hard dependency (`src/CMakeLists.txt:452`). The dispatcher
maps `dt_remote_error_t` → the wire error envelope; `retryable` is derived
from the code, not stored.

Async methods return NULL from the handler and complete later: the pending
object holds the connection ref, request id, and a cancellable that fires
if the connection drops before the job completes.

## 7. `src/control/remote_server.[ch]` + `remote_discovery.[ch]`

- `dt_remote_server_t` holds the `GSocketService`, bound port, token hash,
  connection list, revision tracker, and limits (`MAX_CONNECTIONS 4`,
  `MAX_PENDING 8`, `MAX_AUTH_FAILURES 3`).
- Per-connection state machine: `AWAIT_HELLO → READY`; any frame before a
  valid `hello` (checked with `g_str_equal`-free constant-time compare —
  see below) closes after the failure budget.
- Token compare: fixed-length memcmp over the full length regardless of
  mismatch position (simple constant-time loop; no early exit).
- **First networking code in the tree** (verified: zero `GSocket*` usage
  today), so no in-tree pattern to follow; the design is stock GIO:
  `g_socket_service_new` + `g_socket_listener_add_address` with an explicit
  `127.0.0.1` `GInetSocketAddress` and port 0 (NOT
  `g_socket_listener_add_inet_port`, which binds all interfaces and would
  violate the loopback-only constraint), the effective address from
  `g_socket_listener_add_address`'s out parameter for the OS-assigned
  port, async `g_input_stream_read_async` loops feeding the frame parser.
- **Random bytes: new `src/common/crypto_random.[ch]`** —
  `gboolean dt_crypto_random_bytes(void *buf, size_t len)`; Linux
  `getrandom(2)` (fallback `/dev/urandom` read), Windows `BCryptGenRandom`,
  macOS `SecRandomCopyBytes`. Nothing suitable exists in-tree today
  (verified — the only randomness is `rand()` in ashift and a TEA hash),
  and `g_uuid_string_random` (available at the GLib 2.56 floor) is not
  specified as CSPRNG-backed, so it is not used for the token.
- Lifecycle: init at the end of `dt_init()`'s GUI branch — after
  `dt_gui_gtk_init` (`src/common/darktable.c:1933`) and view/lib init, NOT
  at the D-Bus init point (`darktable.c:1532`), which runs before develop
  state exists. Field `struct dt_remote_server_t *remote_server` on
  `darktable_t`. Shutdown before GUI teardown.
- Preference: `security/enable_remote_control`, default false, declared in
  `data/darktableconfig.xml.in` under the existing `security` tab (pattern:
  the `dtconfig` blocks at lines 37-49). Plus a `--enable-remote-control`
  style core option for integration tests (checked alongside the pref, OR
  semantics, single code path).
- Discovery per the design spec: `<configdir>/mcp/session-<pid>.json`,
  written via `g_file_set_contents`-style temp+rename, `0600` where
  supported, deleted on orderly shutdown.

## 8. Preview rendering (plan step 9 — helper question resolved)

**Decision: export path with a synthetic in-memory format sink; not
`dt_dev_image`, not `dt_imageio_preview`.**

Evidence and recipe:

- `dt_imageio_export_with_flags` (`src/imageio/imageio.c:1045`) accepts any
  `dt_imageio_module_format_t`; two in-tree precedents supply memory sinks:
  `_preview_write_image` (`imageio.c:1719-1740`) and the HDR-merge job's
  ad-hoc format (`src/control/jobs/control_jobs.c:645-664`) — the latter
  also demonstrates running the export on a background job
  (`_control_merge_hdr_job_run`, `control_jobs.c:634`).
- Sink format: `bpp()→8`, `levels()→IMAGEIO_RGB|IMAGEIO_INT8`,
  `mime()→"memory"`, `write_image()` copies the buffer; `filename` is an
  ignored constant. `max_width/max_height` on the format head bound the
  longest edge (clamped [64, 2048]).
- `display_byteorder = FALSE` so the 8-bit path swaps to **RGBA**
  (`imageio.c:1435-1444`) — exactly what `dt_imageio_jpeg_compress`
  expects (it reads bytes [0,1,2] as R,G,B —
  `src/imageio/imageio_jpeg.c:298-300`).
- `icc_type = DT_COLORSPACE_SRGB`: portable interchange for a model
  consumer. The display profile (what `dt_dev_image` bakes in —
  `src/iop/colorout.c:611-617`) would be wrong off this machine. Rejected
  alternatives: `dt_dev_image` is GUI-thread-bound, display-profile BGRA,
  coupled to live zoom state (`src/develop/develop.c:3918-3980`);
  `dt_imageio_preview` is display-profile BGRA too (`imageio.c:1786-1790`).
- JPEG encode: `dt_imageio_jpeg_compress(in, out, w, h, quality)`
  (`imageio_jpeg.c:260-308`) — in-memory libjpeg destination, returns byte
  length, caller allocates `out` ≥ `4*w*h`. It embeds no ICC profile;
  acceptable because sRGB is the no-profile default assumption. (If an
  embedded profile is ever required, that's a follow-up using the JPEG
  format module's ICC writer, which currently needs a path.)
- **History-flush caveat (binding):** the export path re-loads history from
  the database (`imageio.c:1069-1073`). The handler must call
  `dt_dev_write_history(darktable.develop)` on the main thread *before*
  queueing the job, and capture the revision at that moment — the rendered
  preview is stamped with that revision (the *pre-queue stamp*).
- **Completion-time coherence check (binding):** the pre-queue stamp alone is
  not sufficient. Between the stamp and the job's DB read, another main-thread
  op (a second `render_preview`'s prepare, a `set_module_params`, …) can flush
  *newer* history to the database, so the job would render revision-R+1 pixels
  yet the pending still carries revision R. Because every revision bump happens
  on the main thread, the main-thread completion re-observes the live revision
  before sending the response: if it still equals the pre-queue stamp, the DB
  could not have held newer history during the render and the payload is
  coherent; if it drifted, the completion **discards the rendered payload and
  fails the request with `preview_failed`, retryable=`true`** (message: state
  changed during rendering — retry). This applies the branch's standing
  fail-toward-a-spurious-retryable-error rule (§5, `force_bump`) rather than
  ever answering with a swathe of pixels whose revision label is stale.
- Job queue: `DT_JOB_QUEUE_SYSTEM_BG` (does not contend with user exports
  on `USER_EXPORT`, which serializes). Completion marshals back to the main
  thread via a `g_idle_source_new()` attached to the default context (**not**
  `g_main_context_invoke`, which runs inline whenever the calling thread
  transiently owns the context — a worker does during
  `dt_remote_server_stop()`'s drain — and would run the main-thread-asserting
  completion on the worker thread). An attached idle source is only dispatched
  by the thread iterating the default context (gtk_main, or the stop drain),
  guaranteeing the completion runs on the main thread.
- **Shutdown reclaim (binding):** at quit `dt_control_running()` is already
  false, so a still-`QUEUED` `SYSTEM_BG` job is never dequeued and its
  completion never fires. `dt_remote_server_stop()` must therefore reclaim
  such orphans: a refcounted handshake shared by the pending and the job
  params carries an atomic `QUEUED→RUNNING` (worker) / `QUEUED→RECLAIMED`
  (stop) CAS so exactly one side wins; stop releases the pendings it reclaims,
  then drains the main context under a bounded deadline and, past it,
  deliberately **leaks** (never frees) any session still holding live io_refs
  — a late completion touching freed memory is worse than a leak in an exiting
  process.

## 9. Scopes (plan open question resolved — fork is ~70% there)

This fork already split scopes into per-mode plugins
(`src/libs/scopes/{histogram,waveform,vectorscope,split}.c`) with a
function table (`dt_scopes_functions_t`, `src/libs/scopes.h:61-120`) whose
`process` slot is separate from all cairo `draw_*` slots. The lib shell
(`src/libs/histogram.c`) receives the buffer **pushed from inside the
preview pixelpipe** at the gamma-module hook
(`src/develop/pixelpipe_hb.c:2942-2945`) in pre-gamma display-RGB float,
converts to the histogram profile
(`src/libs/histogram.c:160-184`), and runs the *current* mode only.

Factoring (new `src/common/scopes.[ch]`, statically linkable — the lib is a
loadable MODULE and cannot be linked from common code):

1. POD output structs (bins/max for histogram; A8 rasters + dims for
   waveform/parade; A8 graph + RGB24 hue-ring background for vectorscope) —
   split off the GtkWidget members that live in the lib's data structs
   today.
2. The four kernels as pure functions over
   `(const float *input, dt_histogram_roi_t*, profile, config, out*)`:
   histogram already delegates to the GUI-independent
   `dt_histogram_helper` (`src/common/histogram.c:202`); waveform's OMP
   kernel lifts with `orient` as a parameter
   (`src/libs/scopes/waveform.c:58-165`); parade shares waveform's compute
   verbatim (`waveform.c:536`); vectorscope's kernel lifts with
   type/scale/diameter as parameters, dropping the colorpicker-overlay
   block (`vectorscope.c:604-656`) which is GUI-only.
3. The display→histogram-profile conversion becomes a shared helper so GUI
   and remote convert identically.
4. New cairo-free colorizers composite the A8 rasters to RGB for image
   export; histogram needs a small bins→bars renderer (today only
   `_hist_draw` does this via cairo). PNG encoding uses
   `cairo_image_surface` + `cairo_surface_write_to_png_stream` into a
   `GByteArray` — cairo is already a hard dependency; no new library.
5. The lib's `*_process` become thin adapters unpacking their data structs
   into the POD types — GUI and remote share one implementation, neither
   scrapes the other.

Remote capture: the remote scopes service registers at the same gamma-hook
point and **retains a copy of the latest pushed preview buffer** (preview
pipe is small; order tens of MB float) plus a revision, behind a mutex.
`compute_scopes` runs the requested kernels over the retained buffer on a
background job — all results share one buffer and one revision by
construction, exactly as the protocol reference requires. If no buffer has
been pushed yet (fresh darkroom), the handler returns `scope_failed` with a
retryable hint.

Capture gate (cost): the deep copy is gated on a single `g_atomic` flag set
by `dt_remote_server_start()`/`stop()`. When no remote server is active the
push reads that flag and returns before any allocation, deep copy, or
develop access — a true no-op, so darktable users without remote control pay
nothing on every preview run. The slot is cleared at both server start and
stop, so a pre-connect buffer can never be served.

Revision coherence (stamp proves the pixels): the pushed pixels reflect the
history the preview pipe read when it fixed its input, but the push runs at
the *end* of the pipe run, after conversion. A history bump landing in that
window would otherwise let revision-R pixels be stamped R+1. Because revision
bumps happen **only on the main thread** (§5), coherence is proven by
equality: the process-local revision is recorded on the preview pipe's worker
thread just before `dt_dev_pixelpipe_change()` reads history
(`src/develop/develop.c`, preview pipe only), and read again at push. If the
two are equal, no bump occurred during the run, so the pixels provably match
that revision and it is stamped. If they differ (or a push arrives with no
paired pipe-start note, e.g. from a non-preview pipe), the push is **dropped**
and the previous, coherent slot is left intact — the bump that caused the
mismatch has already scheduled a fresh preview run that will push a coherent
buffer moments later. A `compute_scopes` in the gap therefore sees an
older-but-coherent buffer or an empty slot (retryable `scope_failed`), never a
mislabelled one. The `compute_scopes` completion keeps **no** revision-drift
check (design-spec semantics): a result whose stamped revision has since been
superseded is still returned as success for that revision, because the stamp
is now trustworthy at the source. The slot is also reset on
`DT_SIGNAL_DEVELOP_IMAGE_CHANGED` so the previous image's buffer is never
served as the new image's scopes.

## 10. Test seams recap

- `remote_frame`: byte-at-a-time fuzzable, zero deps.
- `remote_protocol`: dispatcher fed parsed `JsonObject` fixtures, no
  sockets; fixtures authored from the protocol reference.
- Schema/value conversion: fixture `dt_introspection_field_t` descriptors,
  no develop context (plan step 1's acceptance gate).
- `crypto_random`: statistical smoke test + length/error paths.
- Scope kernels: pure-function tests over synthetic buffers (constant
  image → known bins; ramp → known percentiles).
- Everything touching `darktable.develop` stays in the Xvfb integration
  suite, as planned.
