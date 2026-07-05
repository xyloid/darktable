---
type: source
tags: [dev-doc, ai, onnx]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/AI.md, raw/dev-doc/AI_Tasks.md]
---

# dev-doc: AI Subsystem

The AI cluster: architecture/backend guide (AI.md) and the per-task
reference (AI_Tasks.md). Documents this fork's ONNX-Runtime-based AI stack —
the subsystem behind the neural restore work visible in recent commits.

## Main claims

- Three-layer design with a hard dependency boundary: `src/ai/` (static lib,
  no darktable core deps) ← `src/common/ai/` (bridge, `USE_AI=ON`) ← UI
  consumers that include only the bridge headers. Distilled into
  [[ai-subsystem]].
- Providers (CPU/CoreML/CUDA/MIGraphX/OpenVINO/DirectML) are resolved at
  runtime by dynamic symbol lookup with platform-specific auto fallback
  chains; `DT_AI_PROVIDER_CONFIGURED` is a sentinel meaning "read prefs at
  call time", never stored. Distilled into [[ai-subsystem]].
- Models are directories with `config.json`, discovered by scan,
  downloadable as `.dtmodel` archives from the darktable-ai GitHub repo.
- Four task families with exact tensor contracts: object mask (SAM2/SegNext
  encoder-decoder with iterative refinement), raw denoise (Bayer packed
  4-channel and Linear demosaicked variants writing DNGs), RGB denoise and
  upscale (sRGB tiled inference writing TIFF). Distilled into
  [[neural-restore-flow]].

## Surprises / gotchas surfaced

- On Linux, ORT is always lazy-loaded via `g_module_open()` because GPU
  provider libs can `abort()` at process startup on unsupported GPUs.
- SAM2 decoder needs `DT_AI_OPT_BASIC` (full optimization breaks shape
  inference) plus a `num_labels` dim override, and is pinned to CPU.
- A declared-but-mismatched `input_kind` in a rawdenoise manifest is a hard
  load error by design (no silent fallback).
- Restore outputs inherit the source's user tags (not `darktable|*`
  auto-tags) so they appear in tag-based collections — matches the recent
  fix commits on this branch.

## Feeds into

[[ai-subsystem]], [[neural-restore-flow]]
