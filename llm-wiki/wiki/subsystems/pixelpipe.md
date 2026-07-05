---
type: subsystem
tags: [pixelpipe, caching, roi, core]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/pixelpipe_architecture.md, raw/dev-doc/IOP_Module_API.md]
---

# Pixelpipe

The pixelpipe is darktable's core image processing engine: it takes an input
image (RAW or raster), passes it through a chain of [[iop-modules]], and
produces output for display or export. Defined in `src/develop/pixelpipe_hb.h`.

## Pipe instances

A `dt_develop_t` holds several `dt_dev_pixelpipe_t` instances that may process
the same image simultaneously:

- `dev->full.pipe` — main darkroom center view
- `dev->preview_pipe` — navigation/overview preview
- `dev->preview2.pipe` — second darkroom window
- export pipes — created on the fly

Pipe types (`dt_dev_pixelpipe_type_t`): `FULL`, `PREVIEW`, `PREVIEW2`,
`EXPORT`, `THUMBNAIL`, plus masks `SCREEN` and `ANY`. Modules check
`dt_pipe_is_full(piece->pipe)` etc. when behavior should differ per pipe.

## Key structures

- `dt_dev_pixelpipe_t` — one pipeline instance. Members: `nodes` (GList of
  pieces), `image`, `input` (float buffer), `cache`, `input_timestamp`,
  `bypass_blendif`, `mask_display`.
- `dt_dev_pixelpipe_iop_t` ("piece") — one module instance *within a pipe*.
  While `dt_iop_module_t` is the module's global state, the piece holds
  per-pipe state: `module`, `data` (processing params — see
  [[params-vs-data]]), `enabled`, `roi_in`/`roi_out`, `blendop_data`,
  `histogram`, `process_cl_ready`, `process_tiling_ready`.

Each pipe gets its own copy of every module's data — this is what makes the
pipes thread-safe against GUI param changes.

## Hash-based caching

Implemented in `src/develop/pixelpipe_cache.c`. Each piece maintains a
cumulative `hash`; for the module at position N it covers:

1. image ID
2. pipe type
3. detail mask state
4. the input/working/output ICC profile infos — profile changes are committed
   globally, not per-module, so they go into the base hash of every lookup
5. hashes of all preceding enabled modules (0..N−1)
6. this module's own `piece->hash` (operation name, instance, params, blending)

`dt_iop_commit_params` updates `piece->hash`; the change propagates down the
pipe. Cache hit → reuse buffer, skip `process()`. Invalidation:
`dt_dev_pixelpipe_cache_invalidate_later(pipe, order)` (everything at
`iop_order` ≥ order) and `..._flush(pipe)` (everything).

Debugging: run with `-d pipe` for hit/miss traces, `-d memory` for cache
memory usage.

## Regions of interest (ROI)

`dt_iop_roi_t` = `{x, y, width, height, float scale}` (0 < scale ≤ 1.0).

- `roi_out` — region the module must produce
- `roi_in` — region it needs from the previous module

Point ops: `roi_in == roi_out`. Geometric ops: transformed. Neighborhood ops:
`roi_in` slightly larger (padding). `piece->iscale` is the input-buffer /
full-image ratio; scale spatial parameters as
`sigma = user_radius * roi_out->scale / piece->iscale`.

## Threading

CPU parallelism via OpenMP (`DT_OMP_FOR`) inside each `process()`; GPU via
optional `process_cl()` with the pipe handling transfers and CPU fallback.

## See also

- [[pixelpipe-processing-flow]] — the full run: change detection, synch,
  ROI back-propagation, forward processing
- [[pipeline-ordering-asymmetry]] — commit_params runs forward, default
  loading runs reverse; a real bug source
- [[introspection]] — how the core hashes/copies param blobs it can't parse
