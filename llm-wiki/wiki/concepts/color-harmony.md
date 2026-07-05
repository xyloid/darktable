---
type: concept
tags: [color, harmony, colorharmonizer, ryb]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/iop/colorharmonizer/README.md, raw/dev-doc/iop/colorharmonizer/business_logic.md]
---

# Color Harmony (the colorharmonizer module)

The `colorharmonizer` IOP nudges hues (and optionally chroma) toward a
palette of geometrically related hue angles — *harmony nodes* — to reduce
chromatic discord. Scene-referred linear RGB in, all processing in
[[darktable-ucs-jch]]. J (lightness) is never modified.

## Harmony geometry

One authoritative table (`src/common/color_harmony.h`) shared with the
vectorscope overlay defines the rules as offsets (fractions of a turn,
1/12 = 30°) from an anchor hue: monochromatic, analogous,
analogous-complementary, complementary, split-complementary, dyad, triad,
tetrad, square, plus free-form custom (2–4 nodes). For **dyad and tetrad the
anchor is the symmetry axis, not a node**. A single table guarantees the
vectorscope guides match processing exactly.

Geometry is defined on the **RYB** artist's wheel, but processing hues are
UCS. The nonlinear RYB↔UCS mapping (Gossett piecewise-linear model through
gamut-clipped sRGB and HSV) is too expensive per pixel, so `init_global()`
builds a pair of 720-entry LUTs (0.5° resolution; inverse built by
brute-force nearest-neighbor scan of the forward table). Node computation in
`commit_params()` round-trips UCS anchor → RYB → apply offsets → UCS nodes.

## The core algorithm: Gaussian winner-take-all attraction

Per pixel with hue h and N nodes:

```
σ = pull_width × 0.5 / N          (more nodes → tighter zones)
dᵢ = circular distance(h, nodeᵢ);  wᵢ = exp(−dᵢ²/2σ²)
Δᵢ = nodeᵢ − h  wrapped to [−0.5, +0.5]
hue_shift = Δ_winner × w_winner    (winner = max weight)
```

Properties: exactly at a node → shift 0 (mathematical guarantee, not a
threshold); far from all nodes → shift ≈ 0; smooth bell in between.
Winner-take-all rather than a weighted average because averaging opposing
nodes (e.g. a complementary pair) would cancel and displace already-correct
pixels — and averaging hues across the wheel produces gray, not a correction.

## Neutral protection & per-node saturation

```
cutoff = protection³ × 0.03
chroma_weight = C / (C + cutoff + 1e-5)      (ε avoids 0/0 NaN)
new_hue = h + hue_shift × pull_strength × chroma_weight
sat_delta = (s_winner − 1) × w_winner
new_C = C × (1 + sat_delta × chroma_weight)
```

Pure grays (C=0) are never touched. The cubic slider mapping spreads
perceptual response across the slider's travel. Pull strength scales hue
only; saturation applies at full strength independently.

## Two implementations, one result

- **CPU** (`src/iop/colorharmonizer.c`): with smoothing off, a fused single
  pass (RGB→JCH→correct→RGB, no intermediate buffers, half the memory
  traffic). With smoothing on, two passes with a JCH cache (3 floats/px) plus
  a 2-channel correction map that gets Gaussian-blurred between passes —
  the forward conversion is ~60% of the per-pixel budget, hence the cache.
- **OpenCL** (`data/kernels/colorharmonizer.cl`): always two kernels
  (map + apply); CAT16 adaptation is folded into pre-multiplied host-side
  matrices; alpha travels in the JCH cache's `.w` because the apply kernel
  deliberately doesn't read the input texture.

Corrections (not hues!) are what gets blurred for smoothing: hue is circular,
and blurring hue values across the 0/1 wrap produces nonsense (avg of 0.95
and 0.05 is 0.50). Blur sigma scales with zoom (`roi_in->scale /
piece->iscale`), the smoothing slider, and pull width.

## Auto-detection ("infer from image")

Chroma-weighted 360-bin hue histogram from the preview pipe, smoothed by 3
passes of a circular 3-tap box filter (≈ Gaussian σ 1.2°), then a grid search
over 9 rules × 360 anchors scoring the fraction of chromatic energy inside
the nodes' Gaussian attraction zones (~4.7M flops, <1 ms). Picks the
(rule, anchor) already best covering the image.

## UI notes

Two-way vectorscope sync (rule/anchor/custom nodes mirror both ways),
import-from-vectorscope button, eyedroppers on anchor and custom node
sliders, collapsible per-node saturation section. Typical use: anchor from
the dominant color, pull strength 0.2–0.3 to start, >0.6 rarely needed.
