---
type: concept
tags: [iop, params, commit-params]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/IOP_Module_API.md, raw/dev-doc/pixelpipe_architecture.md]
---

# params_t vs data_t

The most common source of confusion in the [[iop-modules]] API: two parameter
structs with different jobs.

| | `params_t` | `data_t` |
|---|---|---|
| Lives in | `self->params` | `piece->data` |
| Source | database / UI widgets | built by `commit_params()` |
| Purpose | record user intent, stable + serializable | processing-ready values for `process()` |
| May contain | raw user values (EV, %, enum) | LUTs, splines, normalized values, pointers, runtime state |
| Constraints | no pointers, serializable, version-bumped on change ([[params-serialization-rules]]) | none |

Data flow:

```
Database ──load──→ self->params ←──UI widgets
                        │
                 commit_params()        (also: framework hashes piece->data
                        ▼                afterwards → cache invalidation)
                  piece->data ──→ process()
```

## When you don't need a data_t

If `process()` can use raw user params directly, skip it: default
`init_pipe()` allocates a `params_t`-sized buffer and default
`commit_params()` memcpys into it.

## When you do

- user values need transformation (percent → linear factor, degrees →
  radians, EV → multiplier)
- expensive one-time work belongs in `commit_params()`, not per-pixel
  (splines, LUTs, matrix solving)
- runtime-only state that must not hit the database (profile pointers, gamut
  LUTs)

Examples: `exposure.c` embeds `params_t` plus computed fields; `filmicrgb.c`'s
`data_t` shares no fields with its `params_t` (fully solved spline).

## Why the split matters for threading

Each pipe's piece gets its own committed copy — the [[pixelpipe]] runs on a
snapshot, so the GUI can mutate `self->params` mid-run safely. This is also
why `process()` must read `piece->data`, never `self->params`.

Beware: `commit_params()` runs in forward pipe order, but default loading
iterates in reverse — modules communicating through shared state can read
stale values. See [[pipeline-ordering-asymmetry]].
