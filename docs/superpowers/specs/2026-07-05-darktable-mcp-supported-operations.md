# darktable MCP — Supported Operations Reference

Date: 2026-07-05
Status: reference (companion to `2026-07-05-darktable-mcp-design.md`)
Source inventory: `src/iop/` at commit `81129368e0` (this fork)

## Purpose

A complete inventory of darktable's darkroom operations (IOP modules) and how
the MCP remote-edit surface supports each one. The runtime source of truth is
always `get_module_schema` — this document is the planning and review
reference: which modules the model can usefully edit, which are partially
editable, which are excluded and why.

Parameter storage shapes, semantic coupling, and candidate future parameter
classes are tracked in `2026-07-05-darktable-mcp-parameter-class-investigation.md`.

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
(milestone 3). A module's tier follows from what fraction of its
user-facing controls survive these rules:

- **Tier 1 — full support.** Every user-facing parameter is a supported
  scalar/enum/bool. The model can drive the module exactly as a user would.
  (Some modules carry internal bookkeeping fields; those are marked
  `writable: false` — see the appendix.)
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

Blending and mask parameters (`blendop`) are out of scope for every module,
per the design spec's non-goals.

## Tier 1 — full support (46 modules)

| op | display name | multi | notes |
|---|---|---|---|
| `agx` | AgX | yes | scene-referred tone mapper; look + curve + primaries, all scalars |
| `ashift` | rotate and perspective | no | rotation/lens shift/shear scalars; line-fit state internal (appendix) |
| `basecurve` | base curve | yes | **milestone 3, semantic curves**: `curve.master` via `semantic_values`; fusion scalars writable; reserved native channels stay hidden |
| `bilat` | local contrast | yes | mode enum + detail scalars |
| `bilateral` | surface blur | yes | radius + per-channel sigmas |
| `bloom` | bloom | yes | size/threshold/strength |
| `blurs` | blurs | yes | lens/motion/gaussian blur, fully parametric |
| `cacorrect` | raw chromatic aberrations | no | raw only; bool + iterations enum |
| `cacorrectrgb` | chromatic aberrations | yes | guide channel enum + radius/strength |
| `censorize` | censorize | yes | blur/pixelate radii |
| `colorbalancergb` | color balance rgb | yes | flagship grading module; 4-way Y/C/H + saturation/brilliance, all scalars |
| `colorcontrast` | color contrast | yes | a/b steepness; offsets internal |
| `colorequal` | color equalizer | yes | per-hue-band saturation/hue/brightness as named scalars — ideal for "make the greens less yellow" |
| `colorize` | colorize | yes | hue/saturation/lightness; `version` internal |
| `colorreconstruct` | color reconstruction | yes | threshold/spatial/range scalars; op is the CMake/plugin name (`colorreconstruction.c` is the source filename) |
| `colorzones` | color zones | yes | **milestone 3, semantic curves**: `curve.lightness`/`curve.chroma`/`curve.hue`; periodic when select-by is hue; select-by change resets curves like the GUI |
| `crop` | crop | no | normalized cx/cy/cw/ch; ratio ints are GUI aspect presets (caution) |
| `defringe` | defringe | yes | radius/threshold + mode enum |
| `demosaic` | demosaic | no | raw only; method enums + capture-sharpen scalars |
| `diffuse` | diffuse or sharpen | yes | fully parametric anisotropic diffusion; pairs with presets |
| `dither` | dither or posterize | yes | method enum + `random.damping` (dotted nested field); `palette`, `random.radius`, `random.range` reserved (appendix) |
| `enlargecanvas` | enlarge canvas | yes | per-side percentages + color enum |
| `exposure` | exposure | yes | flagship; EV exposure + black level; mode enum (manual/deflicker) |
| `filmicrgb` | filmic rgb | yes | full scene-referred curve as scalars/enums |
| `flip` | orientation | no | single orientation enum |
| `graduatednd` | graduated density | yes | density/hardness/rotation + hue/saturation |
| `grain` | grain | yes | channel enum + coarseness/strength |
| `hazeremoval` | haze removal | yes | strength/distance |
| `highlights` | highlight reconstruction | no | raw only; method enum + scalars; `blendL`/`blendC` unused (appendix) |
| `highpass` | highpass | yes | sharpness/contrast |
| `hotpixels` | hot pixels | no | raw only; strength/threshold + bools |
| `lowpass` | lowpass | yes | radius/contrast/brightness/saturation; `unbound` internal |
| `nlmeans` | astrophoto denoise | yes | patch size/strength/luma/chroma |
| `primaries` | rgb primaries | yes | per-primary hue/purity scalars |
| `profile_gamma` | unbreak input profile | no | log/gamma scalars + mode enum |
| `rgbcurve` | rgb curve | yes | **milestone 2, semantic curves**: `curve.master`/`curve.red`/`curve.green`/`curve.blue` writable via `semantic_values` (`curve_params` capability); mode/compensation scalars writable; native node arrays stay read-only with `represented_by` |
| `scalepixels` | scale pixels | no | single pixel-aspect scalar (niche, camera-specific) |
| `shadhi` | shadows and highlights | yes | shadows/highlights/radius scalars; `flags`, `reserved2`, `low_approximation` internal |
| `sharpen` | sharpen | yes | radius/amount/threshold |
| `sigmoid` | sigmoid | yes | contrast/skew + per-primary attenuation, all scalars/enums |
| `soften` | soften | yes | orton-effect scalars |
| `splittoning` | split-toning | yes | shadow/highlight hue+saturation, balance, compress |
| `tonecurve` | tone curve | yes | **milestone 3, semantic curves**: `curve.lightness` always, `curve.a`/`curve.b` in independent-Lab mode; color-space scalar writable |
| `toneequal` | tone equalizer | yes | nine named EV-band scalars — the natural target for "lift the shadows" |
| `velvia` | velvia | yes | strength + mid-tones bias |
| `vignette` | vignetting | yes | scale/falloff/brightness/saturation/shape + `center.x`/`center.y` (dotted, rangeless — finiteness-only validation); `unbound` internal |

## Tier 2 — partial support (17 modules)

| op | display name | multi | supported | unsupported / caveats |
|---|---|---|---|---|
| `borders` | framing | yes | `aspect`, `size`, `pos_h`, `pos_v`, frame scalars, `basis` enum | `color[3]`/`frame_color[3]` arrays; unused `*_text` strings |
| `channelmixerrgb` | color calibration | yes | illuminant/adaptation enums, `temperature`, `gamut`, `clip` — usable for conversational white balance | per-channel mixing arrays (`red[4]`…`grey[4]`); illuminant `x`,`y` set by picker |
| `colorbalance` | color balance | yes | `saturation`, `contrast`, `grey`, `saturation_out`, mode enum | `lift`/`gamma`/`gain` arrays (the module's core) |
| `colorcorrection` | color correction | yes | `saturation` | `hia`/`hib`/`loa`/`lob` are unranged Lab-ish grid coordinates; stored semantics unclear |
| `colorharmonizer` | color harmonizer | yes | rule enum, anchor hue, pull strength/width, smoothing | `custom_hue[4]`, `node_saturation[4]` arrays |
| `colorin` | input color profile | no | profile `type`/`intent`/`normalize` enums (caution: pipeline-level change) | ICC `filename` strings |
| `colorout` | output color profile | no | profile `type`/`intent` enums (caution) | ICC `filename` string |
| `denoiseprofile` | denoise (profiled) | yes | `strength`, `radius`, `nbhood`, `shadows`, `bias`, `scattering`, mode enums | wavelet band curves `x`/`y`, noise-fit `a[3]`/`b[3]` (auto-set from camera profile) |
| `lens` | lens correction | yes | `method`/`modify_flags`/`target_geom` enums, `scale`, TCA overrides, fine-tune scalars | `camera[128]`/`lens[128]` strings; EXIF-derived `crop`/`focal`/`aperture`/`distance` (appendix) |
| `lowlight` | lowlight vision | yes | `blueness` | band arrays `transition_x`/`transition_y` |
| `monochrome` | monochrome | yes | `size`, `highlights` | `a`/`b` are unranged Lab filter coordinates set by GUI drag/picker |
| `negadoctor` | negadoctor | no | film stock enum, `D_max`, `offset`, `black`, `gamma`, `soft_clip`, `exposure` | `Dmin[4]`, `wb_high[4]`, `wb_low[4]` RGB arrays (usually set by picker) |
| `overlay` | composite | yes | `opacity`, `scale`, `rotate`, offsets, scale-mode enums | overlay image reference (`imgid`, `filename`) must be chosen in the GUI first |
| `rawdenoise` | raw denoise | yes | `threshold` | wavelet band arrays `x`/`y` |
| `rawprepare` | raw black/white point | no | crop scalars, `raw_white_point`, flat-field enum — **caution: sensor-level values; wrong edits break the image** | `raw_black_level_separate[4]` array |
| `temperature` | white balance | no | `red`/`green`/`blue`/`various` channel coefficients — **stored values are multipliers, not Kelvin**; the GUI's Kelvin/tint is a derived presentation (future metadata layer) | `preset` internal |
| `watermark` | watermark | yes | `opacity`, `scale`, `rotate`, offsets, alignment, scale enums | `filename`/`text`/`font` strings, `color[3]` array |

## Tier 3 — parameter editing excluded (8 modules)

Enable/disable, reset, history, and undo remain available; `set_module_params`
is not useful because the module's essence is unsupported data. The two
remaining curve-shaped entries here are explicitly out of the curve-adapter
pattern's reach: `atrous`'s per-band controls are fixed parallel arrays, not
a control-point curve, and would need a new native layout kind; `rgblevels`'
per-channel data is levels triples (black/grey/white), not curve nodes, and
would need a levels parameter class instead.

| op | display name | why excluded |
|---|---|---|
| `atrous` | contrast equalizer | per-band spline curves (`x`/`y` 2-D arrays) |
| `colorchecker` | color look up table | source/target Lab patch tables |
| `colormapping` | color mapping | acquired source/target histograms and cluster statistics; scalars meaningless without GUI acquisition |
| `liquify` | liquify | serialized warp-node coordinate blob |
| `lut3d` | LUT 3D | LUT file path + embedded compressed LUT blob; enums meaningless without a loaded LUT |
| `rasterfile` | external raster masks | file-path strings |
| `retouch` | retouch | drawn-form data (`rt_forms`); algorithm scalars only act on shapes drawn in the GUI |
| `rgblevels` | rgb levels | per-channel `levels` 2-D array is the whole control |

## Deprecated (16 modules)

Listed by `list_modules` with a deprecated flag when present in an image's
history; never added to fresh edits. Modern replacement in parentheses.

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
Tier 1 support:

1. **Tonal**: `exposure`, `toneequal`, `filmicrgb` or `sigmoid` or `agx`
2. **Color**: `colorbalancergb`, `colorequal`, `channelmixerrgb` (white
   balance via illuminant/temperature)
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
| `basecurve` | — | (curve arrays already excluded by type) |
| `channelmixerrgb` | `x`, `y`, `version` | picker-set illuminant coords; algorithm version |
| `colorcontrast` | `a_offset`, `b_offset`, `unbound` | no GUI, legacy |
| `colorize` | `version` | internal versioning |
| `colorzones` | `splines_version` | internal versioning |
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
