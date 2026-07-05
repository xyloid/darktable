---
type: concept
tags: [color, ucs, perceptual]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/iop/colorharmonizer/README.md, raw/dev-doc/iop/colorharmonizer/business_logic.md]
---

# darktable UCS JCH

The **darktable Uniform Color Space 2022** (Aurélien Pierre, 2022) in its
polar JCH form — the perceptual working space used by color balance RGB,
color equalizer, and the color harmonizer ([[color-harmony]]).

Designed for scene-referred color grading: a perceptually uniform,
**hue-linear** UV* plane derived from CIE xyY, without CAM complexity or
absolute-luminance dependence (works on normalized scene-referred data).
Equal angular steps ≈ equal perceived hue differences, which is why
Gaussian weighting over hue angles behaves predictably in it — unlike
HSL/HSV, where 30° near orange looks much bigger than 30° near blue.

| Channel | Meaning | Range |
|---|---|---|
| J | perceptual lightness (J = L*/L_white) | 0 → ~1 |
| C | chroma / colorfulness, perceptually weighted | 0 → ~2 |
| H | hue angle | −π → +π radians |

## Conversion chain

```
linear RGB (D50) → XYZ D50 → XYZ D65 (CAT16) → xyY → UCS JCH
```

and the exact inverse on the way out (note darktable's PCS is XYZ **D50** —
see [[color-science-conventions]]).

Key helpers: `dt_ioppr_rgb_matrix_to_dt_UCS_JCH()` (CPU one-shot),
`xyY_to_dt_UCS_JCH()` / `dt_UCS_JCH_to_xyY()`, and
`L_white = Y_to_dt_UCS_L_star(1.0f)` — a per-run constant (compute once,
not per pixel; the GPU passes it as a kernel argument).

## Practical notes

- Hue is undefined at C = 0; consumers must protect achromatic pixels
  (the harmonizer's chroma weight drives to zero there).
- Modules often normalize H from [−π, π] to [0, 1) for circular arithmetic
  and convert back only at the JCH→RGB boundary.
