# darktable MCP — Parameter Class Investigation

Date: 2026-07-05
Status: investigation; input to a future parameter-class design
Companions: `2026-07-05-darktable-mcp-design.md`,
`2026-07-05-darktable-mcp-supported-operations.md`
Source inventory: `src/iop/` at commit `81129368e0` (this fork)

## Purpose

Investigate the parameter shapes that do not fit the v1 scalar editing model
and collect enough cross-module evidence to design reusable semantic parameter
classes later. This document is deliberately not a wire-protocol proposal.

The current support tiers mostly measure how much of a module survives a
primitive type filter. Tier 1 is scalar-schema complete, Tier 2 is useful but
scalar-schema partial, and Tier 3 needs a semantic adapter for its essential
controls. Tier 3 therefore means deferred semantic support, not impossible to
support.

## Working distinction: storage type versus parameter class

Generated introspection describes C storage accurately: scalar numeric types,
Boolean, enum, array, struct, union, and opaque data. It also carries field
names, offsets, sizes, descriptions, scalar ranges, defaults, and enum values.
That is enough for generic scalar editing, but it cannot determine what an
array or a group of scalars means to a user.

For example, `float[3]` may be:

- an RGB color that should be edited atomically and interpreted in a named
  color space;
- three independent channel coefficients;
- three level handles with an ordering invariant;
- calibration data that should remain read-only; or
- padding or historical compatibility state.

Likewise, a curve may be stored as a node-struct array plus count and
interpolation arrays, as parallel `x`/`y` sample arrays, or as a fixed levels
table. A future parameter class must describe the semantic value and its
invariants rather than exposing the native memory layout.

## Current v1 baseline

The generic v1 path supports finite scalar floats, doubles, signed/unsigned
integers, Booleans, and enums, including scalar leaves in plain nested
structs. It excludes arrays, strings, unions, complex numbers, opaque fields,
and module-specific coupled state.

The existing transaction requirements remain suitable for richer classes:
copy the complete params block, validate and apply a change to the copy,
commit once, create one history/undo item, synchronize the GUI, invalidate the
pixelpipe, and return the resulting revision. The transaction layer should
not become coupled specifically to scalar patches.

## Candidate class families to investigate

These are working categories, not decided public types.

| candidate class | semantic value | examples | main questions |
|---|---|---|---|
| quantity | scalar with stored units different from presentation units | white-balance multipliers versus Kelvin/tint | conversion authority, units, rounding, valid domains |
| coordinate | related scalars representing a point, extent, or region | monochrome Lab point, color-correction endpoints, crop bounds | coordinate space, normalization, transforms, atomic updates |
| ratio | numerator/denominator plus modes or orientation | crop aspect selection | sentinel values, orientation, recomputing dependent bounds |
| vector | fixed-length ordered numeric value | channel coefficients, sensor black levels | component names, per-component ranges, partial writes |
| color | fixed-length color with a declared color space and alpha policy | border/frame colors, watermark color, negadoctor vectors | encoding, gamut, alpha, display conversion |
| levels | ordered black/mid/white handles, possibly per channel | `rgblevels.levels[3][3]` | ordering, linked channels, picker/autoset behavior |
| curve | bounded ordered control points plus interpolation | base curve, RGB curve, tone curve, color zones | domains, endpoint rules, periodicity, minimum spacing |
| sampled response | fixed bands or samples whose x positions may be implicit or explicit | atrous, raw denoise, profiled denoise, lowlight | whether callers edit samples, control points, or named bands |
| node set | bounded related nodes that are not necessarily a continuous curve | color harmonizer custom hues | node identity, count, periodic ordering, per-node attributes |
| patch table | rows of structured source/target measurements | color checker patches | row identity, Lab encoding, table size, paired columns |
| resource reference | selection from resources known to darktable | ICC profiles, lens/camera records, LUTs, overlay images | stable IDs, discovery, availability, no arbitrary paths |
| acquired dataset | state produced by analyzing an image or picker workflow | color-mapping histograms and clusters | acquisition commands, provenance, asynchronous completion |
| object collection | editable shapes or graph objects with identity and ordering | retouch forms, liquify nodes | CRUD operations, coordinates, references, ordering, undo |
| opaque/internal | compatibility or implementation state not directly editable | version fields, embedded compressed LUT bytes | readable diagnostics versus complete omission |

Some candidates may collapse into a smaller final model. For example, color
could be a specialized vector, levels could be a constrained vector, and a
sampled response could be a curve with fixed x coordinates. Keeping them
separate during investigation prevents their distinct invariants from being
lost too early.

## Proposed Tier 2 / Tier 3 breakpoint

The current tiers mix two different questions:

1. Is this parameter shape implemented by the MCP now?
2. How difficult is this parameter shape inherently?

That makes every unimplemented complex value look like Tier 3 even when it is
self-contained and deterministic. Curves then appear equivalent to acquired
histograms, external files, or retouch geometry, although their ownership and
mutation requirements are substantially simpler.

A more useful architectural breakpoint is whether the semantic value is
self-contained in one module's parameter state.

| tier | proposed definition | representative classes |
|---|---|---|
| Tier 1 | generic primitive introspection is sufficient | scalar, enum, Boolean |
| Tier 2 | bounded declarative value handled by a pure module/class adapter | vector, color, coordinate, ratio, levels, curve, sampled response, node set, patch table |
| Tier 3 | operation requires external state, acquisition, persistent object lifecycle, or another subsystem | resource reference/import, acquired dataset, retouch forms, liquify geometry |

### Tier 2 qualification

A Tier 2 adapter should be able to:

- serialize the complete semantic value in a bounded request;
- validate it without file access, image analysis, or asynchronous work;
- apply it to a copied params block without mutating other application state;
- update all coupled native fields atomically;
- commit one module change as one history/undo item; and
- avoid persistent identities owned outside that params block.

This does not mean Tier 2 values are generic arrays. Each class or module
adapter still owns native layout, invariants, and conversion. It means the
adapter can behave as a deterministic transformation from a semantic value
and a params block to a new params block.

### Tier 3 qualification

A value or operation belongs in Tier 3 when it requires one or more of:

- selecting, importing, or reading an external resource;
- access to a profile, lens, image, font, LUT, or other runtime catalog;
- pixel analysis, picker acquisition, or asynchronous computation;
- stable object IDs, create/delete/reorder operations, or graph topology;
- the mask, geometry, or another subsystem as a second state owner;
- security policy beyond ordinary bounded parameter validation; or
- a mutation spanning more than one independently owned state block.

Tier 3 is still implementable. It needs a workflow or object API rather than
only a semantic parameter adapter.

### Resulting reclassification of current Tier 3 modules

Under this breakpoint, the following are architectural Tier 2 candidates even
though their semantic classes are not implemented yet:

- `atrous`: bounded sampled responses;
- `colorchecker`: bounded source/target patch table;
- `colorzones`: bounded control-point curves;
- `rgbcurve`: bounded control-point curves;
- `rgblevels`: bounded ordered levels; and
- `tonecurve`: bounded control-point curves.

The following remain architectural Tier 3:

- `colormapping`: acquired histograms and cluster statistics;
- `liquify`: graph-linked geometry with object identity;
- `lut3d`: external or embedded resource lifecycle;
- `rasterfile`: external raster resource; and
- `retouch`: form records coupled to the mask subsystem.

This reclassification describes inherent complexity, not current support.
For example, `rgbcurve` would be “declarative composite, deferred,” while
`retouch` would be “contextual/object workflow, deferred.”

### Keep complexity and implementation status separate

The schema and reference should eventually track two dimensions:

| dimension | possible values |
|---|---|
| parameter complexity | primitive, declarative composite, contextual/workflow, cross-subsystem object graph |
| implementation status | supported, partial, read-only, deferred |

This avoids changing the meaning of a tier whenever a new parameter class is
implemented. It also permits a mixed module: for example, a LUT module may
have supported primitive interpolation, a deferred resource reference, and a
read-only embedded payload at the same time.

### Suggested first extension sequence

Curves are the highest-value first declarative extension because they exercise
semantic discovery, bounded collections, cross-field invariants, adapters,
atomic mutation, and GUI synchronization without introducing external
resources or cross-subsystem object ownership.

The proposed sequence is:

1. implement a control-point curve class against `rgbcurve`;
2. apply it to `tonecurve` and `colorzones`, adding channel-domain and
   periodicity metadata where required;
3. implement sampled responses separately for `atrous`, `rawdenoise`,
   `denoiseprofile`, and `lowlight` rather than forcing their fixed bands into
   the control-point model;
4. implement levels as a constrained ordered vector rather than a synthetic
   free-form curve; and
5. defer resource, acquisition, and object-workflow APIs until the semantic
   adapter transaction is established.

## Tier 2 inventory

Tier 2 modules already have useful scalar controls. The entries below record
the state that prevents scalar-schema completeness.

| op | native fields or coupled state | user-visible meaning | candidate classes / notes |
|---|---|---|---|
| `basecurve` | `basecurve[3][20]`, `basecurve_nodes[3]`, `basecurve_type[3]` | control-point base curve; only channel 0 is currently substantive | multi-curve; node count and interpolation must update atomically |
| `borders` | `color[3]`, `frame_color[3]`; unused legacy `*_text` strings | border and frame colors | color; legacy strings remain internal |
| `channelmixerrgb` | six `float[4]` channel arrays; coupled illuminant `x`/`y`, type, temperature, fluorescent/LED subtype | channel mixer matrices and illuminant selection | named vectors or matrix rows; coordinate/quantity adapter for illuminant |
| `colorbalance` | `lift[4]`, `gamma[4]`, `gain[4]` | factor plus RGB components for three grading operations | named vectors; component 0 has different semantics from RGB components |
| `colorcorrection` | scalar `hia`, `hib`, `loa`, `lob` manipulated directly by a 2-D GUI | two Lab-ish endpoints defining selective correction | paired coordinates; structurally scalar but semantically coupled |
| `colorharmonizer` | `custom_hue[4]`, `node_saturation[4]`, `num_custom_nodes` | bounded custom harmony nodes on a periodic hue domain | node set; count and parallel arrays are one value |
| `colorin` | profile `type` plus `filename`; working-profile `type_work` plus `filename_work` | input and working ICC profile selections | two resource references; built-in profiles may not need filenames |
| `colorout` | profile `type` plus `filename` | output ICC profile selection | resource reference |
| `denoiseprofile` | wavelet `x`/`y` arrays; noise-fit `a[3]`/`b[3]`; auto-profile-dependent scalars | per-band denoise response and camera noise calibration | sampled response; calibration vectors should normally be acquired/read-only |
| `lens` | `camera[128]`, `lens[128]`; method-dependent EXIF-derived and fine-tune fields | lens/camera selection and correction model | resource reference with method-specific fields and metadata-derived defaults |
| `lowlight` | `transition_x[]`, `transition_y[]` | lowlight transition response | sampled response/curve |
| `monochrome` | scalar `a`, `b`, and `size` controlled as a draggable Lab filter | filter center and radius | coordinate plus extent; structurally scalar but semantically coupled |
| `negadoctor` | `Dmin[4]`, `wb_high[4]`, `wb_low[4]` | film base color and high/low white-balance vectors, often picker-derived | color or named vectors; clarify fourth component and picker provenance |
| `overlay` | `imgid`, `filename`, plus spatial-transform scalars | selected overlay image | resource reference; ID and filename are coupled and should not be raw writes |
| `rawdenoise` | per-channel `x`/`y` arrays | wavelet denoise response | sampled response/curve |
| `rawprepare` | `raw_black_level_separate[4]` | sensor-channel black levels | sensor vector; high-risk metadata-derived values |
| `temperature` | four channel multipliers; GUI Kelvin/tint is derived | white balance | quantity/presentation adapter; no complex C field is the main limitation |
| `watermark` | `filename`, `text`, `font`, `color[3]` | watermark resource or text styling | resource reference, bounded text, font reference, color |

### Tier 2 observations

- Not every Tier 2 limitation is an unsupported C shape. `colorcorrection`,
  `monochrome`, and `temperature` demonstrate semantic coupling and
  presentation conversion across otherwise supported scalars.
- Fixed colors and vectors are likely lower-risk early extensions than curves:
  their sizes are bounded and their invariants are simpler.
- Resource references must not become generic writable strings. The remote
  surface should select resources already enumerated or registered by
  darktable, using opaque stable IDs where possible.
- Auto-derived calibration state needs provenance and writability metadata.
  Being numerically representable does not make it appropriate for direct
  editing.

## Tier 3 inventory

Tier 3 modules are dominated by state that needs a semantic operation. Some
also contain useful scalars, especially when complex state already exists;
future support may move them to Tier 2 before their essential controls are
fully writable.

| op | native fields or coupled state | user-visible meaning | candidate classes / operations |
|---|---|---|---|
| `atrous` | `x[5][6]`, `y[5][6]`, octave count | five per-band response curves | sampled response with named channels and fixed bands |
| `colorchecker` | six Lab component arrays of up to 49 patches plus `num_patches` | paired source/target color patches | patch table; preserve row identity and Lab triplets |
| `colormapping` | 2048-bin source/target histograms, cluster means/variance/weights, acquisition flags | acquired source and target color distributions | acquired dataset; commands such as acquire source/target, plus scalar tuning |
| `colorzones` | three arrays of up to 20 `(x,y)` nodes, per-curve counts/types, spline version | lightness/chroma/hue curves | multi-curve; hue may require periodic semantics; version remains internal |
| `liquify` | up to 100 graph-linked path nodes containing complex coordinates, handles, warp properties, indices and status | point, line, and curve warps | object collection with module adapter; raw array editing is unsafe |
| `lut3d` | filepath, optional embedded compressed LUT bytes and keypoint count, LUT name | selected or embedded 3-D LUT | resource reference/import operation; interpolation can be scalar-editable once a LUT exists |
| `rasterfile` | path/file strings plus channel mode | external raster-mask image | resource reference constrained by the security model |
| `retouch` | up to 300 form records referencing mask IDs, algorithms and per-form settings; module-level wavelet state | retouch shapes and their processing | object collection coupled to the mask subsystem; create/update/delete/reorder operations |
| `rgbcurve` | three arrays of control points, per-curve counts/types | linked or independent RGB curves | multi-curve with channel-link mode and interpolation |
| `rgblevels` | `levels[3][3]` plus linked/independent mode | black, gray, and white handles per RGB channel | levels; enforce ordering and linked-channel rules |
| `tonecurve` | three arrays of up to 20 nodes, per-curve counts/types and color-space/link mode | L/a/b tone curves | multi-curve with channel-specific domains and interpolation |

### Tier 3 observations

- Tier 3 is not a single technical problem. Curves, resource selection,
  acquired analysis, and geometry collections require different APIs.
- `lut3d`, `retouch`, `rgblevels`, `rgbcurve`, and `tonecurve` have scalars or
  enums that can be useful against existing complex state. A capability may
  therefore be conditional, for example “interpolation writable when a LUT
  resource is present.”
- Preset enumeration and application is a useful bridge. It lets darktable
  populate a valid complex params block without exposing native arrays, but
  it does not replace eventual semantic editing.

The acquisition lifecycle, cross-image reference transfer, and candidate MCP
support slices for `colormapping` are investigated in
`2026-07-05-darktable-mcp-colormapping-investigation.md`.

## Curve-focused investigation

Curve support is the first intended semantic extension. The source inventory
contains three related but distinct storage families.

The module-by-module layouts, invariants, adapter boundary, staged rollout,
and test matrix are investigated in
`2026-07-05-darktable-mcp-curve-investigation.md`.

### Control-point curves

`basecurve`, `colorzones`, `rgbcurve`, and `tonecurve` store bounded `(x,y)`
nodes, node counts, and interpolation types. Common interpolation constants
include cubic spline, Catmull-Rom, and monotone Hermite. The modules use the
common `dt_draw_curve_t` runtime helper, but their parameter layouts and
channel semantics differ.

Questions that a neutral curve value must answer:

- stable curve ID and human-readable channel role;
- x/y domain, units, normalization, and presentation transforms;
- minimum and maximum node counts;
- endpoint requirements and whether endpoints can move or be removed;
- strict x ordering, minimum x spacing, and duplicate handling;
- available interpolation modes and their stable names;
- periodic versus non-periodic evaluation, especially for hue;
- linked versus independent channels;
- whether a picker/autoset result is represented as points or as a distinct
  semantic operation;
- module parameter-version compatibility.

### Sampled or banded responses

`atrous`, `denoiseprofile`, `lowlight`, and `rawdenoise` use parallel `x`/`y`
arrays with fixed channel and band counts. They are presented through curve
widgets but may be better described to a model as named frequency bands than
as arbitrary control-point curves.

Open question: should the public value expose literal samples, normalized
control points, or named bands such as shadows/highlights or coarse/fine?
Treating these as ordinary curves without resolving that question would leak
implementation layout into the protocol.

### Levels

`rgblevels` is curve-adjacent but has a stronger three-handle model. A levels
class can expose black, midpoint, and white directly, validate their ordering,
and describe channel linking without pretending the value is a free-form
curve.

### Earliest curve architecture work

Curve writing can remain deferred, but early implementation should:

1. keep the mutation transaction generic over a prepared params block rather
   than specific to scalar fields;
2. make schema and clients tolerant of new parameter classes and capability
   metadata;
3. report recognized curve groups as semantic read-only entries instead of
   unrelated raw arrays where a module adapter can identify them;
4. give adapters a place to read, validate, and apply a semantic value while
   updating all native companion fields atomically;
5. keep revision checking, history, undo, GUI synchronization, and pixelpipe
   invalidation common to all mutation types;
6. bound node counts and payload sizes before allocation or mutation; and
7. add read-only fixtures for representative control-point, sampled-response,
   periodic, linked-channel, and levels modules.

## Metadata needed beyond primitive introspection

A future class registry or adapter needs to supply information that C
introspection cannot infer reliably:

- semantic class and stable semantic parameter ID;
- native fields participating in the value;
- component or channel names and ordering;
- stored domain, presentation domain, units, and conversions;
- scalar/vector/collection bounds;
- invariants across components or companion fields;
- conditional availability and prerequisites;
- source/provenance: manual, picker, metadata, acquired analysis, preset;
- read, write, create, delete, and acquire capabilities;
- reasons for read-only state;
- module params versions supported by the adapter; and
- sensitivity/security classification for paths and external resources.

An explicit registry is more defensible than a `$DESCRIPTION`-present
heuristic. Descriptions are incomplete, and internal state can have a
description while user-facing coupled state may not.

## Mutation adapter boundary to investigate

Do not expose generic array-element writes as the extension mechanism. They
would make callers responsible for native counts, padding, ordering,
interpolation constants, version fields, and cross-field invariants.

The likely boundary is a module/class adapter operating inside the existing
transaction:

```text
semantic request
  → resolve module + semantic parameter
  → copy complete params block
  → adapter validates and applies value to the copy
  → common transaction validates, commits, records history, redraws
```

Whether the public protocol uses one class-aware `set_parameter` method or
dedicated operations such as `set_curve`, `select_resource`, and
`acquire_dataset` remains open. Dedicated operations may express side effects
and prerequisites more clearly; a class-aware method may simplify discovery.

## Capability discovery questions

The schema should eventually distinguish at least:

- value is readable;
- value is directly writable;
- value can be selected from enumerated resources;
- value can be acquired from the current image;
- collection members can be created/deleted;
- writability depends on existing module state; and
- support is deferred because no adapter exists.

Clients must ignore additive response fields so new classes and capabilities
do not require protocol v2. Requests remain strict.

## Investigation priorities

1. Prototype a neutral curve value against `rgbcurve`, `tonecurve`,
   `colorzones`, `basecurve`, and one sampled-response module.
2. Determine which semantics can be shared through `dt_draw_curve_t` and
   which must remain in IOP-specific adapters.
3. Inventory fixed vectors and colors, including component names and color
   spaces; these may be the lowest-risk class extension.
4. Define resource discovery and opaque resource IDs without introducing
   arbitrary filesystem access.
5. Separate editable user state from acquired/metadata-derived state and
   record provenance in schemas.
6. Test a generic mutation adapter with atomic failure, one-step undo,
   revision conflict, GUI refresh, and params-version fixtures.

## Decisions intentionally deferred

- Exact JSON shapes and method names
- Whether semantic metadata lives in source annotations, a registry, module
  callbacks, or a combination
- Whether curves are exposed as points, bands, sampled values, or multiple
  related classes
- Resource-ID lifetime and portability across darktable installations
- Remote creation and editing of masks, retouch forms, and liquify geometry
- Which acquired values may be imported versus only produced by darktable
- Migration and compatibility guarantees for semantic adapters across params
  versions

## Maintenance

Update this investigation when a module moves between support tiers, when a
new unsupported native shape is found, or when a semantic adapter prototype
establishes or disproves a candidate class. Derive operation identifiers from
the first argument to `add_iop()` in `src/iop/CMakeLists.txt`, not from source
filenames.
