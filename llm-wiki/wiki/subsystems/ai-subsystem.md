---
type: subsystem
tags: [ai, onnx, gpu, models]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/AI.md, raw/dev-doc/AI_Tasks.md]
---

# AI Subsystem

ONNX-Runtime-based inference subsystem (this fork's custom feature set;
consumers include neural restore and AI object masks). Three layers:

- `src/ai/` — self-contained static lib `darktable_ai` (GLib + ONNX Runtime
  only, no darktable core deps): `backend.h` (public API), `backend_common.c`
  (env, model registry, provider resolution), `backend_onnx.c` (ORT C API
  wrapper).
- `src/common/ai/` — bridges backend to darktable core, compiled with
  `USE_AI=ON`: `segmentation.c` (SAM/SegNext masking), `restore.c` (env/ctx
  lifecycle + loaders), `restore_rgb.c` (denoise/upscale, tiled),
  `restore_raw_bayer.c`, `restore_raw_linear.c`.
- `src/common/ai_models.c` (registry/download/prefs) and
  `src/gui/preferences_ai.c` (prefs tab).

Build flags: `USE_AI` (default OFF → defines `HAVE_AI`), `USE_AI_DOWNLOAD`
(default ON when AI on → `HAVE_AI_DOWNLOAD`). AI is also disabled by default
at runtime in preferences: no ORT loaded, no model dirs scanned, all
`dt_ai_env_init()`/`dt_ai_load_model()` return NULL, AI modules hide via
`reload_defaults()`. `dt_ai_models_init_lazy()` handles enabling without
restart.

## ONNX Runtime integration

- Lazy `g_once()` singletons: `g_ort` (OrtApi) and `g_env` (OrtEnv), one per
  process. On Linux ORT is always lazy-loaded via `g_module_open()` so GPU
  provider libs (MIGraphX/ROCm) can't `abort()` at startup on unsupported
  GPUs before prefs are checked.
- Loading: `dt_ai_load_model(env, model_id, file, provider)` → resolves the
  id via the registry, creates session options (all-core intra-op
  parallelism), sets opt level, applies symbolic dim overrides, attaches the
  execution provider, creates the session, introspects I/O names/types/
  shapes, detects dynamic output shapes.
- Inference `dt_ai_run()` takes `dt_ai_tensor_t {data, type, shape, ndim}`
  arrays. Handles transparently: Float32↔Float16 auto-conversion, and
  ORT-allocated outputs for dynamic shapes (copies back and updates the
  caller's shape array).
- Opt levels: `DT_AI_OPT_ALL` (default), `DT_AI_OPT_BASIC` (required for the
  SAM2 decoder — aggressive optimization breaks shape inference on dynamic
  dims), `DT_AI_OPT_DISABLED` (reserved).
- `dt_ai_dim_override_t` binds concrete values to symbolic dims (e.g. SAM2's
  `num_labels`) via `dt_ai_load_model_ext()`.

## Execution providers

CPU, CoreML (macOS), CUDA (Linux/Win), MIGraphX (Linux), OpenVINO,
DirectML (Windows), plus `AUTO`. `DT_AI_PROVIDER_CONFIGURED` (value −1) is a
**sentinel, not a provider**: it means "read the user's preference from
darktablerc at call time" and must never be stored in config or the table.
Consumers respecting user prefs pass `CONFIGURED`; consumers needing a fixed
EP (e.g. the SAM decoder forced to CPU) pass it directly.

Auto-detection fallback chains: macOS CoreML→CPU; Windows DirectML→CPU;
Linux CUDA→MIGraphX→ROCm(legacy)→CPU. Provider functions are resolved at
runtime by dynamic symbol lookup, so one binary works with CPU-only and GPU
ORT builds. `dt_ai_probe_provider()` tests availability without loading a
model (used by prefs UI warnings).

Multi-GPU escape hatch: `plugins/ai/{cuda,migraphx,dml}_device_id` conf keys
or `DT_CUDA_DEVICE_ID` / `DT_MIGRAPHX_DEVICE_ID` / `DT_DML_DEVICE_ID` env
vars (env wins); indexes follow each provider's own enumeration.

## Model registry and download

Models = directories containing `config.json` (`id`, `name`, `task`, `arch`,
`backend`, `num_inputs`), scanned under custom paths then
`~/.local/share/darktable/models/`; first id wins. ID convention:
`<task>-<model>[-<size>]` (e.g. `mask-object-sam21-small`).

Downloadable models are release assets (`.dtmodel` zip archives) of the
[darktable-ai](https://github.com/darktable-org/darktable-ai) repo (also
holds conversion + packaging scripts). darktable's bundled
`data/ai_models.json` lists what exists; download extracts into the models
dir where it's immediately discoverable. Repo configured via
`plugins/ai/repository`.

## Adding a feature / provider

New feature: module in `src/common/ai/` with opaque types wrapping
`dt_ai_*`; register in `src/CMakeLists.txt` under `USE_AI`; add entry to
`data/ai_models.json`; UI consumer (lib or IOP) includes only your header,
never `ai/backend.h` directly. New provider: enum + table entry +
`_enable_acceleration()` case + `dt_ai_probe_provider()` case; a
`_Static_assert` keeps the table in sync.

## See also

- [[neural-restore-flow]] — task specs and data flow for mask / denoise /
  rawdenoise / upscale
