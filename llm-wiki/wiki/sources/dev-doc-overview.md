---
type: source
tags: [dev-doc, iop, reference]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/README.md]
---

# dev-doc: Overview (README)

Entry point of darktable's in-tree IOP developer guide. Serves as a table of
contents for the doc set plus a dense quick reference.

## Contents

- **Doc map** — Module API (API reference, pixelpipe, introspection,
  shortcuts, groups, maths), GUI development (architecture, widget helpers,
  sliders, notebooks, QAP, recipes), AI subsystem, and the new-module guide.
- **Essential headers** cheat list for GUI and processing includes.
- **Processing cheat sheet** — canonical `process()` skeleton: input format
  validation, `DT_OMP_FOR()` loop with `for_each_channel()`, aligned buffer
  allocation, spatial parameter scaling
  (`sigma = user_radius * roi_out->scale / piece->iscale`), trouble messages,
  pipe-type checks.
- **Lifecycle tables** — key functions, the three structs, and the
  gui_init → gui_update → gui_changed → history → commit_params → process
  data flow.
- **File locations** — where IOP modules, GUI helpers, bauhaus, paint icons,
  imagebuf, and tiling live under `src/`.
- **Example modules table** — which module demonstrates which technique
  (`useless.c` as the starting template through `filmicrgb.c`,
  `colorequal.c`, `ashift.c`).

## Feeds into

[[iop-modules]], [[module-lifecycle]], [[gui-event-flow]]
