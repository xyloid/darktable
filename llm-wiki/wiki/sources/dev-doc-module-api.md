---
type: source
tags: [dev-doc, iop, pixelpipe, introspection, math]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/IOP_Module_API.md, raw/dev-doc/pixelpipe_architecture.md, raw/dev-doc/introspection.md, raw/dev-doc/maths.md, raw/dev-doc/New_Module_Guide.md]
---

# dev-doc: Module API & Core Architecture

The core-architecture cluster of the dev-doc set: the IOP module API
reference, pixelpipe architecture, introspection system, math/color helpers,
and the step-by-step new-module guide.

## Main claims

- Every IOP is built around three structs — user-facing `params_t`
  (serialized blob), optional processing `data_t` in `piece->data`, and
  darkroom-only `gui_data_t` — with `commit_params()` as the transformation
  point. Distilled into [[params-vs-data]] and [[iop-modules]].
- The pixelpipe holds several concurrent pipe instances per develop session,
  uses cumulative hash-based caching (profiles enter the base hash because
  they commit globally), and computes ROIs by a backwards pass followed by a
  forwards processing pass. Distilled into [[pixelpipe]] and
  [[pixelpipe-processing-flow]].
- Introspection (`DT_MODULE_INTROSPECTION` + `$MIN/$MAX/$DEFAULT/...` comment
  tags parsed at build time) powers database serialization, automatic GUI
  binding, and Lua access. Distilled into [[introspection]].
- Color matrices are stored transposed for SIMD; internal PCS is XYZ D50;
  CAT16 is the recommended chromatic adaptation. Distilled into
  [[color-science-conventions]].
- New modules must register in CMake **and** every iop_order table **and**
  `_insert_before()`, else fatal startup error. Distilled into
  [[params-serialization-rules]] and [[iop-modules]].

## Surprises / gotchas surfaced

- `commit_params()` forward vs default-loading reverse iteration — the
  stale-`dev->chroma` defaults bug → [[pipeline-ordering-asymmetry]]
- the framework hashes `piece->data` after commit; non-determinism breaks
  caching silently
- `gboolean` not `bool`; version bump + `legacy_params()` on any layout
  change → [[params-serialization-rules]]

## Feeds into

[[iop-modules]], [[pixelpipe]], [[introspection]], [[params-vs-data]],
[[color-science-conventions]], [[module-lifecycle]],
[[pixelpipe-processing-flow]], [[pipeline-ordering-asymmetry]],
[[params-serialization-rules]]
