# darktable MCP — parametric masks (mask Tier 2) low-level design

Date: 2026-07-19
Status: draft design (not yet planned; awaiting review; depends on Tier 1)
Companions: `2026-07-19-darktable-mcp-blend-settings-design.md` (Tier 1 —
the `blend` wire object and `remote_blend.c` this extends),
`2026-07-19-darktable-mcp-mask-support-design-candidates.md` (tier split),
`2026-07-05-darktable-mcp-protocol-reference.md` (wire contract).

## Goal

Read and write parametric ("conditional") masks — per-channel trapezoid
ramps over module input/output pixel values, with polarity, per-slot boost
factors, and the mask-combine setting — as an extension of Tier 1's
`blend` object, behind a `parametric_mask_params` hello capability.

## Non-goals

Drawn masks (mask-mode bits `MASK`/`RASTER`, `mask_id`, `raster_mask_*`,
the `DEVELOP_COMBINE_MASKS_POS` bit — all Tier 3), and the mask
refinement scalars already shipped in Tier 1 (`feathering_*`, `blur_radius`,
`contrast`, `brightness`, `details`).

## Verified storage semantics this design rests on

All verified 2026-07-19 in source:

- **Markers** (`blendif_parameters[4 × 16]`): per channel slot, four
  ascending values `[m0, m1, m2, m3]` in a 0–1 stored domain. Mask factor
  ramps 0→1 over `m0..m1`, is 1 over `m1..m2`, falls 1→0 over `m2..m3`
  (`dt_develop_blendif_process_parameters`, `src/develop/blend.c:161`).
  Open ends: `m0,m1 ≤ 0` → unbounded low; `m2,m3 ≥ 1` → unbounded high.
  Degenerate equal markers are legal (hard edge; the pipeline clamps the
  slope denominator at 0.001).
- **Enable bits** (`blendif` bits 0–15) are *derived* state in the GUI:
  a channel is disabled exactly when `m1 == 0 && m2 == 1` (full span),
  enabled otherwise (`_blendop_blendif_sliders_callback`,
  `blend_gui.c:865-869`).
- **Polarity bits** (`blendif` bits 16–31): the pixel kernels compute
  `invert_mask = (blendif >> 16) XOR (combine_inclusive ? family_mask : 0)`
  and use `1 − factor` for inverted channels
  (`data/kernels/blendop.cl:204,240`; C twins in `src/develop/blends/`).
  So a channel's *effective* inversion is its polarity bit XOR the
  `mask_combine` inclusive flag — polarity is meaningless in isolation.
- Bit 31 (`DEVELOP_BLENDIF_active`) is a legacy flag; current code strips
  it in legacy-params conversion and never sets it.
- **Boost factors** (`blendif_boost_factors[16]`): per *slot*, log2
  multipliers applied as `(stored − offset) × 2^boost` where offset is
  0.5 for Lab a/b slots and 0 otherwise. GUI slider: 0–18 EV displayed
  (soft 0–3), stored = displayed + per-channel offset (−6.64385619 for
  Jz/Cz — `_blend_init_blendif_boost_parameters`). The GUI keeps the
  in/out slots of a channel equal and **rescales existing markers** by
  `2^old/2^new` on boost change so effective thresholds stay put,
  skipping markers at 0/1 to preserve open ends
  (`_blendop_blendif_boost_factor_callback`, `blend_gui.c:1283`).
- **Channel families** (GUI tables `Lab_channels[]`, `rgb_channels[]`,
  `rgbj_channels[]`, `blend_gui.c:2338+`, non-static): Lab →
  `L a b C h`; RGB display → `g R G B H S l`; RGB scene →
  `g R G B Jz Cz hz`. Each channel names an `_in` and an `_out` slot.
  Boost is only available where `boost_factor_enabled` (all except the
  hue channels and HSL `H S l`). Valid slot masks per family:
  `DEVELOP_BLENDIF_Lab_MASK` = 0x3377, `RGB_MASK` = 0x77FF. RAW
  blending has no parametric support (no RAW blendif kernel).
- **mask_mode values** (`dt_develop_mask_mode_names`): every mask type
  carries `DEVELOP_MASK_ENABLED`; "parametric mask" =
  `ENABLED | CONDITIONAL`.
- **mask_combine** (`dt_develop_combine_masks_names`): four values from
  the `INV` and `INCL` bits — exclusive, inclusive, exclusive-inverted,
  inclusive-inverted. `MASKS_POS` is a separate drawn-mask bit (Tier 3).

## Wire contract

### Capability

`hello` advertises `parametric_mask_params` (implies `blend_params` is
also advertised; the sidecar refuses `parametric`/`combine` members
client-side without it).

### Schema (inside Tier 1's `"blend"` schema object)

For blending modules whose *effective* colorspace is Lab, RGB display, or
RGB scene (absent for RAW blending):

```json
"combine": { "type": "enum",
             "values": ["exclusive", "inclusive",
                        "exclusive_inverted", "inclusive_inverted"],
             "writable": true },
"parametric": {
  "channels": {
    "Jz_in": { "markers_domain": [0.0, 1.0],
               "boost": { "writable": true, "offset": -6.64385619,
                          "range": [-6.64385619, 11.35614381] },
               "display_hint": { "factor": 100.0, "offset": 0.0,
                                 "unit": "%", "boost_scales": true } },
    "h_in":  { "markers_domain": [0.0, 1.0],
               "boost": null,
               "display_hint": { "factor": 360.0, "offset": 0.0,
                                 "unit": "°", "boost_scales": false } },
    "...": {}
  }
}
```

- `channels` lists exactly the `_in`/`_out` slots of the effective
  colorspace family — this is the writable vocabulary, and it changes
  when `colorspace` changes.
- `boost` is `null` for channels without boost support. The stored range
  is the GUI's displayed 0–18 EV shifted by the channel offset.
- `display_hint` is **non-normative** metadata for clients that want to
  present GUI-style numbers: `display ≈ (stored − offset_ab) × 2^boost ×
  factor` with `offset_ab` = 0.5 for Lab `a`/`b`. The wire itself always
  carries stored values (the colorbalance stored-value precedent).
- `mask_mode.values` (Tier 1 member) gains `"parametric"`; the schema
  advertises it only when this capability's conditions hold.

### Read (inside the `"blend"` object of `get_module_params`)

```json
"mask_mode": "parametric",
"combine": "exclusive",
"parametric": {
  "L_in":  { "markers": [0.0, 0.0, 0.55, 0.65],
             "inverted": false, "boost": 0.0 },
  "Cz_out": { "markers": [0.1, 0.2, 1.0, 1.0],
              "inverted": true, "boost": -6.64385619 }
}
```

- Only **enabled** slots appear (enable is derived — see below); an
  empty object means "parametric mode on, no channel conditions yet".
- `inverted` is the stored polarity bit. The reference documents the
  kernel XOR rule so a client can compute effective polarity:
  `effective_inverted = inverted XOR (combine is inclusive*)`.
- Slots enabled in storage but outside the current family (legacy edits)
  are not listed; a boolean `"foreign_channels": true` appears when any
  exist, and patches never touch foreign slots.

### Patch (inside the `"blend"` patch object)

```json
"blend": {
  "mask_mode": "parametric",
  "combine": "exclusive",
  "parametric": {
    "Jz_in": { "markers": [0.0, 0.0, 0.5, 0.6] },
    "h_in":  null
  }
}
```

Semantics:

- A channel entry **replaces that slot entirely**: `markers` required,
  `inverted` optional (default `false`), `boost` optional (default = the
  channel's offset, i.e. GUI zero). Unlisted slots are untouched.
- `null` resets a slot: markers `[0,0,1,1]`, polarity bit cleared, boost
  reset to the channel offset — which also *disables* it by the derived
  rule.
- **Enable is derived, never sent**: the engine sets the slot's enable
  bit iff NOT (`m1 == 0 && m2 == 1`), exactly the GUI rule. A patch whose
  markers are full-span simply leaves the channel disabled (this is not
  an error — it is "no condition on this channel").
- **Boost does not rescale markers.** The GUI's threshold-preserving
  marker rescale on boost change is NOT reproduced: each member is
  written verbatim, deterministically. A client changing boost on an
  existing condition must rescale markers itself if it wants
  GUI-equivalent behavior (formula in the reference). Same
  determinism-over-GUI-cleverness rationale as Tier 1's colorspace
  decision.
- Writing `parametric` requires the projected `mask_mode` to include
  parametric (set it in the same call or beforehand); `combine` is
  writable whenever the schema shows it.
- Colorspace + parametric in one call: `colorspace` applies first (Tier 1
  reset semantics wipe the blendif block), then `parametric` members are
  validated against the **new** family and applied on top. Deterministic
  layering, no hidden state.
- Validation order within `blend`: `mask_mode` → `colorspace` →
  `combine` → `parametric` (per channel: name → markers → inverted →
  boost) → Tier 1 numerics. Blend keeps its Tier 1 position at the end of
  the class error precedence.

### Errors

| condition | error |
|---|---|
| `parametric`/`combine` without the capability (old darktable) | sidecar client-side refusal |
| channel name not in the effective family | `unsupported_field`, `parameter: "blend.parametric.<name>"` |
| `parametric` on RAW-blending module | `unsupported_field`, `parameter: "blend.parametric"` |
| markers not exactly 4, non-finite, outside [0,1], or not ascending | `invalid_value`, `constraint: "markers"` with the specific violation |
| `boost` on a channel with `boost: null`, or out of range | `invalid_value`, `constraint: "boost"` |
| `parametric` while projected `mask_mode` lacks parametric | `invalid_value`, `constraint: "requires_parametric_mask_mode"` |
| `mask_mode` write that would change the `MASK` bit, or any write on a raster-bit state | `invalid_value` per the transition appendix in the candidates doc (authoritative; corrected 2026-07-19 — toggling `CONDITIONAL` **is** legal while `MASK` is set: `drawn ↔ drawn+parametric`) |

## Engine design (extends `remote_blend.c`)

New engine-owned channel table (wire names are stable ASCII, not
translated GUI labels):

```c
typedef struct dt_remote_blendif_channel_t
{
  const char *name;                      // "Jz" — wire name, family-scoped
  dt_develop_blendif_channels_t slot_in; // e.g. DEVELOP_BLENDIF_Jz_in
  dt_develop_blendif_channels_t slot_out;
  gboolean boost_supported;
  float boost_offset;                    // stored value of "GUI zero"
  float display_factor;                  // display_hint metadata
  const char *display_unit;
} dt_remote_blendif_channel_t;

// one table per family; NULL-terminated
const dt_remote_blendif_channel_t *
dt_remote_blendif_channels(dt_develop_blend_colorspace_t csp);
```

**GUI-parity test instead of GUI refactor**: unlike Tier 1's mode table
(where the GUI switch is refactored to consume shared data), the GUI's
`Lab_channels[]`/`rgb_channels[]`/`rgbj_channels[]` tables are non-static
`const` and directly linkable — the unit test externs them and asserts
slot indices, boost enablement, and boost offsets match the engine table
entry-for-entry. Drift becomes a test failure with zero GUI changes.

Pure helpers (all headless-testable):

```c
// bitfield <-> per-slot views; masks out DEVELOP_BLENDIF_active
gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot);
gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot);
uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted);
// derived-enable rule, single source of truth
gboolean dt_remote_blendif_markers_enable(const float m[4]);
```

`dt_remote_blend_patch_apply` (Tier 1) grows the `combine` +
`parametric` stages; it still writes only the caller's
`dt_develop_blend_params_t` copy, so the Tier 1 commit path
(`dt_iop_commit_blend_params` → `dt_iop_gui_update` →
`dt_dev_add_history_item(dev, module, FALSE)` → reveal) is completely
unchanged — Tier 2 adds **zero** new commit/threading/history surface.

`mask_mode` mapping gains `"parametric"` ↔ `ENABLED|CONDITIONAL` in both
directions; the compound read-only strings from Tier 1 keep covering
drawn/raster combinations.

## Mask rendering companion (`mask_render` capability — same milestone)

Added to this milestone by the 2026-07-19 scope reorg (hard-challenge
analysis H1): without seeing the mask, an agent cannot verify thresholds,
so parametric masks would demo well and work poorly. darktable already
renders per-module masks for the GUI overlay
(`module->request_mask_display`, `DT_DEV_PIXELPIPE_DISPLAY_MASK` —
`imageop.h:196`, `pixelpipe.h:68`); this exposes that pipeline, read-only.

**Wire.** `render_preview` gains an optional argument:

```json
{ "max_px": 1024, "quality": 90,
  "show_mask": { "op": "exposure", "instance": 1 } }
```

Response: the module's current blend mask as a grayscale JPEG (white =
full effect), same framing as the normal preview so coordinates line up
1:1 for side-by-side reasoning; plus `"mask_of": {op, instance,
mask_mode}` metadata. Works for *any* mask source (uniform, parametric,
drawn, combinations) — which also makes it M-C's primary verification
tool. Errors: module without blending → `unsupported_field`; mask mode
`off` → renders full-white with `mask_mode: "off"` metadata rather than
erroring (a legal question deserves a legal answer).

**Engine.** The render job sets the target module's
`request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK`, runs the same
bounded render path `render_preview` already uses, restores the flag
(saved value, not assumed-zero — the GUI may own it), and serializes the
mask channel. Flag save/restore happens on the GTK thread bracket the
existing render job already has; the one design question to pin at
planning is whether the export-path pipe honors `request_mask_display`
or whether the mask render must ride the darkroom preview pipe with a
snapshot (both exist; the GUI uses the latter).

**Capability**: `mask_render`, independent of `parametric_mask_params`
(a client may render masks of GUI-drawn state on an engine without
parametric write support in principle, but both ship in this milestone).

**Testing.** Unit: flag save/restore bracket; error rows. Integration:
parametric luminance mask → mask render is dark in shadows and bright in
highlights (direct assertion on the mask image — replaces the indirect
"spatially non-uniform preview delta" gate); mask render of `off` mode is
uniform white; flag restored after render (GUI overlay state unchanged).

## Sidecar changes

- `set_module_params`: the `blend` dict passes through; client-side
  capability gate extends to refuse `parametric`/`combine` members
  without `parametric_mask_params`.
- `render_preview`: optional `show_mask` argument passed through, gated
  client-side on `mask_render`; the tool description tells the model to
  use it to verify every parametric threshold it sets.
- Tool docstring + README: a worked example — sky selection via
  luminance: `{"blend": {"mask_mode": "parametric", "parametric":
  {"Jz_in": {"markers": [0.55, 0.65, 1.0, 1.0], "inverted": true}}}}`
  on an exposure instance — plus the two sharp-edge warnings (boost does
  not rescale markers; combine flips effective polarity).

## Protocol reference impact

The Tier 1 blend section gains a "Parametric masks
(`parametric_mask_params`)" subsection: derived-enable rule, the polarity
XOR rule with the kernel citation, open-end marker semantics, the
boost/marker independence note with the GUI-rescale formula, foreign-slot
policy, and the error table.

## Testing

Unit (`test_remote_blend.c` additions):
- pack/unpack round-trips for every slot of every family; `active` bit
  always stripped.
- derived enable: full-span disables, anything else enables; `null`
  resets and disables.
- markers validation: count, order, domain, non-finite each produce the
  tabled error; degenerate equal markers accepted.
- boost: offset defaults per channel (Jz/Cz), refusal on hue channels,
  range edges.
- family gating: Lab channel name against an RGB-scene module errors;
  colorspace switch + parametric in one patch validates against the new
  family; foreign-slot preservation (set an out-of-family bit in the
  fixture, patch another channel, assert the bit survives).
- GUI-parity: engine tables vs externed `*_channels[]` tables.
- mask_mode: `"parametric"` mapping both ways; drawn-bit configurations
  still refuse.

Protocol: fixtures for read/patch/error shapes, shared with
`test_protocol.py`.

Integration: parametric luminance mask on exposure (the README example) →
preview changes and is *spatially* non-uniform (compare two preview
regions' deltas); disable via `null` → uniform again; `combine` flip
changes the rendered mask; capability in hello.

## Decisions recorded (and what review should challenge)

1. **Enable is derived from markers** (the GUI rule), not an explicit
   wire flag — keeps wire state bijective with stored state and makes
   "enabled but full-span" unrepresentable, at the cost of "send
   full-span markers" being a silent no-condition write.
2. **Polarity on the wire is the stored bit** (`inverted`), with the
   combine-XOR rule documented rather than resolved — the alternative
   (exposing effective polarity) would make every channel's meaning
   change when `combine` changes, which is worse.
3. **Boost writes never rescale markers** — deterministic verbatim
   storage over GUI threshold-preservation; the rescale formula is
   documented for clients.
4. **Channel entries replace the whole slot**; `null` resets — no
   per-member merge within a channel.
5. **Only enabled slots in reads**; foreign (out-of-family) slots are
   invisible but preserved, surfaced only as a boolean.
6. **GUI-parity by test, not refactor**, for the channel tables (the GUI
   tables are already linkable data; no behavior-adjacent GUI change is
   needed, unlike Tier 1's mode matrix).
7. Tier 2 adds no commit-path/threading/history surface: everything new
   is pure functions over the params copy.
