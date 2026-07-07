# darktable MCP — Private Protocol Reference

Date: 2026-07-05
Status: reference (companion to `2026-07-05-darktable-mcp-design.md`)
Protocol version: 1

## Purpose

The authoritative message-level contract between the remote server inside
darktable and the Python MCP sidecar. The design spec defines the transport
(framing, authentication, threading, versioning policy); this document
defines every method's request and response shape. The C dispatcher's
validation and the Python client's test fixtures both derive from this file;
when they disagree, this file wins until amended.

## Conventions

- Requests: `{"id": <uint>, "method": <string>, "params": <object>}`.
  `params` may be omitted when a method takes no parameters.
- Success: `{"id", "ok": true, "result": <object>}`. Error:
  `{"id", "ok": false, "error": {"code", "message", "details"?, "retryable"}}`.
  If a request's `id` is missing or not an unsigned integer, the error
  response carries `"id": null` (the request cannot be correlated).
- **Strict params.** Unknown keys in `params` are rejected with
  `invalid_value`. The sidecar and server are versioned together; forward
  compatibility is handled by the protocol version and the `capabilities`
  list, not by silently ignoring fields.
- **Numbers.** JSON numbers only. Non-finite values (NaN, ±Inf) are rejected
  with `invalid_value`. Integer fields reject fractional values.
- **Enums.** Written as the stable introspection name (preferred) or the
  integer representation; always returned as the name. An integer with no
  matching enum member is `invalid_value`.
- **Field names** are introspection names, including dotted paths for scalar
  leaves in nested structs (`random.damping`, `center.x`).
- **Instances** are addressed by `instance` = the module's `multi_priority`.
  Every method that takes `instance` defaults it to 0.
- **Revisions.** Process-local, monotonic, incremented on every observed
  darkroom history change. Any method that reads or mutates darkroom state
  returns the coherent `revision`. Mutations accept optional
  `expected_revision`; a mismatch fails with `revision_conflict` and changes
  nothing.
- **Optional parameters** are marked `?` and shown with their defaults.

## Methods

### hello

First frame on every connection; anything else first is `unauthorized`.

Request params:

```json
{
  "protocol_version": 1,
  "token": "base64url-encoded-random-token",
  "client": "darktable-mcp/0.1.0"
}
```

Result:

```json
{
  "protocol_version": 1,
  "darktable_version": "5.x",
  "pid": 12345,
  "capabilities": ["params", "instances", "history", "preview", "scopes"]
}
```

`pid` lets the sidecar confirm it reached the instance selected during
discovery. Capabilities gate optional features: a build that cannot render
previews omits `"preview"` and the sidecar hides `look_at_image`.

### get_state

No params.

```json
{
  "view": "darkroom",
  "image": {
    "id": 172,
    "filename": "IMG_4021.CR3",
    "width": 6000,
    "height": 4000,
    "exif": {
      "maker": "Canon", "model": "EOS R6", "lens": "RF35mm F1.8",
      "iso": 800, "aperture": 2.8, "exposure_time": 0.005,
      "focal_length": 35.0
    }
  },
  "revision": 31
}
```

`view` is the current darktable view name; `image` is `null` when no image
is open in the darkroom. All editing methods fail with `not_in_darkroom` /
`no_image_open` rather than returning partial state.

### list_modules

No params. Returns one entry per live module instance for the current image,
in pixelpipe order:

```json
{
  "revision": 31,
  "modules": [
    {
      "op": "exposure",
      "instance": 0,
      "instance_name": "",
      "display_name": "exposure",
      "enabled": true,
      "deprecated": false,
      "supports_multiple_instances": true
    }
  ]
}
```

`display_name` and `instance_name` are translated presentation metadata,
never identifiers. Hidden/internal modules (see the supported-operations
reference) are never listed.

### get_module_schema

Request params: `{"module": "exposure"}` (per-op; instances share a schema).

```json
{
  "module": "exposure",
  "display_name": "exposure",
  "params_version": 7,
  "deprecated": false,
  "supports_multiple_instances": true,
  "fields": [
    {
      "name": "exposure",
      "description": "exposure correction",
      "type": "float",
      "minimum": -18.0,
      "maximum": 18.0,
      "default": 0.0,
      "writable": true
    },
    {
      "name": "mode",
      "description": "mode",
      "type": "enum",
      "default": "EXPOSURE_MODE_MANUAL",
      "writable": true,
      "enum_values": [
        { "name": "EXPOSURE_MODE_MANUAL", "value": 0, "description": "manual" },
        { "name": "EXPOSURE_MODE_DEFLICKER", "value": 1, "description": "automatic" }
      ]
    }
  ]
}
```

Field `type` is one of `float`, `int`, `uint`, `bool`, `enum`, or — for
unsupported shapes — `array`, `string`, `struct`, `opaque`. **Unsupported
fields are included with `writable: false`** rather than omitted (decided;
previously plan open decision 3): the model can then explain *why* it cannot
edit something instead of being blind to it. Fields on the
supported-operations internal denylist are scalar-typed but also
`writable: false`. `minimum`/`maximum` are omitted for untagged fields;
validation is then finiteness-only. Schema responses are cacheable per
(darktable version, module op).

Errors: `unknown_module`.

### get_module_params

Request params: `{"module": "exposure", "instance": 0?}`.

```json
{
  "module": "exposure",
  "instance": 0,
  "instance_name": "",
  "enabled": true,
  "revision": 31,
  "values": { "exposure": 0.5, "black": 0.0, "mode": "EXPOSURE_MODE_MANUAL" }
}
```

`values` contains every readable supported field (writable and denylisted
scalars alike; unsupported shapes are absent). Errors: `unknown_module`,
`unknown_instance`.

### set_module_params

Request params:

```json
{
  "module": "exposure",
  "instance": 0,
  "values": { "exposure": 0.7, "black": -0.002 },
  "expected_revision": 31,
  "enable": true
}
```

`values` must be non-empty, contain no duplicate or unknown fields, and every
field must be `writable: true`. The whole patch is validated against the
schema before anything is written; any failure means no change. `enable?`
(default absent) explicitly enables/disables the module in the same single
history item — parameters are never an implicit enable.

Result (values read back from live state):

```json
{
  "module": "exposure",
  "instance": 0,
  "enabled": true,
  "values": { "exposure": 0.7, "black": -0.002 },
  "revision": 32
}
```

Errors: `unknown_module`, `unknown_instance`, `unknown_field`,
`unsupported_field` (known but not writable), `invalid_value`,
`revision_conflict`.

### set_module_enabled

Request params: `{"module", "instance"?, "enabled": true, "expected_revision"?}`.
Result: `{"module", "instance", "enabled", "revision"}`.

### reset_module

Request params: `{"module", "instance"?, "expected_revision"?}`. Restores the
instance's defaults through the normal reset lifecycle.
Result: `{"module", "instance", "enabled", "values", "revision"}` with the
post-reset values.

### create_module_instance

Request params:

```json
{
  "module": "exposure",
  "source_instance": 0,
  "copy_params": false,
  "expected_revision": 31
}
```

Result:

```json
{
  "module": "exposure",
  "instance": 1,
  "instance_name": "1",
  "enabled": true,
  "revision": 33
}
```

Follows the native new-instance lifecycle (two history entries, same as the
GUI button — the response `revision` reflects the final state). Errors:
`unknown_module`, `unknown_instance` (bad `source_instance`),
`instance_not_supported`, `revision_conflict`.

### get_history

Request params: `{"limit": 20?}` (server cap 100). Items ordered oldest to
newest; `seq` is the history stack position.

```json
{
  "revision": 33,
  "items": [
    {
      "seq": 12,
      "op": "exposure",
      "instance": 0,
      "display_name": "exposure",
      "instance_name": "",
      "enabled": true
    }
  ]
}
```

No parameter blobs are returned — use `get_module_params` for values.

### undo

Request params: `{"expected_revision": 33}` — **required**, no `steps`
parameter in v1 (decided; previously plan open decision 1). Semantics:
compare-and-undo. If the live revision equals `expected_revision`, undo
exactly one history transition through darktable's undo system; otherwise
fail with `revision_conflict` and change nothing. This can undo a user edit
only when that edit is the latest transition *and* the caller has read the
matching revision first — interleaved user activity always surfaces as a
conflict, never a surprise rollback. Multi-step undo is future work.

Result: `{"revision": 34}` (undo itself is a history change and produces a
new revision).

### render_preview

Request params: `{"max_px": 1024?, "quality": 85?}`. `max_px` is clamped to
[64, 2048]; `quality` to [50, 95]. Renders the active image through the
normal pixelpipe with current history; asynchronous server-side.

```json
{
  "mime_type": "image/jpeg",
  "width": 1536,
  "height": 1024,
  "revision": 34,
  "data": "...base64..."
}
```

Errors: `preview_failed`, `request_too_large` (result exceeds frame cap —
retry with smaller `max_px`).

### compute_scopes

Request params:

```json
{
  "scopes": ["histogram", "waveform", "parade", "vectorscope"],
  "include_summary": true,
  "include_bins": false,
  "include_images": true,
  "image_size": 512
}
```

`scopes` must be a non-empty subset of the four names. All results in one
response derive from one captured preview buffer and share its `revision`.
The result contains one key per requested scope; `histogram` carries the
numeric summary (and 256 normalized per-channel bins when
`include_bins`); `waveform`/`parade`/`vectorscope` carry bounded PNG images
(`{"mime_type", "width", "height", "data"}`) when `include_images`, plus
compact summaries where defined. Response shapes follow the design spec's
scope-analysis section verbatim; `image_size` is clamped to [128, 1024].

Errors: `scope_failed`, `invalid_value`.

## Error codes by method

`unauthorized`, `request_too_large`, `busy`, and `internal` can occur on any
call and are omitted from the rows. `busy` and `timeout`-adjacent handling
live in the sidecar per the design spec.

| method | not_in_darkroom | no_image_open | unknown_module | unknown_instance | unknown_field | unsupported_field | invalid_value | instance_not_supported | revision_conflict | preview_failed | scope_failed |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `hello` | | | | | | | ✓ | | | | |
| `get_state` | | | | | | | | | | | |
| `list_modules` | ✓ | ✓ | | | | | | | | | |
| `get_module_schema` | | | ✓ | | | | ✓ | | | | |
| `get_module_params` | ✓ | ✓ | ✓ | ✓ | | | ✓ | | | | |
| `set_module_params` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | ✓ | | |
| `set_module_enabled` | ✓ | ✓ | ✓ | ✓ | | | ✓ | | ✓ | | |
| `reset_module` | ✓ | ✓ | ✓ | ✓ | | | ✓ | | ✓ | | |
| `create_module_instance` | ✓ | ✓ | ✓ | ✓ | | | ✓ | ✓ | ✓ | | |
| `get_history` | ✓ | ✓ | | | | | ✓ | | | | |
| `undo` | ✓ | ✓ | | | | | ✓ | | ✓ | | |
| `render_preview` | ✓ | ✓ | | | | | ✓ | | | ✓ | |
| `compute_scopes` | ✓ | ✓ | | | | | ✓ | | | | ✓ |

`retryable` is `true` for `revision_conflict`, `busy`, and transient
`preview_failed`/`scope_failed`; `false` otherwise.

## Resolved decisions recorded here

1. **Undo contract (plan decision 1):** compare-and-undo with a required
   `expected_revision`; no `steps` in v1.
2. **Unsupported schema fields (plan decision 3):** always included with
   `writable: false`; no `include_unsupported` flag.

## Maintenance

Additive, optional response fields may be introduced without a version bump,
per the design spec's compatibility policy. Anything that changes a field's
meaning, requiredness, or type bumps `protocol_version`. Every change here
must land with matching updates to the C dispatcher validation and the
Python fixtures in the same commit.
