# darktable MCP — Curve Parameter Investigation

Date: 2026-07-05
Status: investigation; input to semantic curve parameter design
Companions: `2026-07-05-darktable-mcp-design.md`,
`2026-07-05-darktable-mcp-supported-operations.md`,
`2026-07-05-darktable-mcp-parameter-class-investigation.md`
Source inventory: active curve-bearing IOPs in `src/iop/` at commit
`81129368e0` (this fork)

## Purpose

Investigate a reusable semantic curve surface for MCP without exposing native
node arrays, counts, interpolation arrays, sampled bands, or compatibility
fields as generic array writes.

Curves are the proposed first declarative-composite extension after primitive
scalar editing. They are bounded, self-contained in one module params block,
and do not require file access or cross-subsystem object identity. They still
exercise the important future architecture: semantic discovery, grouped
fields, class-specific validation, module adapters, atomic mutation, GUI
synchronization, history, and undo.

This document covers:

- variable-length control-point curves in `basecurve`, `rgbcurve`,
  `tonecurve`, and `colorzones`;
- fixed-count sampled/banded responses in `atrous`, `rawdenoise`,
  `denoiseprofile`, and `lowlight`; and
- the boundary with the separate `levels` class represented by `rgblevels`.

Proposed JSON and method names are illustrative, not protocol decisions.

The concrete internal data structures, registry, transaction integration, and
protocol extension are proposed in
`2026-07-05-darktable-mcp-curve-classes-low-level-design.md`.

## Main finding

There is no single native curve layout, and a curve-shaped GUI does not imply
one public parameter class.

Three families should remain distinct during design:

| family | native pattern | intended semantic class |
|---|---|---|
| control-point curve | bounded `(x,y)` node array plus count and interpolation | curve |
| sampled/banded response | parallel fixed-count `x[]`/`y[]` arrays, often per channel | sampled response |
| levels | fixed black/mid/white handles per channel | levels |

A sampled response may later prove to be a constrained curve subtype, and
levels may become a constrained vector subtype. Starting with separate
classes avoids losing their stronger user-facing semantics.

## Existing common curve machinery

The GUI and pixelpipe use `dt_draw_curve_t`, a wrapper around `CurveData` and
`CurveSample` from `common/curve_tools.h`. The common implementation supports:

- at most 20 anchors;
- normalized x/y bounds, normally `[0,1]`;
- cubic spline, Catmull-Rom, and monotone Hermite interpolation;
- dense sampled lookup tables;
- V2 periodic and non-periodic sampling paths; and
- point addition and replacement.

This helper is useful evidence for common evaluation semantics, but it is not
a persistent parameter representation and should not become the wire format.
Modules still own:

- which curves exist and what their channels mean;
- native arrays and count/type companion fields;
- defaults, endpoint behavior, and point-spacing rules;
- periodicity and compatibility versions;
- linked-channel modes and which curves are editable;
- picker/autoset actions; and
- stored-to-presentation coordinate transforms.

## Candidate neutral control-point value

A neutral curve value needs enough information to describe the semantic value
without serializing native arrays. One possible shape is:

```json
{
  "class": "curve",
  "id": "master",
  "domain": {
    "x": { "minimum": 0.0, "maximum": 1.0 },
    "y": { "minimum": 0.0, "maximum": 1.0 },
    "periodic_x": false
  },
  "points": [
    { "x": 0.0, "y": 0.0 },
    { "x": 1.0, "y": 1.0 }
  ],
  "interpolation": "MONOTONE_HERMITE"
}
```

The schema, rather than every value response, should normally carry domain,
point-count, endpoint, interpolation, and spacing constraints. Stable
interpolation names are preferable to the native numeric constants.

Open question: should a write replace one whole curve, replace a group of
linked curves, or support point-level operations? Whole-value replacement is
the safest first mutation because it is naturally atomic and idempotent.

## Control-point module inventory

### `basecurve`

Native representation:

- `basecurve[3][20]` node structs;
- `basecurve_nodes[3]` counts; and
- `basecurve_type[3]` interpolation types.

Only curve 0 is currently substantive; the other two slots are reserved.
The default is an identity curve with two endpoints. The GUI edits normalized
stored coordinates, although it may present a log/log view transform.

Relevant behavior:

- at least two nodes are retained by GUI operations;
- endpoints are reset to `(0,0)` or `(1,1)` instead of deleted;
- crossing a neighboring x position deletes the moved node;
- new nodes are inserted in x order;
- node count, active nodes, and interpolation are one semantic value; and
- curve data coexists with scalar exposure-fusion controls that remain
  independent.

Adapter note: expose one stable curve such as `base` or `master`, not three
native array slots.

### `rgbcurve`

Native representation:

- three arrays of up to 20 nodes;
- per-channel node counts and interpolation types;
- `curve_autoscale` linked/independent mode;
- `compensate_middle_grey`; and
- `preserve_colors`.

The native channel constants are R, G, and B, but the semantic role of the
first curve changes with linking mode. In linked mode it acts as the master
curve, and G/B are not independently editable. In independent mode all three
RGB curves are editable.

GUI invariants include:

- normalized x/y coordinates;
- maximum 20 nodes;
- minimum x spacing of 0.0025 between adjacent nodes;
- endpoints cannot be deleted and are reset to identity positions;
- points are kept strictly ordered by x; and
- interpolation may be cubic spline, Catmull-Rom, or monotone Hermite.

The module also creates curves from picker regions. Picker-driven creation is
a semantic operation distinct from replacing explicit points and need not be
part of the first curve API.

`rgbcurve` is the recommended first adapter because it exercises multiple
curves, linking, interpolation, point constraints, and related scalar modes
without periodicity or legacy spline-version behavior.

### `tonecurve`

Native representation:

- L, a, and b arrays of up to 20 nodes;
- per-channel node counts and interpolation types;
- `tonecurve_autoscale_ab` linking/color-processing mode; and
- compatibility fields that should remain internal.

Defaults differ by channel: L uses two identity endpoints, while a and b use
three identity points including the midpoint. When automatic/linked color
handling is enabled, a and b are not independently editable. Manual mode
permits all three curves.

The GUI uses normalized stored coordinates but supports log and semilog
presentation transforms. Those view transforms must not alter the wire value.
New-node insertion currently uses a larger neighbor-distance check than
`rgbcurve`; moved points that cross neighbors are removed. This is evidence
that point-spacing policy cannot be inferred only from `dt_draw_curve_t`.

Adapter note: the schema must describe channel-specific roles and conditional
writability based on `tonecurve_autoscale_ab`.

### `colorzones`

Native representation:

- three arrays of up to 20 nodes for lightness, chroma, and hue adjustments;
- per-curve counts and interpolation types;
- a `channel` enum selecting the input dimension used on the x axis;
- strength and processing mode scalars; and
- `splines_version`, a compatibility field.

The important semantic distinction is between curve channel and selection
channel:

- the three curves control lightness, chroma, and hue output adjustments;
- `channel` determines whether their shared x axis selects by input
  lightness, chroma, or hue; and
- all three curves become periodic when selection is by hue.

Current V2 defaults use two neutral points. When the x domain is periodic,
nodes are positioned away from the duplicated boundary and movement enforces
spacing across the wrap. V1 data uses different endpoint/wrap handling and is
retained for compatibility.

GUI invariants include maximum 20 nodes, strict x order, minimum x spacing of
0.0025, periodic wrap spacing when selecting by hue, and interpolation choice.

Adapter note: periodicity is conditional on the module's selector state, not
an intrinsic property of only the hue-output curve. `splines_version` must not
be caller-controlled; the adapter should generate current-version native
state while continuing to read historical state.

## Control-point comparison

| module | semantic curves | max nodes | periodic | linked/conditional behavior | compatibility concerns |
|---|---:|---:|---|---|---|
| `basecurve` | one active master curve | 20 | no | reserved native slots hidden | historical params layouts |
| `rgbcurve` | master or R/G/B | 20 each | no | G/B read-only in linked mode | mode changes may copy/reset channels |
| `tonecurve` | L/a/b | 20 each | no | a/b read-only outside manual mode | channel defaults and color-processing mode |
| `colorzones` | lightness/chroma/hue adjustments | 20 each | when selecting by hue | periodicity shared across all output curves | V1/V2 spline semantics |

## Sampled/banded response inventory

These modules store fixed-count parallel x/y arrays and render them with curve
widgets. Users can often paint a response over a radius and move interior x
positions, so they are not merely fixed named sliders. They still differ from
control-point curves:

- count and interpolation are generally fixed by the module;
- channel meanings are frequency/noise functions rather than color-transfer
  curves;
- some channels are edited as paired responses;
- UI brush radius is interaction state, not persistent curve state; and
- guard points outside `[0,1]` may be synthesized for evaluation but are not
  stored semantic points.

### `atrous`

Storage is five responses with six samples each:

- luminance boost;
- chrominance boost;
- edge sharpness;
- luminance noise threshold; and
- chrominance noise threshold.

The visible tabs emphasize luma, chroma, and edges, while threshold responses
are paired into those editing modes. Interior x positions remain ordered with
approximately 0.001 separation. The `octaves` field affects how the response
maps to available wavelet scales.

This is better exposed as named band responses than as five anonymous curves.

### `rawdenoise`

Storage is four responses with five samples each:

- all channels;
- red;
- green; and
- blue.

The module has a scalar threshold alongside these responses. Channel tabs and
brush editing operate on the fixed sample arrays. A schema must explain how
the all-channel response interacts with per-channel responses.

### `denoiseprofile`

Storage is six responses with seven samples each:

- all, red, green, and blue; and
- Y0 and U0V0 for the alternate wavelet color mode.

Which channel group is visible/effective depends on `wavelet_color_mode` and
the denoise algorithm mode. Older params versions used fewer bands and fewer
channels, so the adapter must use current semantic IDs rather than exposing
historical dimensions.

Noise-fit `a[3]`/`b[3]` arrays are calibration vectors, not curve samples, and
must remain outside this class.

### `lowlight`

Storage is one transition response with six samples plus the independent
`blueness` scalar. Unlike several denoise responses, interior x positions are
explicitly movable and ordered. The module synthesizes guard points outside
`[0,1]` when constructing the Catmull-Rom curve; callers should only see the
six stored points.

## Sampled-response comparison

| module | responses | stored samples | important coupling |
|---|---:|---:|---|
| `atrous` | 5 | 6 each | paired threshold responses, octave count |
| `rawdenoise` | 4 | 5 each | all versus per-RGB channels, threshold scalar |
| `denoiseprofile` | 6 | 7 each | RGB versus Y0/U0V0 mode, algorithm mode, params migrations |
| `lowlight` | 1 | 6 | movable ordered x positions, synthesized guard points |

## Why levels should remain separate

`rgblevels` stores `levels[3][3]`: black, midpoint, and white handles for each
RGB channel, plus linked/independent mode and preserve-color behavior.

It could be serialized as three three-point curves, but that would discard
the stronger semantics:

- exactly three ordered roles;
- midpoint/gamma behavior differs from an arbitrary spline node;
- linked mode changes which channels are independently meaningful; and
- picker/autoset operations address named handles.

A levels value should expose named black/midpoint/white components and enforce
their ordering. It can reuse the semantic-adapter transaction without using
the curve payload.

## Schema metadata required

Primitive introspection cannot infer the following curve metadata reliably:

- semantic curve ID and localized label;
- curve group and channel role;
- x/y stored domain and optional presentation units;
- periodicity and whether it depends on another field;
- minimum/maximum point count;
- endpoint presence, mobility, and deletion policy;
- strict ordering and minimum x spacing;
- supported interpolation modes and default;
- linked-channel mode and conditional writability;
- fields participating in the native value;
- params versions readable/writable by the adapter;
- whether a curve is explicit control points or a sampled response;
- fixed sample count and named band/channel semantics;
- read-only/deferred reason; and
- related semantic operations such as reset, picker generation, or autoset.

An explicit module/class registry or callback is required. Array dimensions
and field names alone cannot provide these semantics.

## Read model

Curve reading should precede curve writing. A schema may advertise semantic
entries even while they remain deferred:

```json
{
  "id": "master",
  "class": "curve",
  "readable": true,
  "writable": false,
  "deferred_reason": "curve_write_not_implemented",
  "maximum_points": 20,
  "interpolation_values": [
    "CUBIC_SPLINE",
    "CATMULL_ROM",
    "MONOTONE_HERMITE"
  ]
}
```

The value response should return only active semantic points, never unused
native array capacity, compatibility fields, or synthesized guard points.

For linked modules, reading should distinguish effective curves from stored
but inactive companion curves. Possible metadata includes `effective`,
`writable`, `derived_from`, or `inactive_reason`; exact names remain open.

## Mutation model

Do not extend `set_module_params` with arbitrary indexed paths such as
`curve_nodes[2][7].x`. That would expose counts, unused capacity, padding,
ordering, interpolation constants, and compatibility state as caller-owned
invariants.

The intended mutation is whole semantic value replacement inside the common
module transaction:

```text
curve request
  → resolve module instance and semantic curve ID
  → copy complete params block
  → adapter validates semantic points and related mode
  → adapter writes nodes, count, type, and compatibility state to the copy
  → module/class validation checks the completed copy
  → common transaction commits once, records one history item, refreshes GUI
```

Whole replacement supports retries and revision checks cleanly. Point-level
insert/move/delete operations may be added later for interactive clients, but
they should resolve to the same adapter and transaction rather than mutate
native arrays directly.

Multi-curve atomic writes may be required when changing linked/independent
mode or replacing a coordinated set. The protocol should not force one
history item per channel.

## Validation requirements

Common curve validation should cover:

- bounded payload and point count;
- finite x/y values;
- x/y within the declared stored domain;
- strict x ordering;
- module-declared minimum x spacing;
- allowed interpolation names;
- endpoint requirements;
- periodic wrap spacing where applicable;
- duplicate point rejection;
- curve ID and current-mode writability; and
- completed native block consistency after adapter mapping.

Module adapters remain responsible for:

- reserved or inactive channels;
- linked-mode copying/derivation;
- channel-specific defaults;
- compatibility/spline version fields;
- sampled-response fixed counts and paired channels;
- stored/presentation conversion; and
- any stronger processing-specific invariant.

Input should be rejected atomically rather than silently sorting, clamping,
dropping, or merging points. If normalization is later desirable, it should be
an explicit operation whose returned value shows the normalized result.

## Presentation versus stored coordinates

Several GUIs apply log, semilog, zoom, or offset transforms to make editing
easier. These are presentation state, not persistent curve semantics.

The first curve API should use stored normalized coordinates and report them
as such. A later metadata layer may expose photographic units or helper
operations, but writes must not depend on the caller reproducing GUI view
transforms.

Pickers and brush gestures are also presentation/workflow operations:

- a picker can generate or modify curve nodes from image measurements;
- sampled-response brushing changes several y values with a falloff radius;
- zoom and selected-node state are not parameters; and
- UI reset may target one curve, one channel, or a linked group.

Explicit curve replacement can be implemented before these workflows.

## Interpolation semantics

The common native interpolation constants are:

- `CUBIC_SPLINE`;
- `CATMULL_ROM` (presented in some UIs as centripetal); and
- `MONOTONE_HERMITE` (presented as monotonic).

The public value should use stable canonical names while descriptions carry
localized presentation labels. Adapters must reject interpolation modes a
module does not support, even if another curve module supports them.

Periodic evaluation is separate from interpolation type. In `colorzones`, it
depends on the selector channel and spline-version behavior; callers should
not independently toggle a raw `periodic` Boolean that conflicts with module
state.

## Compatibility and history

Curve params layouts have migrated over time. The adapter reads the live
current params block after darktable's normal history migration, so the public
class should not expose historical array dimensions or compatibility fields.

Writes should target the current module params version and set any required
internal version field consistently. Undo/history then preserves the native
current block through existing darktable mechanisms.

One semantic write must produce one history item even when it updates:

- points;
- point count;
- interpolation;
- linked companion curves; and
- internal spline-version state.

GUI synchronization must rebuild or refresh module curve helpers without
generating additional history entries or re-normalizing the committed value.

## Proposed implementation sequence

### Phase 0 — semantic read-only discovery

1. Add an internal curve descriptor/adapter registry.
2. Recognize native field groups for `rgbcurve`, `tonecurve`, `colorzones`,
   and `basecurve`.
3. Expose semantic schemas and current active points as read-only.
4. Add fixtures that prove unused native slots and internal versions are not
   exposed.

### Phase 1 — `rgbcurve` whole-value writes

1. Implement one curve adapter with linked/independent mode awareness.
2. Validate count, bounds, ordering, spacing, endpoints, and interpolation.
3. Replace a curve atomically through the generic mutation transaction.
4. Verify one-step undo, revision conflict, GUI refresh, CPU/OpenCL output,
   and round-trip reads.

### Phase 2 — control-point variation

1. Add `tonecurve` to test channel-specific defaults and conditional a/b
   writability.
2. Add `colorzones` to test periodic domains and compatibility versions.
3. Add `basecurve` to test reserved native channels and legacy layouts.

### Phase 3 — sampled responses

1. Define a separate sampled-response schema.
2. Start with `lowlight` as the single-response fixture.
3. Add `rawdenoise`, then `denoiseprofile`, to test channel modes and params
   migrations.
4. Add `atrous` to test paired response semantics and octave coupling.

### Phase 4 — related operations

Consider reset-to-identity, reset-to-default, picker-generated curves,
sampled-response brush operations, and point-level CRUD only after whole-value
replacement is stable.

## Test matrix

At minimum, tests should cover:

| concern | representative module |
|---|---|
| simple normalized identity and S-curve | `rgbcurve` |
| linked versus independent channels | `rgbcurve`, `tonecurve` |
| all three interpolation modes | `rgbcurve` |
| periodic wrap and selector-dependent periodicity | `colorzones` |
| compatibility field hidden from callers | `colorzones` |
| reserved native curve slots | `basecurve` |
| fixed sample count and movable x positions | `lowlight` |
| all/per-channel response interaction | `rawdenoise` |
| mode-dependent RGB versus Y0/U0V0 channels | `denoiseprofile` |
| paired frequency/threshold responses | `atrous` |
| invalid order, duplicates, NaN/Inf, bounds, excessive count | common validator |
| stale revision and atomic rejection | common transaction |
| one history item and exact undo | every writable adapter |
| schema/value round trip | every adapter |

## Security and resource bounds

Curve editing introduces no filesystem access, but requests still need strict
bounds:

- maximum curves per request;
- maximum points per curve;
- maximum total points and JSON size;
- finite numeric values only;
- fixed interpolation-name length and enum allowlist; and
- no caller-provided native field paths or indices.

The native maximum of 20 control points is already small. Sampled responses
are smaller still. The primary safety risk is inconsistent native state, not
memory volume.

## Open questions

- Should the public mutation be `set_curve`, class-aware `set_parameter`, or a
  batch `set_semantic_parameters` operation?
- Is interpolation part of every curve value or a separately writable
  semantic field?
- Should writes require explicit endpoints, or may adapters synthesize fixed
  endpoints where module semantics permit it?
- Should inactive linked-channel curves be readable, hidden, or returned as
  derived/inactive?
- Can changing link mode and curves be one atomic request?
- Should canonical IDs use `master`, native channel names, or separate IDs by
  mode?
- Are module-specific minimum spacing rules part of the stable schema, or may
  adapters enforce them without exposing exact values?
- Should periodic curves accept points at both 0 and 1, one boundary only, or
  neither duplicated boundary?
- Is a sampled response best represented as ordered points, named bands, or
  both semantic and raw views?
- Should sampled-response x positions be writable for every module that stores
  them, or only where the GUI permits movement?
- How should picker/autoset provenance be reported after the resulting points
  are stored?
- Does GUI synchronization preserve exact committed points for every module,
  or do any callbacks normalize or overwrite them?
- Do CPU and OpenCL paths produce equivalent output for every interpolation
  and boundary mode?

## Acceptance criteria for a future prototype

- No public operation writes array elements, count fields, spline versions, or
  native interpolation integers directly.
- Schemas expose stable semantic curve IDs, channel roles, domains,
  interpolation choices, and conditional writability.
- Reads return only active semantic points and round-trip through the adapter.
- Invalid curves are rejected before any live state changes.
- One semantic write creates one history item and one undo step.
- Linked/independent modes cannot leave companion curves inconsistent.
- Periodic `colorzones` curves validate across the wrap boundary.
- GUI refresh preserves the exact committed semantic value.
- Preview/pixelpipe output changes without requiring GUI interaction.
- Existing scalar editing and params-version migration remain unaffected.

## Maintenance

Update this investigation when curve params layouts, interpolation options,
node limits, periodic behavior, channel-link modes, or common curve helpers
change. A sampled-response design should not be folded into the control-point
class until adapters for at least `lowlight`, `denoiseprofile`, and `atrous`
show that the same semantic model remains clear.
