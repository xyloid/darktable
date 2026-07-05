---
type: subsystem
tags: [introspection, params, build-system]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/introspection.md, raw/dev-doc/IOP_Module_API.md]
---

# Introspection System

Compile-time-generated runtime reflection for C structs, used by
[[iop-modules]] to define their parameters. Activated per module by
`DT_MODULE_INTROSPECTION(version, dt_iop_x_params_t)`.

## Three purposes

1. **Database storage** — serialize/deserialize param structs as binary blobs,
   with versioning and `legacy_params()` upgrades.
2. **GUI generation** — `dt_bauhaus_*_from_params()` reads field metadata to
   auto-create sliders/comboboxes/toggles with correct ranges, defaults, and
   labels (see [[bauhaus-widgets]]).
3. **Lua API** — dynamic param access without per-field binding code.

It is also what lets the [[pixelpipe]] copy and hash `piece->data` blobs
without knowing their internal structure.

## Metadata tags

Comments above `params_t` fields are parsed during the build:

- `$MIN` / `$MAX` — widget range (float, int)
- `$DEFAULT` — default value (all types; `TRUE`/`FALSE` for gboolean; also
  used by `dt_iop_default_init()`)
- `$DESCRIPTION: "text"` — widget label (on enum members: combobox entry text)
- `$VALUES` — valid values for enums

## Internal structure

Compiled into a tree of `dt_introspection_field_t`:

- `dt_introspection_t` — top level: `version`, `size`, `fields`
- `dt_introspection_field_t` — union of per-type descriptors sharing a header
  with `type`, `name`, `offset`, `size`

## Widget binding

`dt_bauhaus_slider_from_params(self, "exposure")` looks up the field, reads
its tags, configures the slider, and binds it to
`(char *)self->params + field->offset`. Moving the slider writes that address
directly.

## Versioning

Any layout change to `params_t` requires bumping the version in
`DT_MODULE_INTROSPECTION` and implementing `legacy_params()` to migrate stored
edits — see [[params-serialization-rules]].
