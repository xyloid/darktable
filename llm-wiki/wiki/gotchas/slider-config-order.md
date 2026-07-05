---
type: gotcha
tags: [gui, sliders, bauhaus, bug-source]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/sliders.md, raw/dev-doc/imageop_gui.md]
---

# Slider Configuration Order (the "%" digits trap)

Misordering [[bauhaus-widgets]] slider configuration can make a slider snap
to its endpoints and reject all intermediate values.

## The mechanism

`_slider_set_normalized()` rounds the internal value to displayed precision:
`rpos = roundf(base * rpos) / base` with
`base = powf(10, digits) * factor`.

`dt_bauhaus_slider_set_format()` has a heuristic: if the format string
contains `%` **and** `fabsf(hard_max) <= 10`, it assumes percentage display
and (a) sets `factor = 100` if factor was still 1.0, (b) applies a
**relative** change `digits -= 2`.

## Failure sequence

1. new slider: `digits = 0`, `factor = 1.0`
2. `set_format("%")` → factor 100, `digits = 0 − 2 = −2`
3. `base = 10⁻² × 100 = 1.0` → `rpos = roundf(rpos)`
4. a [0,1] slider can now only be 0.0 or 1.0 — it snaps to 0%/100%

## Rule

Configure in this order, always:

1. `dt_bauhaus_slider_set_factor()` / `set_offset()` (transformation)
2. `dt_bauhaus_slider_set_format()` (suffix; its side effects run here)
3. `dt_bauhaus_slider_set_digits()` **last** — authoritative precision that
   overwrites whatever `set_format` did

Canonical percentage recipe: factor 100 → format `" %"` → digits 2.
