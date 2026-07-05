# Index

> Catalog of all wiki pages: link + one-line summary per page.
> Updated on every ingest/file operation. Read this first when querying.

## Sources

- [[dev-doc-overview]] — dev-doc README: doc map, processing cheat sheet, lifecycle tables, example modules
- [[dev-doc-module-api]] — IOP module API, pixelpipe architecture, introspection, math/color helpers, new-module guide
- [[dev-doc-gui]] — GUI architecture, bauhaus widgets, sliders, notebooks, recipes, shortcuts, module groups, QAP
- [[dev-doc-ai]] — AI subsystem architecture and per-task reference (mask, rawdenoise, denoise, upscale)
- [[dev-doc-colorharmonizer]] — color harmonizer deep dive: UCS JCH, harmony geometry, CPU/OpenCL paths

## Subsystems

- [[pixelpipe]] — the processing engine: pipe instances, pieces, hash-based caching, ROI, threading
- [[iop-modules]] — IOP module anatomy: the three structs, required/optional functions, registration
- [[introspection]] — compile-time struct reflection powering serialization, GUI binding, and Lua
- [[bauhaus-widgets]] — widget family: _from_params helpers, slider value model, pickers, actions/shortcuts
- [[iop-gui]] — building module UIs: layout API, notebooks, collapsibles, cursors, module groups, QAP
- [[ai-subsystem]] — ONNX Runtime backend: layers, providers, model registry and download

## Concepts

- [[params-vs-data]] — user-intent params_t vs processing data_t, and why commit_params separates them
- [[color-science-conventions]] — transposed matrices, XYZ D50 PCS, CAT16, conversion helpers
- [[darktable-ucs-jch]] — the perceptual UCS 2022 JCH space and its conversion chain
- [[color-harmony]] — colorharmonizer: harmony rules, RYB↔UCS LUTs, Gaussian winner-take-all attraction

## Pipelines

- [[pixelpipe-processing-flow]] — change detection → synch → backwards ROI pass → forwards processing
- [[module-lifecycle]] — full function call order from init_global() to cleanup_global()
- [[gui-event-flow]] — the three event paths, the guard counter, history items, color pickers
- [[neural-restore-flow]] — data flow of AI mask, raw denoise (Bayer/linear), denoise, upscale

## Gotchas

- [[pipeline-ordering-asymmetry]] — commit_params runs forward, default loading reverse; stale shared state
- [[slider-config-order]] — the set_format("%") digits trap; always factor → format → digits
- [[gui-thread-safety]] — no GTK in process(); guards and the two sanctioned update patterns
- [[widget-packing-and-reparenting]] — self->widget set too early; non-static dt_action_def_t dangles
- [[params-serialization-rules]] — gboolean not bool, version bumps, deterministic commits, iop_order fatal error
