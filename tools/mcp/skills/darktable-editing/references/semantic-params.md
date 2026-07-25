# Semantic parameters

Four classes let `set_module_params` write things that are not plain
scalars: curves, vectors, bands, and quantities. Each rides on its own
argument, its own capability gate, and its own whole-value replacement
rule.

**`get_module_schema`'s `semantic_fields` is authoritative** for which
IDs a module exposes, their class, component counts, ranges, and
policies. This file tells you the shapes and the traps; the schema tells
you what is actually there on this darktable, for this module, in its
current state.

Source: `tools/mcp/src/darktable_mcp/server.py`,
`docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
at commit `7537128c55`.

## Shared rules

- **Whole-value replacement.** Each patch replaces the entire named
  curve / vector / band / quantity. Unlisted ones are untouched. There
  is no partial update — read the current value first if you mean to
  nudge rather than replace.
- **Capability gated.** If darktable does not advertise the capability,
  the sidecar refuses client-side with an upgrade message rather than
  silently dropping half your patch.
- **One ID, one argument.** A semantic ID appearing in two of `curves`,
  `vectors`, `bands`, `quantities` is rejected before any wire traffic.
- **Native arrays stay read-only.** The fields a semantic parameter
  represents carry `represented_by` in the schema and
  `writable: false` — with one exception, `wb.temperature` (below).

## Curves — `curves`, capability `curve_params`

```json
{"curve.master": {"points": [[0.0, 0.0], [0.5, 0.55], [1.0, 1.0]],
                  "interpolation": "monotone_hermite"}}
```

Interpolation is optional (`cubic_spline`, `catmull_rom`,
`monotone_hermite`, any case); omitting it keeps the curve's current
one. Response reads points back as `{x, y}` objects.

**Invariants:** 2–20 points; x strictly ascending; adjacent points more
than 0.0025 apart.

**The trap:** coordinates are the module's **stored, pre-display space**
in [0, 1] — not what the GUI draws on its axes. A tone curve's stored
space is not perceptual. Read the current curve first and move points
relative to it rather than inventing absolute coordinates.

| Module | IDs | Notes |
|---|---|---|
| `rgbcurve` | `curve.master`, `curve.red`, `curve.green`, `curve.blue` | |
| `tonecurve` | `curve.lightness`, `curve.a`, `curve.b` | a/b only in independent-Lab mode |
| `colorzones` | `curve.lightness`, `curve.chroma`, `curve.hue` | periodic when select-by is hue; **changing select-by resets all three curves** |
| `basecurve` | `curve.master` | |

## Vectors — `vectors`, capability `vector_params`

```json
{"lift": [1.0, 1.1, 1.0, 0.95]}
```

A flat list of finite numbers. The schema gives each ID's component
count and per-component ranges.

**The trap:** components are **stored-space**, and stored identity is
not always zero. `colorbalance` stores identity `lift`/`gamma`/`gain`
as `1.0`. Writing zeros does not mean "no change" — it means black.

| Module | IDs | Notes |
|---|---|---|
| `colorbalance` | `lift`, `gamma`, `gain` | under the default `SLOPE_OFFSET_POWER` mode the writable aliases are `offset`, `power`, `slope` instead — mode-gated |
| `channelmixerrgb` | `red`, `green`, `blue`, `saturation`, `lightness`, `grey` | mixing rows; `grey` is the black-and-white conversion |
| `rgblevels` | `levels.linked` *or* `levels.red`/`.green`/`.blue` | which one depends on the `autoscale` enum |
| `borders` | `color`, `frame_color` | color subtype, has `display_rgb` |
| `watermark` | `color` | |
| `negadoctor` | `dmin`, `wb_high`, `wb_low` | `dmin` is the film-substrate color |
| `colorharmonizer` | `custom_hue`, `node_saturation` | `custom_hue` writable only when `rule` is `DT_COLORHARMONIZER_CUSTOM`; switching the rule in the same call counts |

## Bands — `bands`, capability `band_params`

```json
{"bands.luma": {"y": [0.5, 0.5, 0.6, 0.6, 0.5, 0.5]}}
```

`y` is required and replaces the whole band: **exactly** the schema's
`count` samples, each within `y_range`.

`x` is accepted **only** when the schema says `x_policy: "interior"`.
Then: endpoints must equal the stored endpoints, positions strictly
ascending, adjacent gaps at least `min_gap`. A band naming an
`x_shared_with` twin mirrors any x write to that twin — you cannot give
the pair different x positions.

| Module | IDs | Count / x policy |
|---|---|---|
| `atrous` | `bands.luma`, `bands.chroma`, `bands.sharpness`, `bands.luma_threshold`, `bands.chroma_threshold` | 6, interior; luma and chroma share x with their threshold twins |
| `denoiseprofile` | `bands.all`, `bands.red`, `bands.green`, `bands.blue`, `bands.y0`, `bands.u0v0` | 7, fixed x |
| `rawdenoise` | `bands.all`, `bands.red`, `bands.green`, `bands.blue` | 5, fixed x |
| `lowlight` | `bands.transition` | 6, interior |

## Quantities — `quantities`, capability `quantity_params`

```json
{"wb.temperature": {"temperature": 5500, "tint": 1.0}}
```

Give **every** component the schema lists, exactly once, each within its
`minimum`/`maximum`. A quantity is derived: the module converts it to
native stored fields through its own hooks.

Currently one exists: `temperature`'s `wb.temperature` (Kelvin + tint) —
the conversational white-balance control.

**Two traps specific to quantities:**

1. **The coefficient conflict.** `wb.temperature` is `represented_by`
   `temperature`'s `red`/`green`/`blue`/`various` multipliers, and those
   multipliers are the one case where a represented field **stays
   writable**. Writing both in the same request is rejected server-side.
   Pick one: `wb.temperature` for anything a person would describe in
   words, the raw multipliers only when you have exact coefficients.
2. **Read-back is lossy.** The response re-derives the quantity from
   what was actually stored, so it may differ slightly from what you
   sent (float narrowing plus a conversion round trip). Compare with a
   tolerance; do not treat a small delta as a failed write.
