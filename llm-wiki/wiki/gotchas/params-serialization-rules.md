---
type: gotcha
tags: [params, database, versioning, iop-order, bug-source]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/IOP_Module_API.md, raw/dev-doc/introspection.md, raw/dev-doc/New_Module_Guide.md]
---

# Params Serialization & Registration Rules

`params_t` is a binary contract with the database, presets, and styles
([[params-vs-data]], [[introspection]]). Breaking it corrupts user edits.

## Struct rules

- **`gboolean`, not `bool`** — 4-byte alignment; `bool` changes the layout.
- **No pointers** — the struct is stored as a raw blob.
- **Any layout change requires a version bump** in
  `DT_MODULE_INTROSPECTION(version, type)` **plus** a `legacy_params()`
  implementation migrating every old version forward (allocate the new
  struct, copy old fields, fill defaults for new ones, set `*new_version`).
  Old edits in users' databases must keep working.

## commit_params determinism

The framework hashes `piece->data` after `commit_params()`; the hash keys the
[[pixelpipe]] cache. Identical inputs must produce identical bytes —
non-determinism (uninitialized padding, time-dependent values) silently
breaks caching.

## iop_order registration (fatal on startup)

A new module must appear in **all** order tables in
`src/common/iop_order.c` (`legacy_order`, `v30_order`, `v50_order`,
`v30_jpg_order`, `v50_jpg_order`) **and** get an `_insert_before()` rule in
`dt_ioppr_get_iop_order_list()` (used dynamically for upgrades/fallbacks).

Missing either one → startup abort:
`[dt_init] ERROR: iop order looks bad, aborting.` with
`missing iop_order for module mymodule`. The string name in `iop_order.c`
must exactly match the module's op name.
