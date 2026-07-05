---
type: pipeline
tags: [pixelpipe, roi, caching]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/pixelpipe_architecture.md]
---

# Pixelpipe Processing Flow

How one run of the [[pixelpipe]] goes from "something changed" to pixels on
screen.

## 1. Change detection

`dt_dev_pixelpipe_change()` checks flags: what changed — history, params,
zoom?

## 2. Synchronization

`dt_dev_pixelpipe_synch_all()` iterates the history stack and calls
`commit_params()` on modules, copying global module state into the
pipe-specific `piece->data` ([[params-vs-data]]). The pipe then runs on a
snapshot — that's the thread-safety contract. Note this iteration is
**forward** pipe order, while default loading iterates in reverse — see
[[pipeline-ordering-asymmetry]].

## 3. ROI back-propagation (backwards pass)

`dt_dev_pixelpipe_process()` starts from the requested output (screen area or
export size) and iterates **backwards** from the last module to the first,
calling each module's `modify_roi_in()`: "if I must produce this output area,
what input area do I need?" This accounts for distortions, lens corrections,
crops, and neighborhood padding.

## 4. Forward processing

Iterate **forwards** from the first module:

- check the cache: if the cumulative hash matches, reuse the buffer and skip
  processing (possibly for several modules at once)
- on miss, call `process()` (or `process_cl()` on GPU)
- store the result in the cache if appropriate

Cache key composition and invalidation rules are on the [[pixelpipe]] page.

## Debugging

`darktable -d pipe` traces cache hits/misses per run; `-d memory` shows cache
memory usage.
