---
name: darktable-editing
description: Use when editing, grading, or adjusting a photograph in a running darktable darkroom through the darktable MCP — exposure, white balance, contrast, shadows, color, denoise, sharpening, crop, vignette, masks — or when darktable MCP tools return not_in_darkroom, unknown_field, unsupported_field, invalid_value, or revision_conflict.
---

# Editing photos through the darktable MCP

## Overview

The MCP drives a **live darkroom session a human may be sitting in front
of**. Every mutation lands in their history stack and is visible on their
screen immediately. Edit like a guest at someone else's desk: read before
you write, verify with pixels, and never clobber a revision you have not
observed.

## Preflight

`get_current_image` first, always. It returns the view name, the image,
and the starting `revision`. If `image` is `null` or the view is not
darkroom, stop and ask the user to open an image — every other tool will
fail with `no_image_open` or `not_in_darkroom`.

## The loop

**Orient → inspect → change → verify.** Repeat per adjustment; do not
batch several unverified edits.

1. **Orient** — `get_current_image` (revision), `list_modules` (what is
   live, in pixelpipe order, with each instance's `multi_priority`).
2. **Inspect** — `get_module_schema` before your first write to a module.
   It is the *only* authoritative source of field names, ranges, enum
   members, `writable` flags, `semantic_fields`, and the blend choice
   sets. The tier tables and this skill are planning aids; the schema is
   truth.
3. **Change** — `set_module_params`, threading `expected_revision`.
4. **Verify** — `render_preview` for the result, `get_scopes` for
   clipping and tonal distribution, `render_preview` with `show_mask`
   for mask coverage.

## The five rules

1. **Schema before first write.** Never guess a field name, range, or
   enum member. Guessing costs a round trip and an `unknown_field`.
2. **Thread `expected_revision` through every mutation.** On
   `revision_conflict`, re-read state and re-plan — do not retry the
   same call. The conflict means the human changed something, and your
   plan was built on a state that no longer exists.
3. **`set_module_params` never enables a disabled module.** Pass
   `enable: true` in the same call, or your perfect parameters sit inert.
4. **Verify with pixels, not parameters.** A successful write proves the
   value was stored, not that the photo looks better. Never report an
   edit as done on the strength of the write alone.
5. **Prefer the modern module.** Never add a deprecated module to a
   fresh edit. `colorbalancergb` over `colorbalance`; `filmicrgb` or
   `sigmoid` for tone mapping; `toneequal` over curve gymnastics.

## Red flags — stop and re-read

- "I know exposure has an `exposure` field" → read the schema.
- "I'll skip the preview, the value went in fine" → rule 4.
- "`revision_conflict`, I'll just retry" → rule 2. Re-read first.
- "I'll set the params and enable it later" → rule 3.
- "Close enough, I'll tell them it's done" → render it and look.

| Rationalization | Reality |
|---|---|
| "The schema call is an extra round trip" | Cheaper than one `invalid_value` plus a re-plan. Reads are free and non-mutating. |
| "I read this module's schema on another image" | Schemas are per-op but choice sets are state-aware — blend and mode-gated vectors change with current values. |
| "The user described exactly what they want" | They described an *outcome*. Parameters are a means; only the render shows whether you achieved it. |
| "Rendering costs tokens" | A wrong edit costs the user their photograph. Render at `max_px: 512` if cost matters. |

## Intent → module

Highest-value mappings. Full table in `references/intent-map.md`.

| The user says | Module | What to move |
|---|---|---|
| too dark / too bright | `exposure` | `exposure` (EV), `black` |
| lift the shadows / tame highlights | `toneequal` | the nine named EV-band scalars |
| more contrast / punchier | `colorbalancergb` | `contrast`, then `filmicrgb` contrast |
| flat, hazy | `hazeremoval` | `strength`; or `bilat` for local contrast |
| blown highlights (raw) | `highlights` | method enum, then `exposure` |
| too warm / too cool / bad white balance | `temperature` | quantity `wb.temperature` = `{temperature, tint}` |
| more vivid / more saturated | `colorbalancergb` | `saturation`, `brilliance` (4-way) |
| the greens look yellow / fix one hue | `colorequal` | per-hue band saturation / hue / brightness |
| black and white | `channelmixerrgb` | vector `grey` |
| warm highlights, cool shadows | `colorbalancergb` | 4-way Y/C/H; or `splittoning` |
| sharpen | `sharpen` | `radius`, `amount`; or `diffuse` for more control |
| noisy | `denoiseprofile` | `strength`, then bands |
| crop / straighten | `crop`, `ashift` | normalized `cx/cy/cw/ch`; `rotation` |
| converging verticals | `ashift` | lens-shift scalars |
| lens distortion / CA / vignetting | `lens` | `method`, `modify_flags` (Tier 2) |
| vignette | `vignette` | `scale`, `falloff`, `brightness` |
| darken the sky | `graduatednd` | `density`, `rotation`; or `exposure` + a drawn gradient mask |
| film grain | `grain` | `coarseness`, `strength` |

**Danger modules.** `rawprepare`, `colorin`, and `colorout` change
sensor- or pipeline-level behavior; a wrong value breaks the image
rather than making it look worse. Do not touch them unless the user
asks for that specifically.

## Errors

| Error | Do this |
|---|---|
| `not_in_darkroom` / `no_image_open` | Stop. Ask the user to open an image in the darkroom. |
| `revision_conflict` | Re-read state, re-plan, then retry. Never blind-retry. |
| `unknown_field` / `unsupported_field` | `get_module_schema`; the field is misnamed or read-only. |
| `invalid_value` | Range, type, or shape violation. The schema has the bounds. |
| `unknown_module` / `unknown_instance` | `list_modules` for valid ops and live `multi_priority` values. |
| `instance_not_supported` | Module is `ONE_INSTANCE`. Edit instance 0 instead. |
| `retry_later` | Preview pipe is reprocessing after a distortion-changing edit. Wait, retry once. |
| `scope_failed` / `preview_failed` | Transient. Retry once. |
| `request_too_large` | Lower `max_px` or `image_size`. |
| `busy` | Too many in-flight requests. Wait briefly. |
| "does not advertise `<capability>`" | This darktable predates that feature. Fall back to native scalar fields where one exists, or tell the user. |

## Going deeper

- `references/intent-map.md` — every editable module keyed by what a
  person actually says, with usage ratings and Tier 2 caveats.
- `references/semantic-params.md` — `curves`, `vectors`, `bands`,
  `quantities`: payload shapes, which modules expose which IDs, and the
  traps in each class.
- `references/masks.md` — blend settings, parametric masks, drawn
  shapes, and attachment. Read before any local (non-global) edit.
- `references/protocol-discipline.md` — revisions, atomicity, history,
  undo granularity, instance addressing.
