---
type: subsystem
tags: [iop, module-api, core]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/IOP_Module_API.md, raw/dev-doc/README.md, raw/dev-doc/New_Module_Guide.md]
---

# IOP Modules

Image Operation (IOP) modules are darkroom processing modules, one C file each
in `src/iop/*.c`. API defined in `src/iop/iop_api.h`, used via
`develop/imageop.h`. `src/iop/useless.c` is the fully-commented template.

## The three structs

| Struct | Lives in | Purpose |
|---|---|---|
| `params_t` | `self->params`, database | User-facing params; serialized as binary blob; drives UI widgets |
| `data_t` (optional) | `piece->data` | Processing-optimized params built by `commit_params()` — see [[params-vs-data]] |
| `gui_data_t` | `self->gui_data` | Widget refs and GUI-only state; darkroom only |

`params_t` fields carry introspection tags in comments (`$MIN`, `$MAX`,
`$DEFAULT`, `$DESCRIPTION`) parsed at build time — see [[introspection]].
Serialization rules (gboolean not bool, version bumps, no pointers) are in
[[params-serialization-rules]].

## Required functions

- `name()` — internal name
- `default_colorspace()` — `IOP_CS_RGB`, `IOP_CS_LAB`, or `IOP_CS_RAW`
- `process(self, piece, ivoid, ovoid, roi_in, roi_out)` — the pixel work.
  Read params from `piece->data` (never `self->params` — thread safety),
  validate input with `dt_iop_have_required_input_format()`, parallelize with
  `DT_OMP_FOR()`, vectorize with `for_each_channel()`. Never call GTK here —
  see [[gui-thread-safety]].

## Key optional functions

- `commit_params()` — transform `self->params` → `piece->data`. The framework
  hashes `piece->data` afterwards; a changed hash invalidates the cache for
  this and all later modules, so output must be deterministic.
- `init_pipe()` / `cleanup_pipe()` — allocate/free `piece->data`; required
  when using a custom `data_t`. Defaults allocate `params_size` bytes and
  memcpy.
- `init_global()` / `cleanup_global()` — once per module *type*; loads OpenCL
  kernels into `self->data` (accessed as `self->global_data` in `process_cl`).
- `reload_defaults()` — per-image defaults (`dt_image_is_raw()` etc.).
- `legacy_params()` — migrate old param versions.
- `process_cl()` — GPU path, falls back to `process()` on failure.
- `tiling_callback()` — report memory needs (`factor`, `maxbuf`, `overhead`,
  `overlap`, `align`) so tiled processing and OpenCL admission work; needed
  whenever the module exceeds `default_tiling_callback()` assumptions.
- geometry functions (`modify_roi_in/out`, `distort_transform/backtransform`,
  `distort_mask`) for modules that change geometry (see `ashift.c`).
- GUI functions — see [[iop-gui]] and [[gui-event-flow]].

The full call order across the module's life is in [[module-lifecycle]].

## Registering a new module

1. `src/iop/CMakeLists.txt`: `add_iop(mymodule "mymodule.c")`
2. `src/common/iop_order.c`: add to **all** order tables (`legacy_order`,
   `v30_order`, `v50_order`, jpg variants) *and* add an `_insert_before()`
   rule in `dt_ioppr_get_iop_order_list()`. Missing either causes a fatal
   startup error — see [[params-serialization-rules]].
3. Implement `default_group()` to pick a tab (`IOP_GROUP_TECHNICAL` /
   `GRADING` / `EFFECTS`). Scene-referred modules go earlier in the pipe,
   display-referred later.
4. Test with `./build.sh` then `./build/bin/darktable -d pipe`.

## Useful helpers

- `dt_iop_alloc_image_buffers(...)` — aligned temp buffers with automatic
  trouble message on failure
- `dt_iop_set_module_trouble_message(...)` — warning icon on module header
- `dt_iop_refresh_center/preview/all(module)` — trigger reprocess from GUI
- flags: `IOP_FLAGS_SUPPORTS_BLENDING`, `IOP_FLAGS_ALLOW_TILING`,
  `IOP_FLAGS_HIDDEN`, `IOP_FLAGS_ONE_INSTANCE`, ...

## Example modules to study

`useless.c` (start here), `exposure.c` (data_t + picker), `filmicrgb.c`
(spline LUT, tabs), `colorbalancergb.c` (gamut LUT), `colorequal.c`
(GtkStack + notebook), `toneequal.c` (matrix solving), `ashift.c` (geometry),
and `colorharmonizer.c` (two-pass CPU/GPU, LUTs — see [[color-harmony]]).
