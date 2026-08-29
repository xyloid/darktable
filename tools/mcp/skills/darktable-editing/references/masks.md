# Blending and masks

Read this before any *local* edit — anything the user scopes to a
region, a tonal range, or a color rather than the whole frame.

Source: `tools/mcp/src/darktable_mcp/server.py`,
`src/control/remote_masks.c`,
`docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
at commit `3a70e83775`.

## Pick the mask type first

| The user means | Use |
|---|---|
| "everywhere" | no mask — just set the module's parameters |
| "the bright parts", "the shadows", "the blue sky", "skin tones" | **parametric** — selects by tone or color, follows the content |
| "this corner", "the top of the frame", "around her face" | **drawn** — circle, ellipse, or gradient |
| "darken the sky but not the white building in it" | **drawn + parametric** — a gradient scoped by luminance |

Parametric is usually the better first reach: it survives recomposition
and needs no coordinates. Drawn masks are for genuinely spatial intent.

## Verify every mask visually

```json
render_preview({"show_mask": {"op": "exposure", "instance": 1}})
```

Requires the `mask_render` capability. White = full effect, black =
none. `off`/`uniform` mask modes render solid white. **Never trust
marker numbers or coordinates without looking at the rendered mask** —
threshold values that sound reasonable routinely select nothing.

## 1. Blend settings — `blend`, capability `blend_params`

Members: `mask_mode`, `colorspace`, `mode`, `reverse`, `fulcrum` (EV),
`opacity` (0–100), `feathering_radius` (0–250 px), `feathering_guide`,
`blur_radius` (0–100 px), `contrast` / `brightness` / `details`
(−1…1; `details` needs a raw image). `colorspace` and `mode` take C
enumerator names — `get_module_schema`'s `blend` section gives this
instance's choice sets.

> **Writing `colorspace` resets `mode`, `reverse`, `fulcrum`, and every
> parametric threshold** to the new space's defaults. This is
> deterministic, not a bug. If you want those values kept, send
> replacements **in the same call**.

`mask_mode` is state-aware and only partly writable:

- `off` and `uniform` — writable in supported non-drawn families.
- `parametric` — writable with `parametric_mask_params`.
- `drawn`, `drawn+parametric` — you may toggle only the *parametric*
  bit while a drawn mask stays attached.
- **Entering or leaving drawn ownership is attach/detach-only.** Trying
  to add or remove the drawn bit via `mask_mode` fails `invalid_value`
  with `drawn_via_attach_only`. Use `set_mask_attachment`.
- Raster is read-only (`raster_unsupported`) and out of scope.

## 2. Parametric masks — `blend.parametric`, capability `parametric_mask_params`

Maps effective-colorspace channel names (e.g. `Jz_in`) to complete slot
replacements:

```json
{"blend": {"mask_mode": "parametric",
           "colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
           "combine": "exclusive",
           "parametric": {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0]}}}}
```

Markers are four ascending values in [0, 1] — a ramp in, then a ramp
out. `[0, 0, 1, 1]` disables the slot; `null` resets it.

**Two working recipes:**

- **Bright sky / highlights:** `Jz_in: {"markers": [0.55, 0.65, 1, 1]}`,
  `combine: "exclusive"`, `inverted` omitted or false.
- **Shadows:** `Jz_in: {"markers": [0, 0, 0.2, 0.35]}`.

Then adjust the two middle markers by looking at `show_mask`.

**Polarity is the classic trap.** Inclusive combine XORs every channel's
effective polarity, so changing `combine` flips what the whole mask
selects. A patch that both changes `combine` *and* sets an explicit
`inverted` is rejected (`inverted_and_combine_conflict`) unless the same
`blend` patch carries `"allow_inverted_combine": true`.

`boost` is an exp2 exponent; it does **not** rescale the markers.

`parametric`, `combine`, and the blendif-owning mask modes are refused
client-side without `parametric_mask_params`.

## 3. Drawn shapes — the five `mask_shapes` tools

Only **circle**, **ellipse**, and **gradient** are editable. Path,
brush, clone, and AI forms appear in `list_mask_shapes` with `id`,
`type`, `name`, and `used_by` but **no geometry** — you can attach and
detach them, not reshape them. Group forms are engine-managed and never
listed.

### Coordinates

Preview-normalized [0, 1] over the rendered image (`space` defaults to
`"preview"`). Centers and anchors map exactly. **Radii and borders are
fractions of the shorter rendered edge**, not of width. Under
perspective correction, sizes and angles may come back with
`geometry.size_mapping: "approximate"` instead of `"exact"` — check it
before treating a read-back size as authoritative.

### Geometry members (all required — omissions are `missing_member`)

| Type | Members | Constraints |
|---|---|---|
| `circle` | `center: [x,y]`, `radius`, `border` | radius > 0, border ≥ 0 |
| `ellipse` | `center: [x,y]`, `radius: [a,b]`, `rotation`, `border`, `border_mode` | radii > 0; `border_mode` is `"equidistant"` or `"proportional"` |
| `gradient` | `anchor: [x,y]`, `rotation`, `compression`, `steepness`, `curvature` | compression > 0, \|curvature\| ≤ 2 |

`update_mask_shape` replaces the **complete** geometry — you cannot
patch one member. Read the shape first, modify, send it all back. A
gradient update preserves its internal linear/sigmoidal transition mode
(that mode is not part of wire geometry).

### Attachment

```json
set_mask_attachment({"op": "exposure", "instance": 1, "shape_id": 12,
                     "attached": true, "state": "union",
                     "inverted": false, "opacity": 0.8})
```

`state` is `union`, `intersection`, `difference`, or `exclusion`.
(`sum` is reserved for a future brush operation and is rejected.)

The **bottom member of a module's group has no combine operation**, so
`state` is accepted and ignored there — `inverted` and `opacity` still
apply. Do not chase a `state` that appears not to take effect on the
first shape you attached.

`opacity` must be finite; it is clamped to [0, 1]. Note this is 0–1
here, while `blend.opacity` is 0–100.

Detaching removes only that module's edge — the shape survives if
another module uses it. An idempotent detach records nothing and leaves
the revision unchanged.

### Shared shapes

One shape can serve several modules. `update_mask_shape` on a shared
shape **edits every module using it**, and reports how many in
`affects_instances`. Check `used_by` from `list_mask_shapes` before
editing a shape you did not create.

### Sequencing

- Do geometry edits (`crop`, `ashift`, `lens`) **before** mask work.
  The first coordinate call after a distortion-changing edit waits on a
  preview reprocess and may return retryable `retry_later`.
- `create_mask_shape` with `attach` records **two** history items, so
  reverting it takes two `undo` calls.
- The engine cancels a live GUI edit on the affected form before
  updating, deleting, or re-attaching. If the user is mid-drag, their
  drag is discarded — prefer to act when they are not actively editing.
