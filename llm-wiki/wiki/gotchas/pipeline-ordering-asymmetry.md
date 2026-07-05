---
type: gotcha
tags: [pixelpipe, ordering, shared-state, bug-source]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/pixelpipe_architecture.md, raw/dev-doc/New_Module_Guide.md]
---

# Pipeline Ordering Asymmetry

Two pipeline operations iterate modules in **opposite** orders:

- `commit_params()` runs in **forward** pipe order (temperature before
  channelmixerrgb) — the normal processing direction.
- `_dt_dev_load_pipeline_defaults()` runs in **reverse** pipe order
  (channelmixerrgb before temperature) — during history reset and default
  loading.

## Why it bites

Modules that communicate via shared state assume "the earlier module has
already run." Example: `temperature.c` writes white-balance coefficients
into `dev->chroma.wb_coeffs` and `channelmixerrgb.c` reads them in its
`commit_params()`. Forward: fine. Reverse-order default loading:
channelmixerrgb runs *first* and reads **stale values from a previous image
or history state**. This caused a real defaults bug.

## Rule

Shared state (like `dev->chroma`) must be explicitly reset before any
reverse-order iteration. When designing a module that reads another module's
committed state, check both iteration directions.

See [[pixelpipe]] for the synch machinery and [[params-vs-data]] for what
`commit_params()` does.
