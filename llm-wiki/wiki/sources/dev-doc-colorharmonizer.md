---
type: source
tags: [dev-doc, colorharmonizer, color, ucs]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/iop/colorharmonizer/README.md, raw/dev-doc/iop/colorharmonizer/business_logic.md]
---

# dev-doc: Color Harmonizer

Two-document deep dive on the `colorharmonizer` IOP: a user/behavior-level
README and a business-logic guide walking the math and both CPU/OpenCL code
paths with file:line references.

## Main claims

- Processing happens in [[darktable-ucs-jch]] because hue must be
  perceptually linear for Gaussian weighting to behave; J is never modified.
- Harmony geometry lives in one table shared with the vectorscope
  (`src/common/color_harmony.h`), defined on the RYB artist's wheel, bridged
  to UCS by a pair of 720-entry LUTs built once in `init_global()`.
- Core algorithm: Gaussian winner-take-all hue attraction —
  `σ = pull_width × 0.5/N`, shift = `Δ_nearest × w_nearest`. Pixels exactly
  on a node never move (mathematical guarantee); winner-take-all avoids
  opposing nodes cancelling. Neutral protection via a hyperbolic chroma ramp
  with cubic slider mapping. All distilled into [[color-harmony]].
- CPU fuses a single pass when smoothing is off; smoothing forces two passes
  with a JCH cache (forward conversion ≈ 60% of per-pixel cost). OpenCL is
  always two kernels with CAT16 folded into pre-multiplied matrices and
  alpha smuggled in the JCH cache's `.w`.
- Corrections, not hues, are what gets spatially blurred — hue is circular
  and blurs wrongly across the 0/1 wrap.
- "Infer from image": chroma-weighted hue histogram + smoothed grid search
  over 9 rules × 360 anchors, <1 ms.

## Notable as a case study

This is the richest worked example in the doc set of an IOP done well:
`commit_params()` precomputation ([[params-vs-data]]), `init_global()` LUTs,
fused vs staged CPU paths, CPU/GPU parity decisions, and vectorscope
integration. Useful reference when building any color IOP
([[iop-modules]]).

## Feeds into

[[color-harmony]], [[darktable-ucs-jch]], [[color-science-conventions]]
