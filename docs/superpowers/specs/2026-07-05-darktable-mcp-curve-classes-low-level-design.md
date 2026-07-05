# darktable MCP — Curve Classes Low-Level Design

Date: 2026-07-05
Status: proposed low-level design; not yet incorporated into protocol v1
Depends on: `2026-07-05-darktable-mcp-design.md`,
`2026-07-05-darktable-mcp-protocol-reference.md`
Investigation: `2026-07-05-darktable-mcp-curve-investigation.md`

## Objective

Add transport-neutral semantic curve discovery, reading, validation, and
atomic mutation without exposing native IOP arrays or requiring public params
struct definitions.

The first implementation supports variable-length control-point curves. It
starts with `rgbcurve`, then adds `tonecurve`, `colorzones`, and `basecurve`.
Fixed sampled/banded responses reuse the semantic-parameter infrastructure but
remain a separate class and later milestone.

## Decisions

1. **Semantic overlay, not generic array writes.** Generated introspection
   remains the storage source of truth. A static registry groups native fields
   into semantic parameters and supplies metadata introspection cannot infer.
2. **No duplicated params structs.** Curve code accesses native fields through
   the introspection tree and internal descriptor paths. It never copies IOP
   struct declarations into `src/control`.
3. **No IOP ABI extension for the first implementation.** Most adapters are
   declarative descriptors. Optional registry callbacks handle module-specific
   transitions or invariants using introspection-backed accessors.
4. **Whole-value replacement first.** A write replaces one or more complete
   semantic curves. Point-level CRUD is deferred.
5. **One common transaction.** Scalar and semantic patches can be validated
   against one temporary params block and committed as one history item.
6. **Stored coordinates on the wire.** GUI log/semilog/zoom transforms are not
   part of curve values.
7. **Control-point curve, sampled response, and levels remain distinct
   classes.** They may share primitives and transaction code.
8. **Native compatibility fields are adapter-owned.** Callers cannot write
   counts, spline versions, or interpolation integers directly.

## Non-goals

- Picker-generated curves
- Brush/gesture replay for sampled responses
- Generic arbitrary indexed introspection paths
- Editing deprecated curve modules in the first rollout
- Preserving GUI selection, zoom, or focused channel
- Defining photographic presentation units
- Cross-module curve copy/paste compatibility
- Replacing darktable's native params/history representation

## Architecture

```text
private protocol / MCP request
          │
          ▼
neutral scalar + semantic patch
          │
          ▼
remote-edit transaction
  ├── resolve live module and revision
  ├── copy complete params block
  ├── apply scalar patch to copy
  ├── resolve semantic descriptor
  ├── validate/apply curve through adapter
  ├── validate completed params block
  └── commit once + history + GUI/pixelpipe update
          │
          ▼
curve registry + introspection cursor
  ├── static semantic metadata
  ├── native field/index mapping
  ├── conditional predicates
  └── optional module-specific callbacks
          │
          ▼
generated IOP introspection + params copy
```

`remote_protocol` owns JSON. `remote_curve` and `remote_edit` use neutral C
types and never include socket, JSON, or MCP concerns.

## Proposed files

```text
src/control/remote_parameters.h
src/control/remote_parameters.c
src/control/remote_curve.h
src/control/remote_curve.c
src/control/remote_curve_registry.c
src/tests/unittests/control/test_remote_curve.c
src/tests/unittests/control/test_remote_curve_registry.c
```

Modify:

```text
src/control/remote_edit.h
src/control/remote_edit.c
src/control/remote_protocol.c
src/CMakeLists.txt
src/tests/unittests/CMakeLists.txt
tools/mcp/src/darktable_mcp/server.py
tools/mcp/tests/test_protocol.py
tools/mcp/tests/test_tools.py
```

The exact split between `remote_parameters.c` and `remote_curve.c` may be
collapsed initially, but the public neutral types and registry must remain
independent from JSON serialization.

## Neutral semantic types

### Common class and status

```c
typedef enum dt_remote_parameter_class_t
{
  DT_REMOTE_PARAMETER_CURVE = 1,
  DT_REMOTE_PARAMETER_SAMPLED_RESPONSE,
  DT_REMOTE_PARAMETER_LEVELS
} dt_remote_parameter_class_t;

typedef enum dt_remote_writability_t
{
  DT_REMOTE_WRITABLE_NEVER = 0,
  DT_REMOTE_WRITABLE_NOW,
  DT_REMOTE_WRITABLE_CONDITIONAL
} dt_remote_writability_t;
```

The class enum is internal and extensible. Serialized class names are stable
strings and do not expose these integer values.

### Curve schema

```c
typedef enum dt_remote_curve_interpolation_t
{
  DT_REMOTE_CURVE_CUBIC_SPLINE = 0,
  DT_REMOTE_CURVE_CATMULL_ROM,
  DT_REMOTE_CURVE_MONOTONE_HERMITE
} dt_remote_curve_interpolation_t;

typedef enum dt_remote_curve_endpoint_policy_t
{
  DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL = 0,
  DT_REMOTE_CURVE_BOUNDARY_POINTS_REQUIRED,
  DT_REMOTE_CURVE_BOUNDARY_POINTS_FIXED_IDENTITY
} dt_remote_curve_endpoint_policy_t;

typedef enum dt_remote_spacing_rule_t
{
  DT_REMOTE_SPACING_NONE = 0,
  DT_REMOTE_SPACING_AT_LEAST,
  DT_REMOTE_SPACING_GREATER_THAN
} dt_remote_spacing_rule_t;

typedef enum dt_remote_predicate_operator_t
{
  DT_REMOTE_PREDICATE_EQ = 0,
  DT_REMOTE_PREDICATE_NE
} dt_remote_predicate_operator_t;

typedef struct dt_remote_parameter_condition_t
{
  char *field;                // owned stable primitive field name
  dt_remote_predicate_operator_t op;
  char *enum_name;            // owned stable enum name
} dt_remote_parameter_condition_t;

typedef struct dt_remote_curve_axis_t
{
  double minimum;
  double maximum;
  char *unit;                 // owned, nullable; v1 normally "normalized"
} dt_remote_curve_axis_t;

typedef struct dt_remote_curve_schema_t
{
  char *name;                // stable semantic ID, for example "curve.master"
  char *display_name;        // translated presentation label
  char *description;         // translated, nullable
  dt_remote_curve_axis_t x;
  dt_remote_curve_axis_t y;
  guint minimum_points;
  guint maximum_points;
  double minimum_x_spacing;
  dt_remote_spacing_rule_t adjacent_spacing_rule;
  double minimum_wrap_spacing;
  dt_remote_spacing_rule_t wrap_spacing_rule;
  gboolean strict_x_order;
  gboolean periodic_x;
  dt_remote_curve_endpoint_policy_t boundary_point_policy;
  guint interpolation_mask;
  dt_remote_curve_interpolation_t default_interpolation;
  dt_remote_writability_t writability;
  dt_remote_parameter_condition_t *active_when;   // owned, nullable
  dt_remote_parameter_condition_t *writable_when; // owned, nullable
  dt_remote_parameter_condition_t *periodic_when; // owned, nullable
} dt_remote_curve_schema_t;
```

Returned schemas own every string and use paired destroy functions. Registry
descriptors use static strings; conversion copies them into response objects.

`periodic_x` means unconditionally periodic. `periodic_when` represents the
conditional case. Conditions are structured field/operator/enum triples, not
free-form expressions. A value read from a live instance carries the resolved
Boolean.

### Curve value

```c
typedef struct dt_remote_curve_point_t
{
  double x;
  double y;
} dt_remote_curve_point_t;

typedef struct dt_remote_curve_value_t
{
  char *name;                // owned semantic ID
  GArray *points;            // dt_remote_curve_point_t, owned
  dt_remote_curve_interpolation_t interpolation;
  gboolean active;
  gboolean effective;
  gboolean writable_now;
  gboolean periodic_x;
} dt_remote_curve_value_t;
```

`active` means the semantic curve exists in the current mode. `effective`
means processing currently consumes it. `writable_now` is resolved against
the projected params block and may differ from the per-op schema's general
capability.

### Semantic patch

```c
typedef struct dt_remote_curve_patch_t
{
  char *name;
  GArray *points;
  gboolean has_interpolation;
  dt_remote_curve_interpolation_t interpolation;
} dt_remote_curve_patch_t;

typedef struct dt_remote_semantic_patch_t
{
  dt_remote_parameter_class_t class_id;
  union
  {
    dt_remote_curve_patch_t curve;
  } value;
} dt_remote_semantic_patch_t;
```

The request parser owns and frees patches. It rejects unknown members,
duplicate semantic IDs, oversized point lists, and non-numeric/non-finite JSON
before invoking `remote_edit`.

Extend the neutral module patch:

```c
typedef struct dt_remote_patch_t
{
  GPtrArray *scalar_values;   // existing primitive entries
  GPtrArray *semantic_values; // dt_remote_semantic_patch_t
  gboolean has_enable;
  gboolean enable;
} dt_remote_patch_t;
```

At least one scalar value, semantic value, or explicit enable change is
required.

## Registry descriptors

Registry descriptors are static and never serialized directly.

### Introspection path

```c
typedef enum dt_remote_path_segment_type_t
{
  DT_REMOTE_PATH_FIELD,
  DT_REMOTE_PATH_INDEX
} dt_remote_path_segment_type_t;

typedef struct dt_remote_path_segment_t
{
  dt_remote_path_segment_type_t type;
  union
  {
    const char *field;
    guint index;
  } value;
} dt_remote_path_segment_t;

typedef struct dt_remote_introspection_path_t
{
  const dt_remote_path_segment_t *segments;
  guint length;
} dt_remote_introspection_path_t;
```

Paths are compiled static data and cannot originate from a remote request.
They resolve through `dt_introspection_get_child()` and
`dt_introspection_access_array()`, checking type and bounds at every segment.

The resolver returns both the leaf `dt_introspection_field_t *` and a pointer
inside the supplied params block. No pointer is retained after the operation.

### Native control-point layout

```c
typedef struct dt_remote_native_curve_layout_t
{
  dt_remote_introspection_path_t nodes; // fixed-capacity node array
  dt_remote_introspection_path_t count; // active point count leaf
  dt_remote_introspection_path_t type;  // interpolation leaf
  const char *x_field;                  // node member, normally "x"
  const char *y_field;                  // node member, normally "y"
  dt_remote_introspection_path_t internal_version; // length 0 if absent
  gint64 internal_version_value;
} dt_remote_native_curve_layout_t;
```

The generic adapter resolves:

```text
nodes[point].x_field
nodes[point].y_field
count
type
```

The static path for each semantic descriptor contains any native channel
index. The only runtime index is the point loop, bounded by the validated
request count and native array capacity. The adapter verifies that `nodes` is
an array of structs, x/y are supported floating fields, and count/type leaves
are integer or enum compatible. A registry mismatch is an internal error and
disables the semantic descriptor; it must never fall back to guessed offsets.

### Predicate

```c
typedef struct dt_remote_parameter_predicate_t
{
  const char *field;
  dt_remote_predicate_operator_t op;
  const char *enum_name;
} dt_remote_parameter_predicate_t;
```

Initial predicates compare one primitive enum field with one stable enum name.
This is sufficient for `rgbcurve` and `tonecurve`. Predicates resolve enum
names through introspection; descriptors do not hard-code native integers.

Do not build a general expression language. More complex state uses an
adapter callback.

### Semantic descriptor and module adapter

```c
typedef struct dt_remote_curve_axis_descriptor_t
{
  double minimum;
  double maximum;
  const char *unit;
} dt_remote_curve_axis_descriptor_t;

typedef struct dt_remote_curve_descriptor_t
{
  const char *name;
  const char *display_name_msgid;
  const char *description_msgid;
  dt_remote_native_curve_layout_t native;
  dt_remote_curve_axis_descriptor_t x;
  dt_remote_curve_axis_descriptor_t y;
  guint minimum_points;
  guint maximum_points;
  double minimum_x_spacing;
  dt_remote_spacing_rule_t adjacent_spacing_rule;
  double minimum_wrap_spacing;
  dt_remote_spacing_rule_t wrap_spacing_rule;
  gboolean strict_x_order;
  dt_remote_curve_endpoint_policy_t boundary_point_policy;
  guint interpolation_mask;
  dt_remote_curve_interpolation_t default_interpolation;
  const dt_remote_parameter_predicate_t *active_when;
  const dt_remote_parameter_predicate_t *writable_when;
  const dt_remote_parameter_predicate_t *periodic_when;
} dt_remote_curve_descriptor_t;

typedef struct dt_remote_curve_module_adapter_t
{
  const char *operation;
  guint minimum_params_version;
  guint maximum_params_version;
  const dt_remote_curve_descriptor_t *curves;
  guint curve_count;
  const char *const *prepare_fields;
  guint prepare_field_count;

  gboolean (*prepare)(const struct dt_remote_curve_context_t *ctx,
                      const void *old_params,
                      void *new_params,
                      const dt_remote_patch_t *patch,
                      dt_remote_error_t **error);

  gboolean (*validate_completed)(const struct dt_remote_curve_context_t *ctx,
                                 const void *new_params,
                                 dt_remote_error_t **error);
} dt_remote_curve_module_adapter_t;
```

`prepare_fields` lists primitive fields whose changes have semantic side
effects on curve storage. `prepare` runs when a request contains a semantic
curve or changes one of those fields; it handles module-specific transitions
before curve patches are applied. `validate_completed` handles invariants not
expressible by individual curve descriptors. Both callbacks manipulate params
only through internal introspection accessors.

The first adapters should be mostly declarative. Callbacks are not permission
to duplicate GUI code casually; every callback needs focused tests against
the corresponding module behavior.

## Registry lifecycle

```c
const dt_remote_curve_module_adapter_t *
dt_remote_curve_registry_lookup(const char *operation,
                                guint params_version);

gboolean dt_remote_curve_registry_validate(
    const dt_remote_curve_module_adapter_t *adapter,
    const dt_introspection_t *introspection,
    dt_remote_error_t **error);
```

Lookup uses the stable IOP operation name. Validation occurs on first schema or
value access for a `(operation, params_version)` pair and may be cached for the
process lifetime because generated introspection is immutable after module
load.

The cache stores only validation status and static descriptors, never pointers
into a live module params block.

If no adapter exists, behavior remains unchanged: native array fields appear
unsupported and no semantic curve is advertised.

## Initial registry mapping

### `rgbcurve`

Expose four conditional semantic IDs over three native channels:

| semantic ID | native channel | active/writable condition |
|---|---:|---|
| `curve.master` | 0 | `curve_autoscale != DT_S_SCALE_MANUAL_RGB` |
| `curve.red` | 0 | `curve_autoscale == DT_S_SCALE_MANUAL_RGB` |
| `curve.green` | 1 | `curve_autoscale == DT_S_SCALE_MANUAL_RGB` |
| `curve.blue` | 2 | `curve_autoscale == DT_S_SCALE_MANUAL_RGB` |

Using separate `master` and `red` IDs prevents one semantic ID from changing
meaning when mode changes, even though they share native storage.

Common descriptor values:

- domain `[0,1] × [0,1]`;
- 2–20 points;
- strict x ordering;
- minimum x spacing 0.0025;
- optional domain-boundary points; and
- all three interpolation modes.

Here, “boundary point” means a point whose x is exactly the domain minimum or
maximum. The GUI permits the first and last control points to move away from
those boundaries. Attempting to delete one resets it to `(0,0)` or `(1,1)`,
but that deletion gesture is not a storage invariant. The two-point minimum
already ensures there are outer control points; the endpoint policy must not
require boundary or identity coordinates. Remote writes never silently
normalize existing or requested values.

The adapter declares `curve_autoscale` and `compensate_middle_grey` as prepare
fields. Its callback reproduces two non-GUI storage transitions:

- on entry to manual RGB mode, copy channel 0 into untouched identity G/B
  channels exactly as `rgbcurve.gui_changed()` does; and
- when middle-grey compensation changes, transform existing stored points
  through the current work profile before applying explicit curve patches.

An explicit semantic curve in the same request is interpreted in the
post-transition stored coordinate system and therefore overwrites the
prepared native channel. These semantics require integration tests with and
without an available work profile; the callback must not invoke GUI handlers.

### `tonecurve`

| semantic ID | native channel | active/writable condition |
|---|---:|---|
| `curve.lightness` | 0 | always |
| `curve.a` | 1 | `tonecurve_autoscale_ab == DT_S_SCALE_MANUAL` |
| `curve.b` | 2 | `tonecurve_autoscale_ab == DT_S_SCALE_MANUAL` |

Domain is stored `[0,1] × [0,1]`, with 2–20 points. The adapter preserves
channel-specific defaults and hides compatibility fields. Log/semilog GUI
presentation is irrelevant to stored values.

### `colorzones`

Expose `curve.lightness`, `curve.chroma`, and `curve.hue`, all active. Each
maps to its native output channel. `periodic_when` is:

```text
channel == DT_IOP_COLORZONES_h
```

This predicate applies to all three output curves because `channel` selects
the shared input x-axis dimension. The adapter always writes the current
`splines_version` and implements wrap-spacing validation.

### `basecurve`

Expose only `curve.master`, mapped to native channel 0. Reserved native
channels remain invisible. Domain is `[0,1] × [0,1]`, count is 2–20, and all
supported native interpolation modes are mapped through stable names.

## Curve engine API

```c
gboolean dt_remote_curve_list_schema(
    const dt_iop_module_so_t *module_so,
    GPtrArray **out, // dt_remote_curve_schema_t
    dt_remote_error_t **error);

gboolean dt_remote_curve_read_values(
    const dt_iop_module_t *module,
    const void *params,
    GHashTable **out, // name -> dt_remote_curve_value_t
    dt_remote_error_t **error);

gboolean dt_remote_curve_apply_patch(
    const dt_iop_module_t *module,
    const void *old_params,
    void *new_params,
    const dt_remote_patch_t *patch,
    dt_remote_error_t **error);
```

Schema listing is per operation/params version and returns conditional
metadata. Value reading is per live instance and resolves `active`,
`effective`, `writable_now`, and periodicity against current params.

`apply_patch` receives both old and projected params so adapters can detect
mode transitions. It assumes scalar fields have already been written to
`new_params` and applies semantic patches in deterministic registry order.

## Validation algorithm

For each curve patch:

1. Resolve semantic ID and verify class is curve.
2. Evaluate active/writable predicate against projected params.
3. Check point count against descriptor bounds and global request limits.
4. Reject NaN, infinity, or values outside x/y domains.
5. Require strict ascending x when configured.
6. Check adjacent minimum spacing using the descriptor's inclusive/exclusive
   comparison rule; `rgbcurve` requires delta strictly greater than 0.0025.
7. Check periodic spacing between the final and first point across the wrap
   using its independently declared comparison rule.
8. Enforce the domain-boundary point policy.
9. Resolve interpolation; preserve current interpolation if omitted.
10. Verify interpolation is in the descriptor allowlist.
11. Resolve native arrays/count/type through introspection.
12. Clear unused native point capacity to deterministic zero when safe for the
    module, or leave it unchanged if the adapter declares padding significant.
13. Write active points, count, interpolation, and adapter-owned version.
14. After every curve is applied, run adapter `validate_completed()`.

No validation path clamps, sorts, deduplicates, inserts boundary points, changes
interpolation, or drops points implicitly.

Errors use existing top-level codes:

- `unknown_field`: unknown semantic ID;
- `unsupported_field`: class not implemented or condition not satisfied;
- `invalid_value`: malformed points, bounds, order, spacing, boundary points, or
  interpolation; and
- `internal`: registry no longer matches generated introspection.

Error details should include `parameter`, `point_index`, `constraint`, and the
relevant bound or current condition where applicable.

## Transaction integration

Extend `dt_remote_set_module_params()` rather than creating an independent
history path:

1. validate view, image, module instance, and expected revision;
2. copy the complete live params block;
3. parse/validate primitive patch types;
4. write primitive values to the copy;
5. invoke adapter `prepare(old, copy, patch)` if a semantic curve is present
   or a declared prepare field changed;
6. validate and apply semantic values to the copy;
7. validate the completed primitive and semantic state;
8. apply explicit enable state to the same logical mutation;
9. enter normal undo/history lifecycle;
10. copy params to live state;
11. synchronize GUI once, add one history item, invalidate pixelpipe, redraw;
12. read requested primitive and semantic values back from live state; and
13. return one resulting revision.

Predicates evaluate against the projected copy after scalar writes and adapter
preparation. This allows a future request to change a link mode and replace
the newly active curves atomically.

If any curve fails, scalar changes and other curves are discarded with the
temporary block.

## Protocol extension

### Capability

Advertise a capability after handshake:

```json
"capabilities": ["params", "semantic_params", "curve_params", "history", "preview"]
```

Clients must not send semantic request members unless `semantic_params` and
the relevant class capability are present. Older strict servers therefore
continue to reject unknown request members safely.

Whether adding capability-gated optional request members requires a protocol
version bump must be resolved in the protocol reference before implementation.

### Schema response

Add an optional `semantic_fields` array to `get_module_schema`:

```json
{
  "module": "rgbcurve",
  "params_version": 1,
  "fields": [
    {
      "name": "curve_nodes",
      "type": "array",
      "writable": false,
      "represented_by": ["curve.master", "curve.red", "curve.green", "curve.blue"]
    }
  ],
  "semantic_fields": [
    {
      "name": "curve.master",
      "class": "curve",
      "display_name": "master",
      "readable": true,
      "writable": true,
      "writable_when": {
        "field": "curve_autoscale",
        "not_equals": "DT_S_SCALE_MANUAL_RGB"
      },
      "x": { "minimum": 0.0, "maximum": 1.0, "unit": "normalized" },
      "y": { "minimum": 0.0, "maximum": 1.0, "unit": "normalized" },
      "points": {
        "minimum": 2,
        "maximum": 20,
        "strict_x_order": true,
        "minimum_x_spacing": 0.0025,
        "minimum_x_spacing_comparison": "greater_than",
        "boundary_point_policy": "optional"
      },
      "interpolation_values": [
        "CUBIC_SPLINE",
        "CATMULL_ROM",
        "MONOTONE_HERMITE"
      ]
    }
  ]
}
```

`writable: true` means the server implements writes for the class in some
valid state. `writable_when` describes the static condition. Per-instance
reads resolve `writable_now`.

Native fields remain present with `writable: false`, preserving the existing
unsupported-field decision. `represented_by` is additive explanatory
metadata.

### Value response

Add optional `semantic_values` to `get_module_params`:

```json
{
  "module": "rgbcurve",
  "instance": 0,
  "revision": 31,
  "values": {
    "curve_autoscale": "DT_S_SCALE_AUTOMATIC_RGB"
  },
  "semantic_values": {
    "curve.master": {
      "class": "curve",
      "active": true,
      "effective": true,
      "writable_now": true,
      "points": [
        { "x": 0.0, "y": 0.0 },
        { "x": 1.0, "y": 1.0 }
      ],
      "interpolation": "MONOTONE_HERMITE"
    }
  }
}
```

Return active semantic values by default. If inactive values are useful for
mode transitions later, add an explicit read option rather than silently
duplicating native channel 0 as both `master` and `red`.

### Mutation request

Extend `set_module_params` with optional `semantic_values`:

```json
{
  "module": "rgbcurve",
  "instance": 0,
  "values": {},
  "semantic_values": {
    "curve.master": {
      "class": "curve",
      "points": [
        { "x": 0.0, "y": 0.0 },
        { "x": 0.25, "y": 0.18 },
        { "x": 0.75, "y": 0.82 },
        { "x": 1.0, "y": 1.0 }
      ],
      "interpolation": "MONOTONE_HERMITE"
    }
  },
  "expected_revision": 31,
  "enable": true
}
```

At least one of `values`, `semantic_values`, or `enable` must change state.
Both maps reject duplicate keys. The result includes read-back
`semantic_values` for every semantic entry written.

The Python sidecar may expose a dedicated ergonomic MCP tool such as
`set_curve` while translating it to this generic private-protocol mutation.
That tool does not create a separate darktable mutation implementation.

## Serialization rules

- JSON numbers parse as double, must be finite, and are range-checked before
  conversion to native float.
- Curve point objects require exactly `x` and `y`.
- `class` is required in mutation values to prevent future ambiguous shapes.
- `points` is required and replaces the whole curve.
- `interpolation` is optional; omission preserves the current native value.
- Unknown object members are rejected.
- Stable semantic IDs and interpolation names are ASCII protocol identifiers.
- Translated labels are response metadata only.
- Response point order is ascending native semantic order.
- Unused native capacity and guard points are never serialized.

## Threading and ownership

All live module lookup, predicate evaluation, params copying, semantic reads,
and mutation run on darktable's GUI/main context, consistent with scalar
remote edits.

Static registry descriptors are process-lifetime constants. Schema/value/patch
objects are heap-owned neutral objects with explicit destroy functions:

```c
void dt_remote_curve_schema_free(dt_remote_curve_schema_t *schema);
void dt_remote_curve_value_free(dt_remote_curve_value_t *value);
void dt_remote_semantic_patch_free(dt_remote_semantic_patch_t *patch);
```

No returned object retains:

- live params pointers;
- introspection leaf pointers whose module lifetime is uncertain;
- GUI curve helpers; or
- translated strings owned by temporary GTK widgets.

Registry validation may retain the stable module introspection pointer only if
module SO lifetime is process-wide; otherwise retain only validation state.

## GUI synchronization

After the temporary block is committed, use the existing common module GUI
update path exactly once. The adapter must not call GUI callbacks while
building the temporary block.

Integration tests must verify that GUI update:

- rebuilds internal `dt_draw_curve_t` helpers as needed;
- does not add a second history item;
- does not normalize, reorder, or overwrite committed points;
- switches conditional visibility consistently with scalar mode; and
- works when the module is not focused or its GUI is collapsed.

If a module cannot synchronize safely through the common update path, add a
transport-neutral post-commit hook to the remote mutation service. Do not call
static GUI event handlers from `remote_curve`.

## Sampled-response extension point

Reserve a separate internal value and descriptor:

```c
typedef struct dt_remote_sample_t
{
  double x;
  double y;
} dt_remote_sample_t;

typedef struct dt_remote_sampled_response_value_t
{
  char *name;
  GArray *samples;
} dt_remote_sampled_response_value_t;
```

Its descriptor fixes sample count, channel role, x writability, and any paired
response relationships. It can reuse introspection cursors, neutral patches,
transaction integration, error details, and serialization conventions.

Do not implement sampled responses by pretending every sample array is a
variable control-point curve. Their fixed counts and named frequency/channel
roles are part of the public semantics.

## Unit tests

### Introspection cursor

- Resolve nested field and array-index sequences.
- Reject wrong field type, out-of-bounds index, missing child, and type drift.
- Prove offsets target a temporary params fixture, not live storage.
- Never accept a path supplied as request data.

### Common curve validator

- Minimum/maximum count.
- Optional, required, and fixed-identity domain-boundary points.
- NaN and infinity.
- Strict ascending x.
- Duplicate x.
- Adjacent spacing.
- Periodic wrap spacing.
- Endpoint policies.
- Allowed and disallowed interpolation.
- No mutation on failure.

### Registry

- Validate every descriptor against generated/fixture introspection.
- Lookup by stable operation and params version.
- Reject descriptor/native shape mismatch.
- Hide reserved basecurve channels.
- Resolve all conditional predicates by enum name.

### `rgbcurve` adapter

- Linked mode exposes only `curve.master` as active.
- Manual mode exposes R/G/B and not master.
- Native channel 0 round-trips under the correct semantic ID.
- All interpolation names map correctly.
- Mode transition plus multi-curve patch is atomic.
- First/last points may move away from domain boundaries and round-trip
  without normalization.
- Entering manual mode copies channel 0 to identity G/B before explicit
  per-channel replacements.
- Middle-grey compensation transforms untouched channels before explicit
  curve replacements.
- Invalid green/blue write in linked mode returns `unsupported_field`.
- Unused native capacity is not returned.

### Transaction

- Scalar plus curve patch commits once.
- Any invalid scalar/curve leaves every field unchanged.
- Multiple curves produce one history item.
- Explicit enable participates in the same history item.
- Stale revision leaves state unchanged.
- Read-back equals live semantic state.

## Integration tests

For a running darktable test instance:

1. open an image in darkroom;
2. obtain `rgbcurve` schema and identity curve;
3. apply a four-point curve with expected revision;
4. verify schema/value read-back and visible preview change;
5. verify GUI points and interpolation;
6. undo once and verify exact identity restoration;
7. switch linked/manual mode and test conditional IDs;
8. repeat with CPU and OpenCL pixelpipes where available; and
9. repeat while the module GUI is collapsed/unfocused.

Later integration fixtures add `tonecurve`, periodic `colorzones`, and
reserved-channel `basecurve` behavior.

## Failure and compatibility policy

- A registry mismatch disables only semantic support for that operation and
  returns/logs an actionable internal error; primitive support remains.
- A new params version requires registry validation and tests before semantic
  writes are enabled for that version.
- Reads may be enabled before writes when a new layout is understood only
  partially.
- Native array fields remain read-only even if semantic support is disabled.
- Protocol clients discover support through capabilities and semantic schema;
  they never infer it from darktable version strings.
- Semantic IDs and interpolation names become protocol compatibility surface
  once shipped.

## Implementation order

1. Add neutral semantic patch/value ownership and destroy functions.
2. Add safe introspection cursor and fixture tests.
3. Add curve validator and serializer fixtures.
4. Add registry validation and read-only `rgbcurve` descriptor.
5. Extend schema/value responses behind `curve_params` capability.
6. Integrate semantic patches into the existing temporary-block transaction.
7. Enable `rgbcurve` writes and integration tests.
8. Add tone curve, color zones, and base curve adapters.
9. Add sampled-response neutral types and start with lowlight.

## Unresolved implementation decisions

- Confirm the failure policy for a middle-grey compensation transition when
  no work profile is available.
- Confirm exact `rgbcurve` mode-transition behavior without invoking GUI
  callbacks on temporary params.
- Decide whether capability-gated optional request fields require a protocol
  version bump.
- Decide whether inactive semantic values need an explicit read option.
- Decide whether internal descriptors live centrally or are generated from a
  small declarative source file.
- Determine whether any IOP ultimately requires a transport-neutral module
  callback instead of introspection-based central logic.
- Define a common post-commit hook only if normal GUI synchronization proves
  insufficient.
- Decide whether first MCP exposure is a generic semantic patch tool or an
  ergonomic `set_curve` wrapper.

## Acceptance gate

The curve class is ready for broader module adoption when:

- `rgbcurve` schema and values round-trip without native-layout leakage;
- invalid points cannot mutate live state;
- linked/manual conditions are enforced against projected params;
- scalar, curve, enable, revision, history, GUI, and pixelpipe behavior form
  one atomic mutation;
- one undo restores the exact prior curve and mode;
- registry drift fails closed; and
- the sidecar can expose the capability without breaking primitive-only
  clients or servers.
