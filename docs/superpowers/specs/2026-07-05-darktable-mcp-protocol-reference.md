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
- **Numbers.** JSON numbers only. Non-finite values (NaN, ±Inf) in requests
  are rejected with `invalid_value`. Integer fields reject fractional
  values. In **responses**, a non-finite float — a stored scalar, or a
  quantity component value produced by a module's read hook — is
  serialized as JSON `null` (JSON has no NaN/Inf token; this occurs in
  practice — `temperature` stores NaN in its unused `various` coefficient
  on every RGB camera, and its tint conversion yields NaN when all three
  RGB coefficients are legally written to zero).
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
  "capabilities": ["params", "semantic_params", "curve_params",
                   "vector_params", "band_params", "quantity_params",
                   "blend_params", "parametric_mask_params", "mask_render",
                   "mask_shapes",
                   "instances", "history", "preview", "scopes"]
}
```

`pid` lets the sidecar confirm it reached the instance selected during
discovery. Capabilities gate optional features: a build that cannot render
previews omits `"preview"` and the sidecar hides `look_at_image`.

`semantic_params` advertises the semantic-parameter read surface
(`semantic_fields` in schemas, `semantic_values` in params reads);
`curve_params` additionally advertises writable curve-class semantic
parameters (the `semantic_values` request member on `set_module_params`).
The server never advertises `curve_params` without accepting that request
member. `vector_params` (milestone 4) additionally advertises writable
vector-class semantic parameters — plain vectors, color triples, and
levels triples (the `semantic_values` request member's vector entries on
`set_module_params`); the server never advertises `vector_params` without
accepting vector entries in `semantic_values`. `band_params` (milestone 5)
additionally advertises writable bands-class semantic parameters —
fixed-count sampled-response band sets (the `semantic_values` request
member's bands entries on `set_module_params`); the server never
advertises `band_params` without accepting band entries in
`semantic_values`. `quantity_params` (milestone 6) additionally
advertises writable quantity-class semantic parameters — named component
groups whose stored units differ from their presentation units, converted
by the module's own hooks (the `semantic_values` request member's
quantity entries on `set_module_params`); the server never advertises
`quantity_params` without accepting quantity entries in
`semantic_values`. A client must send curve patches only when
`curve_params` is present, vector patches only when `vector_params` is
present, band patches only when `band_params` is present, and quantity
patches only when `quantity_params` is present; capability-gated
optional request members do not bump `protocol_version`
(see Maintenance).

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

When the server advertises `semantic_params` and the op has semantic
parameters, the schema additionally carries `semantic_fields`: an array of
semantic descriptors (`name` like `"curve.master"`, `class: "curve"`,
`x`/`y` axis ranges, a `points` limits object, the allowed
`interpolation_values`, and optional `active_when`/`writable_when`/
`periodic_when` conditions on scalar fields).

When the server also advertises `vector_params` (milestone 4), semantic
descriptors with `class: "vector"` may also appear. A `subtype` tag
(`"vector"` | `"color"` | `"levels"`) marks the descriptor's flavor;
`color_space` is present only when `subtype = "color"` (the only value in
use today is `"display_rgb"`); `ordering` is present only when `subtype =
"levels"`. Subtype and color-space strings are stable wire vocabulary, like
curve interpolation names. Normative shape:

```json
{
  "name": "lift", "class": "vector", "display_name": "lift",
  "subtype": "vector",              // "vector" | "color" | "levels"
  "color_space": "display_rgb",     // present only when subtype = "color"
  "readable": true, "writable": true,
  "writable_when": { "field": "mode", "op": "ne", "value": "SLOPE_OFFSET_POWER" },
  "components": [
    { "name": "factor", "minimum": 0.0, "maximum": 2.0 },
    { "name": "red",    "minimum": 0.0, "maximum": 2.0 },
    ...
  ],
  "ordering": {                     // present only when subtype = "levels"
    "rule": "strictly_increasing",
    "minimum_gap": 1.19e-7, "minimum_gap_comparison": "at_least"
  }
}
```

When the server advertises `band_params` (milestone 5), semantic
descriptors with `class: "bands"` may also appear: a fixed-count set of
band samples (a sampled response curve stored as parallel x/y arrays).
`count` is the exact number of samples; `y_range` bounds every y sample;
`x_policy` is `"fixed"` (x positions are immutable) or `"interior"` (the
interior x positions are writable; the endpoints stay pinned); `min_gap`
is present only under `"interior"` and is the minimum adjacent x spacing,
compared **at-least** (a gap exactly equal to `min_gap` is accepted);
`x_shared_with` is present only on twin channels that share their x
positions with another band of the same module (the link is symmetric).
All policy and member names are stable wire vocabulary. Normative shape:

```json
{
  "name": "bands.luma", "class": "bands", "display_name": "luma",
  "readable": true, "writable": true,
  "count": 6,
  "y_range": { "minimum": 0.0, "maximum": 1.0 },
  "x_policy": "interior",                   // "fixed" | "interior"
  "min_gap": 0.001,                         // present only when x_policy = "interior"
  "x_shared_with": "bands.luma_threshold"   // present only on twin channels
}
```

When the server advertises `quantity_params` (milestone 6), semantic
descriptors with `class: "quantity"` may also appear: a named group of
scalar components whose stored units differ from their presentation
units, converted between the two domains by the module's own hooks.
`derived: true` marks the field as a presentation-domain projection of
the stored params: readback runs the reverse conversion and is **lossy**
(a written 5500 K reads back as approximately 5500, not exactly —
clients compare with tolerance; the mutation echo is the authoritative
stored state). `unit` is present on a component only when it has one.
Normative shape:

```json
{
  "name": "wb.temperature", "class": "quantity",
  "display_name": "white balance",
  "readable": true, "writable": true, "derived": true,
  "components": [
    { "name": "temperature", "unit": "kelvin",
      "minimum": 1901.0, "maximum": 25000.0 },
    { "name": "tint", "minimum": 0.135, "maximum": 2.326 }
  ]
}
```

Native storage fields backing a semantic parameter stay listed in `fields`
with `writable: false` and a `represented_by` array naming the semantic IDs
that represent them. This applies uniformly across the curve-bearing ops
(`rgbcurve`, `tonecurve`, `colorzones`, `basecurve`), the vector-bearing
ops (`colorbalance`, `channelmixerrgb`, `rgblevels`, `borders`,
`watermark`, `negadoctor`, `colorharmonizer`), and the band-bearing ops
(`atrous`, `denoiseprofile`, `rawdenoise`, `lowlight`) — see the
`get_module_schema_rgbcurve_*` fixtures for the full curve shape and the
vector/band registry tests for the vector and band shapes; each op follows
the identical wire shape with its own field/semantic names. The one
deliberate exception is the quantity-bearing op `temperature`: its
`red`/`green`/`blue`/`various` coefficient scalars were writable before
the quantity class existed (milestone 1) and **stay `writable: true`**
while also carrying `represented_by: ["wb.temperature"]` — the conflict
rule under `set_module_params` below keeps the two surfaces from
colliding in one request.

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
scalars alike; unsupported shapes are absent).

When the server advertises `semantic_params` and the op has semantic
parameters, the result additionally carries `semantic_values`: every
semantic ID mapped to its current value -- for curves `{"class": "curve",
"active", "effective", "writable_now", "points": [{"x", "y"}, ...],
"interpolation"}`; for vectors (milestone 4) `{"class": "vector",
"active": ..., "effective": ..., "writable_now": ..., "values": [...]}` —
the same status flags curve values carry; for bands (milestone 5)
`{"class": "bands", "active": ..., "effective": ..., "writable_now": ...,
"y": [...], "x": [...]}` — the current samples and their stored x
positions, both as number arrays of the schema's `count` length; for
quantities (milestone 6) `{"class": "quantity", "active": ...,
"effective": ..., "writable_now": ..., "values": {"temperature": 5502.7,
"tint": 1.003}}` — an object mapping each schema component name to its
current presentation-domain value, produced by the module's read hook
(derived and lossy — see `set_module_params`). Inactive
parameters (e.g. `curve.red` in linked mode, `levels.red` in
linked-autoscale mode) are always present with `active: false`, never
omitted.

Errors: `unknown_module`, `unknown_instance`.

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

When the server advertises `curve_params`, the request may also carry a
`semantic_values?` object patching semantic curve parameters in the same
atomic history item (and `values` may then be `{}`; at least one of the
two must be non-empty):

```json
{
  "semantic_values": {
    "curve.master": {
      "class": "curve",
      "points": [{ "x": 0.0, "y": 0.0 }, { "x": 0.25, "y": 0.18 },
                 { "x": 0.75, "y": 0.82 }, { "x": 1.0, "y": 1.0 }],
      "interpolation": "MONOTONE_HERMITE"
    }
  }
}
```

Each entry requires `class: "curve"`, takes 2-64 points on the wire (the
schema's `points` limits are enforced by the engine), each point exactly
`{x, y}` with finite numbers, and an optional `interpolation` from the
schema's `interpolation_values` (omitted keeps the current one). A patch
replaces the whole named curve; unnamed curves are untouched. Unknown or
extra members anywhere in the shape are `invalid_value`; an unknown
semantic ID is `unknown_field`; writing a curve whose `writable_when`
condition fails (checked against the *projected* params, so a mode switch
in the same request counts) is `unsupported_field`. Curve validation errors
carry `details: {"parameter", "point_index", "constraint"}`.

When the server advertises `vector_params` (milestone 4), the same
`semantic_values?` object may also carry vector entries alongside curve
entries, in the same atomic history item:

```json
{
  "semantic_values": {
    "lift": { "class": "vector", "values": [1.0, 1.0, 1.0, 1.0] }
  }
}
```

Each entry requires `class: "vector"`, and supplies `values`: a JSON number array whose length must match
the descriptor's component count exactly — a patch is a **complete
replacement** of the named vector, never a partial write (a client wanting
to change one component does a read-modify-write). Components are carried
as doubles and validated against the schema's per-component
`minimum`/`maximum` in double domain before narrowing to the native storage
type on write; nothing is clamped. Unknown or extra members anywhere in the
shape are `invalid_value`; an unknown semantic ID is `unknown_field`;
writing a vector whose `writable_when` condition fails (checked against the
*projected* params, so a mode switch in the same request counts) is
`unsupported_field`; a `levels`-subtype vector whose components fail the
schema's `ordering` constraint is also `invalid_value`. Vector validation
errors carry `details: {"parameter", "component_index", "constraint"}`
(`component_index` present only when the failure is attributable to one
component).

When the server advertises `band_params` (milestone 5), the same
`semantic_values?` object may also carry bands entries alongside curve and
vector entries, in the same atomic history item:

```json
{
  "semantic_values": {
    "bands.luma": {
      "class": "bands",
      "y": [0.5, 0.6, 0.7, 0.6, 0.5, 0.5],
      "x": [0.0, 0.15, 0.4, 0.6, 0.85, 1.0]
    }
  }
}
```

Each entry requires `class: "bands"` and `y`: a JSON number array of 1-8
samples on the wire (the schema's exact `count` is enforced by the engine),
each finite and within the schema's `y_range` — a patch is a **complete
replacement** of the named band set, never a partial write. `x` is
optional and, when present, must have the same length as `y`. Samples are
carried as doubles and validated in double domain before narrowing to the
native `float` storage on write; nothing is clamped. Sending `x` to a
band whose `x_policy` is `"fixed"` is `unsupported_field`. Under
`"interior"`, the first and last x must equal the stored endpoints (after
`float` narrowing), the positions must be strictly ascending, and every
adjacent gap must be at least `min_gap`. An x write to a band with
`x_shared_with` mirrors the same x to the twin's native array; two entries
in one request carrying different x for the same twin group is
`invalid_value` (identical x on both is accepted and applied once).
Unknown or extra members anywhere in the shape are `invalid_value`; an
unknown semantic ID is `unknown_field`. Band validation errors carry
`details: {"parameter", "array", "index", "constraint"}` — `array` is
`"y"` or `"x"`, naming the offending array, and `index` is present only
when the failure is attributable to one sample.

When the server advertises `quantity_params` (milestone 6), the same
`semantic_values?` object may also carry quantity entries alongside
curve, vector, and band entries, in the same atomic history item:

```json
{
  "semantic_values": {
    "wb.temperature": {
      "class": "quantity",
      "values": { "temperature": 5500.0, "tint": 1.0 }
    }
  }
}
```

Each entry requires `class: "quantity"` and `values`: an object
containing **exactly** the schema's component names, each a finite
number within its component domain — a patch is a **complete
replacement** of the named quantity, never a partial write (the
conversion is joint; a caller changing only the temperature reads the
current pair first and sends the current tint back). Missing, extra, or
unknown component members are `invalid_value`; nothing is clamped. The
write runs through the module's own conversion hooks against the stored
params, and readback is **lossy**: the reverse conversion means a
written 5500 K reads back as approximately 5500, not exactly — the
mutation echo is the authoritative stored state and clients compare
with tolerance. **Coefficient coexistence:** unlike every previous
class, `temperature`'s native `red`/`green`/`blue`/`various` scalars
stay writable alongside `wb.temperature` (see `get_module_schema`
above); one request that writes any of those scalars in `values` *and*
carries a `wb.temperature` entry is rejected with `invalid_value` and
nothing changes — the error details name the offending scalar as
`parameter` with `constraint: "native_conflict"`. Unknown or extra
members anywhere in the shape are `invalid_value`; an unknown semantic
ID is `unknown_field`. Quantity validation errors carry
`details: {"parameter", "component", "constraint"}` (`component` naming
the offending component, present only when the failure is attributable
to one component).

Semantic IDs are unique across the curve, vector, band, and quantity
registries within a module, so a `semantic_values` object may freely mix
entries of all four classes by name with no collision. Error precedence
within one request: parse errors (shape, class, duplicate-ID) surface
first, then engine validation in dispatch-table order — curve, then
vector, then bands, then quantity.

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

When the patch carried `semantic_values`, the result echoes a
`semantic_values` object reading back exactly the written semantic IDs
(full value shape, as in `get_module_params`), placed between `values` and
`revision`.

Errors: `unknown_module`, `unknown_instance`, `unknown_field`,
`unsupported_field` (known but not writable), `invalid_value`,
`revision_conflict`.

### Blend settings (`blend_params` capability)

Mask Tier 1 (M-A): per-instance blend controls — opacity, blend mode,
blending colorspace, fulcrum, feathering, blur, contrast, brightness,
details, and the off/uniform mask mode — ride the existing methods as an
additive wire surface gated on the `blend_params` hello capability.

**Schema member.** `get_module_schema` accepts an optional `instance`
argument (default 0). Unlike the rest of the schema (computed from the
loaded module .so alone), the `blend` member is **live-session data**:
it appears only when darktable is in darkroom with an image open and
that instance exists and supports blending — otherwise the member is
simply omitted, never an error. Its contents are computed against the
live instance: `mask_mode` (writable vocabulary `["off", "uniform"]` at
the Tier-1 base; when `parametric_mask_params` is advertised the
vocabulary becomes state-aware and gains `"parametric"` — see Parametric
masks below), plus `writable`/`current_extra_bits` reflecting whether a
drawn/parametric/raster configuration is present), `colorspace` (the
per-module choice set, `default`, and writability), `mode` (the writable
mode set for the instance's **effective** colorspace, in GUI order),
`reverse`, `feathering_guide` (the four C enumerator names), and the
seven float fields with hard `range` (fulcrum −18..18 EV, opacity 0–100
%, feathering_radius 0–250 px, blur_radius 0–100 px,
contrast/brightness/details −1..1), `soft_range` where the GUI has one
(fulcrum −3..3), `unit`, and `writable` (`details` is writable only for
raw images).

**Read.** `get_module_params` attaches a `blend` member with the
complete current blend state. Enum values are C enumerator names
(`"DEVELOP_BLEND_NORMAL2"`, `"DEVELOP_BLEND_CS_RGB_SCENE"`,
`"DEVELOP_MASK_GUIDE_IN_AFTER_BLUR"`) — except `mask_mode`, which uses
the transition-appendix vocabulary (mask-support design candidates,
appendix): at the Tier-1 base the writable states are `"off"`/`"uniform"`
and the read-only compounds are `"parametric"`, `"drawn"`,
`"drawn+parametric"`, `"raster"`. With `parametric_mask_params`,
`"parametric"` and `"drawn+parametric"` become writable per the
state-aware transition matrix documented under Parametric masks. A stored
colorspace equal to the module's default reads back as
`"DEVELOP_BLEND_CS_NONE"` ("not explicitly chosen");
`effective_colorspace` always carries the resolved space.

**Patch.** `set_module_params` accepts a sibling `blend` object next to
`values`. A blend-only patch (`values: {}`) is legal. The blend patch is
atomic with the rest of the call and produces exactly **one** history
item; it never implicitly enables a disabled module. Validation order
inside the blend object: `mask_mode` → `colorspace` → `mode` →
`reverse` → `feathering_guide` → numeric fields (table order); blend
errors rank after every semantic-class error (the blend step runs after
the class-ops loop). Writing `colorspace` **deterministically resets**
`mode`, `reverse`, `fulcrum`, and the blendif parameters to the new
space's defaults; later members of the same patch then apply on top
(the GUI's history-scavenging restore is deliberately not reproduced).
At the Tier-1 base `mask_mode` is writable only to `"off"`/`"uniform"`,
and only while the stored mode has no drawn/parametric/raster bits — see
the mask_mode transition appendix for the full state machine;
`parametric_mask_params` broadens this to the state-aware matrix under
Parametric masks (the `mask_configuration_present` constraint below
correspondingly does not fire for the newly writable transitions). The
mutation result's
`blend` member reads back the **complete** post-commit blend object,
never just the touched members.

**Errors.** Only existing codes, with
`details: {"parameter": "blend.<name>", "constraint": "<slug>"}`
(`"parameter": "blend"` for the empty-object case):

| condition | code | constraint |
|---|---|---|
| unknown member `blend.<x>` | `unsupported_field` | — |
| empty blend object | `invalid_value` | `empty` |
| `mask_mode` write while stored has drawn/parametric/raster bits | `invalid_value` | `mask_configuration_present` |
| `mask_mode` value not `"off"`/`"uniform"` | `invalid_value` | `unknown_value` |
| `colorspace` write on a fixed (RAW/NONE-default) module | `invalid_value` | `colorspace_not_available` |
| `colorspace` value not in the choice set (incl. `CS_NONE`, `CS_RAW`, Lab on non-Lab) | `invalid_value` | `colorspace_not_available` |
| `colorspace` unknown string | `invalid_value` | `unknown_value` |
| `mode` unknown enumerator string | `invalid_value` | `unknown_value` |
| `mode` valid enumerator not in the projected colorspace's set (incl. deprecated) | `invalid_value` | `mode_not_available_in_colorspace` |
| `reverse` not a boolean | `invalid_value` | `wrong_type` |
| `feathering_guide` unknown string | `invalid_value` | `unknown_value` |
| float member not a JSON number | `invalid_value` | `wrong_type` |
| float non-finite | `invalid_value` | `non_finite` |
| float outside hard range | `invalid_value` | `range` |
| `details` write when image is not rawprepare-supported | `invalid_value` | `requires_raw_image` |

A `blend` member on a module without `IOP_FLAGS_SUPPORTS_BLENDING`
fails the whole request with `unsupported_field`.

### Parametric masks (`parametric_mask_params`)

Mask Tier 2 (M-B): read and write parametric ("conditional") blendif
masks — per-channel trapezoid ramps over module input/output values, with
polarity, per-slot boost, and the mask-combine setting — as additive
members inside the Tier-1 `blend` object, gated on the
`parametric_mask_params` hello capability. This capability **implies**
`blend_params` (a server never advertises it without `blend_params`); the
sidecar refuses `parametric`/`combine` members client-side without it.

**State-aware `mask_mode`.** With this capability the `mask_mode`
vocabulary and `writable` flag depend on the stored mask state, per the
authoritative transition appendix (mask-support design candidates). The
engine owns only the `ENABLED` and `CONDITIONAL` bits; the drawn (`MASK`)
bit is never toggled here, and any stored raster bit makes the row
read-only. The full non-raster transition matrix — a transition succeeds
exactly when the stored and target drawn (`MASK`) ownership agree:

| stored \ target | `off` | `uniform` | `parametric` | `drawn` | `drawn+parametric` |
|---|---|---|---|---|---|
| `off` | ✓ | ✓ | ✓ | ✗ | ✗ |
| `uniform` | ✓ | ✓ | ✓ | ✗ | ✗ |
| `parametric` | ✓ | ✓ | ✓ | ✗ | ✗ |
| `drawn` | ✗ | ✗ | ✗ | ✓ | ✓ |
| `drawn+parametric` | ✗ | ✗ | ✗ | ✓ | ✓ |

Entering or leaving drawn state (`✗` cells) fails `invalid_value`,
`constraint: "drawn_via_attach_only"` — drawn geometry is created/attached
only through the GUI (M-C). `drawn ↔ drawn+parametric` **is** legal in a
supported family. Any write on a stored raster state fails `invalid_value`,
`constraint: "raster_unsupported"`. Schema `mask_mode.values` is
state-aware: non-drawn rows expose `off`/`uniform`/`parametric`, drawn rows
expose `drawn`/`drawn+parametric`, raster rows are non-writable. Every
unowned/future stored bit is preserved verbatim across a transition.

**Effective-family / RAW / NONE.** The parametric schema, `combine`, and
the writable channel vocabulary appear only when the instance's
**effective** colorspace (Tier-1 `effective_colorspace`) is one of the
three supported families:

- Lab (`DEVELOP_BLEND_CS_LAB`): channels `L a b C h`.
- RGB display (`DEVELOP_BLEND_CS_RGB_DISPLAY`): `g R G B H S l` (the HSL
  value channel is wire name `l`, lowercase, to disambiguate from Lab `L`).
- RGB scene (`DEVELOP_BLEND_CS_RGB_SCENE`): `g R G B Jz Cz hz`.

RAW blending (`DEVELOP_BLEND_CS_RAW`) and `DEVELOP_BLEND_CS_NONE` have **no
parametric channel semantics**: the schema omits `parametric`/`combine`,
and such patches fail `unsupported_field`. (RAW does have real CPU/OpenCL
mask kernels, but they intentionally ignore the blendif channel
arrays/`CONDITIONAL`, and the GUI marks RAW blendif unsupported — so there
is simply no channel table, not a missing kernel.) Family availability is
checked against the **projected** family after the same-call colorspace
stage, so a RAW → RGB switch plus `mask_mode: "parametric"` in one patch is
legal. A pre-existing legacy `CONDITIONAL` bit in a RAW/NONE instance stays
representable as the current `mask_mode` and may be left or removed, but no
patch may newly introduce that unsupported state, and no `parametric`/
`combine` members are ever exposed there.

**Channels: markers, enable, polarity, boost.** Each channel names an
`_in` and an `_out` slot (e.g. `Jz_in`, `L_out`). A slot's condition is a
4-tuple `markers: [m0, m1, m2, m3]` in a `[0.0, 1.0]` stored domain,
ascending `m0 ≤ m1 ≤ m2 ≤ m3`. The mask factor ramps 0→1 over `m0..m1`, is
1 over `m1..m2`, falls 1→0 over `m2..m3`. **Open ends:** `m0,m1 ≤ 0` is
unbounded low, `m2,m3 ≥ 1` unbounded high; degenerate equal markers are a
legal hard edge.

- **Derived-enable rule (single source of truth):** a slot is *disabled*
  iff its markers are the full-span identity (`m1 == 0.0 && m2 == 1.0`,
  i.e. `[0,0,1,1]`), *enabled* otherwise — exactly the GUI rule. Enable is
  never sent on the wire; it is derived on every patch, and only enabled
  in-family slots appear in reads. Sending full-span markers is a legal
  "no condition on this channel", not an error.
- **Polarity (`inverted`)** is the stored polarity bit (`blendif` bits
  16–31), carried verbatim on the wire and never resolved server-side. The
  pixel kernels compute effective inversion as
  `invert_mask = (blendif >> 16) XOR (mask_combine inclusive ? family_mask : 0)`
  (`data/kernels/blendop.cl:204`; C twins in `src/develop/blends/`), so the
  documented client rule is
  `effective_inverted = inverted XOR (combine is inclusive)`. Legacy bit 31
  (`DEVELOP_BLENDIF_active`) is never set and always stripped.
- **Boost** is a per-slot log2 exponent applied as
  `(stored − marker_offset) × 2^boost`, where `marker_offset = 0.5` for Lab
  `a`/`b` and `0` elsewhere. The **boost value** itself carries a per-channel
  storage offset (`boost.offset`): `−6.64385619` for `Jz`/`Cz`, `0` for all
  other channels — this is a *separate* concept from the Lab a/b
  `marker_offset`. Stored boost range is the displayed `0..18` EV shifted by
  that offset: `[0.0, 18.0]` for offset-0 channels, `[-6.64385619,
  11.35614381]` for `Jz`/`Cz`. Channels without boost (`h`, `H`, `S`, `l`,
  `hz`) carry `"boost": null` in the schema and reject any `boost` write.
  **Boost never rescales markers on the wire** — the GUI's
  threshold-preserving marker rescale is deliberately not reproduced; each
  member is written verbatim, so a client changing boost must rescale its
  own markers if it wants GUI-equivalent behavior.

Schema `parametric.channels` also carries non-normative `display_hint`
metadata (`factor`, `offset`, `unit`, `boost_scales`) for clients that want
GUI-style numbers via `display ≈ (stored − marker_offset) × 2^boost ×
factor`; the wire itself always carries stored `0..1` values.

**Schema (inside the `blend` schema object):**

```json
"combine": { "type": "enum",
             "values": ["exclusive", "inclusive",
                        "exclusive_inverted", "inclusive_inverted"],
             "writable": true },
"allow_inverted_combine": { "type": "bool", "writable": true,
                            "write_only": true },
"parametric": {
  "channels": {
    "Jz_in": { "markers_domain": [0.0, 1.0],
               "boost": { "writable": true, "offset": -6.64385619,
                          "range": [-6.64385619, 11.35614381] },
               "display_hint": { "factor": 100.0, "offset": 0.0,
                                 "unit": "%", "boost_scales": true } },
    "hz_in": { "markers_domain": [0.0, 1.0], "boost": null,
               "display_hint": { "factor": 360.0, "offset": 0.0,
                                 "unit": "°", "boost_scales": false } }
  }
}
```

**Read (inside the `blend` object):**

```json
"combine": "exclusive",
"parametric": {
  "L_in":  { "markers": [0.0, 0.0, 0.55, 0.65], "inverted": false, "boost": 0.0 },
  "Cz_out": { "markers": [0.1, 0.2, 1.0, 1.0], "inverted": true, "boost": -6.64385619 }
}
```

Only **enabled in-family** slots appear (an empty `parametric` object means
"parametric mode on, no channel conditions yet"). Slots enabled in storage
but outside the effective family (legacy edits) are never listed and never
touched by patches; a boolean `"foreign_channels": true` is surfaced when
any exist. Reserved slot 15 and legacy bit 31 are not channels and never
set that flag. **Defensive serialization:** a non-finite legacy marker or
boost value in storage serializes as JSON `null` rather than a NaN/Inf
literal, keeping reads strict-JSON.

**`combine` mapping** (wire enum ↔ storage `INV`/`INCL` low bits; the
`DEVELOP_COMBINE_MASKS_POS` drawn bit is preserved untouched): `exclusive`
= `NORM_EXCL` (`0x00`), `inclusive` = `NORM_INCL` (`0x02`),
`exclusive_inverted` = `INV_EXCL` (`0x01`), `inclusive_inverted` =
`INV_INCL` (`0x03`).

**Patch (inside the `blend` patch object):**

```json
"blend": {
  "mask_mode": "parametric",
  "combine": "exclusive",
  "parametric": {
    "Jz_in": { "markers": [0.0, 0.0, 0.5, 0.6] },
    "h_in":  null
  }
}
```

A channel entry **replaces that slot entirely**: `markers` required,
`inverted` optional (default `false`), `boost` optional (default = the
channel's `boost.offset`, i.e. GUI zero). No per-member merge within a
channel. `null` resets a slot: identity markers `[0,0,1,1]`, polarity bit
cleared, boost reset to the channel offset — which also disables it by the
derived rule. Unlisted slots are untouched. Writing `parametric` requires
the *projected* `mask_mode` to include the parametric bit (set it in the
same call or beforehand); `combine` is writable whenever the schema shows
it. When `colorspace` and `parametric` appear in one patch, `colorspace`
applies first (Tier-1 reset wipes the blendif block), then `parametric`
members are validated against and applied on top of the new family.

Validation order inside `blend`: `mask_mode` → `colorspace` → strict
`allow_inverted_combine` → `combine` → `parametric` (per channel: name →
allowed members → markers → inverted → boost) → the Tier-1 mode/reverse/
feathering/numeric stages. Blend keeps its Tier-1 position at the end of the
class error precedence.

**Conflict override.** Because a `combine` change flips every channel's
effective polarity, a patch that both changes `combine` (to a value
differing from stored) **and** sets any explicit `inverted` inside
`parametric` is refused `invalid_value`,
`constraint: "inverted_and_combine_conflict"`, unless the same `blend`
patch also carries `"allow_inverted_combine": true` (a write-only
confirmation member; the engine is authoritative).

**Errors** (in addition to the Tier-1 blend rows;
`details: {"parameter": "blend.<path>", "constraint": "<slug>"}`, with
`"constraint"` only where a slug is listed):

| condition | code | constraint |
|---|---|---|
| `parametric`/`combine` sent without `parametric_mask_params` | sidecar client-side refusal | — |
| `combine` on a family without parametric support (RAW/NONE) | `unsupported_field` | — (`parameter: blend.combine`) |
| `combine` value not one of the four | `invalid_value` | `unknown_value` |
| `allow_inverted_combine` not a boolean | `invalid_value` | `wrong_type` |
| `parametric` on a family without parametric support (RAW/NONE) | `unsupported_field` | — (`parameter: blend.parametric`) |
| `parametric` while projected `mask_mode` lacks the parametric bit | `invalid_value` | `requires_parametric_mask_mode` |
| `parametric` not an object | `invalid_value` | `wrong_type` |
| channel name not an `_in`/`_out` slot of the effective family | `unsupported_field` | — (`parameter: blend.parametric.<name>`) |
| channel entry neither object nor `null` | `invalid_value` | `wrong_type` |
| unknown member inside a channel entry | `unsupported_field` | — (`parameter: blend.parametric.<name>.<member>`) |
| `markers` missing, or not an array of exactly 4 numbers | `invalid_value` | `markers` |
| markers non-finite, not ascending, or outside `[0,1]` | `invalid_value` | `markers` |
| `inverted` not a boolean | `invalid_value` | `wrong_type` |
| `inverted` set together with a `combine` change, no override | `invalid_value` | `inverted_and_combine_conflict` |
| `boost` on a channel with `boost: null` | `invalid_value` | `boost` |
| `boost` not a number, non-finite, or out of the channel's range | `invalid_value` | `boost` |
| `mask_mode` transition that changes the drawn (`MASK`) bit | `invalid_value` | `drawn_via_attach_only` |
| any `mask_mode` write on a stored raster-bit state | `invalid_value` | `raster_unsupported` |
| `mask_mode` value not recognized | `invalid_value` | `unknown_value` |

### Mask render (`mask_render`)

Mask Tier 2 (M-B): a read-only rendering of the blend mask a module would
apply, so an agent can *see* the mask it configured. Gated on the
independent `mask_render` hello capability. `render_preview` gains an
optional `show_mask` argument:

```json
{ "max_px": 1024, "quality": 90,
  "show_mask": { "op": "exposure", "instance": 1 } }
```

`show_mask` is a **strict** object: exactly `{op, instance}` (`instance`
optional, default 0). A bare boolean, empty `op`, unknown key, wrong member
type, or an out-of-range `instance` is `invalid_value`. The target module
is rendered with its per-pipe display-mask opt-in through the same bounded
export path as a normal preview, so the mask lines up 1:1 with the ordinary
preview for side-by-side reasoning. The response is a grayscale JPEG where
**white = full effect**, plus a `mask_of` object:

```json
{
  "mime_type": "image/jpeg",
  "width": 768, "height": 512,
  "revision": 35,
  "mask_of": { "op": "exposure", "instance": 1, "mask_mode": "parametric" },
  "data": "...base64..."
}
```

- **Disabled-target inspection.** The target's blend piece is temporarily
  enabled *inside the throwaway export pipe only* even when the live module
  is disabled — no live GUI flag is changed or restored — so a mask can be
  inspected without enabling the module in the session.
- **`off`/`uniform` → white.** For a stored `mask_mode` with no spatial-mask
  bit (`CONDITIONAL`/`MASK`/`RASTER`), the blend path emits no display mask;
  the engine renders normally for correct framing, then fills the buffer
  white before encoding (full effect everywhere). `mask_of.mask_mode`
  reports the *true* stored mode string (e.g. `"off"`, `"uniform"`).
- **Non-blending target.** A `show_mask.op` on a module without
  `IOP_FLAGS_SUPPORTS_BLENDING` fails `unsupported_field`,
  `{"parameter": "show_mask.op"}`; a missing target fails the export rather
  than returning an ordinary image mislabeled as a mask.
- Other errors are the ordinary `render_preview` set (`preview_failed`,
  `request_too_large`).

At the MCP boundary the tool returns `show_mask` results as **mixed
content**: a JSON text block carrying `mime_type`, dimensions, `revision`,
and `mask_of`, immediately followed by the native JPEG image block (the
same idiom as `get_scopes`). Ordinary previews remain image-only.

### Drawn masks (`mask_shapes` capability)

`mask_shapes` gates drawn-mask management. It is distinct from the hidden
`mask_manager` *module*: that module remains unavailable through
`list_modules` and `set_module_params`; these methods are the supported
drawn-mask API. All five methods run on the GTK main thread. Mutations use
the ordinary `expected_revision` compare-and-swap contract.

**Coordinate contract.** Editable geometry is preview-normalized; raw wire
`create_mask_shape` and `update_mask_shape` requests must carry top-level
`"space": "preview"` (the sidecar supplies that default). The server
back-transforms geometry through the current preview pipe before storing it.
Every listed non-group shape includes top-level `space` and `editable`; only
editable entries include preview `geometry` and verbatim stored
`raw_geometry`. Points map exactly. Scalar radii and borders are fractions
of the shorter rendered-image edge and are sampled with four isotropic pixel
probe arms; if their transformed-distance spread exceeds **1%**,
`geometry.size_mapping` is `"approximate"`, otherwise it is `"exact"`. The
angle probe arm is an isotropic pixel offset of **0.05 times the shorter
image edge**, so neither size nor angle is distorted merely by a non-square
image's normalized-coordinate aspect ratio.

Create and update require every member in the matching row and reject unknown
members. All numeric members must be finite and float-representable.

| type | required `geometry` members |
|---|---|
| `circle` | `center: [x,y]`, `radius`, `border` |
| `ellipse` | `center: [x,y]`, `radius: [a,b]`, `rotation`, `border`, `border_mode: "equidistant"` or `"proportional"` |
| `gradient` | `anchor: [x,y]`, `rotation`, `compression`, `steepness`, `curvature` |

Preview `center`/`anchor` coordinates may lie in `[-0.5, 1.5]`. Circle and
ellipse radii must be positive and borders nonnegative on input; after
conversion, stored raw radii and borders must lie in `[0.0005, 1.0]`
(`proportional` ellipse border is already stored-scale). Gradient
`compression` is in `(0, 1.0]`, `curvature` in `[-2, 2]`, and rotations and
`steepness` are otherwise finite.

Only circle, ellipse, and gradient are editable. Every non-group form is
listed; path, brush, clone, and other deferred forms have `editable: false`,
retain `id`, `type`, `name`, and transitive `used_by`, but have no `geometry`
or `raw_geometry`. Group forms are engine-managed and never listed. Deferred
forms may still be attached. For each module, `used_by` reports the first
depth-first membership edge's state, inversion, and opacity.

**Pipe freshness.** Coordinate calls proceed immediately only when
`dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID`. Otherwise the shared
`dt_remote_transform_ensure_fresh` helper enqueues one preview reprocess and
polls every 5 ms for `pixelpipe_synchronization_timeout` iterations; only a
non-positive configuration uses the 2000-iteration fallback. `INVALID` or a
timeout returns retryable `retry_later`. This polling occurs on the GTK main
thread: the first coordinate call after a distortion-changing edit can stall
a co-located user's UI until the preview pipe reprocesses.

**GUI edit-session guard.** Before update or delete, before creation into an
existing group, and before an attachment mutation, the engine cancels a live
GUI edit targeting the form or affected group. If a user is dragging a form
that is remotely deleted, the drag is discarded; the next motion sees no
visible form and completes without a use-after-free or crash.

#### `list_mask_shapes`

No params. Returns `{ "shapes": [<shape entries>], "revision": <uint> }`.
It is read-only; it may need the freshness wait described above.

#### `create_mask_shape`

```json
{ "type": "circle", "space": "preview",
  "geometry": { "center": [0.62, 0.41], "radius": 0.10, "border": 0.03 },
  "name": null, "attach": { "op": "exposure", "instance": 1 },
  "expected_revision": 41 }
```

`attach` is optional. Geometry is complete for its type; omitted members are
`invalid_value` / `missing_member`. Returns `{ "shape": <entry>,
"revision": <uint> }`. An unattached creation records one forced-new global
masks-history item. Creation with `attach` records two: the unattached form,
then membership/mask-mode. It preserves the target module's enabled state.

#### `update_mask_shape`

```json
{ "id": 12, "space": "preview",
  "geometry": { "center": [0.60, 0.45], "radius": 0.12, "border": 0.03 },
  "name": "subject", "expected_revision": 42 }
```

Replaces the complete editable geometry (and optionally renames) without
changing type. A gradient creation defaults its internal transition mode to
sigmoidal; because that mode is not part of wire geometry, updates preserve
the existing linear or sigmoidal mode. Returns `{ "shape": <entry>,
"affects_instances": <int>, "revision": <uint> }`; shared-shape edits affect
every listed user. It records one forced-new global masks-history item.

#### `delete_mask_shape`

```json
{ "id": 12, "expected_revision": 43 }
```

Deletes a non-group shape transitively from all groups and returns
`{ "removed_from": [<module refs>], "revision": <uint> }`. It records one
final global masks-history item plus one forced-new module item for every
unique module whose base group is retired by the cascade. Removed forms are
retired to `dev->allforms`, preserving replay-safe ownership.

#### `set_mask_attachment`

```json
{ "op": "exposure", "instance": 1, "shape_id": 12, "attached": true,
  "state": "union", "inverted": false, "opacity": 0.8,
  "expected_revision": 44 }
```

With `attached: true`, this creates or updates the membership and returns
`{ "shape": <entry>, "revision": <uint> }`. With `attached: false`, it
removes only the direct membership; the shape remains available to other
modules. The allowed attachment states are `union`, `intersection`,
`difference`, and `exclusion`; `sum` is reserved for a future brush-specific
wire operation and unsupported in v1. The first member has no combine
operation, so supplied `state` is accepted but ignored there, while
`inverted` and `opacity` still apply. Opacity must be finite and is clamped to
`[0,1]`. Each state-changing attachment call records one module history item.
An idempotent detach with no direct edge records zero items and leaves the
revision unchanged.

One undo call reverts one history item. Consequently callers need N undo
calls to revert an N-item operation; this is the accepted undo-granularity
wart rather than hidden batching.

| condition | code | details |
|---|---|---|
| missing shape on update/delete | `not_found` | `parameter: "id"` |
| missing shape on attachment | `not_found` | `parameter: "shape_id"` |
| missing module or instance | `unknown_module` / `unknown_instance` | `op` / `instance` |
| non-editable or group mutation | `unsupported_field` | relevant parameter |
| unsupported raster attachment target | `invalid_value` | `parameter: "op"`, `constraint: "raster_unsupported"` |
| `state: "sum"` (reserved, unsupported in v1) | `invalid_value` | `constraint: "sum_is_brush_only"` |
| invalid or timed-out preview pipe | `retry_later` | retryable |
| revision mismatch | `revision_conflict` | existing CAS details |

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

Request params: `{"max_px": 1024?, "quality": 85?, "show_mask"?}`. `max_px`
is clamped to [64, 2048]; `quality` to [50, 95]. Renders the active image
through the normal pixelpipe with current history; asynchronous
server-side. The optional `show_mask` argument (capability `mask_render`)
renders a module's blend mask instead of the image — see Mask render above;
it adds `unsupported_field` to this method's error set.

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

| method | not_in_darkroom | no_image_open | unknown_module | unknown_instance | unknown_field | unsupported_field | invalid_value | instance_not_supported | revision_conflict | preview_failed | scope_failed | not_found | retry_later |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `hello` | | | | | | | ✓ | | | | | | |
| `get_state` | | | | | | | | | | | | | |
| `list_modules` | ✓ | ✓ | | | | | | | | | | | |
| `get_module_schema` | | | ✓ | | | | ✓ | | | | | | |
| `get_module_params` | ✓ | ✓ | ✓ | ✓ | | | ✓ | | | | | | |
| `set_module_params` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | ✓ | | | | |
| `set_module_enabled` | ✓ | ✓ | ✓ | ✓ | | | ✓ | | ✓ | | | | |
| `reset_module` | ✓ | ✓ | ✓ | ✓ | | | ✓ | | ✓ | | | | |
| `create_module_instance` | ✓ | ✓ | ✓ | ✓ | | | ✓ | ✓ | ✓ | | | | |
| `get_history` | ✓ | ✓ | | | | | ✓ | | | | | | |
| `undo` | ✓ | ✓ | | | | | ✓ | | ✓ | | | | |
| `render_preview` | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | | | ✓ | | | |
| `compute_scopes` | ✓ | ✓ | | | | | ✓ | | | | ✓ | | |
| `list_mask_shapes` | ✓ | ✓ | | | | | ✓ | | | | | | ✓ |
| `create_mask_shape` | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | | ✓ | ✓ | | | ✓ |
| `update_mask_shape` | ✓ | ✓ | | | | ✓ | ✓ | | ✓ | ✓ | | ✓ | ✓ |
| `delete_mask_shape` | ✓ | ✓ | | | | ✓ | ✓ | | ✓ | | | ✓ | |
| `set_mask_attachment` | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | | ✓ | | | ✓ | ✓ |

`retryable` is `true` for `revision_conflict`, `busy`, transient
`preview_failed`/`scope_failed`, and `retry_later`; `false` otherwise.

## Resolved decisions recorded here

1. **Undo contract (plan decision 1):** compare-and-undo with a required
   `expected_revision`; no `steps` in v1.
2. **Unsupported schema fields (plan decision 3):** always included with
   `writable: false`; no `include_unsupported` flag.
3. **Capability-gated request members (milestone 2):** an optional request
   member that a client only sends when the server advertises the matching
   capability (e.g. `semantic_values` under `curve_params`) is an additive
   change and does not bump `protocol_version`. Strict-params rejection of
   the member on a server without the capability is exactly the safety this
   relies on: a mixed patch can fail loudly, never half-apply.

## Maintenance

Additive, optional response fields — and optional *request* members gated by
a hello capability (decision 3 above) — may be introduced without a version
bump, per the design spec's compatibility policy. Anything that changes a
field's meaning, requiredness, or type bumps `protocol_version`. Every
change here must land with matching updates to the C dispatcher validation
and the Python fixtures in the same commit. New curve-bearing ops are
registry entries in `src/control/remote_curve_registry.c` and require no
protocol change — the wire shape documented above already generalizes. New
vector-bearing ops are likewise registry entries in
`src/control/remote_vector_registry.c` (milestone 4), and new
quantity-bearing ops are registry entries in
`src/control/remote_quantity_registry.c` plus the module's own
`remote_quantity_read`/`remote_quantity_write` iop API hooks
(milestone 6); the registries share the `semantic_values` wire envelope
and enforce unique semantic IDs across classes within a module.
