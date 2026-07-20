# darktable MCP — Supported Operations Reference

Date: 2026-07-16
Status: reference (supersedes `2026-07-05-darktable-mcp-supported-operations.md`;
companion to `2026-07-05-darktable-mcp-design.md`)
Source inventory: `src/iop/` and `src/control/remote_*` at milestone-6
completion (`976409dc65`, branch `worktree-mcp-remote-edit`)

## Purpose

A complete inventory of darktable's darkroom operations (IOP modules) and how
the MCP remote-edit surface supports each one, as implemented through
milestone 5. The runtime source of truth is always `get_module_schema` — this
document is the planning and review reference: which modules the model can
usefully edit, which are partially editable, which are excluded and why, when
each level of support arrived, and how often photographers typically reach
for each module.

Parameter storage shapes, semantic coupling, and candidate future parameter
classes are tracked in `2026-07-05-darktable-mcp-parameter-class-investigation.md`.
The wire contract lives in `2026-07-05-darktable-mcp-protocol-reference.md`.

## MCP tool surface

The sidecar exposes twelve tools; all of them work uniformly across every
listed module regardless of tier:

| Category | Tools |
|---|---|
| state and introspection | `get_current_image`, `list_modules`, `get_module_schema`, `get_module_params` |
| editing | `set_module_params` (scalars, plus semantic curves via `curves`, semantic vectors via `vectors`, semantic bands via `bands`, and semantic quantities via `quantities`) |
| module lifecycle | `set_module_enabled`, `reset_module`, `create_module_instance` |
| history | `get_history`, `undo` |
| visual feedback | `render_preview`, `get_scopes` |

The tiers below describe only what `set_module_params` can usefully edit.

## Milestone progression

Each milestone extended what `set_module_params` can write. The **since**
column in the tier tables names the milestone that established each
operation's current support level; `m1` means the baseline rules alone
already covered it.

| milestone | what it added | operations affected |
|---|---|---|
| **m1** | remote-edit protocol, the full twelve-tool sidecar surface, and scalar writes: finite `float`/`double`, integer, Boolean, and enum fields, including dotted scalar leaves inside plainly nested structs (`random.damping`, `center.x`). Tiering established. Design: `2026-07-05-darktable-mcp-design.md`; plan: `2026-07-05-darktable-mcp-implementation-plan.md`. | every module (baseline) |
| **m2** | semantic parameter classes and the curve engine (`src/control/remote_curve.c`): whole-curve replacement of named control-point curves, gated by the `curve_params` hello capability; per-module internal-field denylist (appendix). Design: `2026-07-11-darktable-mcp-milestone2-denylist-rgbcurve-design.md`. | `rgbcurve` → Tier 1 |
| **m3** | curve adapters for the remaining control-point-curve modules, including periodic (hue) curves and multi-channel Lab modes. Plan: `2026-07-11-darktable-mcp-milestone3-curve-adapters.md`. | `tonecurve`, `colorzones`, `basecurve` → Tier 1 |
| **m4** | the vector semantic class (`src/control/remote_vector.c`): named fixed-length vectors in three subtypes (plain `vector`, `color` with `display_rgb`, ordered `levels` triples), gated by the `vector_params` capability; 19 semantic names across five adapters. Design: `2026-07-12-darktable-mcp-milestone4-vector-class-design.md`. | `colorbalance`, `channelmixerrgb`, `rgblevels`, `borders` → Tier 1; `watermark` color writable (stays Tier 2 for its strings) |
| **m5** | the sampled-response (bands) semantic class (`src/control/remote_band.c`): whole-set replacement of fixed-count band arrays, with fixed or interior x policy and twin-channel x sharing, gated by the `band_params` capability; 16 semantic names across four adapters. Ride-alongs: the class-ops dispatch table in `remote_edit.c` replacing the per-class seams, and the sidecar unifying all caller-input errors on ToolError. Design: `2026-07-16-darktable-mcp-milestone5-bands-class-design.md`. | `atrous`, `denoiseprofile`, `rawdenoise`, `lowlight` → Tier 1 |
| **m6** | the quantity semantic class (`src/control/remote_quantity.c`): named component groups whose stored units differ from their presentation units, converted by an optional iop API hook pair (`remote_quantity_read`/`remote_quantity_write` — the first time a remote engine calls module code instead of reading blobs through introspection offsets), gated by the `quantity_params` capability; `wb.temperature` (Kelvin + tint) on `temperature`, whose native coefficient scalars deliberately stay writable alongside it (coexistence exception). Ride-alongs: `negadoctor` and `colorharmonizer` vector adapters on the unchanged milestone-4 engine. Design: `2026-07-17-darktable-mcp-milestone6-quantity-class-design.md`. | `temperature`, `negadoctor`, `colorharmonizer` → Tier 1 |

## How support is determined

The design spec limits `set_module_params` v1 to finite scalar `float`,
`double`, integer, Boolean, and enum fields, including scalar leaves inside
plainly nested structs, which are addressed by introspection's dotted names
(`random.damping`, `center.x`). Arrays, strings, curves, coordinate blobs,
unions, and opaque data are reported `writable: false` or omitted. Since
milestone 2, *semantic parameter classes* can lift specific modules past
that rule: an op with a registered curve adapter exposes writable
curve-class semantic parameters (gated by the `curve_params` hello
capability) even though its native node arrays stay `writable: false` —
`rgbcurve` (milestone 2) then `tonecurve`, `colorzones`, and `basecurve`
(milestone 3). Milestone 4 adds a second semantic class, the named
fixed-length vector (plain vectors, color triples, and levels triples),
exposing writable `vector`-class semantic parameters (gated by the
`vector_params` capability) for `colorbalance`, `channelmixerrgb`,
`rgblevels`, `borders`, and `watermark`, while their native arrays stay
`writable: false`. Milestone 5 adds a third class, the sampled response
(fixed-count band arrays with fixed or interior x positions and
twin-channel x sharing), exposing writable `bands`-class semantic
parameters (gated by the `band_params` capability) for `atrous`,
`denoiseprofile`, `rawdenoise`, and `lowlight`, again leaving the native
arrays `writable: false`. Milestone 6 adds a fourth class, the quantity
(named scalar components whose stored units differ from their presentation
units, converted by the module's own optional `remote_quantity_read`/
`remote_quantity_write` hooks), exposing the writable `wb.temperature`
Kelvin/tint pair (gated by the `quantity_params` capability) for
`temperature` — uniquely, the native coefficient scalars it represents
*stay* writable alongside it (they have been writable since milestone 1;
a request mixing a coefficient scalar with `wb.temperature` is rejected) —
and its vector ride-alongs promote `negadoctor` and `colorharmonizer`.
A module's tier follows from what fraction of its
user-facing controls survive these rules:

- **Tier 1 — full support.** Every user-facing parameter is a supported
  scalar/enum/bool or semantic parameter. The model can drive the module
  exactly as a user would. (Some modules carry internal bookkeeping fields;
  those are marked `writable: false` — see the appendix.)
- **Tier 2 — partial support.** The core controls are supported scalars, but
  some user-facing state lives in unsupported field types (arrays, curves,
  strings) or has stored values whose meaning differs from the GUI
  presentation. The MCP exposes the supported subset.
- **Tier 3 — parameter editing excluded.** The module's essence is
  unsupported data (curve nodes, drawn shapes, file paths, embedded LUTs,
  acquired histograms). `set_module_params` is useless or dangerous;
  `list_modules`, `set_module_enabled`, `reset_module`, history, and undo
  still work uniformly.
- **Deprecated.** Modules darktable itself flags `IOP_FLAGS_DEPRECATED`.
  They are listed (old edits reference them) and technically editable per
  their field shapes, but responses flag them and the model should never add
  them to a fresh edit — each has a modern replacement.
- **Hidden/internal.** Modules flagged `IOP_FLAGS_HIDDEN` or with no user
  parameters. Never returned by `list_modules`.

Modules flagged `IOP_FLAGS_ONE_INSTANCE` reject `create_module_instance`
with `instance_not_supported` (column "multi" below).

Blend settings (opacity, mode, colorspace, refinement controls,
off/uniform mask mode) are supported for every module with
`IOP_FLAGS_SUPPORTS_BLENDING` via the `blend_params` capability.
Parametric (blendif) masks and read-only mask rendering are supported via
`parametric_mask_params` and `mask_render`; creating/attaching drawn
geometry remains out of scope (M-C).

## Usage vocabulary

The **usage** column is a qualitative estimate of how often photographers
reach for each module in typical darktable practice — editorial judgment
from community knowledge (documentation, forums, default workflows), not
telemetry. It exists to help prioritize testing, tool descriptions, and
conversational steering.

- **core** — part of the default scene-referred workflow or adjusted on
  nearly every edit.
- **common** — regularly reached for across many edits.
- **occasional** — situational; used when the image calls for it.
- **rare** — niche or largely superseded; most users never touch it.
- A trailing `*` marks modules darktable auto-applies (raw pipeline
  defaults, EXIF orientation, or the chosen workflow preference), so their
  presence in an image's history is not a user choice; the rating reflects
  how often users deliberately *adjust* them. Note `filmicrgb` or `sigmoid`
  is also auto-applied — whichever the workflow preference selects.

## Tier 1 — full support (57 modules)

| op | display name | multi | since | usage | notes |
|---|---|---|---|---|---|
| `agx` | AgX | yes | m1 | common | scene-referred tone mapper; look + curve + primaries, all scalars |
| `ashift` | rotate and perspective | no | m1 | common | rotation/lens shift/shear scalars; line-fit state internal (appendix) |
| `atrous` | contrast equalizer | yes | m5 | common | **milestone 5, semantic bands**: `bands.luma`/`bands.chroma`/`bands.sharpness`/`bands.luma_threshold`/`bands.chroma_threshold` (six samples each, interior x; the luma and chroma pairs share x with their threshold twins) writable via `semantic_values` (`band_params` capability); `mix` scalar writable; native `x`/`y` arrays stay read-only with `represented_by`; `octaves` auto-derived (appendix) |
| `basecurve` | base curve | yes | m3 | occasional | **milestone 3, semantic curves**: `curve.master` via `semantic_values`; fusion scalars writable; reserved native channels stay hidden |
| `bilat` | local contrast | yes | m1 | common | mode enum + detail scalars |
| `bilateral` | surface blur | yes | m1 | rare | radius + per-channel sigmas |
| `bloom` | bloom | yes | m1 | rare | size/threshold/strength |
| `blurs` | blurs | yes | m1 | occasional | lens/motion/gaussian blur, fully parametric |
| `borders` | framing | yes | m4 | occasional | **milestone 4, semantic vectors**: `color`/`frame_color` writable via `semantic_values` (`vector_params` capability); `aspect`, `size`, `pos_h`, `pos_v`, frame scalars, `basis` enum writable; native arrays stay read-only with `represented_by`; unused `*_text` strings (appendix) |
| `cacorrect` | raw chromatic aberrations | no | m1 | occasional | raw only; bool + iterations enum |
| `cacorrectrgb` | chromatic aberrations | yes | m1 | occasional | guide channel enum + radius/strength |
| `censorize` | censorize | yes | m1 | rare | blur/pixelate radii |
| `channelmixerrgb` | color calibration | yes | m4 | core* | **milestone 4, semantic vectors**: `red`/`green`/`blue`/`saturation`/`lightness`/`grey` mixing rows writable via `semantic_values` (`vector_params` capability); illuminant/adaptation enums, `temperature`, `gamut`, `clip` scalars writable — usable for conversational white balance; native arrays stay read-only with `represented_by`; illuminant `x`,`y` set by picker; auto-applied under the modern chromatic-adaptation default |
| `colorbalance` | color balance | yes | m4 | occasional | **milestone 4, semantic vectors**: `lift`/`gamma`/`gain` (mode-gated aliases `offset`/`power`/`slope` under `SLOPE_OFFSET_POWER`) writable via `semantic_values` (`vector_params` capability); `saturation`, `contrast`, `grey`, `saturation_out`, mode enum scalars writable; native arrays stay read-only with `represented_by`; largely superseded by `colorbalancergb` |
| `colorbalancergb` | color balance rgb | yes | m1 | core | flagship grading module; 4-way Y/C/H + saturation/brilliance, all scalars |
| `colorcontrast` | color contrast | yes | m1 | rare | a/b steepness; offsets internal |
| `colorequal` | color equalizer | yes | m1 | common | per-hue-band saturation/hue/brightness as named scalars — ideal for "make the greens less yellow" |
| `colorharmonizer` | color harmonizer | yes | m6 | occasional | **milestone 6, semantic vectors**: `custom_hue` (writable only when `rule` is `DT_COLORHARMONIZER_CUSTOM` — a rule switch in the same request counts) and `node_saturation` writable via `semantic_values` (`vector_params` capability); rule enum, anchor hue, pull strength/width, smoothing scalars writable; native arrays stay read-only with `represented_by` |
| `colorize` | colorize | yes | m1 | rare | hue/saturation/lightness; `version` internal |
| `colorreconstruct` | color reconstruction | yes | m1 | rare | threshold/spatial/range scalars; op is the CMake/plugin name (`colorreconstruction.c` is the source filename) |
| `colorzones` | color zones | yes | m3 | common | **milestone 3, semantic curves**: `curve.lightness`/`curve.chroma`/`curve.hue`; periodic when select-by is hue; select-by change resets the curves (but not strength/mode, unlike the GUI's full reset) |
| `crop` | crop | no | m1 | core | normalized cx/cy/cw/ch; ratio ints are GUI aspect presets (caution) |
| `defringe` | defringe | yes | m1 | rare | radius/threshold + mode enum |
| `demosaic` | demosaic | no | m1 | occasional* | raw only; method enums + capture-sharpen scalars |
| `denoiseprofile` | denoise (profiled) | yes | m5 | common | **milestone 5, semantic bands**: `bands.all`/`bands.red`/`bands.green`/`bands.blue`/`bands.y0`/`bands.u0v0` wavelet curves (seven samples each, fixed x) writable via `semantic_values` (`band_params` capability); `strength`, `radius`, `nbhood`, `shadows`, `bias`, `scattering`, mode enums writable; native `x`/`y` arrays stay read-only with `represented_by`; noise-fit `a[3]`/`b[3]` stay excluded (appendix) |
| `diffuse` | diffuse or sharpen | yes | m1 | common | fully parametric anisotropic diffusion; pairs with presets |
| `dither` | dither or posterize | yes | m1 | rare | method enum + `random.damping` (dotted nested field); `palette`, `random.radius`, `random.range` reserved (appendix) |
| `enlargecanvas` | enlarge canvas | yes | m1 | rare | per-side percentages + color enum |
| `exposure` | exposure | yes | m1 | core* | flagship; EV exposure + black level; mode enum (manual/deflicker) |
| `filmicrgb` | filmic rgb | yes | m1 | core | full scene-referred curve as scalars/enums |
| `flip` | orientation | no | m1 | rare* | single orientation enum; set automatically from EXIF |
| `graduatednd` | graduated density | yes | m1 | occasional | density/hardness/rotation + hue/saturation |
| `grain` | grain | yes | m1 | occasional | channel enum + coarseness/strength |
| `hazeremoval` | haze removal | yes | m1 | occasional | strength/distance |
| `highlights` | highlight reconstruction | no | m1 | occasional* | raw only; method enum + scalars; `blendL`/`blendC` unused (appendix) |
| `highpass` | highpass | yes | m1 | rare | sharpness/contrast |
| `hotpixels` | hot pixels | no | m1 | occasional | raw only; strength/threshold + bools |
| `lowlight` | lowlight vision | yes | m5 | rare | **milestone 5, semantic bands**: `bands.transition` (six samples, interior x) writable via `semantic_values` (`band_params` capability); `blueness` scalar writable; native `transition_x`/`transition_y` arrays stay read-only with `represented_by` |
| `lowpass` | lowpass | yes | m1 | rare | radius/contrast/brightness/saturation; `unbound` internal |
| `negadoctor` | negadoctor | no | m6 | rare | **milestone 6, semantic vectors**: `dmin` (color, `display_rgb` — the film-substrate color), `wb_high`, `wb_low` writable via `semantic_values` (`vector_params` capability); film stock enum, `D_max`, `offset`, `black`, `gamma`, `soft_clip`, `exposure` scalars writable; native arrays stay read-only with `represented_by` |
| `nlmeans` | astrophoto denoise | yes | m1 | rare | patch size/strength/luma/chroma |
| `primaries` | rgb primaries | yes | m1 | occasional | per-primary hue/purity scalars |
| `profile_gamma` | unbreak input profile | no | m1 | rare | log/gamma scalars + mode enum |
| `rawdenoise` | raw denoise | yes | m5 | occasional | **milestone 5, semantic bands**: `bands.all`/`bands.red`/`bands.green`/`bands.blue` wavelet curves (five samples each, fixed x) writable via `semantic_values` (`band_params` capability); `threshold` scalar writable; native `x`/`y` arrays stay read-only with `represented_by` |
| `rgbcurve` | rgb curve | yes | m2 | common | **milestone 2, semantic curves**: `curve.master`/`curve.red`/`curve.green`/`curve.blue` writable via `semantic_values` (`curve_params` capability); mode/compensation scalars writable; native node arrays stay read-only with `represented_by` |
| `rgblevels` | rgb levels | yes | m4 | occasional | **milestone 4, semantic vectors**: `levels.linked` (autoscale linked) and `levels.red`/`levels.green`/`levels.blue` (autoscale independent) writable via `semantic_values` (`vector_params` capability); `autoscale`/`preserve_colors` enums writable; native `levels[3][3]` array stays read-only with `represented_by` |
| `scalepixels` | scale pixels | no | m1 | rare | single pixel-aspect scalar (niche, camera-specific) |
| `shadhi` | shadows and highlights | yes | m1 | occasional | shadows/highlights/radius scalars; `flags`, `reserved2`, `low_approximation` internal |
| `sharpen` | sharpen | yes | m1 | common | radius/amount/threshold |
| `sigmoid` | sigmoid | yes | m1 | core* | contrast/skew + per-primary attenuation, all scalars/enums |
| `soften` | soften | yes | m1 | rare | orton-effect scalars |
| `splittoning` | split-toning | yes | m1 | occasional | shadow/highlight hue+saturation, balance, compress |
| `temperature` | white balance | no | m6 | core* | **milestone 6, semantic quantity**: `wb.temperature` — a Kelvin + tint pair written atomically, converted to/from the stored RGB multipliers by the module's own hooks — writable via `semantic_values` (`quantity_params` capability); readback is derived and lossy (compare with tolerance). The `red`/`green`/`blue`/`various` multiplier scalars **stay writable** alongside it (coexistence exception) and carry `represented_by`; a request mixing a coefficient scalar with `wb.temperature` is rejected; `preset` internal (appendix) |
| `tonecurve` | tone curve | yes | m3 | common | **milestone 3, semantic curves**: `curve.lightness` always, `curve.a`/`curve.b` in independent-Lab mode; color-space scalar writable |
| `toneequal` | tone equalizer | yes | m1 | core | nine named EV-band scalars — the natural target for "lift the shadows" |
| `velvia` | velvia | yes | m1 | occasional | strength + mid-tones bias |
| `vignette` | vignetting | yes | m1 | common | scale/falloff/brightness/saturation/shape + `center.x`/`center.y` (dotted, rangeless — finiteness-only validation); `unbound` internal |

## Tier 2 — partial support (8 modules)

| op | display name | multi | since | usage | supported | unsupported / caveats |
|---|---|---|---|---|---|---|
| `colorcorrection` | color correction | yes | m1 | rare | `saturation` | `hia`/`hib`/`loa`/`lob` are unranged Lab-ish grid coordinates; stored semantics unclear |
| `colorin` | input color profile | no | m1 | rare* | profile `type`/`intent`/`normalize` enums (caution: pipeline-level change) | ICC `filename` strings |
| `colorout` | output color profile | no | m1 | rare* | profile `type`/`intent` enums (caution) | ICC `filename` string |
| `lens` | lens correction | yes | m1 | common | `method`/`modify_flags`/`target_geom` enums, `scale`, TCA overrides, fine-tune scalars | `camera[128]`/`lens[128]` strings; EXIF-derived `crop`/`focal`/`aperture`/`distance` (appendix) |
| `monochrome` | monochrome | yes | m1 | occasional | `size`, `highlights` | `a`/`b` are unranged Lab filter coordinates set by GUI drag/picker |
| `overlay` | composite | yes | m1 | rare | `opacity`, `scale`, `rotate`, offsets, scale-mode enums | overlay image reference (`imgid`, `filename`) must be chosen in the GUI first |
| `rawprepare` | raw black/white point | no | m1 | rare* | crop scalars, `raw_white_point`, flat-field enum — **caution: sensor-level values; wrong edits break the image** | `raw_black_level_separate[4]` array |
| `watermark` | watermark | yes | m4 | occasional | `opacity`, `scale`, `rotate`, offsets, alignment, scale enums; **milestone 4, semantic vectors**: `color` writable via `semantic_values` (`vector_params` capability); native array stays read-only with `represented_by` | `filename`/`text`/`font` strings still unsupported (near-Tier 1) |

## Tier 3 — parameter editing excluded (6 modules)

Enable/disable, reset, history, and undo remain available (and have been
since milestone 1); `set_module_params` is not useful because the module's
essence is unsupported data.

| op | display name | usage | why excluded |
|---|---|---|---|
| `colorchecker` | color look up table | rare | source/target Lab patch tables |
| `colormapping` | color mapping | rare | acquired source/target histograms and cluster statistics; scalars meaningless without GUI acquisition |
| `liquify` | liquify | occasional | serialized warp-node coordinate blob |
| `lut3d` | LUT 3D | occasional | LUT file path + embedded compressed LUT blob; enums meaningless without a loaded LUT |
| `rasterfile` | external raster masks | rare | file-path strings |
| `retouch` | retouch | common | drawn-form data (`rt_forms`); algorithm scalars only act on shapes drawn in the GUI |

## Deprecated (16 modules)

Listed by `list_modules` with a deprecated flag when present in an image's
history; never added to fresh edits. Usage for every module here is
**legacy edits only** — they appear when opening images edited with old
darktable versions. Modern replacement in parentheses.

| op | display name | replacement |
|---|---|---|
| `basicadj` | basic adjustments | exposure + filmic/sigmoid |
| `channelmixer` | channel mixer | channelmixerrgb (color calibration) |
| `clahe` | old local contrast | bilat (local contrast) |
| `clipping` | crop and rotate | crop + ashift |
| `colisa` | contrast brightness saturation | colorbalancergb |
| `colortransfer` | color transfer | colormapping |
| `equalizer` | legacy equalizer | atrous (contrast equalizer) |
| `filmic` | filmic | filmicrgb |
| `globaltonemap` | global tonemap | filmicrgb / sigmoid |
| `invert` | invert | negadoctor |
| `levels` | levels | rgblevels |
| `relight` | fill light | toneequal |
| `spots` | spot removal | retouch |
| `tonemap` | tone mapping | filmicrgb / sigmoid |
| `vibrance` | vibrance | colorbalancergb (global vibrance) |
| `zonesystem` | zone system | toneequal |

## Hidden / internal (7 modules)

Never returned by `list_modules`: `finalscale`, `gamma` (display transform),
`mask_manager`, `overexposed`, `rawoverexposed`, `rotatepixels` — all
`IOP_FLAGS_HIDDEN` and/or `IOP_FLAGS_NO_HISTORY_STACK` — plus `useless`
(the example/template module, built only in development configurations).

## Recommended conversational priority

The modules to exercise first in tests and to steer the model toward in tool
descriptions, because they cover most conversational editing requests with
Tier 1 support (these are the `core`/`common` rows above):

1. **Tonal**: `exposure`, `toneequal`, `filmicrgb` or `sigmoid` or `agx`
2. **Color**: `colorbalancergb`, `colorequal`, `temperature` (white
   balance in real Kelvin/tint via `wb.temperature`), `channelmixerrgb`
   (white balance via illuminant/temperature)
3. **Presence**: `diffuse`, `sharpen`, `bilat`, `hazeremoval`, `velvia`
4. **Noise**: `denoiseprofile`
5. **Geometry/finishing**: `crop`, `flip`, `ashift`, `vignette`, `grain`,
   `splittoning`, `graduatednd`

## Appendix — internal fields to mark `writable: false`

Scalar fields that pass the type filter but must not be offered as editable
because they are bookkeeping, auto-derived, or unused. The schema generator
needs an explicit per-module denylist for these (or a `$DESCRIPTION`-absent
heuristic verified per module):

| op | fields | reason |
|---|---|---|
| `ashift` | `cl`, `cr`, `ct`, `cb`, `last_drawn_lines_count` (the `last_drawn_lines`/`last_quad_lines` arrays are already excluded by type; there is no separate `last_drawn_lines_version` field) | auto-crop results and line-fit state |
| `atrous` | `octaves` | auto-derived from image size in commit_params |
| `basecurve` | — | (curve arrays already excluded by type) |
| `borders` | `aspect_text`, `pos_h_text`, `pos_v_text` | marked UNUSED in source |
| `channelmixerrgb` | `x`, `y`, `version` | picker-set illuminant coords; algorithm version |
| `colorcontrast` | `a_offset`, `b_offset`, `unbound` | no GUI, legacy |
| `colorize` | `version` | internal versioning |
| `colorzones` | `splines_version` | internal versioning; semantic curve writes stamp it to V2, so untouched curves in a legacy V1 edit are re-rendered under V2 spline math |
| `crop` | `ratio_n`, `ratio_d` | GUI aspect-preset state; writable only with care |
| `denoiseprofile` | `a[3]`, `b[3]` (arrays), `fix_anscombe_and_nlmeans_norm`, `use_new_vst`, `wb_adaptive_anscombe` | camera-profile fit; backward-compat switches |
| `dither` | `palette`, `random.radius`, `random.range` | reserved for future extensions |
| `filmicrgb` | `version`, `spline_version` | color-science compat enums; changing them alters interpretation of other fields |
| `highlights` | `blendL`, `blendC` | marked unused in source |
| `lens` | `crop`, `focal`, `aperture`, `distance`, `has_been_set`, `md_version`, `reserved` | EXIF-derived / bookkeeping |
| `lowpass` | `unbound` | legacy |
| `overlay` | `imgid`, `dummy0..2` | image reference and padding |
| `relight` | `center` | no GUI range |
| `shadhi` | `reserved2`, `flags`, `low_approximation` | legacy/unbound flags |
| `temperature` | `preset` | GUI preset bookkeeping |
| `tonecurve` | `tonecurve_preset`, `tonecurve_unbound_ab` | preset bookkeeping |
| `vignette` | `unbound` | legacy clipping flag |

## Maintenance

Regenerate the inventory when modules are added or params structs change:
the raw data comes from `DT_MODULE_INTROSPECTION` declarations, params
structs, `flags()` implementations, and `name()` strings in `src/iop/*.c`
and `src/iop/*.cc`. Derive operation identifiers from the first argument to
`add_iop()` in `src/iop/CMakeLists.txt`, not from source filenames; notably,
the `colorreconstruct` operation is implemented by `colorreconstruction.c`.
Cross-check any tier change against the design spec's supported-type list
before moving a module between tiers.

For the two progression columns: update **since** whenever a milestone
changes a module's support level (and add a row to the milestone table);
**usage** is an editorial estimate — revisit it when darktable's default
workflow changes (e.g. a new default tone mapper) or when a module is
deprecated or replaced.
