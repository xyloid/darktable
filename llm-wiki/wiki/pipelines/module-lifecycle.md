---
type: pipeline
tags: [iop, lifecycle]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/IOP_Module_API.md, raw/dev-doc/README.md]
---

# Module Lifecycle

The function call order for an [[iop-modules|IOP module]] from load to
unload.

```
Module load:        init_global()                    [once per module TYPE:
                                                      OpenCL kernels, shared LUTs]
Image open:         init() → reload_defaults() → gui_init()
Pipe creation:      init_pipe()                      [per pipe: allocate piece->data]
Params change:      gui_update() → gui_changed()
User edits widget:  [auto-callback] → gui_changed()
                       → commit_params()             [self->params → piece->data]
                       → process()                   [reads piece->data]
Image switch:       reload_defaults() → gui_update() → gui_changed()
Darkroom exit:      gui_cleanup() → cleanup_pipe() [per pipe] → cleanup()
Module unload:      cleanup_global()
```

Role summary:

| Function | Purpose |
|---|---|
| `gui_init()` | create/configure widgets — do **not** set values here |
| `gui_update()` | sync widgets from `self->params`; end with `gui_changed(self, NULL, NULL)` |
| `gui_changed()` | all conditional visibility/sensitivity/label logic |
| `gui_cleanup()` | free manual GUI allocations |
| `init_pipe()` / `cleanup_pipe()` | allocate/free `piece->data` (needed with a custom data_t) |
| `commit_params()` | transform params → processing data ([[params-vs-data]]) |
| `reload_defaults()` | adjust defaults per image type |
| `init_global()` / `cleanup_global()` | once-per-type resources |
| `process_cl()` | GPU path, falls back to `process()` |
| `tiling_callback()` | memory requirements for tiling |
| `color_picker_apply()` | consume picked colors |

The event paths that trigger the middle of this diagram are detailed in
[[gui-event-flow]]. Threading rules for `process()` are in
[[gui-thread-safety]].
