---
type: pipeline
tags: [ai, denoise, upscale, rawdenoise, masks]
created: 2026-07-04
updated: 2026-07-04
sources: [raw/dev-doc/AI_Tasks.md, raw/dev-doc/AI.md]
---

# Neural Restore & AI Task Flows

Data flow of each AI task on top of the [[ai-subsystem]]. The restore tasks
(denoise / upscale / rawdenoise) share consumer `src/libs/neural_restore.c`,
which writes output to a sibling file (TIFF or DNG), auto-imports it via
`_import_image`, groups it with the source, and copies the source's
user-applied tags (internal `darktable|*` tags skipped) so outputs stay
visible in tag-based collections.

## Object mask (task `"mask"`)

`src/common/ai/segmentation.h` → consumer `src/develop/masks/object.c`.
SAM 2.1 (`arch: sam2`, 3 encoder outputs, 3 mask candidates, box prompts) or
SegNext (`arch: segnext`, 2 outputs, single mask, no boxes).

1. image exported sRGB uint8; encoder runs once per image (cached): resize
   longest side to 1024, zero-pad to 1024², normalize (SAM: ImageNet
   mean/std; SegNext: [0,1]), HWC→CHW
2. each click runs the light decoder (points + labels + previous low-res
   mask + `has_mask_input` flag)
3. post: pick highest-IoU mask, crop padding, bilinear resize to image dims,
   sigmoid → [0,1] mask shape
4. iterative refinement feeds `low_res_masks` back;
   `dt_seg_reset_prev_mask()` clears mask only, `dt_seg_reset_encoding()`
   clears everything (image change)

Decoder ONNX export constraints: no `orig_im_size` input, fixed 1024²
`masks` output (interpolate baked into graph), only `num_points` dynamic.
The SAM2 decoder also needs `DT_AI_OPT_BASIC` and a `num_labels` dim
override, and is forced to the CPU provider.

## Raw denoise (task `"rawdenoise"`) — two variants

Sensor-level denoise on the CFA mosaic *before* the pipeline; output DNG
re-imports as a normal raw.

**Bayer** (`restore_raw_bayer.h`, RGGB family; input_kind `bayer_v1`):
raw mosaic straight from rawspeed → per-channel black subtract, WB normalize
(daylight from `adobe_XYZ_to_CAM`), range normalize → pack 2T×2T CFA into
4-channel T×T tensor (R,G1,G2,B; non-RGGB force-cropped to RGGB origin) →
tiled inference (model demosaics internally via PixelShuffle, returns 3ch
2T×2T camRGB) → invert normalization/WB, optional `match_gain` → re-mosaic →
uint16 CFA Bayer DNG (`dt_imageio_dng_write_cfa_bayer`).

**Linear** (`restore_raw_linear.h`, X-Trans/Foveon/mono-CFA; input_kind
`linear_v1`): minimal darktable pipeline (`rawprepare → highlights →
demosaic`, temperature disabled) reusing sensor-aware demosaic → daylight WB
+ camRGB→lin_rec2020 matrix → optional exposure boost to `target_mean`
(default 0.30) → tiled inference with per-tile match_gain → invert everything
→ LinearRaw DNG (`dt_imageio_dng_write_linear`). The X-Trans loader currently
falls through to linear; it exists so a dedicated model can arrive via a
manifest-only change.

A declared-but-mismatched `input_kind` is a hard load error; manifests
predating the label are treated as `bayer_v1`.

Tiling: 512/384/256/192 (half-res for bayer), 16-px overlap (=32 sensor px);
Bayer corner tiles mirror-padded *inside* the effective-RGGB-cropped
rectangle (matches RawNIND training).

## Denoise (task `"denoise"`)

Full pipeline export → linear Rec.709 float4 → convert to sRGB (models
operate in sRGB) → tile (2048…256, 64-px overlap, mirror padding, memory
budget ¼ of darktable's) → ONNX (single-input NAFNet/UNet/NIND `[1,3,H,W]`
in [0,1], or 2-input FFDNet with a sigma map) → reassemble → back to linear →
optional DWT detail recovery (5-band wavelet on the luminance residual:
fine/noise bands thresholded hard, coarse/texture bands preserved, blended
back at user strength) → TIFF with ICC + EXIF → auto-import.

## Upscale (task `"upscale"`)

Same as denoise but output is `[1,3,H·S,W·S]` (S = 2 or 4; separate
`model_x2.onnx` / `model_x4.onnx`). Smaller tiles (512…192, 16-px overlap)
due to the S² memory multiplier; TIFF scanlines are streamed without
buffering the full output (4× of 60 MP ≈ 3.6 GB).
