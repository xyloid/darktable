---
type: concept
tags: [color, math, simd, matrices]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/maths.md]
---

# Color Science & Math Conventions

darktable ships SIMD-optimized math and color helpers — never hand-roll
matrix multiplies or color conversions in a module.

## Transposed 3×3 matrices

To let the compiler use broadcast multipliers instead of dot products,
**color matrices in darktable are commonly stored transposed**. Apply with
`dt_apply_transposed_color_matrix(in, M_transposed, out)`
(`common/colorspaces.h`), which computes
`out[c] = M[0][c]*in[0] + M[1][c]*in[1] + M[2][c]*in[2]`.

When combining matrices remember `(A·B)ᵀ = Bᵀ·Aᵀ` — use
`dt_colormatrix_mul(dst, M1, M2)` and track transposition state, or you get
silent channel swaps.

## XYZ is D50 internally

darktable's profile connection space is **XYZ D50**. Research papers and
other software usually use **D65** — bridge with `dt_XYZ_D50_2_XYZ_D65()` /
`dt_XYZ_D65_2_XYZ_D50()`. (The [[color-harmony|color harmonizer]] does
exactly this on its way into [[darktable-ucs-jch]].)

## Conversion helpers

`common/colorspaces_inline_conversions.h`: `dt_XYZ_to_RGB_clipped`,
`dt_RGB_to_XYZ` (pipe working space, usually linear Rec2020 ↔ XYZ),
`dt_Lab_to_XYZ` / `dt_XYZ_to_Lab`, `dt_xyY_to_XYZ` / `dt_XYZ_to_xyY`.

## Chromatic adaptation (CAT)

`common/chromatic_adaptation.h` provides pre-computed transposed matrices for
CAT16 (recommended) and Bradford. Adaptation is a von Kries scaling in LMS:
convert XYZ → LMS (`XYZ_to_CAT16_LMS_trans`), scale by white points, convert
back. Don't hardcode your own CAT matrices.

## General math

`common/math.h` / `common/darktable.h`: `CLAMP`, `dt_fast_exp2f`,
`dt_fast_log2f`; prefer standard C99 `fmaxf`/`fminf`/`copysignf` where
auto-vectorization matters. `RAD_2_DEG` lives in `common/math.h`.
